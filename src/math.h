#pragma once
#include <QByteArray>
#include <QPainterPath>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QTextFormat>
#include <QVector>
class QTextDocument;

// TeX math as pandoc reads it: $…$ in a line of text, $$…$$ displayed.
struct MathSpan {
    QString tex;
    bool display = false;
};
// A formula as LaTeX sets it in a beamer frame: glyph outlines, sharp at any size.
struct Formula {
    QByteArray svg;       // dvisvgm's picture, as cached on disk; empty when LaTeX failed.
    QPainterPath outline; // The same shapes, ready to fill.
    QRectF box;           // In TeX points at beamer's 11pt, with the baseline at y = 0.
    QString error;
};
// Display formulas keep the middle of their line however the text around them is aligned.
constexpr int MathBlockProperty = QTextFormat::UserProperty + 1;
// The Markdown with each formula outside code taken out and a private-use character left in its
// place, so Markdown never reads the underscores, stars and backslashes inside it.
QString withoutMath(const QString &markdown, QVector<MathSpan> *math);
// Sets every formula not already cached in one LaTeX run. `serif` takes beamer's serif math
// font theme instead of its sans-serif default.
void prepareFormulas(const QVector<MathSpan> &math, bool serif);
Formula typesetFormula(const MathSpan &math, bool serif);
// Puts the typeset formulas where withoutMath left its placeholders.
void insertMath(QTextDocument &doc, const QVector<MathSpan> &math, bool serif);
// What LaTeX made of each formula it could not set, or why it could set none.
QStringList mathProblems(const QString &text);
