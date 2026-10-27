#pragma once
#include "deck.h"
#include <QMutex>
#include <QQuickImageProvider>
#include <QQuickPaintedItem>
#include <QThreadPool>
#include <atomic>
#include <memory>
class QTextDocument;
struct Media {
    QString file, path, poster, error, background;
    QString svg; // A ```svg fence, drawn as vectors where a picture would be
    QString mermaid; // A ```mermaid fence, drawn the same way once Mermaid lays it out
    int svgBlocks = 0;
    bool video = false, span = false, loop = false, muted = false, autoplay = true;
    double overlay = 0;
    QString text;
    // A placed image sits at a position, sized as a percentage of the slide.
    QString position;
    double size = 0;
    Qt::Alignment alignment = Qt::AlignCenter;
    // A split image takes one side of the slide (left, right, or bottom) and the text the
    // other; share is that side's percentage of the slide.
    QString split;
    double share = 0;
    bool placed() const { return !position.isEmpty(); }
    bool beside() const { return placed() || !split.isEmpty(); }
};
constexpr double defaultPlacedSize = 10;
const QStringList &mediaSplits();
QRectF splitTextRect(const Media &media);
const QStringList &mediaPositions();
QRectF mediaRect(const Media &media);
QRectF mediaImageRect(const Media &media, const QSizeF &image);
QString withReplacedMedia(const QString &source, const QString &file);
QImage softenedImage(const QImage &image, const QSizeF &slideSize);
QString withMedia(const QString &source, const QString &reference);
QString withMediaDirectives(const QString &source, const QStringList &remove,
                            const QStringList &add);
QString speakerNotes(const QString &source);
// The text with fenced and indented code, and inline code too unless told not to, blanked out,
// so patterns only find what is written outside it.
QString outsideCode(QString source, bool maskInline = true);
// Build steps, written as pandoc writes them for beamer: a line of ". . ." pauses, and each
// item of an incremental list (`incremental: true`, or inside ::: incremental) is a step.
struct SlideSteps {
    QString text;      // The slide's text without the markers, trimmed as it is drawn.
    QVector<int> cuts; // Where in text each step but the last ends.
    int media = 0;     // The step the slide's picture appears on.
    int count() const { return qMax(int(cuts.size()), media) + 1; }
    // The text a step shows; a negative step is the finished slide.
    QString shown(int step) const;
};
SlideSteps slideSteps(const QString &source, const Media &media, bool incremental);
// A one-line label in an SVG, which the editor lets you retype on the slide.
struct SvgLabel {
    QString text, raw;   // As shown, and as written in the file between the tags.
    int start = -1, end = -1; // Where raw sits in the file.
    QRectF box;          // In the SVG's own units.
    qreal size = 0;
    QString anchor, fill, family;
    int weight = 400;
};
// The labels of an SVG: <text> elements at one x and y holding only text, and no transforms.
QVector<SvgLabel> svgLabels(const QString &svg, QRectF *viewBox = nullptr);
// Where an SVG's view box lands on the 1920×1080 slide.
QRectF svgDrawnRect(const Media &media, const QRectF &viewBox);
// The file with one label's text replaced, everything else as it was.
QString withSvgLabel(const QString &svg, const SvgLabel &label, const QString &text);
// A hidden slide stays in the deck but is skipped when presenting and left out of exports.
bool slideHidden(const QString &source);
// Sets an attribute in the slide's <!-- hype: --> comment; an empty value removes it.
QString withSlideProperty(QString source, const QString &key, const QString &value);
Media parseMedia(const QString &source, const QString &base);
QString ensurePoster(const QString &video, const QString &base);
// The palette gives a Mermaid diagram the colors it is drawn in, so checking it reuses that drawing.
class QSvgRenderer;
// Draws an SVG on any painter; into a PDF it drops the invisible pens Qt would stroke there.
void renderSvg(QSvgRenderer &svg, QPainter *painter, const QRectF &bounds);
QStringList slideProblems(const QString &source, const QString &base, const QVariantMap &palette = {});
void layoutSlideText(QTextDocument &document, const QString &markdown, const QVariantMap &palette,
                     qreal fontSize, qreal width, bool centered, bool code);
void paintSlide(QPainter *painter, const QRectF &target, const QString &source, const QString &base,
                const QVariantMap &palette, QString *warning = nullptr, bool overlayOnly = false,
                bool backgroundOnly = false);
class SlideItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(Deck *deck READ deck WRITE setDeck NOTIFY deckChanged)
    Q_PROPERTY(bool overlayOnly MEMBER m_overlayOnly NOTIFY deckChanged)
  public:
    explicit SlideItem(QQuickItem *parent = nullptr);
    Deck *deck() const { return m_deck; }
    void setDeck(Deck *deck);
    void paint(QPainter *painter) override;
  signals:
    void deckChanged();

  private:
    Deck *m_deck = nullptr;
    bool m_overlayOnly = false;
};
class Thumbnails : public QQuickAsyncImageProvider {
  public:
    explicit Thumbnails(Deck *deck);
    ~Thumbnails() override;
    void shutdown();
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &requestedSize) override;

  private:
    QMutex m_submissions;
    std::shared_ptr<std::atomic_bool> m_stopping = std::make_shared<std::atomic_bool>(false);
    QThreadPool m_thumbnails, m_previews, m_cached, m_videos;
    std::shared_ptr<std::atomic_uint64_t> m_generation = std::make_shared<std::atomic_uint64_t>(0);
};
