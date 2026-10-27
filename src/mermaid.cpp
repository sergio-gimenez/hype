#include "mermaid.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace {
QMutex mutex; // Guards the cache, and lets one Chromium run at a time.
QHash<QString, Diagram> cache;

// Mermaid's base theme, colored from the deck: nodes in the background with an accent border,
// text and lines in the foreground, and the deck's font.
QByteArray config(const QVariantMap &palette) {
    const QString theme = palette.value("mermaid_theme").toString();
    if (!theme.isEmpty())
        return QJsonDocument(QJsonObject{{"theme", theme}}).toJson(QJsonDocument::Compact);
    const QString background = palette.value("background", "#ffffff").toString(),
                  foreground = palette.value("foreground", "#000000").toString(),
                  accent = palette.value("accent", foreground).toString();
    QJsonObject variables{{"fontFamily", palette.value("font", "JetBrains Mono").toString()}};
    for (const char *name : {"background", "primaryColor", "secondaryColor", "tertiaryColor", "mainBkg",
                             "edgeLabelBackground", "clusterBkg", "actorBkg", "noteBkgColor"})
        variables.insert(name, background);
    for (const char *name : {"primaryTextColor", "secondaryTextColor", "tertiaryTextColor", "textColor",
                             "titleColor", "lineColor", "clusterBorder", "actorTextColor", "signalColor",
                             "signalTextColor", "labelTextColor", "noteTextColor"})
        variables.insert(name, foreground);
    for (const char *name : {"primaryBorderColor", "secondaryBorderColor", "tertiaryBorderColor",
                             "nodeBorder", "actorBorder", "noteBorderColor"})
        variables.insert(name, accent);
    return QJsonDocument(QJsonObject{{"theme", "base"}, {"themeVariables", variables}})
        .toJson(QJsonDocument::Compact);
}
QString cacheFolder() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/hype/mermaid";
}
bool run(const QString &program, const QStringList &arguments, const QString &folder, QString *output) {
    QProcess process;
    process.setWorkingDirectory(folder);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, arguments);
    const bool finished = process.waitForStarted(10000) && process.waitForFinished(60000);
    if (!finished)
        process.kill();
    *output = QString::fromUtf8(process.readAll());
    return finished && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}
Diagram draw(const QString &source, const QByteArray &settings) {
    Diagram diagram;
    const QString mmdc = QStandardPaths::findExecutable("mmdc"),
                  pdftocairo = QStandardPaths::findExecutable("pdftocairo");
    if (mmdc.isEmpty() || pdftocairo.isEmpty()) {
        diagram.error = "Mermaid needs mmdc (npm install -g @mermaid-js/mermaid-cli) and pdftocairo (poppler)";
        return diagram;
    }
    QTemporaryDir folder;
    QFile input(folder.filePath("diagram.mmd")), options(folder.filePath("config.json"));
    if (!folder.isValid() || !input.open(QIODevice::WriteOnly) || input.write(source.toUtf8()) < 0 ||
        !options.open(QIODevice::WriteOnly) || options.write(settings) < 0) {
        diagram.error = "Could not prepare Mermaid: " + input.errorString();
        return diagram;
    }
    input.close();
    options.close();
    QString output;
    // Chromium prints the laid-out diagram as a vector PDF, cropped to it.
    if (!run(mmdc, {"-q", "-i", "diagram.mmd", "-o", "diagram.pdf", "-c", "config.json", "-b", "transparent", "--pdfFit"},
             folder.path(), &output)) {
        // Mermaid's own message, from its first error line up to the stack trace.
        static const QRegularExpression frame("^\\s+at |\\.js:\\d+");
        QStringList message;
        for (const QString &line : output.split('\n')) {
            if (message.isEmpty() && !line.contains("rror"))
                continue;
            if (frame.match(line).hasMatch() || line.trimmed().isEmpty())
                break;
            message << line.trimmed();
        }
        diagram.error = "Mermaid: " + (message.isEmpty() ? output.trimmed().section('\n', 0, 0) : message.join(' '));
        return diagram;
    }
    if (!run(pdftocairo, {"-svg", "diagram.pdf", "diagram.svg"}, folder.path(), &output)) {
        diagram.error = "pdftocairo could not convert the diagram: " + output.trimmed().section('\n', -1);
        return diagram;
    }
    QFile svg(folder.filePath("diagram.svg"));
    if (!svg.open(QIODevice::ReadOnly)) {
        diagram.error = "Could not read Mermaid's output";
        return diagram;
    }
    diagram.svg = svg.readAll();
    return diagram;
}
} // namespace

Diagram mermaidDiagram(const QString &source, const QVariantMap &palette) {
    const QByteArray settings = config(palette);
    const QString key = QString::fromLatin1(
        QCryptographicHash::hash("hype-mermaid 1\n" + settings + "\n" + source.toUtf8(), QCryptographicHash::Sha1).toHex());
    QMutexLocker lock(&mutex);
    if (const auto found = cache.constFind(key); found != cache.constEnd())
        return *found;
    QFile saved(cacheFolder() + "/" + key + ".svg");
    Diagram diagram;
    if (saved.open(QIODevice::ReadOnly))
        diagram.svg = saved.readAll();
    else {
        diagram = draw(source, settings);
        // Failures stay in memory only, so installing Mermaid later is noticed next time.
        if (!diagram.svg.isEmpty() && QDir().mkpath(cacheFolder()) && saved.open(QIODevice::WriteOnly))
            saved.write(diagram.svg);
    }
    cache.insert(key, diagram);
    return diagram;
}
