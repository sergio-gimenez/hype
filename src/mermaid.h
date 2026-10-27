#pragma once
#include <QByteArray>
#include <QString>
#include <QVariantMap>

// A ```mermaid fence drawn as GitHub draws it: the Mermaid CLI lays it out in Chromium, and the
// page it prints becomes an SVG of outlines, so it needs no fonts and stays vector in PDF.
struct Diagram {
    QByteArray svg; // Empty when Mermaid could not draw it.
    QString error;
};
// The diagram in the deck's colors and font, or in one of Mermaid's own themes when the deck
// names one as `mermaid_theme`. Each is drawn once, then served from memory and disk.
Diagram mermaidDiagram(const QString &source, const QVariantMap &palette);
