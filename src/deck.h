#pragma once
#include <QAbstractListModel>
#include <QColor>
#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QImage>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
class QProcess;

struct Slide {
    QString source;
    int start = 0;
    int end = 0;
};
struct ParsedDeck {
    QString header;
    QVector<Slide> slides;
    QString error;
    int errorOffset = -1; // Where the unfinished front matter or code fence opens.
};
ParsedDeck parseDeck(const QString &source);
QString scalar(const QString &header, const QString &key, const QString &fallback = {});
QString setScalar(QString header, const QString &key, const QString &value);

class Deck : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString source READ source NOTIFY changed)
    Q_PROPERTY(QString slideSource READ slideSource NOTIFY selectionChanged)
    Q_PROPERTY(QString slideText READ slideText NOTIFY selectionChanged)
    Q_PROPERTY(int selected READ selected WRITE select NOTIFY selectionChanged)
    Q_PROPERTY(int selectionFirst READ selectionFirst NOTIFY selectionChanged)
    Q_PROPERTY(int selectionLast READ selectionLast NOTIFY selectionChanged)
    Q_PROPERTY(int selectionCount READ selectionCount NOTIFY selectionChanged)
    Q_PROPERTY(QString speakerNotes READ speakerNotes NOTIFY selectionChanged)
    Q_PROPERTY(bool selectionHidden READ selectionHidden NOTIFY selectionChanged)
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    Q_PROPERTY(bool dirty READ dirty NOTIFY changed)
    Q_PROPERTY(QString path READ path NOTIFY changed)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QString sizeLabel READ sizeLabel NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool compressingImage READ compressingImage NOTIFY compressingImageChanged)
    Q_PROPERTY(bool exporting READ exporting NOTIFY exportChanged)
    Q_PROPERTY(double exportProgress READ exportProgress NOTIFY exportChanged)
    Q_PROPERTY(QString exportStatus READ exportStatus NOTIFY exportChanged)
    Q_PROPERTY(bool exportFailed READ exportFailed NOTIFY exportChanged)
    Q_PROPERTY(QStringList fontNames READ fontNames CONSTANT)
    Q_PROPERTY(QString fontName READ fontName NOTIFY fontChanged)
    Q_PROPERTY(QStringList themeNames READ themeNames CONSTANT)
    Q_PROPERTY(QStringList mediaPositions READ mediaPositions CONSTANT)
    Q_PROPERTY(QString themeName READ themeName NOTIFY changed)
    Q_PROPERTY(QColor background READ background NOTIFY changed)
    Q_PROPERTY(QColor foreground READ foreground NOTIFY changed)
    Q_PROPERTY(QColor accent READ accent NOTIFY changed)
    Q_PROPERTY(QVariantMap media READ media NOTIFY selectionChanged)
  public:
    explicit Deck(QObject *parent = nullptr, const QString &exportProgram = {});
    ~Deck() override;
    bool compressingImage() const { return m_compressingImage; }
    bool exporting() const { return m_exporting; }
    double exportProgress() const { return m_exportProgress; }
    QString exportStatus() const { return m_exportStatus; }
    bool exportFailed() const { return m_exportFailed; }
    bool loadExportSnapshot(const QString &path);
    Q_INVOKABLE void startExport(const QString &format, const QString &path);
    Q_INVOKABLE void cancelExport();
    enum { NumberRole = Qt::UserRole + 1 };
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    QString source() const { return m_source; }
    QString slideSource() const;
    QString slideText() const;
    QString speakerNotes() const;
    bool selectionHidden() const;
    Q_INVOKABLE bool isHidden(int index) const;
    // The slide a presentation reaches from `from` after `steps` shown slides, skipping hidden ones.
    Q_INVOKABLE int shownSlide(int from, int steps) const;
    // How many build steps the slide reveals, one at a time, while presenting.
    Q_INVOKABLE int stepCount(int index) const;
    // Exports leave hidden slides out unless asked to include them.
    void setIncludeHidden(bool include) { m_includeHidden = include; }
    // A PDF gives each build step a page of its own, as beamer does, instead of the finished slide.
    void setExpandSteps(bool expand) { m_expandSteps = expand; }
    QVector<int> exportedSlides() const;
    int selected() const { return m_selected; }
    int selectionFirst() const { return qMin(m_anchor, m_selected); }
    int selectionLast() const { return qMax(m_anchor, m_selected); }
    int selectionCount() const { return selectionLast() - selectionFirst() + 1; }
    int count() const { return m_parsed.slides.size(); }
    int revision() const { return m_revision; }
    bool dirty() const { return m_source != m_saved; }
    QString path() const { return m_path; }
    QString title() const;
    qint64 totalBytes() const;
    QString sizeLabel() const;
    QString status() const { return m_status; }
    QStringList fontNames() const;
    QString fontName() const;
    Q_INVOKABLE void chooseFont(const QString &family);
    QStringList themeNames() const;
    QString themeName() const;
    QColor background() const;
    QColor foreground() const;
    QColor accent() const;
    QVariantMap palette() const;
    // The palette plus, under a running header, where this slide sits in the deck.
    QVariantMap slidePalette(int index) const;
    QVariantMap media() const;
    QStringList mediaPositions() const;
    QString baseDir() const;
    QString dialogDirectory() const;
    QString slide(int index) const;
    bool loadPath(const QString &path, bool remember = true);
    bool reopenLastPresentation();
    bool savePath(const QString &path);
    bool saveCopyPath(const QString &path);
    void enableAutosave(const QString &recoveryDirectory = {});
    Q_INVOKABLE bool flushAutosave();
    Q_INVOKABLE QVariantList recoveryVersions() const;
    Q_INVOKABLE bool restoreVersion(const QString &name);
    bool exportPdf(const QString &path);
    bool exportPptx(const QString &path);
    // For PowerPoint, animations become videos and opaque slides are stored as JPEG.
    bool renderImages(const QString &directory, int width = 1920, bool powerPoint = false);
    Q_INVOKABLE void select(int index);
    Q_INVOKABLE void extendSelection(int index);
    Q_INVOKABLE void moveSelection(int direction);
    Q_INVOKABLE void dropSelection(int slot);
    Q_INVOKABLE void selectAt(int position);
    Q_INVOKABLE int sourcePosition() const;
    Q_INVOKABLE int sourceEndPosition() const;
    Q_INVOKABLE void editSource(const QString &value);
    Q_INVOKABLE void editSlide(const QString &value);
    Q_INVOKABLE void moveSlide(int from, int to);
    Q_INVOKABLE void addSlide();
    Q_INVOKABLE void duplicateSlide();
    Q_INVOKABLE void deleteSlide();
    Q_INVOKABLE void toggleHidden();
    // The label of the selected slide's SVG under a point, in 1920×1080 slide units: its text
    // and where and how large it is drawn, for an editor laid over it. Empty if none is there.
    Q_INVOKABLE QVariantMap svgLabelAt(double x, double y) const;
    // Retypes that label in the SVG file, if the file still holds it there. Undo restores the file.
    Q_INVOKABLE bool editSvgLabel(const QString &file, int start, int end, const QString &raw,
                                  const QString &text);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void chooseTheme(const QString &name);
    Q_INVOKABLE void openDialog();
    Q_INVOKABLE void save();
    Q_INVOKABLE void saveAs();
    Q_INVOKABLE void newDeck();
    Q_INVOKABLE void importDialog();
    Q_INVOKABLE bool importMedia(const QUrl &url, bool newSlide = false);
    Q_INVOKABLE bool pasteMedia();
    Q_INVOKABLE QString savePastedMedia(const QString &name);
    Q_INVOKABLE void cancelPaste();
    Q_INVOKABLE void exportDialog(const QString &format);
    // A negative step is the finished slide.
    Q_INVOKABLE QString renderId(int index, int step = -1) const;
    Q_INVOKABLE void matchImageBackground(bool enabled);
    Q_INVOKABLE void setMediaBackground(const QString &mode);
    Q_INVOKABLE void setMediaMode(const QString &mode);
    Q_INVOKABLE void setMediaPosition(const QString &position);
    Q_INVOKABLE void setMediaSize(double percent);
    Q_INVOKABLE void setStatus(const QString &status);
  signals:
    void changed(); // Document content, path, saved state, or media files; always followed by selectionChanged.
    void selectionChanged(); // Selection range or the selected slide's content/media.
    void fontChanged();
    void statusChanged();
    void opened(bool existing);
    void compressingImageChanged();
    void exportChanged();
    void exportAdvanced(double progress, const QString &message);
    void exportFinished(bool success);
    void pasteRequested(const QString &name, const QString &extension, bool video);

  private:
    struct State {
        QString source;
        int selected;
        int anchor;
        ParsedDeck parsed;
        // A step that changed a picture's file rather than the presentation.
        QString file;
        QByteArray before, after;
    };
    mutable QString m_paletteHeader, m_mediaSource, m_mediaBase;
    mutable QString m_sizeSource, m_sizeBase;
    mutable qint64 m_totalBytes = -1, m_assetBytes = 0;
    mutable QStringList m_sizeFiles;
    mutable QElapsedTimer m_sizeClock;
    struct RenderIdentity {
        QString source, base, id;
        QVariantMap palette;
        QElapsedTimer clock;
    };
    mutable QHash<int, RenderIdentity> m_renderIds;
    mutable QVariantMap m_mediaCache;
    mutable QVariantMap m_paletteCache;
    QString m_source, m_saved, m_path, m_status;
    ParsedDeck m_parsed;
    int m_selected = 0, m_anchor = 0, m_revision = 0;
    QVector<State> m_undo, m_redo;
    QMap<QString, QString> m_themes;
    QFileSystemWatcher m_watcher, m_mediaWatcher;
    QTimer m_reloadTimer, m_mediaTimer;
    QString m_notifiedFont;
    bool m_externalChange = false;
    bool m_includeHidden = false, m_expandSteps = false;
    QString m_recoveryDirectory, m_checkpointSource, m_checkpointPath;
    QTimer m_autosaveTimer, m_autosaveDeadline;
    bool m_recovering = false;
    bool m_compressingImage = false;
    quint64 m_pasteGeneration = 0;
    bool m_exporting = false, m_exportFailed = false;
    double m_exportProgress = 0;
    QString m_exportStatus;
    QString m_exportProgram;
    QProcess *m_exportProcess = nullptr;
    bool m_exportCancelled = false;
    struct PendingPaste {
        QString source, extension, path, document;
        QByteArray data;
        bool video = false;
        int selected = 0;
    } m_paste;
    void apply(const QString &source, int selected, bool history = true, int anchor = -1,
               const ParsedDeck *structure = nullptr);
    void replaceHeader(const QString &header);
    void replaceSlides(const QStringList &slides, int selected, int anchor = -1);
    bool confirmDiscard();
    bool validateStructure(const QString &operation);
    bool checkpoint();
    void recoverDraft();
    void retireDraft();
    QString recoveryFolder() const;
    bool restoreSnapshot(const QByteArray &bytes, bool opening);
    void discoverThemes();
    void watch();
    void watchMedia();
    void notifyChanged();
    void reloadExternal(const QString &disk);
    State snapshot() const { return {m_source, m_selected, m_anchor, m_parsed, {}, {}, {}}; }
    QString editableSvg() const;
    bool writeMedia(const QString &path, const QByteArray &bytes);
};
