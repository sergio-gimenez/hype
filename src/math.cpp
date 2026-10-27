#include "math.h"
#include "renderer.h"
#include <QAbstractTextDocumentLayout>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFontMetricsF>
#include <QHash>
#include <QMutex>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QXmlStreamReader>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextObjectInterface>

namespace {
constexpr int MathObject = QTextFormat::UserObject + 1;
constexpr int TexProperty = QTextFormat::UserProperty + 2;
constexpr int DisplayProperty = QTextFormat::UserProperty + 3;
constexpr int SerifProperty = QTextFormat::UserProperty + 4;
constexpr ushort FirstPlaceholder = 0xE000;
constexpr int MostFormulas = 6400; // The private-use area's size.
constexpr qreal TexEm = 10.95;     // beamer's 11pt, in TeX points.

QMutex mutex; // Guards the cache, and lets one LaTeX run at a time.
QHash<QString, Formula> cache;

QString cacheKey(const MathSpan &math, bool serif) {
    const QString identity = QString("hype-math 2\n%1 %2\n").arg(serif ? "serif" : "sans", math.display ? "display" : "text") + math.tex;
    return QString::fromLatin1(QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha1).toHex());
}
QString cacheFolder() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/hype/math";
}
// SVG path data, as dvisvgm writes glyph outlines: moves, lines, and cubic and quadratic curves.
QPainterPath pathData(const QString &data) {
    static const QRegularExpression token(R"([MmLlHhVvCcSsQqTtZz]|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)");
    QPainterPath path;
    QList<QString> tokens;
    auto matches = token.globalMatch(data);
    while (matches.hasNext())
        tokens << matches.next().captured();
    QChar command;
    QPointF current, start, control; // control: the last curve's second control point, for S and T.
    QChar previous;
    int i = 0;
    const auto number = [&] { return i < tokens.size() ? tokens[i++].toDouble() : 0.0; };
    const auto isNumber = [&] { return i < tokens.size() && !tokens[i][0].isLetter(); };
    while (i < tokens.size()) {
        if (!isNumber())
            command = tokens[i++][0];
        const bool relative = command.isLower();
        const QPointF origin = relative ? current : QPointF();
        const auto point = [&] { const qreal x = number(); return origin + QPointF(x, number()); };
        switch (command.toUpper().unicode()) {
        case 'M':
            current = start = point();
            path.moveTo(current);
            command = relative ? 'l' : 'L'; // Further pairs are lines.
            break;
        case 'L': current = point(); path.lineTo(current); break;
        case 'H': current.setX((relative ? current.x() : 0) + number()); path.lineTo(current); break;
        case 'V': current.setY((relative ? current.y() : 0) + number()); path.lineTo(current); break;
        case 'C': {
            const QPointF first = point(), second = point(), end = point();
            path.cubicTo(first, second, end);
            control = second;
            current = end;
            break;
        }
        case 'S': {
            const QPointF first = QStringLiteral("CcSs").contains(previous) ? 2 * current - control : current;
            const QPointF second = point(), end = point();
            path.cubicTo(first, second, end);
            control = second;
            current = end;
            break;
        }
        case 'Q': {
            const QPointF middle = point(), end = point();
            path.quadTo(middle, end);
            control = middle;
            current = end;
            break;
        }
        case 'T': {
            const QPointF middle = QStringLiteral("QqTt").contains(previous) ? 2 * current - control : current;
            const QPointF end = point();
            path.quadTo(middle, end);
            control = middle;
            current = end;
            break;
        }
        case 'Z': path.closeSubpath(); current = start; break;
        default: return path; // Nothing dvisvgm writes; stop rather than guess.
        }
        previous = command;
    }
    return path;
}
// dvisvgm's picture: glyphs defined once and placed with <use>, and rules as <rect>.
Formula fromSvg(const QByteArray &svg) {
    Formula formula;
    QHash<QString, QPainterPath> glyphs;
    QXmlStreamReader xml(svg);
    QPainterPath outline;
    outline.setFillRule(Qt::WindingFill);
    while (!xml.atEnd()) {
        if (xml.readNext() != QXmlStreamReader::StartElement)
            continue;
        const auto a = xml.attributes();
        const auto number = [&](const char *name) { return a.value(name).toDouble(); };
        if (xml.name() == u"svg") {
            const QStringList box = a.value("viewBox").toString().split(' ', Qt::SkipEmptyParts);
            if (box.size() == 4)
                formula.box = QRectF(box[0].toDouble(), box[1].toDouble(), box[2].toDouble(), box[3].toDouble());
        } else if (xml.name() == u"path" && a.hasAttribute("id"))
            glyphs.insert(a.value("id").toString(), pathData(a.value("d").toString()));
        else if (xml.name() == u"path")
            outline.addPath(pathData(a.value("d").toString()));
        else if (xml.name() == u"use") {
            QString link = a.value("xlink:href").toString();
            if (link.isEmpty())
                link = a.value("href").toString();
            outline.addPath(glyphs.value(link.mid(1)).translated(number("x"), number("y")));
        } else if (xml.name() == u"rect")
            outline.addRect(number("x"), number("y"), number("width"), number("height"));
    }
    if (xml.hasError() || formula.box.isEmpty()) {
        formula.error = "LaTeX gave an unreadable picture";
        return formula;
    }
    formula.svg = svg;
    formula.outline = outline;
    return formula;
}
bool run(const QString &program, const QStringList &arguments, const QString &folder, QString *output) {
    QProcess process;
    process.setWorkingDirectory(folder);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, arguments);
    const bool finished = process.waitForStarted(10000) && process.waitForFinished(30000);
    if (!finished)
        process.kill();
    *output = QString::fromUtf8(process.readAll());
    return finished && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}
// One beamer document with each formula on its own page, cropped to the formula with its
// baseline at the origin, then turned into outlines so no fonts are needed to draw it.
QVector<Formula> compile(const QVector<MathSpan> &math, bool serif) {
    QVector<Formula> made(math.size());
    const QString latex = QStandardPaths::findExecutable("latex"),
                  dvisvgm = QStandardPaths::findExecutable("dvisvgm");
    const auto fail = [&](const QString &error) {
        for (auto &formula : made)
            formula.error = error;
        return made;
    };
    if (latex.isEmpty() || dvisvgm.isEmpty())
        return fail("Math needs LaTeX: install latex and dvisvgm (TeX Live)");
    QTemporaryDir folder;
    QString tex = "\\documentclass{beamer}\n\\usepackage{amsmath,amssymb,mathtools,bm}\n";
    if (serif)
        tex += "\\usefonttheme[onlymath]{serif}\n";
    tex += "\\usepackage[active,tightpage]{preview}\n\\setlength\\PreviewBorder{0pt}\n\\begin{document}\n";
    for (const auto &formula : math)
        tex += QString("\\begin{preview}$%1%2$\\end{preview}\n").arg(formula.display ? "\\displaystyle " : "", formula.tex);
    tex += "\\end{document}\n";
    QFile file(folder.filePath("math.tex"));
    if (!folder.isValid() || !file.open(QIODevice::WriteOnly) || file.write(tex.toUtf8()) < 0)
        return fail("Could not prepare LaTeX: " + file.errorString());
    file.close();
    QString output;
    if (!run(latex, {"-interaction=nonstopmode", "-halt-on-error", "-no-shell-escape", "math.tex"}, folder.path(), &output)) {
        // The first error line, as LaTeX words it.
        static const QRegularExpression error("^! (.+)$", QRegularExpression::MultilineOption);
        const QString message = error.match(output).captured(1).trimmed();
        return fail("LaTeX: " + (message.isEmpty() ? QString("could not set the math") : message));
    }
    // Each glyph drawn where it sits: the default defines a glyph only on the first page that
    // uses it, which leaves later pages pointing at nothing.
    if (!run(dvisvgm, {"--page=1-", "--bbox=preview", "--no-fonts=1", "--output=math-%p.svg", "math.dvi"}, folder.path(), &output))
        return fail("dvisvgm could not convert the math: " + output.trimmed().section('\n', -1));
    // Page numbers are padded to the same width, so names sort in page order.
    const QStringList pages = QDir(folder.path()).entryList({"math-*.svg"}, QDir::Files, QDir::Name);
    if (pages.size() != math.size())
        return fail("LaTeX set the math on the wrong number of pages");
    for (int i = 0; i < pages.size(); ++i) {
        QFile page(folder.filePath(pages[i]));
        if (page.open(QIODevice::ReadOnly))
            made[i] = fromSvg(page.readAll());
        else
            made[i].error = "Could not read LaTeX's output";
    }
    return made;
}

class MathHandler : public QObject, public QTextObjectInterface {
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)
  public:
    QSizeF intrinsicSize(QTextDocument *doc, int, const QTextFormat &format) override {
        const Formula formula = lookup(format);
        if (formula.svg.isEmpty()) {
            const QFontMetricsF metrics(font(doc, format));
            return {metrics.horizontalAdvance(source(format)), metrics.ascent()};
        }
        const qreal scale = em(doc, format) / TexEm;
        // A formula in a line sits on its baseline and hangs below it like a descender; one on
        // its own line takes its whole height.
        return {formula.box.width() * scale,
                (format.boolProperty(MathBlockProperty) ? formula.box.height() : -formula.box.top()) * scale};
    }
    void drawObject(QPainter *painter, const QRectF &rect, QTextDocument *doc, int,
                    const QTextFormat &format) override {
        const Formula formula = lookup(format);
        const QColor color = format.foreground().color();
        if (formula.svg.isEmpty()) {
            painter->save();
            painter->setFont(font(doc, format));
            painter->setPen(QColor("#d04040"));
            painter->drawText(QPointF(rect.left(), rect.bottom()), source(format));
            painter->restore();
            return;
        }
        // The outlines are filled in the text's color, never stroked, so they print as thin as
        // LaTeX drew them.
        const qreal scale = rect.width() / formula.box.width();
        painter->save();
        painter->translate(rect.left() - formula.box.left() * scale, rect.top() - formula.box.top() * scale);
        painter->scale(scale, scale);
        painter->fillPath(formula.outline, color);
        painter->restore();
    }

  private:
    static Formula lookup(const QTextFormat &format) {
        return typesetFormula({format.stringProperty(TexProperty), format.boolProperty(DisplayProperty)},
                              format.boolProperty(SerifProperty));
    }
    static QString source(const QTextFormat &format) {
        const QString delimiter = format.boolProperty(DisplayProperty) ? "$$" : "$";
        return delimiter + format.stringProperty(TexProperty) + delimiter;
    }
    // The text's size where the formula sits. A heading may carry a relative size, which the
    // layout resolves against the document's font as Qt's HTML sizes do.
    static qreal em(QTextDocument *doc, const QTextFormat &format) {
        if (format.hasProperty(QTextFormat::FontSizeAdjustment) && doc) {
            static const qreal factors[] = {0.7, 0.8, 1.0, 1.2, 1.5, 2.0, 2.4};
            const int step = qBound(0, format.intProperty(QTextFormat::FontSizeAdjustment) + 2, 6);
            return doc->defaultFont().pixelSize() * factors[step];
        }
        const int pixels = format.toCharFormat().font().pixelSize();
        return pixels > 0 ? pixels : (doc ? doc->defaultFont().pixelSize() : 16);
    }
    static QFont font(QTextDocument *doc, const QTextFormat &format) {
        QFont font = format.toCharFormat().font();
        font.setPixelSize(qMax(1, qRound(em(doc, format))));
        return font;
    }
};
MathHandler *handler() {
    // Shared by every document on every thread; it holds no state of its own.
    static MathHandler *shared = new MathHandler;
    return shared;
}
} // namespace

QString withoutMath(const QString &markdown, QVector<MathSpan> *math) {
    if (!markdown.contains('$'))
        return markdown;
    // Pandoc's rules: $$ pairs may span lines; a single $ opens before a non-space, closes after
    // one, and is not followed by a digit, so prices stay prices. \$ is a dollar sign.
    static const QRegularExpression formula(
        R"((?<!\\)\$\$([\s\S]+?)(?<!\\)\$\$|(?<![\\$])\$(?=[^\s$])((?:\\.|[^$\\\n])+?)(?<=\S)\$(?!\d))");
    const QString visible = outsideCode(markdown);
    QString out;
    int last = 0;
    auto matches = formula.globalMatch(visible);
    while (matches.hasNext() && math->size() < MostFormulas) {
        const auto match = matches.next();
        const bool display = match.capturedStart(1) >= 0;
        const int group = display ? 1 : 2;
        const QString tex = markdown.mid(match.capturedStart(group), match.capturedLength(group)).trimmed();
        if (tex.isEmpty())
            continue;
        out += markdown.mid(last, match.capturedStart() - last);
        const QChar placeholder(ushort(FirstPlaceholder + math->size()));
        math->append({tex, display});
        // Display math alone on its lines becomes a paragraph of its own, set in the middle.
        const int lineStart = markdown.lastIndexOf('\n', qMax(0, match.capturedStart() - 1));
        const bool startsLine = match.capturedStart() == 0 || markdown.mid(lineStart + 1, match.capturedStart() - lineStart - 1).isEmpty();
        const int lineEnd = markdown.indexOf('\n', match.capturedEnd());
        const bool endsLine = markdown.mid(match.capturedEnd(), (lineEnd < 0 ? markdown.size() : lineEnd) - match.capturedEnd()).trimmed().isEmpty();
        out += display && startsLine && endsLine ? "\n\n" + QString(placeholder) + "\n\n" : QString(placeholder);
        last = match.capturedEnd();
    }
    return out + markdown.mid(last);
}
void prepareFormulas(const QVector<MathSpan> &math, bool serif) {
    QMutexLocker lock(&mutex);
    QVector<MathSpan> missing;
    QStringList keys;
    for (const auto &formula : math) {
        const QString key = cacheKey(formula, serif);
        if (cache.contains(key) || keys.contains(key))
            continue;
        QFile saved(cacheFolder() + "/" + key + ".svg");
        if (saved.open(QIODevice::ReadOnly)) {
            if (const Formula loaded = fromSvg(saved.readAll()); loaded.error.isEmpty()) {
                cache.insert(key, loaded);
                continue;
            }
        }
        missing << formula;
        keys << key;
    }
    if (missing.isEmpty())
        return;
    QVector<Formula> made = compile(missing, serif);
    // One bad formula stops the whole run; set each alone to find which.
    if (missing.size() > 1 && !made.first().error.isEmpty() && !made.first().error.startsWith("Math needs"))
        for (int i = 0; i < missing.size(); ++i)
            made[i] = compile({missing[i]}, serif).first();
    QDir().mkpath(cacheFolder());
    for (int i = 0; i < missing.size(); ++i) {
        if (!made[i].error.isEmpty())
            made[i].error += " in " + QString(missing[i].display ? "$$%1$$" : "$%1$").arg(missing[i].tex);
        else {
            QFile saved(cacheFolder() + "/" + keys[i] + ".svg");
            if (saved.open(QIODevice::WriteOnly))
                saved.write(made[i].svg);
        }
        // Failures stay in memory only, so a fixed TeX installation is tried again next time.
        cache.insert(keys[i], made[i]);
    }
}
Formula typesetFormula(const MathSpan &math, bool serif) {
    prepareFormulas({math}, serif);
    QMutexLocker lock(&mutex);
    return cache.value(cacheKey(math, serif));
}
void insertMath(QTextDocument &doc, const QVector<MathSpan> &math, bool serif) {
    if (math.isEmpty())
        return;
    prepareFormulas(math, serif);
    doc.documentLayout()->registerHandler(MathObject, handler());
    for (int i = 0; i < math.size(); ++i) {
        const QString placeholder(QChar(ushort(FirstPlaceholder + i)));
        QTextCursor cursor = doc.find(placeholder);
        if (cursor.isNull())
            continue;
        const bool alone = cursor.block().text() == placeholder;
        QTextCharFormat format = cursor.charFormat();
        format.setObjectType(MathObject);
        format.setProperty(TexProperty, math[i].tex);
        format.setProperty(DisplayProperty, math[i].display);
        format.setProperty(SerifProperty, serif);
        format.setProperty(MathBlockProperty, alone);
        cursor.insertText(QString(QChar::ObjectReplacementCharacter), format);
        if (alone) {
            QTextBlockFormat block = cursor.blockFormat();
            block.setProperty(MathBlockProperty, true);
            cursor.setBlockFormat(block);
        }
    }
}
QStringList mathProblems(const QString &text) {
    QVector<MathSpan> math;
    withoutMath(text, &math);
    if (math.isEmpty())
        return {};
    prepareFormulas(math, false);
    QStringList problems;
    for (const auto &formula : math)
        if (const QString error = typesetFormula(formula, false).error; !error.isEmpty())
            problems << (error.startsWith("Math needs") ? error.section(" in ", 0, 0) : error);
    problems.removeDuplicates();
    return problems;
}

#include "math.moc"
