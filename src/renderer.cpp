#include "renderer.h"
#include "budget.h"
#include "images.h"
#include "math.h"
#include "mermaid.h"
#include "syntax.h"
#include <QAbstractTextDocumentLayout>
#include <QCache>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QMutex>
#include <QPaintEngine>
#include <QPainter>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QSvgRenderer>
#include <QTemporaryFile>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextTable>
#include <QTimer>
#include <QWaitCondition>
#include <QXmlStreamReader>

static QRegularExpression mediaRe(R"(!\[([^\]]*)\]\((?:<([^>]+)>|([^\s)]+))\))");
namespace {
class ImageCache {
    QMutex mutex;
    QCache<QString, QImage> images;
  public:
    explicit ImageCache(int kilobytes) : images(kilobytes) {}
    QImage get(const QString &key) {
        QMutexLocker lock(&mutex);
        auto image = images.object(key);
        return image ? *image : QImage();
    }
    void put(const QString &key, const QImage &image, int minimumEntries = 0) {
        QMutexLocker lock(&mutex);
        const int cost = qMax(1, int(image.sizeInBytes() / 1024));
        // High-DPI frames are several times larger; keep room for a useful number of them.
        if (minimumEntries && images.maxCost() < cost * minimumEntries)
            images.setMaxCost(cost * minimumEntries);
        images.insert(key, new QImage(image), cost);
    }
};
}
QString outsideCode(QString source, bool maskInline) {
    int position = 0, fenceLength = 0;
    QChar fence;
    static const QRegularExpression marker("^ {0,3}(`{3,}|~{3,})(.*)$");
    while (position < source.size()) {
        int end = source.indexOf('\n', position);
        if (end < 0)
            end = source.size();
        QString line = source.mid(position, end - position);
        auto match = marker.match(line);
        bool protectedLine = fenceLength > 0;
        if (match.hasMatch()) {
            QString run = match.captured(1);
            protectedLine = true;
            if (!fenceLength) {
                fence = run[0];
                fenceLength = run.size();
            } else if (run[0] == fence && run.size() >= fenceLength &&
                       match.captured(2).trimmed().isEmpty())
                fenceLength = 0;
        }
        if (protectedLine || line.startsWith("    "))
            source.replace(position, end - position, QString(end - position, ' '));
        position = end + 1;
    }
    if (!maskInline)
        return source;
    static const QRegularExpression inlineCode("(`+)([^`]|`(?!`))*?\\1");
    auto matches = inlineCode.globalMatch(source);
    QVector<QPair<int, int>> ranges;
    while (matches.hasNext()) {
        auto m = matches.next();
        ranges.append({m.capturedStart(), m.capturedLength()});
    }
    for (auto range : ranges)
        source.replace(range.first, range.second, QString(range.second, ' '));
    return source;
}
static QString withoutComments(QString source) {
    auto matches = QRegularExpression("<!--[\\s\\S]*?-->").globalMatch(outsideCode(source));
    QVector<QPair<int, int>> ranges;
    while (matches.hasNext()) {
        auto m = matches.next();
        ranges.append({m.capturedStart(), m.capturedLength()});
    }
    for (auto it = ranges.crbegin(); it != ranges.crend(); ++it)
        source.remove(it->first, it->second);
    return source;
}
QString speakerNotes(const QString &source) {
    const QString visible = outsideCode(source);
    auto matches = QRegularExpression("<!--[\\s\\S]*?-->").globalMatch(visible);
    QStringList notes;
    static const QRegularExpression directive("^hype:", QRegularExpression::CaseInsensitiveOption);
    while (matches.hasNext()) {
        const auto match = matches.next();
        QString note = source.mid(match.capturedStart() + 4, match.capturedLength() - 7).trimmed();
        if (!note.isEmpty() && !directive.match(note).hasMatch())
            notes.append(note);
    }
    return notes.join("\n\n");
}
static bool isVideoFile(const QString &file) {
    return QStringList{"mp4", "m4v", "mov", "webm", "mkv"}.contains(
        QFileInfo(file).suffix().toLower());
}
// The one list of placed-image positions and where each pins its image.
static const QList<std::pair<QString, Qt::Alignment>> &placements() {
    static const QList<std::pair<QString, Qt::Alignment>> list{
        {"top-left", Qt::AlignLeft | Qt::AlignTop},
        {"top", Qt::AlignHCenter | Qt::AlignTop},
        {"top-right", Qt::AlignRight | Qt::AlignTop},
        {"left", Qt::AlignLeft | Qt::AlignVCenter},
        {"center", Qt::AlignCenter},
        {"right", Qt::AlignRight | Qt::AlignVCenter},
        {"bottom-left", Qt::AlignLeft | Qt::AlignBottom},
        {"bottom", Qt::AlignHCenter | Qt::AlignBottom},
        {"bottom-right", Qt::AlignRight | Qt::AlignBottom}};
    return list;
}
const QStringList &mediaPositions() {
    static const QStringList names = [] {
        QStringList result;
        for (const auto &placement : placements())
            result << placement.first;
        return result;
    }();
    return names;
}
static QString assetPath(const QString &base, QString file, bool video) {
    if (QFileInfo(file).isAbsolute())
        return file;
    return QDir(base).filePath(file.startsWith("images/") || file.startsWith("videos/")
                                   ? file
                                   : (video ? "videos/" : "images/") + file);
}
QString withMedia(const QString &source, const QString &reference) {
    QString visible = outsideCode(source);
    auto comments = QRegularExpression("<!--[\\s\\S]*?-->").globalMatch(visible);
    while (comments.hasNext()) {
        const auto comment = comments.next();
        visible.replace(comment.capturedStart(), comment.capturedLength(),
                        QString(comment.capturedLength(), ' '));
    }
    auto match = mediaRe.match(visible);
    QString updated = source;
    if (match.hasMatch())
        updated.replace(match.capturedStart(), match.capturedLength(), reference);
    else
        updated += "\n" + reference + "\n";
    return updated;
}
QString withReplacedMedia(const QString &source, const QString &file) {
    // A new logo keeps the old one's corner and size, a split image its side; other layouts
    // start fresh.
    const auto current = parseMedia(source, {});
    const QString flags =
        isVideoFile(file) ? QString()
        : current.placed()
            ? QString("position=%1 size=%2%").arg(current.position, QString::number(current.size))
        : !current.split.isEmpty()
            ? QString("split=%1 size=%2%").arg(current.split, QString::number(current.share)) +
                  (current.background.isEmpty() ? QString() : " background=" + current.background)
            : QString();
    return withMedia(source, "![" + flags + "](<" + file + ">)");
}
QString withMediaDirectives(const QString &source, const QStringList &remove,
                            const QStringList &add) {
    const QString marker = "\x01HYPE_MEDIA\x01";
    const QString marked = withMedia(source, marker);
    const int start = marked.indexOf(marker);
    const int length = source.size() - marked.size() + marker.size();
    if (start < 0 || length <= 0)
        return source;
    QString reference = source.mid(start, length);
    const int end = reference.indexOf("](");
    if (end < 2)
        return source;
    QString flags = reference.mid(2, end - 2).trimmed();
    static const QRegularExpression tokens(R"re(([a-z]+)(?:=("(?:[^"\\]|\\.)*"|[^\s]+))?)re");
    const auto first = tokens.match(flags);
    const bool directives =
        first.hasMatch() && first.capturedStart() == 0 &&
        (QStringList{"fit", "span", "loop", "muted"}.contains(first.captured(1)) ||
         !first.captured(2).isEmpty());
    QStringList kept = add;
    if (directives) {
        auto matches = tokens.globalMatch(flags);
        while (matches.hasNext()) {
            const auto token = matches.next();
            if (!remove.contains(token.captured(1)))
                kept << token.captured();
        }
    } else if (!flags.isEmpty()) {
        kept << "alt=\"" + flags.replace("\\", "\\\\").replace("\"", "\\\"") + "\"";
    }
    reference.replace(2, end - 2, kept.join(' '));
    return withMedia(source, reference);
}
const QStringList &mediaSplits() {
    static const QStringList names{"left", "right", "bottom"};
    return names;
}
static Media readMedia(const QString &source, const QString &base) {
    Media result;
    result.text = withoutComments(source);
    auto m = mediaRe.match(outsideCode(result.text));
    if (!m.hasMatch())
        return result;
    result.file = m.captured(2).isEmpty() ? m.captured(3) : m.captured(2);
    result.video = isVideoFile(result.file);
    result.path = assetPath(base, result.file, result.video);
    result.text.remove(m.capturedStart(), m.capturedLength());
    result.span = !result.video &&
                  outsideCode(result.text)
                      .contains(QRegularExpression("^# ", QRegularExpression::MultilineOption));
    QString flags = m.captured(1).trimmed();
    static const QRegularExpression tokens(R"re(([a-z]+)(?:=("(?:[^"\\]|\\.)*"|[^\s]+))?)re");
    auto first = tokens.match(flags);
    bool directives =
        first.hasMatch() && first.capturedStart() == 0 &&
        (QStringList{"fit", "span", "loop", "muted"}.contains(first.captured(1)) ||
         !first.captured(2).isEmpty());
    QString explicitOverlay;
    bool fit = false, span = false, explicitPosition = false;
    if (directives) {
        auto it = tokens.globalMatch(flags);
        int consumed = 0;
        while (it.hasNext()) {
            auto token = it.next();
            if (!flags.mid(consumed, token.capturedStart() - consumed).trimmed().isEmpty())
                result.error = "Invalid media directive";
            consumed = token.capturedEnd();
            QString key = token.captured(1), value = token.captured(2);
            if (value.startsWith('"'))
                value = value.mid(1, value.size() - 2).replace("\\\"", "\"").replace("\\\\", "\\");
            if (key == "span") {
                result.span = true;
                span = true;
            } else if (key == "fit") {
                result.span = false;
                fit = true;
            } else if (key == "loop")
                result.loop = value != "false";
            else if (key == "muted")
                result.muted = value != "false";
            else if (key == "autoplay")
                result.autoplay = value != "false";
            else if (key == "overlay")
                explicitOverlay = value;
            else if (key == "position") {
                // Errors name the directive, never the author's value, which may hold markup.
                result.position = value;
                explicitPosition = true;
                if (!mediaPositions().contains(value))
                    result.error = "Unknown position";
            } else if (key == "size") {
                // Plain decimals only: toDouble() would also accept "nan" and "1e1".
                static const QRegularExpression percentage(R"(^(\d{1,3}(?:\.\d+)?)%$)");
                const auto number = percentage.match(value);
                result.size = number.hasMatch() ? number.captured(1).toDouble() : 0;
                if (result.size < 1 || result.size > 100) {
                    result.error = "Size must be a percentage from 1% to 100%";
                    result.size = 0;
                }
                if (result.position.isEmpty())
                    result.position = "center";
            } else if (key == "split") {
                result.split = value;
                if (!mediaSplits().contains(value))
                    result.error = "Unknown split";
            } else if (key == "background") {
                result.background = value;
                if (value != "auto" && value != "theme" && value != "blur" && !QColor(value).isValid())
                    result.error = "Invalid background color";
            } else if (key == "poster")
                result.poster = assetPath(base, value, false);
            else if (key != "alt")
                result.error = "Unknown media directive: " + key;
        }
        if (!flags.mid(consumed).trimmed().isEmpty())
            result.error = "Invalid media directive";
    }
    if (fit && span)
        result.error = "Choose either span or fit";
    if (!result.split.isEmpty()) {
        if (result.video)
            result.error = "Only images can be split";
        else if (explicitPosition)
            result.error = "Choose either split or position";
        else if (fit || span || !explicitOverlay.isEmpty())
            result.error = "A split image cannot use fit, span, or overlay";
        // Its background is a plain color for the image's side of the slide.
        else if (!result.background.isEmpty() && !QColor(result.background).isValid())
            result.error = "A split image's background must be a color";
        // size= is the image side's share of the slide; it is not a placement.
        result.share = result.size > 0 ? result.size : result.split == "bottom" ? 60 : 50;
        result.position.clear();
        result.size = 0;
        result.span = false;
        result.overlay = 0;
        return result;
    }
    if (result.placed()) {
        if (result.video)
            result.error = "Only images can be placed";
        else if (fit || span || !result.background.isEmpty() || !explicitOverlay.isEmpty())
            result.error = "A placed image cannot use fit, span, background, or overlay";
        if (result.size == 0)
            result.size = defaultPlacedSize;
        for (const auto &[position, alignment] : placements())
            if (position == result.position)
                result.alignment = alignment;
        // A placed image leaves the slide's own layout and colors alone.
        result.span = false;
        result.overlay = 0;
        return result;
    }
    // A background choice implies fitting unless span was explicitly requested.
    if (!span && (result.background == "blur" || result.background == "auto"))
        result.span = false;
    result.overlay = (!result.video || result.span) && !result.text.trimmed().isEmpty() ? 0.25 : 0;
    if (!explicitOverlay.isEmpty()) {
        bool ok;
        double opacity = explicitOverlay.toDouble(&ok);
        if (!ok || opacity < 0 || opacity > 1)
            result.error = "Overlay must be between 0 and 1";
        else
            result.overlay = opacity;
    }
    return result;
}
// The first fence in this language: taken out of the text, its lines returned in body.
static bool takeFence(QString &text, const QString &language, QString *body) {
    static const QRegularExpression marker("^ {0,3}(`{3,}|~{3,})(.*)$");
    int position = 0, fenceLength = 0, start = -1, content = 0;
    QChar fence;
    while (position < text.size()) {
        int end = text.indexOf('\n', position);
        if (end < 0)
            end = text.size();
        const auto match = marker.match(text.mid(position, end - position));
        if (match.hasMatch()) {
            const QString run = match.captured(1);
            if (!fenceLength) {
                fence = run[0];
                fenceLength = run.size();
                if (match.captured(2).trimmed().section(' ', 0, 0).compare(
                        language, Qt::CaseInsensitive) == 0) {
                    start = position;
                    content = qMin(int(text.size()), end + 1);
                }
            } else if (run[0] == fence && run.size() >= fenceLength &&
                       match.captured(2).trimmed().isEmpty()) {
                fenceLength = 0;
                if (start >= 0) {
                    *body = text.mid(content, position - content);
                    text.remove(start, qMin(int(text.size()), end + 1) - start);
                    return true;
                }
            }
        }
        position = end + 1;
    }
    return false;
}
// The deck's colors by name, as var(--accent), and its colors and font as the SVG's defaults,
// so a drawing follows the theme unless it says otherwise.
static QByteArray themedSvg(const QString &svg, const QVariantMap &palette) {
    static const QRegularExpression variable(R"(var\(\s*--([a-z_-]+)\s*\))"),
        root("<svg\\b([^>]*)>");
    QString themed;
    int last = 0;
    for (auto it = variable.globalMatch(svg); it.hasNext();) {
        const auto m = it.next();
        const QString value = palette.value(m.captured(1).replace('-', '_')).toString();
        themed +=
            svg.mid(last, m.capturedStart() - last) + (value.isEmpty() ? m.captured(0) : value);
        last = m.capturedEnd();
    }
    themed += svg.mid(last);
    if (const auto m = root.match(themed); m.hasMatch()) {
        const QString attributes = m.captured(1), ink = palette.value("foreground").toString();
        QString defaults;
        if (!attributes.contains(QRegularExpression("(^|\\s)xmlns\\s*=")))
            defaults += " xmlns=\"http://www.w3.org/2000/svg\"";
        if (!attributes.contains(QRegularExpression("(^|\\s)color\\s*=")))
            defaults += " color=\"" + ink + "\"";
        if (!attributes.contains(QRegularExpression("(^|\\s)fill\\s*=")))
            defaults += " fill=\"" + ink + "\"";
        if (!attributes.contains(QRegularExpression("(^|\\s)font-family\\s*=")))
            defaults += " font-family=\"" +
                        palette.value("font", "JetBrains Mono").toString().toHtmlEscaped() + "\"";
        themed.insert(m.capturedStart(1), defaults);
    }
    return themed.toUtf8();
}
// Why an ```svg fence can't be drawn, or empty when it can. Lines count within the fence.
static QString svgProblem(const QString &svg) {
    QXmlStreamReader xml(svg);
    bool first = true;
    while (!xml.atEnd()) {
        if (xml.readNext() == QXmlStreamReader::StartElement && first) {
            first = false;
            if (xml.name() != QLatin1String("svg"))
                return "An svg block holds one <svg> element";
        }
    }
    if (xml.hasError())
        return QString("SVG line %1: %2").arg(xml.lineNumber()).arg(xml.errorString());
    static const QRegularExpression variable(R"(var\(\s*--([a-z_-]+)\s*\))");
    static const QStringList colors{
        "background", "foreground", "accent", "green",           "red",
        "yellow",     "magenta",    "cyan",   "dark_foreground", "font"};
    for (auto it = variable.globalMatch(svg); it.hasNext();) {
        const QString name = it.next().captured(1);
        if (!colors.contains(QString(name).replace('-', '_')))
            return "Unknown theme color --" + name + "; use --" +
                   colors.mid(0, 9).join(", --").replace('_', '-');
    }
    QSvgRenderer renderer(themedSvg(svg, {{"foreground", "#000000"}}));
    if (!renderer.isValid())
        return "Qt can't draw this SVG";
    if (renderer.viewBoxF().isEmpty())
        return "Give the SVG a viewBox, or a width and height";
    return {};
}
Media parseMedia(const QString &source, const QString &base) {
    // Parsing is pure: file existence and modification checks remain at call sites.
    static thread_local QCache<QString, Media> cache(8 * 1024 * 1024);
    const QString key = base + QChar(0) + source;
    if (const auto result = cache.object(key))
        return *result;
    auto result = readMedia(source, base);
    result.svgBlocks = takeFence(result.text, "svg", &result.svg);
    if (result.svgBlocks) {
        QString remaining = result.text, extra;
        result.svgBlocks += takeFence(remaining, "svg", &extra);
    } else if (takeFence(result.text, "mermaid", &result.mermaid)) {
        QString remaining = result.text, extra;
        result.svgBlocks = 1 + takeFence(remaining, "mermaid", &extra);
    }
    cache.insert(key, new Media(result), qMax(1, int((key.size() + result.text.size()) * 2)));
    return result;
}
// Takes out the step markers and notes where each step starts. Every pause and every
// incremental item is a cut, even where two fall together, so two scans of the same slide
// number their steps alike.
static SlideSteps splitSteps(const QString &text, bool incremental) {
    SlideSteps steps;
    if (!incremental && !text.contains(". . .") && !text.contains(":::")) {
        steps.text = text;
        return steps;
    }
    static const QRegularExpression fenceRe("^ {0,3}(`{3,}|~{3,})(.*)$");
    static const QRegularExpression pauseRe("^ {0,3}\\. \\. \\.\\s*$");
    static const QRegularExpression divRe(
        "^ {0,3}:{3,}\\s*(?:\\{\\s*\\.?([\\w-]+)\\s*\\}|([\\w-]+))?\\s*$");
    static const QRegularExpression itemRe("^\\s*(?:>\\s*)*(?:[-*+]|\\d+[.)])\\s");
    QVector<bool> modes{incremental};
    QVector<bool> ours; // For each open div, whether it is an (non)incremental one.
    QChar fence;
    int fenceLength = 0;
    // As in beamer, the first item after a pause, or at the top, shows with what precedes it.
    bool fresh = true;
    const QStringList lines = text.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        const QString &line = lines[i];
        const auto keep = [&] {
            steps.text += line;
            if (i + 1 < lines.size())
                steps.text += '\n';
        };
        if (const auto match = fenceRe.match(line); match.hasMatch() || fenceLength) {
            const QString run = match.captured(1);
            if (!fenceLength && match.hasMatch()) {
                fence = run[0];
                fenceLength = run.size();
            } else if (match.hasMatch() && run[0] == fence && run.size() >= fenceLength &&
                       match.captured(2).trimmed().isEmpty())
                fenceLength = 0;
            keep();
            continue;
        }
        if (pauseRe.match(line).hasMatch()) {
            steps.cuts << int(steps.text.size());
            fresh = true;
            continue;
        }
        if (const auto div = divRe.match(line); div.hasMatch()) {
            const QString name = div.captured(1) + div.captured(2);
            if (name == "incremental" || name == "nonincremental") {
                ours << true;
                modes << (name == "incremental");
                continue;
            }
            if (name.isEmpty() && !ours.isEmpty()) {
                if (ours.takeLast()) {
                    modes.removeLast();
                    continue;
                }
            } else if (!name.isEmpty())
                ours << false;
        }
        if (modes.last() && itemRe.match(line).hasMatch()) {
            if (!fresh)
                steps.cuts << int(steps.text.size());
            fresh = false;
        }
        keep();
    }
    return steps;
}
SlideSteps slideSteps(const QString &source, const Media &media, bool incremental) {
    SlideSteps steps = splitSteps(media.text, incremental);
    // The picture is drawn apart from the text, on the step where its reference sits.
    if (!media.file.isEmpty() || media.svgBlocks) {
        const SlideSteps whole = splitSteps(withoutComments(source), incremental);
        static const QRegularExpression svgFence("^ {0,3}(?:`{3,}|~{3,})\\s*(?:svg|mermaid)\\b",
                                                 QRegularExpression::MultilineOption);
        const int at = !media.file.isEmpty() ? int(mediaRe.match(outsideCode(whole.text)).capturedStart())
                                             : int(svgFence.match(whole.text).capturedStart());
        for (int cut : whole.cuts)
            steps.media += at >= 0 && cut <= at;
    }
    // Trim as the slide is drawn. A last step that shows nothing new is no step.
    int lead = 0;
    while (lead < steps.text.size() && steps.text[lead].isSpace())
        ++lead;
    steps.text = steps.text.trimmed();
    for (int &cut : steps.cuts)
        cut = qBound(0, cut - lead, int(steps.text.size()));
    while (!steps.cuts.isEmpty() && steps.cuts.last() >= steps.text.size() &&
           steps.media < steps.cuts.size())
        steps.cuts.removeLast();
    return steps;
}
QString SlideSteps::shown(int step) const {
    return step < 0 || step >= cuts.size() ? text : text.left(cuts[step]);
}
static QString createPoster(const QString &video, const QString &base) {
    QFileInfo info(video);
    if (!info.exists())
        return {};
    QFile file(video);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return {};
    const QString digest = QString::fromLatin1(hash.result().toHex().left(16));
    QString path = base + "/images/.hype-poster-" + digest + ".jpg";
    if (QFile::exists(path))
        return path;
    QDir().mkpath(base + "/images");
    QTemporaryFile poster(base + "/images/.hype-poster-XXXXXX.jpg");
    if (!poster.open()) return {};
    poster.close();
    QProcess ffmpeg;
    ffmpeg.start("ffmpeg", {"-v", "error", "-y", "-i", video, "-frames:v", "1", "-vf",
                            "scale=1280:-2", poster.fileName()});
    if (!ffmpeg.waitForFinished(30000) || ffmpeg.exitCode() != 0) {
        ffmpeg.kill();
        ffmpeg.waitForFinished();
        return {};
    }
    // Parallel thumbnail/preview requests must never read a half-written poster.
    if (!QFile::exists(path) && !QFile::rename(poster.fileName(), path) && !QFile::exists(path))
        return {};
    return path;
}
QString ensurePoster(const QString &video, const QString &base) {
    static QMutex mutex;
    static QWaitCondition ready;
    static QSet<QString> pending;
    static QCache<QString, QString> cache(1024);
    const QFileInfo file(video);
    const QString key = base + QChar(0) + video + ':' + QString::number(file.size()) + ':' +
                        QString::number(file.lastModified().toMSecsSinceEpoch());
    {
        QMutexLocker lock(&mutex);
        while (pending.contains(key))
            ready.wait(&mutex);
        if (auto path = cache.object(key); path && QFileInfo::exists(*path))
            return *path;
        pending.insert(key);
    }
    const QString result = createPoster(video, base);
    {
        QMutexLocker lock(&mutex);
        if (!result.isEmpty())
            cache.insert(key, new QString(result));
        pending.remove(key);
        ready.wakeAll();
    }
    return result;
}
static QString slideProperty(const QString &source, const QString &key);
QStringList slideProblems(const QString &source, const QString &base, const QVariantMap &palette) {
    QStringList errors;
    if (const QString vertical = slideProperty(source, "vertical");
        !vertical.isEmpty() && !QStringList{"top", "center", "bottom"}.contains(vertical))
        errors << "Vertical alignment must be top, center, or bottom";
    auto media = parseMedia(source, base);
    if (!media.error.isEmpty())
        errors << media.error;
    if (media.svgBlocks && media.mermaid.isEmpty()) {
        if (const QString problem = svgProblem(media.svg); !problem.isEmpty())
            errors << problem;
    }
    if (!media.mermaid.isEmpty())
        if (const QString problem = mermaidDiagram(media.mermaid, palette).error; !problem.isEmpty())
            errors << problem;
    if (!media.file.isEmpty() && !QFileInfo::exists(media.path))
        errors << "Missing media: " + media.file;
    if (!media.file.isEmpty() && !media.video && QFileInfo::exists(media.path) &&
        !QImageReader(media.path).canRead())
        errors << "Cannot decode image: " + media.file;
    if (!media.poster.isEmpty() && !QFileInfo::exists(media.poster))
        errors << "Missing poster";
    errors << mathProblems(media.text);
    auto matches = mediaRe.globalMatch(outsideCode(withoutComments(source)));
    int count = 0;
    while (matches.hasNext()) {
        matches.next();
        ++count;
    }
    if (count + media.svgBlocks > 1)
        errors << "Use one media item per slide (combine artwork before importing)";
    return errors;
}
static QImage loadedImage(const QString &path, QSize canvas, bool span) {
    static ImageCache cache(256 * 1024);
    const QFileInfo info(path);
    QString key = path + QString::number(info.lastModified().toMSecsSinceEpoch()) + ":" +
                  QString::number(info.size()) + ":" + QString::number(canvas.width()) + "x" +
                  QString::number(canvas.height()) + (span ? ":span" : ":fit");
    if (const auto image = cache.get(key); !image.isNull())
        return image;
    QImage image = readSizedImage(path, canvas, span);
    if (span && !image.isNull()) {
        // Embed only the visible center crop, especially in PDF. Keep originals
        // intact so switching between Fit and Span remains reversible.
        QSize crop = image.size().scaled(canvas, Qt::KeepAspectRatioByExpanding);
        const double scale = double(image.width()) / crop.width();
        const QSize visible(qRound(canvas.width() * scale), qRound(canvas.height() * scale));
        if (visible != image.size())
            image = image.copy((image.width() - visible.width()) / 2,
                               (image.height() - visible.height()) / 2,
                               visible.width(), visible.height());
    }
    if (!image.isNull())
        cache.put(key, image);
    return image;
}
static QImage boxBlur(QImage image, int radius) {
    image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int width = image.width(), height = image.height(), diameter = radius * 2 + 1;
    // Three separable box passes approximate a Gaussian. Clamp the edges and
    // average premultiplied channels so transparent pictures keep clean edges.
    // Sums stay exact integers; a table replaces four divisions per pixel.
    QVector<uchar> average(255 * diameter + 1);
    for (int sum = 0; sum < average.size(); ++sum)
        average[sum] = uchar(sum / diameter);
    auto pixel = [&](int sum[4]) {
        return qRgba(average[sum[0]], average[sum[1]], average[sum[2]], average[sum[3]]);
    };
    auto add = [](int sum[4], QRgb color, int sign) {
        sum[0] += sign * qRed(color); sum[1] += sign * qGreen(color);
        sum[2] += sign * qBlue(color); sum[3] += sign * qAlpha(color);
    };
    QImage output(image.size(), image.format());
    std::vector<int> columns(size_t(width) * 4);
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < height; ++y) {
            const auto *in = reinterpret_cast<const QRgb *>(image.constScanLine(y));
            auto *out = reinterpret_cast<QRgb *>(output.scanLine(y));
            int sum[4] = {};
            for (int i = -radius; i <= radius; ++i) add(sum, in[qBound(0, i, width - 1)], 1);
            for (int x = 0; x < width; ++x) {
                out[x] = pixel(sum);
                add(sum, in[qBound(0, x - radius, width - 1)], -1);
                add(sum, in[qBound(0, x + radius + 1, width - 1)], 1);
            }
        }
        // Vertical: keep a running sum per column and walk whole rows, so the pass
        // reads memory in order instead of striding down each column.
        std::fill(columns.begin(), columns.end(), 0);
        auto row = [&](int y) {
            return reinterpret_cast<const QRgb *>(output.constScanLine(qBound(0, y, height - 1)));
        };
        for (int i = -radius; i <= radius; ++i)
            for (int x = 0, *sum = columns.data(); x < width; ++x, sum += 4) add(sum, row(i)[x], 1);
        for (int y = 0; y < height; ++y) {
            auto *out = reinterpret_cast<QRgb *>(image.scanLine(y));
            const QRgb *leaving = row(y - radius), *entering = row(y + radius + 1);
            for (int x = 0, *sum = columns.data(); x < width; ++x, sum += 4) {
                out[x] = pixel(sum);
                add(sum, leaving[x], -1);
                add(sum, entering[x], 1);
            }
        }
    }
    return image;
}
static QImage blurredBackground(const QImage &image) {
    static ImageCache cache(16 * 1024);
    const QString key = QString::number(image.cacheKey());
    QImage blurred = cache.get(key);
    if (blurred.isNull()) {
        blurred = boxBlur(image.scaled(320, 180, Qt::IgnoreAspectRatio, Qt::SmoothTransformation), 8);
        cache.put(key, blurred);
    }
    return blurred;
}
QImage softenedImage(const QImage &image, const QSizeF &slideSize) {
    // A roughly two-pixel softness at 1080p, scaled with the picture at 4K.
    // Text is painted afterwards and stays sharp. Share the cache across preview
    // workers so changing a headline doesn't blur the same picture again.
    if (image.isNull() || slideSize.isEmpty()) return image;
    const int radius = qRound(2.0 * image.width() / slideSize.width());
    if (radius == 0) return image;
    static ImageCache cache(64 * 1024);
    const QString key = QString::number(image.cacheKey()) + '/' + QString::number(radius);
    QImage softened = cache.get(key);
    if (softened.isNull()) {
        softened = boxBlur(image, radius);
        cache.put(key, softened);
    }
    return softened;
}
static QString slideProperty(const QString &source, const QString &key) {
    QRegularExpression re("<!--\\s*hype:[\\s\\S]*?\\b" + key + "=\"([^\"]*)\"[\\s\\S]*?-->");
    return re.match(source).captured(1);
}
bool slideHidden(const QString &source) {
    // Most slides never mention it, so skip the pattern for them. A code sample showing the
    // comment does not hide its slide.
    return source.contains("hidden=") && slideProperty(outsideCode(source), "hidden") == "true";
}
QString withSlideProperty(QString source, const QString &key, const QString &value) {
    static const QRegularExpression comment("<!--\\s*hype:([\\s\\S]*?)-->");
    const QRegularExpression attribute("\\s*\\b" + key + "=\"[^\"]*\"");
    QVector<QRegularExpressionMatch> found;
    auto matches = comment.globalMatch(outsideCode(source));
    while (matches.hasNext())
        found << matches.next();
    // Work from the last comment back, so earlier offsets stay valid. The first one takes the value.
    for (qsizetype n = found.size() - 1; n >= 0; --n) {
        const auto &m = found[n];
        QString body = source.mid(m.capturedStart(1), m.capturedLength(1));
        const bool had = body.contains(attribute);
        body.remove(attribute);
        body = body.trimmed();
        if (n == 0 && !value.isEmpty())
            body += (body.isEmpty() ? "" : " ") + key + "=\"" + value + "\"";
        else if (!had)
            continue;
        int start = m.capturedStart(), end = m.capturedEnd();
        if (body.isEmpty()) {
            // A comment alone on its line leaves no blank line behind.
            // A negative lastIndexOf start counts from the end, so the first line needs no search.
            const int lineStart = start > 0 ? source.lastIndexOf('\n', start - 1) + 1 : 0;
            int lineEnd = source.indexOf('\n', end);
            if (lineEnd < 0)
                lineEnd = source.size();
            if (source.mid(lineStart, start - lineStart).trimmed().isEmpty() &&
                source.mid(end, lineEnd - end).trimmed().isEmpty()) {
                start = lineStart;
                end = qMin(lineEnd + 1, int(source.size()));
            }
            source.remove(start, end - start);
        } else {
            source.replace(start, end - start, "<!-- hype: " + body + " -->");
        }
    }
    if (found.isEmpty() && !value.isEmpty()) {
        int first = 0;
        while (first < source.size() && source[first].isSpace())
            ++first;
        source.insert(first, "<!-- hype: " + key + "=\"" + value + "\" -->\n");
    }
    return source;
}
static QString preserveLineBreaks(QString markdown) {
    markdown.replace("\r\n", "\n").replace('\r', '\n');
    const QStringList visible = outsideCode(markdown, false).split('\n');
    QStringList lines = markdown.split('\n');
    for (int i = 0; i + 1 < lines.size(); ++i) {
        if (!visible[i].trimmed().isEmpty() && !lines[i].endsWith("  ") && !lines[i].endsWith('\\'))
            lines[i] += "  ";
    }
    return lines.join('\n');
}
static const QRegularExpression &tableDelimiter() {
    // A delimiter row, with or without outer pipes: | --- | :-: | or --- | ---:
    static const QRegularExpression re("^ {0,3}(?=[^\\n]*\\|)\\|? *:?-+:? *(?:\\| *:?-+:? *)*\\|? *$",
                                       QRegularExpression::MultilineOption);
    return re;
}
// Markdown trims table cells. Keep the one space each side that reads as
// separation, and turn any more into non-breaking spaces that pad the cell.
static QString padTableCells(const QString &markdown) {
    QStringList lines = markdown.split('\n');
    const QStringList visible = outsideCode(markdown).split('\n');
    auto pad = [&](int index) {
        QString &line = lines[index];
        const QString &mask = visible[index];
        QList<int> pipes;
        for (int i = 0; i < mask.size(); ++i)
            if (mask[i] == '|' && (i == 0 || mask[i - 1] != '\\'))
                pipes.append(i);
        if (pipes.isEmpty())
            return;
        if (pipes.first() > 0 && !mask.left(pipes.first()).trimmed().isEmpty())
            pipes.prepend(-1);
        if (pipes.last() < mask.size() - 1 && !mask.mid(pipes.last() + 1).trimmed().isEmpty())
            pipes.append(mask.size());
        // Work from the end so earlier positions stay valid.
        for (int c = pipes.size() - 1; c > 0; --c) {
            int start = pipes[c - 1] + 1, end = pipes[c];
            if (line.mid(start, end - start).trimmed().isEmpty())
                continue;
            int trailing = 0, leading = 0;
            while (line[end - 1 - trailing] == ' ')
                ++trailing;
            while (line[start + leading] == ' ')
                ++leading;
            if (trailing > 1)
                line.replace(end - trailing, trailing - 1, QString(trailing - 1, QChar::Nbsp));
            if (leading > 1)
                line.replace(start + 1, leading - 1, QString(leading - 1, QChar::Nbsp));
        }
    };
    for (int i = 1; i < lines.size(); ++i) {
        if (!tableDelimiter().match(visible[i]).hasMatch() || !visible[i - 1].contains('|'))
            continue;
        pad(i - 1);
        for (int row = i + 1; row < lines.size() && visible[row].contains('|') &&
                              !visible[row].trimmed().isEmpty(); ++row)
            pad(row);
    }
    return lines.join('\n');
}
// Words stay whole while the fit search shrinks the text; only text that cannot fit even at
// the smallest size, such as a long URL, breaks mid-word.
static void sizeSlideText(QTextDocument &doc, const QVariantMap &palette, qreal fontSize,
                          qreal width, bool centered, bool code, bool breakWords = false) {
    // A null page size suspends layout while every format below changes; the
    // final setTextWidth lays the document out once instead of once per run.
    doc.setPageSize(QSizeF(0, 0));
    QFont font(code ? QString("JetBrains Mono")
                    : palette.value("font", "JetBrains Mono").toString());
    font.setPixelSize(qRound(fontSize));
    font.setHintingPreference(QFont::PreferNoHinting);
    doc.setDefaultFont(font);
    // A deck with a fixed text size sets its headings by ratio and gives body text more air.
    const qreal fixed = palette.value("text_size").toDouble();
    const qreal headingScale = fixed > 0 ? palette.value("heading_size").toDouble() / fixed : 1.8;
    const QString headingFamily =
        code ? font.family() : palette.value("heading_font", font.family()).toString();
    const int headingWeight = palette.value("heading_weight", 700).toInt();
    doc.setDocumentMargin(0);
    QTextOption option;
    option.setUseDesignMetrics(true);
    option.setWrapMode(code         ? QTextOption::NoWrap
                       : breakWords ? QTextOption::WrapAtWordBoundaryOrAnywhere
                                    : QTextOption::WordWrap);
    doc.setDefaultTextOption(option);
    doc.setDefaultStyleSheet(
        QString("body { color: %1; } a { color: %2; } pre { white-space: pre; }")
            .arg(palette["foreground"].toString(), palette["accent"].toString()));
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        QTextCursor cursor(block);
        QTextBlockFormat bf = block.blockFormat();
        int level = bf.headingLevel();
        // Table cells keep the alignment their column's delimiter asked for, and displayed
        // math the middle of its line.
        if (bf.boolProperty(MathBlockProperty))
            bf.setAlignment(Qt::AlignHCenter);
        else if (!cursor.currentTable())
            bf.setAlignment(centered ? Qt::AlignHCenter : Qt::AlignLeft);
        bf.setTopMargin(level ? fontSize * 0.15 : 0);
        bf.setBottomMargin(fontSize * (fixed > 0 ? (level ? 0.7 : 0.6) : 0.22));
        bf.setLineHeight(fixed > 0 ? (level ? 110 : 140) : 115, QTextBlockFormat::ProportionalHeight);
        if (code) {
            bf.setBottomMargin(0);
            bf.setTopMargin(0);
            bf.setLineHeight(120, QTextBlockFormat::ProportionalHeight);
        }
        cursor.setBlockFormat(bf);
        // Replacing a run's format can merge it with its neighbor and invalidate fragments, so
        // take each run's extent and format first.
        struct Run { int start, end; QTextCharFormat format; };
        QList<Run> runs;
        for (auto it = block.begin(); !it.atEnd(); ++it)
            if (const auto fragment = it.fragment(); fragment.isValid())
                runs.append({fragment.position(), fragment.position() + fragment.length(),
                             fragment.charFormat()});
        for (const Run &fragment : runs) {
            QTextCursor text(&doc);
            text.setPosition(fragment.start);
            text.setPosition(fragment.end, QTextCursor::KeepAnchor);
            QTextCharFormat cf;
            QFont f = font;
            f.setPixelSize(qRound(fontSize * (level == 1   ? headingScale
                                              : !level     ? 1.0
                                              : fixed > 0 ? (headingScale + 1) / 2
                                                           : 1.25)));
            cf.setProperty(QTextFormat::FontPixelSize, f.pixelSize());
            cf.setFontFamilies({level ? headingFamily : font.family()});
            // Qt drops both heading weight and its relative size at inline
            // formatting boundaries. The relative size overrides FontPixelSize
            // during layout, so restore both to preserve the existing heading
            // appearance, while keeping underline/italic/etc. local to each run.
            if (level) {
                cf.setFontWeight(headingWeight);
                cf.setProperty(QTextFormat::FontSizeAdjustment, 4 - level);
            }
            QColor color(palette["foreground"].toString());
            if (fragment.format.fontWeight() >= QFont::Bold && !level)
                color = QColor(palette["accent"].toString());
            if (block.text().startsWith(QString::fromUtf8("—"))) {
                cf.setProperty(QTextFormat::FontPixelSize, qRound(fontSize * 0.7));
            }
            cf.setForeground(color);
            if (level && fixed > 0) {
                // A relative size, even zero, overrides the pixel size; at a fixed size the
                // heading's pixel size is exact, so the run drops it instead.
                QTextCharFormat run = fragment.format;
                run.merge(cf);
                run.clearProperty(QTextFormat::FontSizeAdjustment);
                text.setCharFormat(run);
            } else
                text.mergeCharFormat(cf);
        }
    }
    for (auto it = doc.rootFrame()->begin(); !it.atEnd(); ++it)
        if (auto table = qobject_cast<QTextTable *>(it.currentFrame())) {
            auto fmt = table->format();
            fmt.setBorder(0);
            fmt.setBorderCollapse(true);
            fmt.setCellPadding(fontSize * 0.25);
            fmt.setCellSpacing(0);
            // Size to the content rather than the slide, so columns stay close enough to read across.
            fmt.setWidth(QTextLength());
            fmt.setAlignment(centered ? Qt::AlignHCenter : Qt::AlignLeft);
            table->setFormat(fmt);
            // Qt aligns body cells by the delimiter row but leaves the header at the default.
            for (int column = 0; table->rows() > 1 && column < table->columns(); ++column) {
                QTextCursor header = table->cellAt(0, column).firstCursorPosition();
                auto bf = header.blockFormat();
                bf.setAlignment(table->cellAt(1, column).firstCursorPosition().blockFormat().alignment());
                header.setBlockFormat(bf);
            }
            // A gap between columns, a firm rule under the header and faint ones between rows.
            QColor rule(palette["foreground"].toString());
            rule.setAlphaF(0.2);
            for (int row = 0; row < table->rows(); ++row)
                for (int column = 0; column < table->columns(); ++column) {
                    auto cell = table->cellAt(row, column);
                    auto cf = cell.format().toTableCellFormat();
                    if (column)
                        cf.setLeftPadding(fontSize * 0.8);
                    if (column + 1 < table->columns())
                        cf.setRightPadding(fontSize * 0.8);
                    if (row + 1 < table->rows()) {
                        cf.setBottomBorder(row ? fontSize * 0.03 : fontSize * 0.06);
                        cf.setBottomBorderStyle(QTextFrameFormat::BorderStyle_Solid);
                        cf.setBottomBorderBrush(row ? rule : QColor(palette["accent"].toString()));
                    }
                    cell.setFormat(cf);
                }
        }
    doc.setTextWidth(width);
}
void layoutSlideText(QTextDocument &doc, const QString &markdown, const QVariantMap &palette,
                     qreal fontSize, qreal width, bool centered, bool code) {
    doc.setUndoRedoEnabled(false);
    QVector<MathSpan> math;
    const QString text = withoutMath(markdown, &math);
    doc.setMarkdown(preserveLineBreaks(padTableCells(text)), QTextDocument::MarkdownDialectGitHub);
    insertMath(doc, math, palette.value("math_font").toString() == "serif");
    sizeSlideText(doc, palette, fontSize, width, centered, code);
}
static QMutex fitMutex;
static QCache<QString, qreal> fittedSizes(4096);
static qreal fittedSize(const QString &key) {
    QMutexLocker lock(&fitMutex);
    const auto size = fittedSizes.object(key);
    return size ? *size : -1;
}
static void rememberFit(const QString &key, qreal size) {
    QMutexLocker lock(&fitMutex);
    fittedSizes.insert(key, new qreal(size));
}
static QRectF alignedRect(const QSizeF &size, const QRectF &within, Qt::Alignment alignment) {
    const qreal x = alignment & Qt::AlignLeft    ? within.left()
                    : alignment & Qt::AlignRight ? within.right() - size.width()
                                                 : within.center().x() - size.width() / 2;
    const qreal y = alignment & Qt::AlignTop      ? within.top()
                    : alignment & Qt::AlignBottom ? within.bottom() - size.height()
                                                  : within.center().y() - size.height() / 2;
    return QRectF(QPointF(x, y), size);
}
// Split panes share one gutter; the image side bleeds a little closer to the slide edge.
QRectF splitTextRect(const Media &media) {
    const qreal at = media.share / 100;
    if (media.split == "left")
        return QRectF(1920 * at + 50, 90, 1920 * (1 - at) - 50 - 120, 900);
    if (media.split == "right")
        return QRectF(130, 90, 1920 * (1 - at) - 50 - 130, 900);
    return QRectF(130, 60, 1660, 1080 * (1 - at) - 60 - 20);
}
QRectF mediaRect(const Media &media) {
    if (!media.split.isEmpty()) {
        const qreal at = media.share / 100;
        if (media.split == "left")
            return QRectF(90, 90, 1920 * at - 90 - 30, 900);
        if (media.split == "right")
            return QRectF(1920 * (1 - at) + 30, 90, 1920 * at - 30 - 90, 900);
        return QRectF(100, 1080 * (1 - at) + 10, 1720, 1080 * at - 10 - 50);
    }
    if (media.placed()) {
        const QSizeF box(1920 * media.size / 100, 1080 * media.size / 100);
        // The margin shrinks as the box grows, so large images stay on the slide.
        const qreal margin = qMin(60.0, (1080 - box.height()) / 2);
        return alignedRect(box, QRectF(0, 0, 1920, 1080).adjusted(margin, margin, -margin, -margin),
                           media.alignment);
    }
    return media.span ? QRectF(0, 0, 1920, 1080)
                      : (!media.video || media.text.trimmed().isEmpty() ? QRectF(70, 50, 1780, 980)
                                                        : QRectF(100, 280, 1720, 730));
}
QRectF mediaImageRect(const Media &media, const QSizeF &image) {
    const QRectF rect = mediaRect(media);
    QSizeF scaled = image;
    scaled.scale(rect.size(), media.span ? Qt::KeepAspectRatioByExpanding : Qt::KeepAspectRatio);
    // Placed images hug their corner or edge; everything else is centered.
    return alignedRect(scaled, rect, media.alignment);
}
static qreal svgLength(const QString &value) {
    QString number = value.trimmed();
    if (number.endsWith("px"))
        number.chop(2);
    bool ok = false;
    const qreal length = number.toDouble(&ok);
    return ok ? length : -1;
}
QVector<SvgLabel> svgLabels(const QString &svg, QRectF *viewBox) {
    QVector<SvgLabel> labels;
    struct Style {
        QString size = "16", anchor = "start", fill = "#000000", weight = "400", family = "sans-serif";
        bool moved = false; // Under a transform, so x and y are not where it is drawn.
    };
    QVector<Style> styles{Style()};
    QXmlStreamReader xml(svg);
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isEndElement() && styles.size() > 1)
            styles.removeLast();
        if (!xml.isStartElement())
            continue;
        const auto attributes = xml.attributes();
        Style style = styles.last();
        const auto take = [&](const char *name, QString &into) {
            if (attributes.hasAttribute(name))
                into = attributes.value(name).toString();
        };
        take("font-size", style.size);
        take("text-anchor", style.anchor);
        take("fill", style.fill);
        take("font-weight", style.weight);
        take("font-family", style.family);
        style.moved = style.moved || attributes.hasAttribute("transform");
        if (xml.name() == u"svg" && styles.size() == 1 && viewBox) {
            const QStringList box = attributes.value("viewBox").toString().split(
                QRegularExpression("[\\s,]+"), Qt::SkipEmptyParts);
            if (box.size() == 4)
                *viewBox = QRectF(box[0].toDouble(), box[1].toDouble(), box[2].toDouble(), box[3].toDouble());
            else
                *viewBox = QRectF(0, 0, svgLength(attributes.value("width").toString()),
                                  svgLength(attributes.value("height").toString()));
        }
        if (xml.name() != u"text") {
            styles << style;
            continue;
        }
        // A label is a <text> at one x and y that holds nothing but text, so its words can be
        // replaced in the file without touching anything around them.
        SvgLabel label;
        label.start = int(xml.characterOffset());
        const qreal x = svgLength(attributes.value("x").toString()),
                    y = svgLength(attributes.value("y").toString()), size = svgLength(style.size);
        bool plain = !style.moved && x >= 0 && y >= 0 && size > 0;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isEndElement())
                break;
            if (xml.isCharacters() && !xml.isCDATA())
                label.text += xml.text();
            else {
                plain = false;
                if (xml.isStartElement())
                    xml.skipCurrentElement();
            }
        }
        label.end = int(svg.lastIndexOf('<', xml.characterOffset() - 1));
        if (!plain || label.text.trimmed().isEmpty() || label.end <= label.start)
            continue;
        label.raw = svg.mid(label.start, label.end - label.start);
        label.size = size;
        label.anchor = style.anchor;
        label.fill = style.fill;
        label.weight = style.weight == "bold" ? 700 : style.weight == "normal" ? 400 : style.weight.toInt();
        if (label.weight < 100)
            label.weight = 400;
        label.family = style.family.section(',', 0, 0).remove('"').remove('\'').trimmed();
        // Measure at a large size and scale down, so small labels keep their fractional width.
        QFont font(label.family);
        font.setPixelSize(100);
        font.setWeight(QFont::Weight(label.weight));
        const QFontMetricsF metrics(font);
        const QString shown = label.text.simplified();
        const qreal width = metrics.horizontalAdvance(shown) * size / 100;
        const qreal left = style.anchor == "middle" ? x - width / 2 : style.anchor == "end" ? x - width : x;
        label.box = QRectF(left, y - metrics.ascent() * size / 100, width,
                           (metrics.ascent() + metrics.descent()) * size / 100);
        labels << label;
    }
    return labels;
}
QRectF svgDrawnRect(const Media &media, const QRectF &viewBox) {
    // Vectors fill the media's rect at the picture's own aspect, as paintSlide draws them.
    const QRectF dest = mediaImageRect(media, viewBox.size());
    QSizeF fitted = viewBox.size();
    fitted.scale(dest.size(), Qt::KeepAspectRatio);
    return QRectF(dest.center() - QPointF(fitted.width(), fitted.height()) / 2, fitted);
}
QString withSvgLabel(const QString &svg, const SvgLabel &label, const QString &text) {
    QString escaped = text;
    escaped.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;");
    return svg.left(label.start) + escaped + svg.mid(label.end);
}
namespace {
// QtSvg marks "no stroke" with a pen that has nothing to paint, and Qt's PDF engine strokes
// such a pen anyway, one SVG unit wide, in the fill's color: every filled shape grows a rim,
// and small glyphs turn bold. This engine passes QtSvg's drawing on to the real painter with
// those pens dropped.
class SvgPassThrough : public QPaintEngine, public QPaintDevice {
  public:
    explicit SvgPassThrough(QPainter *target)
        : QPaintEngine(AllFeatures), m_target(target), m_base(target->worldTransform()),
          m_opacity(target->opacity()), m_clipped(target->hasClipping()),
          m_clip(m_clipped ? target->clipPath() : QPainterPath()) {}
    QPaintEngine *paintEngine() const override { return const_cast<SvgPassThrough *>(this); }
    bool begin(QPaintDevice *) override { return true; }
    bool end() override { return true; }
    Type type() const override { return User; }
    void updateState(const QPaintEngineState &state) override {
        const auto dirty = state.state();
        if (dirty & DirtyTransform)
            m_target->setWorldTransform(state.transform() * m_base);
        if (dirty & DirtyPen) {
            const QPen pen = state.pen();
            m_target->setPen(pen.brush().style() == Qt::NoBrush ? QPen(Qt::NoPen) : pen);
        }
        if (dirty & DirtyBrush)
            m_target->setBrush(state.brush());
        if (dirty & DirtyBrushOrigin)
            m_target->setBrushOrigin(state.brushOrigin());
        if (dirty & DirtyOpacity)
            m_target->setOpacity(m_opacity * state.opacity());
        if (dirty & DirtyHints)
            m_target->setRenderHints(state.renderHints());
        if (dirty & DirtyCompositionMode)
            m_target->setCompositionMode(state.compositionMode());
        if (dirty & DirtyFont)
            m_target->setFont(state.font());
        if (dirty & (DirtyClipPath | DirtyClipRegion | DirtyClipEnabled))
            clip(state);
    }
    void drawPath(const QPainterPath &path) override { m_target->drawPath(path); }
    void drawPolygon(const QPointF *points, int count, PolygonDrawMode mode) override {
        if (mode == PolylineMode)
            m_target->drawPolyline(points, count);
        else
            m_target->drawPolygon(points, count, mode == WindingMode ? Qt::WindingFill : Qt::OddEvenFill);
    }
    void drawPixmap(const QRectF &rect, const QPixmap &pixmap, const QRectF &source) override {
        m_target->drawPixmap(rect, pixmap, source);
    }
    void drawImage(const QRectF &rect, const QImage &image, const QRectF &source,
                   Qt::ImageConversionFlags flags) override {
        m_target->drawImage(rect, image, source, flags);
    }
    void drawTextItem(const QPointF &point, const QTextItem &item) override { m_target->drawTextItem(point, item); }

  protected:
    int metric(PaintDeviceMetric metric) const override {
        const QPaintDevice *device = m_target->device();
        switch (metric) {
        case PdmWidth: return device->width();
        case PdmHeight: return device->height();
        case PdmWidthMM: return device->widthMM();
        case PdmHeightMM: return device->heightMM();
        case PdmNumColors: return device->colorCount();
        case PdmDepth: return device->depth();
        case PdmDpiX: return device->logicalDpiX();
        case PdmDpiY: return device->logicalDpiY();
        case PdmPhysicalDpiX: return device->physicalDpiX();
        case PdmPhysicalDpiY: return device->physicalDpiY();
        case PdmDevicePixelRatio: return int(device->devicePixelRatio());
        case PdmDevicePixelRatioScaled: return int(device->devicePixelRatioF() * devicePixelRatioFScale());
        default: return 0;
        }
    }

  private:
    // A drawing's clip narrows the clip the slide already set; it never widens it.
    void clip(const QPaintEngineState &state) {
        const QTransform current = m_target->worldTransform();
        m_target->setWorldTransform(m_base);
        if (m_clipped)
            m_target->setClipPath(m_clip);
        else
            m_target->setClipping(false);
        m_target->setWorldTransform(current);
        if (!state.isClipEnabled() || state.clipOperation() == Qt::NoClip)
            return;
        QPainterPath path = state.clipPath();
        if (path.isEmpty() && !state.clipRegion().isEmpty())
            path.addRegion(state.clipRegion());
        m_target->setClipPath(path, m_clipped ? Qt::IntersectClip : Qt::ReplaceClip);
    }
    QPainter *m_target;
    QTransform m_base;
    qreal m_opacity;
    bool m_clipped;
    QPainterPath m_clip;
};
} // namespace
void renderSvg(QSvgRenderer &svg, QPainter *p, const QRectF &bounds) {
    if (!p->paintEngine() || p->paintEngine()->type() != QPaintEngine::Pdf) {
        svg.render(p, bounds);
        return;
    }
    p->save();
    {
        SvgPassThrough device(p);
        QPainter through(&device);
        svg.render(&through, bounds);
    }
    p->restore();
}
void paintSlide(QPainter *p, const QRectF &target, const QString &source, const QString &base,
                const QVariantMap &inputPalette, QString *warning, bool overlayOnly,
                bool backgroundOnly) {
    p->save();
    p->setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
                      QPainter::SmoothPixmapTransform);
    p->translate(target.topLeft());
    p->scale(target.width() / 1920.0, target.height() / 1080.0);
    QVariantMap palette = inputPalette;
    QString bg = slideProperty(source, "background"), fg = slideProperty(source, "foreground");
    if (QColor(bg).isValid())
        palette["background"] = bg;
    if (QColor(fg).isValid())
        palette["foreground"] = fg;
    if (!overlayOnly)
        p->fillRect(QRectF(0, 0, 1920, 1080), QColor(palette["background"].toString()));
    auto media = parseMedia(source, base);
    // A build step shows part of the text, laid out as the whole so nothing moves as it grows.
    const SlideSteps steps = slideSteps(source, media, palette.value("incremental").toBool());
    const int step = palette.value("step", -1).toInt();
    const bool showMedia = step < 0 || step >= steps.media;
    const QString text = steps.text;
    auto problems = slideProblems(source, base, inputPalette);
    QRectF area(130, 90, 1660, 900);
    // An svg fence in the theme's colors, or a Mermaid diagram as Mermaid drew it.
    const QByteArray drawing = !media.svg.isEmpty() ? themedSvg(media.svg, palette)
                               : !media.mermaid.isEmpty() ? mermaidDiagram(media.mermaid, inputPalette).svg
                                                          : QByteArray();
    if (!drawing.isEmpty() && media.file.isEmpty()) {
        // Like a video, a drawing under a headline leaves the headline a band at the top.
        const QRectF rect =
            text.isEmpty() ? QRectF(70, 50, 1780, 980) : QRectF(100, 280, 1720, 730);
        if (!text.isEmpty())
            area = QRectF(130, 40, 1660, 205);
        QSvgRenderer svg(drawing);
        if (!overlayOnly && !backgroundOnly && showMedia && svg.isValid() && !svg.viewBoxF().isEmpty()) {
            QSizeF size = svg.viewBoxF().size();
            size.scale(rect.size(), Qt::KeepAspectRatio);
            svg.setAspectRatioMode(Qt::KeepAspectRatio);
            renderSvg(svg, p, QRectF(rect.center() - QPointF(size.width(), size.height()) / 2, size));
        }
    }
    if (!media.file.isEmpty()) {
        QString path =
            media.video ? (media.poster.isEmpty() ? ensurePoster(media.path, base) : media.poster)
                        : media.path;
        const QRectF rect = mediaRect(media);
        const QSize pixels = p->deviceTransform().mapRect(rect).size().toSize();
        // An SVG with nothing over it is drawn as vectors, sharp at any size and in PDF. Its
        // small raster only gives its shape and edge colors for the background.
        const bool vector = !media.video && (text.isEmpty() || media.beside()) &&
                            QStringList{"svg", "svgz"}.contains(QFileInfo(path).suffix().toLower());
        QImage image = loadedImage(path, vector ? QSize(480, 270) : pixels, media.span);
        // Video backgrounds use the first frame, even with a custom poster.
        const QImage backdrop = media.video && !media.span && !media.poster.isEmpty() &&
            (media.background == "blur" || media.background == "auto")
            ? loadedImage(ensurePoster(media.path, base), QSize(320, 180), false) : image;
        if (!overlayOnly && !media.span && media.background == "blur") {
            if (!backdrop.isNull())
                p->drawImage(QRectF(0, 0, 1920, 1080), blurredBackground(backdrop));
        }
        if ((!media.video || !media.background.isEmpty()) && !media.span && !media.beside() &&
            media.background != "theme" && (bg.isEmpty() || !media.background.isEmpty())) {
            QColor color(media.background);
            if ((media.background.isEmpty() || media.background == "auto") && !backdrop.isNull()) {
                // Quantized edge votes ignore transparent pixels and tolerate compression noise.
                QMap<int, QVector<QColor>> votes;
                for (int i = 0; i < 64; ++i) {
                    int x = i * (backdrop.width() - 1) / 63, y = i * (backdrop.height() - 1) / 63;
                    for (QPoint point : {QPoint(x, 0), QPoint(x, backdrop.height() - 1), QPoint(0, y),
                                         QPoint(backdrop.width() - 1, y)}) {
                        QColor c = backdrop.pixelColor(point);
                        if (c.alpha() < 240)
                            continue;
                        votes[(c.red() / 16) * 256 + (c.green() / 16) * 16 + c.blue() / 16].append(
                            c);
                    }
                }
                QVector<QColor> best;
                for (auto it = votes.cbegin(); it != votes.cend(); ++it)
                    if (it.value().size() > best.size())
                        best = it.value();
                if (best.size() >= 128) {
                    int r = 0, g = 0, b = 0;
                    for (const QColor &c : best) {
                        r += c.red();
                        g += c.green();
                        b += c.blue();
                    }
                    color = QColor(r / best.size(), g / best.size(), b / best.size());
                }
            }
            if (color.isValid()) {
                if (!overlayOnly)
                    p->fillRect(QRectF(0, 0, 1920, 1080), color);
                if (fg.isEmpty()) {
                    QString ink = (color.redF() * 0.2126 + color.greenF() * 0.7152 +
                                   color.blueF() * 0.0722) > .55
                                      ? "#161616"
                                      : "#ffffff";
                    palette["foreground"] = ink;
                    palette["accent"] = ink;
                }
            }
        }

        if (!overlayOnly && !media.split.isEmpty() && QColor(media.background).isValid()) {
            const qreal at = media.share / 100;
            p->fillRect(media.split == "left"    ? QRectF(0, 0, 1920 * at, 1080)
                        : media.split == "right" ? QRectF(1920 * (1 - at), 0, 1920 * at, 1080)
                                                 : QRectF(0, 1080 * (1 - at), 1920, 1080 * at),
                        QColor(media.background));
        }
        if (!overlayOnly && !backgroundOnly && showMedia && !image.isNull()) {
            const QRectF dest = mediaImageRect(media, image.size());
            p->save();
            p->setClipRect(rect);
            QSvgRenderer svg;
            if (vector && svg.load(path)) {
                svg.setAspectRatioMode(Qt::KeepAspectRatio);
                renderSvg(svg, p, dest);
            } else
                p->drawImage(dest, !media.video && !media.beside() && !text.isEmpty()
                                       ? softenedImage(image, dest.size())
                                       : image);
            p->restore();
        } else if (!overlayOnly && !backgroundOnly && showMedia) {
            p->setPen(QColor(palette["accent"].toString()));
            const QString message = "Missing media\n" + media.file;
            QFont diagnostic("sans");
            // Shrink the notice to fit small placed boxes, and never draw past them.
            for (int size = 32; size >= 8; size -= 2) {
                diagnostic.setPixelSize(size);
                if (QRectF(QFontMetricsF(diagnostic).boundingRect(rect, Qt::AlignCenter, message))
                        .width() <= rect.width())
                    break;
            }
            p->save();
            p->setClipRect(rect);
            p->setFont(diagnostic);
            p->drawText(rect, Qt::AlignCenter, message);
            p->restore();
        }
        // Text beside a placed image keeps the theme's colors and full area; beside a split
        // image it keeps the colors and takes the other side.
        if (!media.split.isEmpty())
            area = splitTextRect(media);
        else if (!media.placed() && (!media.video || media.span)) {
            if (!backgroundOnly)
                p->fillRect(QRectF(0, 0, 1920, 1080), QColor(0, 0, 0, qRound(media.overlay * 255)));
            if (fg.isEmpty() && !text.isEmpty())
                palette["foreground"] = "#ffffff";
        } else if (!media.placed() && !text.isEmpty())
            area = QRectF(130, 40, 1660, 205);
    }
    if (backgroundOnly) {
        p->restore();
        return;
    }
    // Text clears a running header. At a fixed size it starts at the same height on every
    // slide, so headings line up from one slide to the next.
    const qreal fixed = palette.value("text_size").toDouble();
    const QString header = palette.value("header").toString();
    if (!header.isEmpty())
        area.setTop(qMax(area.top(), fixed > 0 ? 150.0 : 110.0));
    else if (fixed > 0)
        area.setTop(qMax(area.top(), 90.0));
    if (!text.isEmpty()) {
        bool code = text.contains(
            QRegularExpression("^ {0,3}(`{3,}|~{3,})", QRegularExpression::MultilineOption));
        bool quote = text.startsWith('>');
        bool list = text.contains(
            QRegularExpression("^\\s*(?:[-*+] |[0-9]+[.)] )", QRegularExpression::MultilineOption));
        bool table = text.contains(tableDelimiter());
        // A table alone on its slide sits in the middle; beside other text it lines up with it.
        const bool tableOnly = table && !text.split('\n').filter(QRegularExpression("^(?!.*\\|).*\\S")).size();
        bool centered = tableOnly || !(code || quote || list || table);
        // Text in a side column reads from its left edge, like a page.
        if (media.split == "left" || media.split == "right")
            centered = false;
        const QString alignment = slideProperty(source, "alignment");
        if (alignment == "left")
            centered = false;
        if (alignment == "center")
            centered = true;
        bool stack = (text.contains('\n') || text.contains('\r')) && !text.startsWith('#') &&
                     !quote && !list && !code;
        qreal low = 8, high = code ? 56 : quote ? 64 : list || table ? 72 : stack ? 128 : 76;
        if (media.video && !media.span)
            high = 48;
        if (fixed > 0)
            high = fixed;
        QTextDocument doc;
        // Layout happens in 1080p slide units, so every render size, the PDF and
        // a theme change all reuse one search. Colors never affect the fit.
        const QString fit = QString("%1 %2 %3 %4 %5 ").arg(high).arg(area.width()).arg(area.height())
                                .arg(centered).arg(code) + palette.value("font").toString() + ' ' +
                                palette.value("heading_font").toString() + ' ' +
                                palette.value("heading_weight").toString() + ' ' +
                                palette.value("heading_size").toString() + '\n' + text;
        if (const qreal fitted = fittedSize(fit); fitted > 0) {
            low = fitted;
            layoutSlideText(doc, text, palette, low, area.width(), centered, code);
        } else {
            layoutSlideText(doc, text, palette, high, area.width(), centered, code);
            const bool fits = doc.size().height() <= area.height() && doc.idealWidth() <= area.width() + 1;
            if (fits)
                low = high;
            for (int iteration = 0; !fits && iteration < 9; ++iteration) {
                qreal size = (low + high) / 2;
                sizeSlideText(doc, palette, size, area.width(), centered, code);
                if (doc.size().height() <= area.height() && doc.idealWidth() <= area.width() + 1)
                    low = size;
                else
                    high = size;
            }
            if (!fits)
                sizeSlideText(doc, palette, low, area.width(), centered, code);
            rememberFit(fit, low);
        }
        const bool breakWords = !code && doc.idealWidth() > area.width() + 1;
        if (breakWords)
            sizeSlideText(doc, palette, low, area.width(), centered, code, true);
        highlightCode(doc, palette);
        if (fixed > 0 && low < fixed - 0.5 && warning)
            *warning = QString("Text shrinks to %1px to fit, below the deck's %2px")
                           .arg(qRound(low)).arg(qRound(fixed));
        else if (low < 24 && fixed <= 0 && warning)
            *warning = "Text fits below 24px on a 1080p slide";
        const QString vertical = slideProperty(source, "vertical");
        const qreal room = qMax(0.0, area.height() - doc.size().height());
        const bool top = vertical == "top" || (vertical.isEmpty() && fixed > 0);
        const qreal offset = vertical == "bottom" ? room : top ? 0 : room / 2;
        // An earlier step lays out only its part at the size and place of the whole. Text flows
        // from the top, so the part lands exactly where it sits on the finished slide.
        QTextDocument part;
        const QString shown = steps.shown(step);
        if (shown.size() < text.size()) {
            layoutSlideText(part, shown, palette, low, area.width(), centered, code);
            if (breakWords)
                sizeSlideText(part, palette, low, area.width(), centered, code, true);
            highlightCode(part, palette);
        }
        p->save();
        p->translate(area.x(), area.y() + offset);
        QAbstractTextDocumentLayout::PaintContext context;
        context.palette.setColor(QPalette::Text, QColor(palette["foreground"].toString()));
        (shown.size() < text.size() ? part : doc).documentLayout()->draw(p, context);
        p->restore();
    }
    if (!header.isEmpty()) {
        // A running header: the deck's name on the left, the slide's section and number on
        // the right, small and spaced out in the text's color.
        QFont small(palette.value("font", "JetBrains Mono").toString());
        small.setPixelSize(15);
        small.setWeight(QFont::Medium);
        small.setCapitalization(QFont::AllUppercase);
        small.setLetterSpacing(QFont::AbsoluteSpacing, 15 * 0.18);
        small.setHintingPreference(QFont::PreferNoHinting);
        QColor ink(palette["foreground"].toString());
        ink.setAlphaF(0.6);
        p->setFont(small);
        p->setPen(ink);
        const QRectF band(130, 50, 1660, 40);
        QString place = slideProperty(source, "section");
        if (const int number = palette.value("slide_number").toInt(); number > 0)
            place += (place.isEmpty() ? "" : "   ") + QString("%1").arg(number, 2, 10, QChar('0'));
        p->drawText(band, Qt::AlignRight | Qt::AlignVCenter, place);
        const qreal used = QFontMetricsF(small).horizontalAdvance(place);
        p->drawText(band.adjusted(0, 0, -used - 40, 0), Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetricsF(small).elidedText(header, Qt::ElideRight, band.width() - used - 40));
    }
    if (!problems.isEmpty()) {
        p->setPen(Qt::white);
        p->fillRect(QRectF(0, 1000, 1920, 80), QColor("#9b3030"));
        QFont diagnostic("sans");
        diagnostic.setPixelSize(21);
        p->setFont(diagnostic);
        p->drawText(QRectF(30, 1005, 1860, 70), Qt::AlignVCenter, problems.join(" · "));
        if (warning)
            *warning = problems.join("; ");
    }
    p->restore();
}
SlideItem::SlideItem(QQuickItem *parent) : QQuickPaintedItem(parent) { setAntialiasing(true); }
void SlideItem::setDeck(Deck *deck) {
    if (m_deck)
        disconnect(m_deck, nullptr, this, nullptr);
    m_deck = deck;
    if (deck)
        connect(deck, &Deck::selectionChanged, this, [this] { update(); });
    emit deckChanged();
    update();
}
void SlideItem::paint(QPainter *p) {
    if (m_deck)
        paintSlide(p, boundingRect(), m_deck->slideSource(), m_deck->baseDir(),
                   m_deck->slidePalette(m_deck->selected()),
                   nullptr, m_overlayOnly);
}
// Qt multiplies an Image's sourceSize by the screen's scale, so a 340px thumbnail arrives as
// 680px on a 2x display and the stage as 3840px. Classify requests against the largest one
// seen instead of fixed pixel sizes: the stage is the big one, everything else is small.
static std::atomic_int largestWidth{1920};
static std::atomic_int stageWidth{1920}, stageHeight{1080};
static bool stageSized(const QSize &dimensions) {
    int largest = largestWidth.load();
    while (dimensions.width() > largest && !largestWidth.compare_exchange_weak(largest, dimensions.width())) {}
    return dimensions.width() * 2 > largestWidth.load();
}
static ImageCache &slideCache(const QSize &dimensions) {
    // Full previews must not evict the much smaller sidebar and overview thumbnails.
    static ImageCache thumbnails(96 * 1024), previews(128 * 1024);
    return stageSized(dimensions) ? previews : thumbnails;
}
static QString slideCacheKey(const QString &id, const QSize &dimensions) {
    return id + QString::number(dimensions.width()) + "x" + QString::number(dimensions.height());
}
static QImage renderedSlide(const QString &id, QSize *size, const QSize &requested) {
    const QSize dimensions = requested.isValid() ? requested : QSize(320, 180);
    auto &renders = slideCache(dimensions);
    const QString key = slideCacheKey(id, dimensions);
    if (const auto cached = renders.get(key); !cached.isNull()) {
        if (size) *size = cached.size();
        return cached;
    }
    QByteArray bytes =
        QByteArray::fromBase64(id.section('/', 0, 0).toLatin1(), QByteArray::Base64UrlEncoding);
    QDataStream stream(bytes);
    QString source, base;
    QVariantMap palette;
    stream >> source >> base >> palette;
    if (stream.status() != QDataStream::Ok)
        return {};
    QImage image(dimensions, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter p(&image);
    paintSlide(&p, image.rect(), source, base, palette, nullptr, id.endsWith("/overlay"),
               id.endsWith("/background"));
    p.end();
    // Room for the slides around the selection at full size, or a whole deck of thumbnails.
    // Full-size frames reach 33 MB at 2x, so keep as many as a twentieth of free memory holds.
    int frames = 14;
    if (const qint64 free = budget::availableBytes(); free > 0)
        frames = int(qBound(qint64(4), free / 20 / qMax(qint64(1), qint64(image.sizeInBytes())), qint64(14)));
    renders.put(key, image, stageSized(dimensions) ? frames : 400);
    if (size)
        *size = image.size();
    return image;
}

namespace {
class SlideResponse : public QQuickImageResponse {
    QImage m_image;
    std::atomic_bool m_cancelled = false;
  public:
    void cancel() override { m_cancelled = true; }
    void complete(const QImage &image) {
        if (!m_cancelled) m_image = image;
        emit finished();
    }
    void render(const QString &id, const QSize &size) {
        if (!m_cancelled) m_image = renderedSlide(id, nullptr, size);
        emit finished();
    }
    QQuickTextureFactory *textureFactory() const override {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }
};
}

Thumbnails::Thumbnails(Deck *deck) {
    // Full-size renders are the slow ones; give the stage and its prefetches most of the cores,
    // as far as memory allows: each full-size render holds about 100 MB while it works.
    const int cores = QThread::idealThreadCount();
    m_thumbnails.setMaxThreadCount(qBound(2, cores / 4, 4));
    m_previews.setMaxThreadCount(qMin(qBound(2, cores / 2, 6), budget::workers(100LL << 20, 6, 0.2, 0, 2)));
    m_cached.setMaxThreadCount(1);
    m_videos.setMaxThreadCount(2);
    auto timer = new QTimer(this);
    timer->setInterval(60);
    timer->setSingleShot(true);
    connect(deck, &Deck::selectionChanged, this, [this, timer] {
        if (m_stopping->load())
            return;
        ++*m_generation;
        timer->start();
    });
    connect(timer, &QTimer::timeout, this, [this, deck = QPointer<Deck>(deck)] {
        QMutexLocker submissions(&m_submissions);
        if (!deck || m_stopping->load())
            return;
        const auto generation = m_generation;
        const auto current = generation->load();
        for (int offset : {1, -1, 2, -2, 3, -3}) {
            const int index = deck->selected() + offset;
            if (index < 0 || index >= deck->count())
                continue;
            // Do not start video decoding or animation playback speculatively.
            const auto media = parseMedia(deck->slide(index), deck->baseDir());
            QImageReader reader(media.path);
            if (media.video || (!media.path.isEmpty() && reader.supportsAnimation() && reader.imageCount() > 1))
                continue;
            const QString id = deck->renderId(index);
            // Prefetch at the size the stage actually asks for, or the work is never used.
            const QSize size(stageWidth.load(), stageHeight.load());
            m_previews.start([generation, current, id, size] {
                if (generation->load() == current)
                    renderedSlide(id, nullptr, size);
            }, -1);
        }
    });
    timer->start();
}
Thumbnails::~Thumbnails() { shutdown(); }
void Thumbnails::shutdown() {
    {
        // Fence submissions before draining. Qt Quick can retain this provider
        // after the engine dies; no later request may start touching Qt fonts.
        QMutexLocker submissions(&m_submissions);
        m_stopping->store(true);
        ++*m_generation;
    }
    m_thumbnails.waitForDone();
    m_previews.waitForDone();
    m_cached.waitForDone();
    m_videos.waitForDone();
}
QImage Thumbnails::requestImage(const QString &id, QSize *size, const QSize &requested) {
    // Synchronous entry point used by rendering tests, not the QML engine.
    QMutexLocker submissions(&m_submissions);
    return m_stopping->load() ? QImage() : renderedSlide(id, size, requested);
}
QQuickImageResponse *Thumbnails::requestImageResponse(const QString &id, const QSize &requested) {
    QMutexLocker submissions(&m_submissions);
    auto response = new SlideResponse;
    const auto stopping = m_stopping;
    if (stopping->load()) {
        // Finished responses are valid even before the loader connects its
        // signal handler; QQuickImageResponse records its finished state.
        response->complete({});
        return response;
    }
    const QSize dimensions = requested.isValid() ? requested : QSize(320, 180);
    const QImage cached = slideCache(dimensions).get(slideCacheKey(id, dimensions));
    if (!cached.isNull()) {
        // A ready image must not wait behind unrelated decoding or prefetches.
        m_cached.start([response, cached, stopping] {
            response->complete(stopping->load() ? QImage() : cached);
        });
        return response;
    }
    QByteArray bytes =
        QByteArray::fromBase64(id.section('/', 0, 0).toLatin1(), QByteArray::Base64UrlEncoding);
    QDataStream stream(bytes);
    QString source, base;
    stream >> source >> base;
    // Video poster decoding cannot occupy the workers needed for ordinary slides.
    const bool video = parseMedia(source, base).video;
    const bool stage = stageSized(dimensions);
    if (stage) {
        stageWidth.store(dimensions.width());
        stageHeight.store(dimensions.height());
    }
    auto &pool = video ? m_videos : stage ? m_previews : m_thumbnails;
    pool.start(
        [response, id, requested, stopping] {
            if (stopping->load())
                response->complete({});
            else
                response->render(id, requested);
        },
        1);
    return response;
}
