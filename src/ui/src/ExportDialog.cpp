#include "imageeditor/ui/ExportDialog.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/ui/ToolOptionsButton.hpp"
#include "imageeditor/ui/ToolOptionsNumber.hpp"
#include "imageeditor/ui/PopupOwnership.hpp"
#include "imageeditor/ui/PdfExportPanel.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyleOptionButton>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>
#include <cmath>

namespace imageeditor::ui {
namespace {
    class ExportCombo final : public QComboBox {
    public:
        explicit ExportCombo(QWidget* parent)
            : QComboBox(parent)
        {
            setItemDelegate(new QStyledItemDelegate(this));
        }

    protected:
        void wheelEvent(QWheelEvent* event) override { event->ignore(); }
    };

    class MatteButton final : public QPushButton {
    public:
        explicit MatteButton(QWidget* parent)
            : QPushButton(parent)
        {
            setAutoDefault(false);
            setMinimumHeight(32);
        }
        QColor color { Qt::white };

    protected:
        void paintEvent(QPaintEvent*) override
        {
            QPainter p(this);
            QStyleOptionButton option;
            initStyleOption(&option);
            option.text.clear();
            style()->drawControl(QStyle::CE_PushButton, &option, &p, this);
            p.setRenderHint(QPainter::Antialiasing);
            p.setBrush(color);
            p.setPen(palette().color(QPalette::Mid));
            p.drawRoundedRect(QRectF(8, 7, 30, height() - 14), 3, 3);
            p.setPen(
                palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled, QPalette::ButtonText));
            p.drawText(
                rect().adjusted(48, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, color.name().toUpper());
        }
    };

    class PreviewSurface final : public QWidget {
    public:
        explicit PreviewSurface(QWidget* parent)
            : QWidget(parent)
        {
            setObjectName(QStringLiteral("ExportPreviewImage"));
            setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            setAttribute(Qt::WA_OpaquePaintEvent);
        }
        QImage decoded;
        bool fit { true };

    protected:
        void paintEvent(QPaintEvent* event) override
        {
            QPainter p(this);
            p.fillRect(event->rect(), palette().color(QPalette::Base));
            if (decoded.isNull()) {
                p.setPen(palette().color(QPalette::PlaceholderText));
                p.drawText(rect().adjusted(20, 20, -20, -20), Qt::AlignCenter | Qt::TextWordWrap,
                    tr("The encoded image preview will appear here."));
                return;
            }
            const qreal dpr = devicePixelRatioF();
            QSizeF output(decoded.size());
            output /= dpr;
            if (fit) {
                const qreal factor = std::min({ 1.0, width() / output.width(), height() / output.height() });
                output *= factor;
            }
            const QRectF target((width() - output.width()) / 2, (height() - output.height()) / 2,
                output.width(), output.height());
            // A display-only brush. It is never part of decoded or passed to a codec.
            if (checker_.isNull() || checkerLight_ != themeColor(ThemeColor::CheckerLight)
                || checkerDark_ != themeColor(ThemeColor::CheckerDark)) {
                checkerLight_ = themeColor(ThemeColor::CheckerLight);
                checkerDark_ = themeColor(ThemeColor::CheckerDark);
                checker_ = QPixmap(20, 20);
                checker_.fill(checkerLight_);
                QPainter tile(&checker_);
                tile.fillRect(0, 0, 10, 10, checkerDark_);
                tile.fillRect(10, 10, 10, 10, checkerDark_);
            }
            p.fillRect(target, QBrush(checker_));
            p.setRenderHint(QPainter::SmoothPixmapTransform, fit);
            p.drawImage(target, decoded);
        }

    private:
        QPixmap checker_;
        QColor checkerLight_, checkerDark_;
    };

    class ExportPreview final : public QScrollArea {
    public:
        explicit ExportPreview(QWidget* parent)
            : QScrollArea(parent)
            , surface_(new PreviewSurface(this))
        {
            setObjectName(QStringLiteral("ExportPreviewScroll"));
            setFrameShape(QFrame::NoFrame);
            setAlignment(Qt::AlignCenter);
            setWidgetResizable(false);
            setWidget(surface_);
            setMinimumSize(200, 180);
            verticalScrollBar()->setProperty("editorScrollBar", true);
            horizontalScrollBar()->setProperty("editorScrollBar", true);
        }
        void setImage(const QImage& image)
        {
            surface_->decoded = image;
            updateSurface();
        }
        void setFit(bool fit)
        {
            surface_->fit = fit;
            updateSurface();
        }

    protected:
        void resizeEvent(QResizeEvent* event) override
        {
            QScrollArea::resizeEvent(event);
            updateSurface();
        }

    private:
        void updateSurface()
        {
            const auto& image = surface_->decoded;
            if (surface_->fit || image.isNull()) {
                setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
                setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
                surface_->resize(viewport()->size());
            } else {
                setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
                setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
                const auto dpr = surface_->devicePixelRatioF();
                surface_->resize(int(std::ceil(image.width() / dpr)), int(std::ceil(image.height() / dpr)));
            }
            surface_->update();
        }
        PreviewSurface* surface_;
    };

    QString imageFilter(ExportFormat format)
    {
        switch (format) {
        case ExportFormat::Png:
            return QStringLiteral("PNG image (*.png)");
        case ExportFormat::Jpeg:
            return QStringLiteral("JPEG image (*.jpg *.jpeg)");
        case ExportFormat::WebP:
            return QStringLiteral("WebP image (*.webp)");
        case ExportFormat::Pdf:
            return QStringLiteral("PDF document (*.pdf)");
        }
        return { };
    }
    QString bytesLabel(qint64 bytes)
    {
        return QLocale().formattedDataSize(bytes)
            + QStringLiteral(" (%1 bytes)").arg(QLocale().toString(bytes));
    }
} // namespace

struct ExportDialog::Impl {
    ExportDialog& owner;
    ExportSettings draft;
    QSize canvas;
    double ratio { 1 };
    QWidget* controls { nullptr };
    ExportCombo* format { nullptr };
    QLineEdit* destination { nullptr };
    QLabel* proposedPath { nullptr };
    QLabel* dimensionsHint { nullptr };
    ToolOptionsNumber* width { nullptr };
    ToolOptionsNumber* height { nullptr };
    ToolOptionsNumber* scale { nullptr };
    ToolOptionsButton* aspect { nullptr };
    QStackedWidget* options { nullptr };
    PdfExportPanel* pdf {nullptr};
    QList<QWidget*> sizeControls;
    QLabel *previewTitle {nullptr}, *previewHint {nullptr};
    int pdfPages {0};
    QCheckBox* pngFlatten { nullptr };
    MatteButton* pngColor { nullptr };
    MatteButton* jpegColor { nullptr };
    ExportCombo* webpMode { nullptr };
    CompactValueControl* pngEffort { nullptr };
    CompactValueControl* jpegQuality { nullptr };
    CompactValueControl* webpQuality { nullptr };
    CompactValueControl* webpEffort { nullptr };
    ExportPreview* preview { nullptr };
    QLabel* status { nullptr };
    QPushButton* exportButton { nullptr };
    QPushButton* cancelButton { nullptr };
    QPushButton* location { nullptr };
    QPushButton* resetButton { nullptr };
    QString exportedPath;
    QWidget* successOverlay { nullptr };
    QList<QPointer<QWidget>> successDisabled;
    bool updating { false }, writing { false };
    bool pendingSizeEdit { false };

    Impl(ExportDialog& dialog, ExportSettings initial, QSize original)
        : owner(dialog)
        , draft(std::move(initial))
        , canvas(original)
    {
        if (!canvas.isValid())
            canvas = QSize(1, 1);
        if (!draft.size.isValid())
            draft.size = canvas;
        if (!draft.pngMatteColor.isValid())
            draft.pngMatteColor = Qt::white;
        if (!draft.jpegMatteColor.isValid())
            draft.jpegMatteColor = Qt::white;
        draft.pngMatteColor.setAlpha(255);
        draft.jpegMatteColor.setAlpha(255);
        ratio = double(draft.size.width()) / draft.size.height();
    }
    CompactValueControl* number(QWidget* parent, const char* name, const QString& label, double minimum,
        double maximum, double initial, int decimals = 0)
    {
        auto* field = new CompactValueControl(parent);
        field->setObjectName(QString::fromLatin1(name));
        field->setAccessibleName(label);
        field->setPrefix(label + QStringLiteral(": "));
        field->setDecimals(decimals);
        field->setRange(minimum, maximum);
        field->setSingleStep(1);
        field->setValue(initial);
        field->setMinimumHeight(32);
        return field;
    }
    ToolOptionsNumber* editableNumber(QWidget* parent, const char* name, const QString& label, double minimum,
        double maximum, double initial, int decimals = 0)
    {
        // Same direct-entry/hold-to-step control as New Document. Keyboard
        // tracking stays off: incomplete typing must not round or rewrite the
        // input, resize its paired dimension, or launch expensive previews.
        auto* field = new ToolOptionsNumber(parent);
        field->setObjectName(QString::fromLatin1(name));
        field->setAccessibleName(label);
        field->setPrefix(label + QStringLiteral(": "));
        field->setDecimals(decimals);
        field->setRange(minimum, maximum);
        field->setValue(initial);
        field->setFixedHeight(32);
        return field;
    }
    void notify()
    {
        if (updating)
            return;
        exportButton->setEnabled(false);
        exportedPath.clear();
        location->hide();
        status->setForegroundRole(QPalette::Text);
        status->setText(owner.tr("Updating encoded preview…"));
        updatePath();
        if (owner.onSettingsChanged)
            owner.onSettingsChanged();
    }
    void updatePath()
    {
        const auto proposed = exportPathForFormat(draft.destination, draft.format);
        const auto text = proposed.isEmpty() ? owner.tr("Choose a destination for the exported image.")
                                             : owner.tr("Output: %1").arg(QDir::toNativeSeparators(proposed));
        proposedPath->setText(text);
        proposedPath->setToolTip(text);
    }
    void updateDimensions()
    {
        const QSignalBlocker widthBlock(width), heightBlock(height), scaleBlock(scale);
        width->setValue(draft.size.width());
        height->setValue(draft.size.height());
        scale->setValue(100.0 * draft.size.width() / canvas.width());
        const double x = double(draft.size.width()) / canvas.width();
        const double y = double(draft.size.height()) / canvas.height();
        dimensionsHint->setText(owner.tr("Original: %1 × %2 px%3")
                .arg(canvas.width())
                .arg(canvas.height())
                .arg(std::abs(x - y) > .001 ? owner.tr(" · custom proportions") : QString()));
    }
    void updateFormat()
    {
        const bool isPdf=draft.format==ExportFormat::Pdf;
        options->setCurrentIndex(int(draft.format));
        for(auto* control:sizeControls)control->setVisible(!isPdf);
        pdf->setPdfVisible(isPdf);
        previewTitle->setText(isPdf?owner.tr("Page preview"):owner.tr("Encoded preview"));
        previewHint->setText(isPdf?owner.tr("Canonical page preview · checkerboard is never exported. PDF viewers may rasterize text edges differently.")
            :owner.tr("Decoded from the encoded file · checkerboard is preview-only."));
    }
    void setBusy(bool isWriting, bool previewReady = false)
    {
        writing = isWriting;
        cancelButton->setVisible(writing);
        controls->setEnabled(!writing);
        resetButton->setEnabled(!writing);
        exportButton->setEnabled(!writing && previewReady && !pendingSizeEdit);
        preview->setEnabled(true);
    }
    void resetSettings()
    {
        ExportSettings defaults;
        defaults.format = draft.format;
        defaults.destination = draft.destination;
        defaults.size = canvas;
        draft = defaults;
        pendingSizeEdit = false;
        ratio = double(canvas.width()) / canvas.height();
        updateDimensions();
        const auto setValue = [](QDoubleSpinBox* field, double value) {
            const QSignalBlocker block(field);
            field->setValue(value);
        };
        setValue(pngEffort, draft.pngEffort);
        setValue(jpegQuality, draft.jpegQuality);
        setValue(webpQuality, draft.webpQuality);
        setValue(webpEffort, draft.webpEffort);
        {
            const QSignalBlocker aspectBlock(aspect), pngBlock(pngFlatten), webpBlock(webpMode);
            aspect->setChecked(draft.aspectLocked);
            pngFlatten->setChecked(draft.pngMatte);
            webpMode->setCurrentIndex(draft.webpLossless ? 1 : 0);
        }
        pngColor->color = draft.pngMatteColor;
        jpegColor->color = draft.jpegMatteColor;
        pngColor->setEnabled(draft.pngMatte);
        webpQuality->setEnabled(!draft.webpLossless);
        pngColor->update();
        jpegColor->update();
        pdf->reset();
        notify(); // One coherent update, not one encoder request per control.
    }
    void chooseMatte(bool png)
    {
        QWidget* host = &owner;
        while (host->parentWidget())
            host = host->parentWidget();
        QColorDialog picker(png ? draft.pngMatteColor : draft.jpegMatteColor, host);
        picker.setObjectName(QStringLiteral("ExportMatteDialog"));
        picker.setWindowTitle(owner.tr("Export background color"));
        picker.setOption(QColorDialog::DontUseNativeDialog);
        picker.setWindowFlag(Qt::Tool);
        picker.setWindowModality(Qt::ApplicationModal);
        (void)host->winId();
        (void)picker.winId();
        picker.windowHandle()->setTransientParent(host->windowHandle());
        if (picker.exec() != QDialog::Accepted || !picker.selectedColor().isValid())
            return;
        auto color = picker.selectedColor();
        color.setAlpha(255);
        (png ? draft.pngMatteColor : draft.jpegMatteColor) = color;
        auto* button = png ? pngColor : jpegColor;
        button->color = color;
        button->update();
        notify();
    }
};

ExportDialog::ExportDialog(ExportSettings initial, QSize canvasSize, QWidget* parent)
    : QDialog(parent, parent ? Qt::SubWindow : Qt::Dialog)
    , impl_(std::make_unique<Impl>(*this, std::move(initial), canvasSize))
{
    auto& d = *impl_;
    setObjectName(QStringLiteral("ExportDialog"));
    setWindowTitle(tr("Export"));
    if (parent)
        setAttribute(Qt::WA_ShowWithoutActivating);
    setSizeGripEnabled(false);
    setFixedSize(1040, 680);
    setProperty("workspacePreferredSize", QSize(1040, 680));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(22, 18, 22, 18);
    root->setSpacing(12);
    auto* header = new QHBoxLayout;
    auto* icon = new QLabel(this);
    icon->setPixmap(QIcon::fromTheme(QStringLiteral("document-export"),
        toolGlyph(ToolGlyph::ExternalLink, themeColor(ThemeColor::Accent)))
        .pixmap(QSize(20, 20), devicePixelRatioF()));
    header->addWidget(icon);
    auto* title = new QLabel(tr("Export"), this);
    title->setObjectName(QStringLiteral("ToolTitle"));
    header->addWidget(title);
    header->addStretch();
    auto* close = new QPushButton(toolGlyph(ToolGlyph::Close, themeColor(ThemeColor::SecondaryText)), {}, this);
    close->setObjectName(QStringLiteral("ExportHeaderClose"));
    close->setAccessibleName(tr("Close Export"));
    close->setToolTip(tr("Close"));
    close->setAutoDefault(false);
    close->setFixedSize(30, 30);
    close->setIconSize({18, 18});
    close->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: none; padding: 4px; }"
        "QPushButton:hover { background: %1; }").arg(themeColor(ThemeColor::Surface).name()));
    connect(close, &QPushButton::clicked, this, &ExportDialog::reject);
    header->addWidget(close);
    root->addLayout(header);
    d.controls = new QWidget(this);
    d.controls->setObjectName(QStringLiteral("ExportControls"));
    auto* content = new QVBoxLayout(d.controls);
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(10);
    auto* fileRow = new QHBoxLayout;
    fileRow->setSpacing(8);
    d.format = new ExportCombo(d.controls);
    d.format->setObjectName(QStringLiteral("ExportFormat"));
    d.format->setAccessibleName(tr("Export format"));
    d.format->addItems({ tr("PNG"), tr("JPEG"), tr("WebP"), tr("PDF") });
    d.format->setCurrentIndex(int(d.draft.format));
    d.format->setMinimumWidth(115);
    fileRow->addWidget(d.format);
    d.destination = new QLineEdit(d.draft.destination, d.controls);
    d.destination->setObjectName(QStringLiteral("ExportDestination"));
    d.destination->setAccessibleName(tr("Destination"));
    d.destination->setPlaceholderText(tr("Choose an export destination…"));
    d.destination->setMinimumWidth(0);
    fileRow->addWidget(d.destination, 1);
    auto* browse = new QPushButton(tr("Browse…"), d.controls);
    browse->setObjectName(QStringLiteral("ExportBrowse"));
    browse->setAutoDefault(false);
    fileRow->addWidget(browse);
    content->addLayout(fileRow);
    d.proposedPath = new QLabel(d.controls);
    d.proposedPath->setObjectName(QStringLiteral("ExportResolvedPath"));
    d.proposedPath->setTextFormat(Qt::PlainText);
    d.proposedPath->setForegroundRole(QPalette::PlaceholderText);
    d.proposedPath->setWordWrap(true);
    d.proposedPath->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    d.proposedPath->setMaximumHeight(d.proposedPath->fontMetrics().lineSpacing() * 2 + 2);
    d.proposedPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    content->addWidget(d.proposedPath);
    auto* body = new QHBoxLayout;
    body->setSpacing(18);
    content->addLayout(body, 1);
    auto* settingsScroll = new QScrollArea(d.controls);
    settingsScroll->setObjectName(QStringLiteral("ExportSettingsScroll"));
    settingsScroll->setFrameShape(QFrame::NoFrame);
    settingsScroll->setWidgetResizable(true);
    settingsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    settingsScroll->verticalScrollBar()->setProperty("editorScrollBar", true);
    settingsScroll->setMinimumWidth(265);
    settingsScroll->setMaximumWidth(325);
    body->addWidget(settingsScroll, 1);
    auto* settings = new QWidget(settingsScroll);
    auto* form = new QVBoxLayout(settings);
    form->setContentsMargins(0, 0, 8, 0);
    form->setSpacing(10);
    settingsScroll->setWidget(settings);
    auto* sizeTitle = new QLabel(tr("Output size"), settings);
    sizeTitle->setObjectName(QStringLiteral("SectionLabel"));
    form->addWidget(sizeTitle);
    d.width = d.editableNumber(
        settings, "ExportWidth", tr("Width"), 1, kMaximumExportDimension, d.draft.size.width());
    d.height = d.editableNumber(
        settings, "ExportHeight", tr("Height"), 1, kMaximumExportDimension, d.draft.size.height());
    d.width->setSuffix(tr(" px"));
    d.height->setSuffix(tr(" px"));
    for (auto* field : { d.width, d.height })
        field->setToolTip(tr("Type an exact pixel dimension. Press Enter or leave the field to apply."));
    form->addWidget(d.width);
    form->addWidget(d.height);
    auto* scaleRow = new QHBoxLayout;
    scaleRow->setSpacing(8);
    d.scale = d.editableNumber(settings, "ExportScale", tr("Scale"), .01,
        100.0 * kMaximumExportDimension / std::max(d.canvas.width(), d.canvas.height()), 100, 2);
    d.scale->setSuffix(QStringLiteral("%"));
    d.scale->setToolTip(tr("Type a percentage and press Enter or leave the field to apply. "
                           "Scale both original document dimensions together. "
                           "Unlock aspect and edit Width/Height for custom proportions."));
    scaleRow->addWidget(d.scale, 1);
    d.aspect = new ToolOptionsButton(QStringLiteral("ExportAspectLock"), tr("Lock aspect ratio"),
        tr("Preserve the current width-to-height "
           "ratio when editing either dimension"),
        ToolOptionsButton::Kind::Toggle, settings, ToolOptionsButton::Presentation::Icon);
    d.aspect->setIcon(toolGlyph(ToolGlyph::AspectLock));
    d.aspect->setChecked(d.draft.aspectLocked);
    scaleRow->addWidget(d.aspect);
    form->addLayout(scaleRow);
    d.dimensionsHint = new QLabel(settings);
    d.dimensionsHint->setObjectName(QStringLiteral("ExportDimensionsHint"));
    d.dimensionsHint->setForegroundRole(QPalette::PlaceholderText);
    d.dimensionsHint->setWordWrap(true);
    form->addWidget(d.dimensionsHint);
    d.sizeControls={sizeTitle,d.width,d.height,d.scale,d.aspect,d.dimensionsHint};
    auto* optionsTitle = new QLabel(tr("Format options"), settings);
    optionsTitle->setObjectName(QStringLiteral("SectionLabel"));
    form->addWidget(optionsTitle);
    d.options = new QStackedWidget(settings);
    d.options->setObjectName(QStringLiteral("ExportFormatOptions"));
    form->addWidget(d.options);
    const auto page = [&] {
        auto* widget = new QWidget(d.options);
        auto* layout = new QVBoxLayout(widget);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(10);
        d.options->addWidget(widget);
        return std::pair(widget, layout);
    };
    const auto help = [&](QVBoxLayout* layout, const QString& text) {
        auto* label = new QLabel(text, settings);
        label->setWordWrap(true);
        label->setObjectName(QStringLiteral("MutedLabel"));
        layout->addWidget(label);
    };
    const auto [png, pngLayout] = page();
    d.pngEffort = d.number(png, "ExportPngEffort", tr("Compression effort"), 0, 9, d.draft.pngEffort);
    pngLayout->addWidget(d.pngEffort);
    d.pngFlatten = new QCheckBox(tr("Flatten transparency over a color"), png);
    d.pngFlatten->setObjectName(QStringLiteral("ExportPngMatte"));
    d.pngFlatten->setChecked(d.draft.pngMatte);
    pngLayout->addWidget(d.pngFlatten);
    d.pngColor = new MatteButton(png);
    d.pngColor->setObjectName(QStringLiteral("ExportPngMatteColor"));
    d.pngColor->setAccessibleName(tr("PNG background color"));
    d.pngColor->color = d.draft.pngMatteColor;
    d.pngColor->setEnabled(d.draft.pngMatte);
    pngLayout->addWidget(d.pngColor);
    help(pngLayout,
        tr("Lossless RGB/RGBA · transparency is preserved unless "
           "flattened. Compression changes encoding "
           "effort and file size, never visual quality."));
    pngLayout->addStretch();
    const auto [jpeg, jpegLayout] = page();
    d.jpegQuality = d.number(jpeg, "ExportJpegQuality", tr("Quality"), 1, 100, d.draft.jpegQuality);
    jpegLayout->addWidget(d.jpegQuality);
    d.jpegColor = new MatteButton(jpeg);
    d.jpegColor->setObjectName(QStringLiteral("ExportJpegMatteColor"));
    d.jpegColor->setAccessibleName(tr("JPEG matte color"));
    d.jpegColor->color = d.draft.jpegMatteColor;
    jpegLayout->addWidget(d.jpegColor);
    help(jpegLayout,
        tr("JPEG has no transparency. The finished document is "
           "flattened over this color. Inspect colored "
           "edges at 100%. Quality 91–100 uses 4:4:4 color; lower "
           "values use 4:2:0. Maximum quality is still lossy."));
    jpegLayout->addStretch();
    const auto [webp, webpLayout] = page();
    d.webpMode = new ExportCombo(webp);
    d.webpMode->setObjectName(QStringLiteral("ExportWebpMode"));
    d.webpMode->setAccessibleName(tr("WebP compression mode"));
    d.webpMode->addItems({ tr("Lossy"), tr("Lossless") });
    d.webpMode->setCurrentIndex(d.draft.webpLossless ? 1 : 0);
    webpLayout->addWidget(d.webpMode);
    d.webpQuality = d.number(webp, "ExportWebpQuality", tr("Quality"), 0, 100, d.draft.webpQuality);
    d.webpQuality->setEnabled(!d.draft.webpLossless);
    webpLayout->addWidget(d.webpQuality);
    d.webpEffort = d.number(webp, "ExportWebpEffort", tr("Encoding effort"), 0, 6, d.draft.webpEffort);
    webpLayout->addWidget(d.webpEffort);
    help(webpLayout,
        tr("Lossless is an explicit encoding mode, not Quality 100. "
           "Alpha is encoded losslessly. Maximum "
           "output: 16383 × 16383 px."));
    webpLayout->addStretch();
    d.pdf=new PdfExportPanel(d.draft.pdf,d.options,d.controls);
    d.options->addWidget(d.pdf);
    d.options->setCurrentIndex(int(d.draft.format));
    form->addStretch();
    auto* previewColumn = new QVBoxLayout;
    previewColumn->setSpacing(8);
    body->addLayout(previewColumn, 2);
    previewColumn->addWidget(d.pdf->itemsWidget());
    auto* previewHeader = new QHBoxLayout;
    auto* previewTitle = new QLabel(tr("Encoded preview"), d.controls);
    previewTitle->setObjectName(QStringLiteral("SectionLabel"));
    d.previewTitle=previewTitle;
    previewHeader->addWidget(previewTitle);
    previewHeader->addStretch();
    auto* fit = new ToolOptionsButton(QStringLiteral("ExportPreviewFit"), tr("Fit"),
        tr("Fit the encoded result in the preview without enlarging it"), ToolOptionsButton::Kind::Toggle,
        d.controls);
    auto* actual = new ToolOptionsButton(QStringLiteral("ExportPreview100"), QStringLiteral("100%"),
        tr("One output pixel per display pixel; use the scroll bars to inspect "
           "larger images"),
        ToolOptionsButton::Kind::Toggle, d.controls);
    auto* zoom = new QButtonGroup(this);
    zoom->addButton(fit);
    zoom->addButton(actual);
    fit->setChecked(true);
    previewHeader->addWidget(fit);
    previewHeader->addWidget(actual);
    previewColumn->addLayout(previewHeader);
    d.preview = new ExportPreview(d.controls);
    previewColumn->addWidget(d.preview, 1);
    previewColumn->addWidget(d.pdf->navigationWidget());
    auto* previewHint
        = new QLabel(tr("Decoded from the encoded file · checkerboard is preview-only."), d.controls);
    previewHint->setObjectName(QStringLiteral("MutedLabel"));
    d.previewHint=previewHint;
    previewHint->setWordWrap(true);
    previewColumn->addWidget(previewHint);
    root->addWidget(d.controls, 1);
    d.status = new QLabel(tr("Preparing export preview…"), this);
    d.status->setObjectName(QStringLiteral("ExportStatus"));
    d.status->setTextFormat(Qt::PlainText);
    d.status->setWordWrap(true);
    d.status->setAlignment(Qt::AlignCenter);
    d.status->setMinimumHeight(36);
    d.status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(d.status);
    auto* buttons = new QHBoxLayout;
    d.resetButton = new QPushButton(tr("Reset Settings"), this);
    d.resetButton->setObjectName(QStringLiteral("ExportReset"));
    d.resetButton->setAutoDefault(false);
    d.resetButton->setToolTip(tr("Restore original dimensions (100%), aspect lock, and default encoding "
                                 "options and background colors. Keep the current format and filename."));
    buttons->addWidget(d.resetButton);
    d.location = new QPushButton(tr("Open location"), this);
    d.location->setObjectName(QStringLiteral("ExportOpenLocation"));
    d.location->setAutoDefault(false);
    d.location->hide();
    buttons->addWidget(d.location);
    buttons->addStretch();
    d.cancelButton = new QPushButton(tr("Cancel"), this);
    d.cancelButton->setObjectName(QStringLiteral("ExportCancel"));
    d.cancelButton->setAutoDefault(false);
    d.cancelButton->setToolTip(tr("Cancel this export and keep the export panel open."));
    d.cancelButton->hide();
    d.exportButton = new QPushButton(tr("Export"), this);
    d.exportButton->setObjectName(QStringLiteral("ExportWrite"));
    d.exportButton->setAutoDefault(false);
    d.exportButton->setEnabled(false);
    buttons->addWidget(d.cancelButton);
    buttons->addWidget(d.exportButton);
    root->addLayout(buttons);
    d.pdf->changed=[this]{impl_->notify();};
    d.pdf->pageChanged=[this]{if(onPdfPageChanged)onPdfPageChanged();};
    d.pdf->thumbnailsChanged=[this]{if(onPdfThumbnailsRequested)onPdfThumbnailsRequested();};
    d.updateFormat();

    connect(d.resetButton, &QPushButton::clicked, this, [this] { impl_->resetSettings(); });
    for (auto* field : { d.width, d.height, d.scale }) {
        field->installEventFilter(this);
        field->findChild<QLineEdit*>()->installEventFilter(this);
        connect(field->findChild<QLineEdit*>(), &QLineEdit::textEdited, this, [this] {
            impl_->pendingSizeEdit = true;
            impl_->exportButton->setEnabled(false);
        });
        connect(field, &QDoubleSpinBox::editingFinished, this, [this] {
            // valueChanged handles real changes. A same-value or corrected
            // input still needs to release the pending-input readiness guard.
            if (impl_->pendingSizeEdit) {
                impl_->pendingSizeEdit = false;
                impl_->notify();
            }
        });
    }
    connect(d.destination, &QLineEdit::textChanged, this, [this](const QString& path) {
        impl_->draft.destination = path;
        impl_->notify();
    });
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getSaveFileName(popupTopLevelOwner(this), tr("Export destination"),
            exportPathForFormat(impl_->draft.destination, impl_->draft.format),
            imageFilter(impl_->draft.format), nullptr, QFileDialog::DontConfirmOverwrite);
        if (!path.isEmpty())
            setDestination(path);
    });
    connect(d.format, &QComboBox::currentIndexChanged, this, [this](int index) {
        auto& state = *impl_;
        state.draft.format = ExportFormat(index);
        state.updateFormat();
        state.notify();
    });
    connect(d.width, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        auto& state = *impl_;
        state.pendingSizeEdit = false;
        state.draft.size.setWidth(int(std::lround(value)));
        if (state.draft.aspectLocked) {
            const auto paired = int(std::lround(value / state.ratio));
            state.draft.size.setHeight(std::clamp(paired, 1, int(kMaximumExportDimension)));
            if (paired > int(kMaximumExportDimension))
                state.draft.size.setWidth(int(std::lround(kMaximumExportDimension * state.ratio)));
        }
        state.updateDimensions();
        state.notify();
    });
    connect(d.height, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        auto& state = *impl_;
        state.pendingSizeEdit = false;
        state.draft.size.setHeight(int(std::lround(value)));
        if (state.draft.aspectLocked) {
            const auto paired = int(std::lround(value * state.ratio));
            state.draft.size.setWidth(std::clamp(paired, 1, int(kMaximumExportDimension)));
            if (paired > int(kMaximumExportDimension))
                state.draft.size.setHeight(int(std::lround(kMaximumExportDimension / state.ratio)));
        }
        state.updateDimensions();
        state.notify();
    });
    connect(d.aspect, &QToolButton::toggled, this, [this](bool locked) {
        auto& state = *impl_;
        state.draft.aspectLocked = locked;
        if (locked)
            state.ratio = double(state.draft.size.width()) / state.draft.size.height();
        state.notify();
    });
    connect(d.scale, &QDoubleSpinBox::valueChanged, this, [this](double percent) {
        auto& state = *impl_;
        state.pendingSizeEdit = false;
        state.draft.size = QSize(std::max(1, int(std::lround(state.canvas.width() * percent / 100))),
            std::max(1, int(std::lround(state.canvas.height() * percent / 100))));
        state.ratio = double(state.canvas.width()) / state.canvas.height();
        state.updateDimensions();
        state.notify();
    });
    connect(d.pngEffort, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        impl_->draft.pngEffort = int(value);
        impl_->notify();
    });
    connect(d.pngFlatten, &QCheckBox::toggled, this, [this](bool enabled) {
        impl_->draft.pngMatte = enabled;
        impl_->pngColor->setEnabled(enabled);
        impl_->notify();
    });
    connect(d.pngColor, &QPushButton::clicked, this, [this] { impl_->chooseMatte(true); });
    connect(d.jpegQuality, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        impl_->draft.jpegQuality = int(value);
        impl_->notify();
    });
    connect(d.jpegColor, &QPushButton::clicked, this, [this] { impl_->chooseMatte(false); });
    connect(d.webpMode, &QComboBox::currentIndexChanged, this, [this](int mode) {
        impl_->draft.webpLossless = mode == 1;
        impl_->webpQuality->setEnabled(mode == 0);
        impl_->notify();
    });
    connect(d.webpQuality, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        impl_->draft.webpQuality = int(value);
        impl_->notify();
    });
    connect(d.webpEffort, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        impl_->draft.webpEffort = int(value);
        impl_->notify();
    });
    connect(fit, &QToolButton::clicked, this, [this] { impl_->preview->setFit(true); });
    connect(actual, &QToolButton::clicked, this, [this] { impl_->preview->setFit(false); });
    connect(d.cancelButton, &QPushButton::clicked, this, [this] {
        if (impl_->writing && onCancelRequested)
            onCancelRequested();
    });
    connect(d.exportButton, &QPushButton::clicked, this, [this] {
        for (auto* field : findChildren<QDoubleSpinBox*>()) {
            if (auto* compact = dynamic_cast<CompactValueControl*>(field))
                compact->finishEditing();
            else
                field->interpretText();
        }
        if (impl_->exportButton->isEnabled() && onExportRequested)
            onExportRequested();
    });
    connect(d.location, &QPushButton::clicked, this, [this] {
        openExportedLocation(false);
    });
    d.updateDimensions();
    d.updatePath();
}

ExportDialog::~ExportDialog() = default;
bool ExportDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (impl_->pendingSizeEdit
        && (event->type() == QEvent::FocusOut
            || (event->type() == QEvent::KeyPress
                && (static_cast<QKeyEvent*>(event)->key() == Qt::Key_Return
                    || static_cast<QKeyEvent*>(event)->key() == Qt::Key_Enter)))) {
        auto* field = qobject_cast<QDoubleSpinBox*>(watched);
        if (!field)
            field = qobject_cast<QDoubleSpinBox*>(watched->parent());
        if (field) {
            field->interpretText();
            // Qt's shared stepper need not emit valueChanged/finished when
            // Enter reaffirms the same value. Release readiness explicitly,
            // including in the child-surface modal focus bridge.
            if (impl_->pendingSizeEdit) {
                impl_->pendingSizeEdit = false;
                impl_->notify();
            }
        }
    }
    return QDialog::eventFilter(watched, event);
}
void ExportDialog::reject()
{
    if (impl_->successOverlay) {
        closeSuccessOverlay();
        return;
    }
    if (onCloseRequested)
        onCloseRequested();
    else
        QDialog::reject();
}
ExportSettings ExportDialog::settings() const { return impl_->draft; }
void ExportDialog::setDestination(const QString& path) { impl_->destination->setText(path); }
void ExportDialog::setProgress(const QString& stage, bool writing)
{
    impl_->setBusy(writing);
    impl_->status->setForegroundRole(QPalette::Text);
    impl_->status->setText(stage);
}
void ExportDialog::setPreview(const QImage& decoded, qint64 actualBytes)
{
    if (const auto error = validateExport(impl_->draft, impl_->canvas); !error.isEmpty()) {
        setError(error);
        return;
    }
    if (const auto error = validateExportDestination(impl_->draft.destination); !error.isEmpty()) {
        setError(error);
        return;
    }
    const bool ready = !decoded.isNull() && decoded.size() == impl_->draft.size && actualBytes > 0;
    impl_->setBusy(false, ready);
    impl_->preview->setImage(decoded);
    impl_->status->setForegroundRole(QPalette::Text);
    impl_->status->setText(!ready ? tr("No current encoded preview is available.")
                                  : tr("%1 × %2 px · actual encoded size: %3")
                                        .arg(decoded.width())
                                        .arg(decoded.height())
                                        .arg(bytesLabel(actualBytes)));
}
void ExportDialog::setError(const QString& message)
{
    impl_->setBusy(false);
    impl_->status->setForegroundRole(QPalette::BrightText);
    impl_->status->setText(message);
}
void ExportDialog::setCancelled(bool previewReady)
{
    impl_->setBusy(false, previewReady);
    impl_->status->setForegroundRole(QPalette::Text);
    impl_->status->setText(tr("Export cancelled. You can adjust the settings and try again."));
}
void ExportDialog::configurePdf(const PdfExportSnapshot& source){impl_->pdf->setSource(source);}
void ExportDialog::setPdfPlan(const PdfExportPlan& plan){impl_->pdfPages=int(plan.pages.size());impl_->pdf->setPlan(plan);}
int ExportDialog::pdfPreviewPage()const{return impl_->pdf->previewPage();}
QString ExportDialog::pdfInputError()const{return impl_->pdf->inputError();}
std::vector<core::LayerId> ExportDialog::neededPdfThumbnails()const{return impl_->pdf->visibleThumbnails();}
void ExportDialog::setPdfThumbnail(core::LayerId id,const QImage& image){impl_->pdf->setThumbnail(id,image);}
void ExportDialog::setPdfPreview(const PdfExportPlan& plan,int page,const QImage& image)
{
    if(!plan||page<0||std::size_t(page)>=plan.pages.size()||image.isNull())return;
    const auto error=validateExportDestination(impl_->draft.destination);
    if(!error.isEmpty()){setError(error);return;}
    const auto& p=plan.pages[std::size_t(page)];
    impl_->setBusy(false,pdfInputError().isEmpty());impl_->preview->setImage(image);
    impl_->status->setForegroundRole(QPalette::Text);
    impl_->status->setText(tr("Page %1 of %2 · %3 × %4 pt · %5 × %6 px%7")
        .arg(page+1).arg(plan.pages.size()).arg(p.points.width(),0,'f',2).arg(p.points.height(),0,'f',2)
        .arg(p.pixels.width()).arg(p.pixels.height()).arg(p.blankFallback?tr(" · empty bounds: canvas-size blank page"):QString()));
}
void ExportDialog::setExported(const QString& destination, QSize size, qint64 bytes)
{
    const bool pdf=impl_->draft.format==ExportFormat::Pdf;
    impl_->setBusy(false, (pdf||size == impl_->draft.size) && bytes > 0);
    impl_->exportedPath = destination;
    impl_->status->setForegroundRole(QPalette::Text);
    impl_->status->setText(pdf?tr("Exported %1 · %2 page(s) · %3").arg(QFileInfo(destination).fileName()).arg(impl_->pdfPages).arg(bytesLabel(bytes)):tr("Exported %1 · %2 × %3 px · %4")
            .arg(QFileInfo(destination).fileName())
            .arg(size.width())
            .arg(size.height())
            .arg(bytesLabel(bytes)));
    impl_->status->setToolTip(QDir::toNativeSeparators(destination));
    impl_->location->show();

    closeSuccessOverlay();
    // This is a child widget on the existing export surface, never a native
    // dialog/window. Disable underlying controls for keyboard as well as mouse.
    for (auto* child : findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
        if (child->isEnabled() && !child->isWindow()) {
            impl_->successDisabled.append(child);
            child->setEnabled(false);
        }
    }
    auto* overlay = new QFrame(this);
    impl_->successOverlay = overlay;
    overlay->setObjectName(QStringLiteral("ExportSuccessOverlay"));
    overlay->setGeometry(rect());
    auto* outer = new QVBoxLayout(overlay);
    outer->setContentsMargins(20, 20, 20, 20);
    outer->addStretch();
    auto* row = new QHBoxLayout;
    row->addStretch();
    auto* card = new QFrame(overlay);
    card->setObjectName(QStringLiteral("ExportSuccessCard"));
    card->setMaximumWidth(480);
    auto* content = new QVBoxLayout(card);
    content->setContentsMargins(20, 16, 20, 20);
    content->setSpacing(14);
    const auto accent = themeColor(ThemeColor::Accent);
    auto veil = themeColor(ThemeColor::Background);
    veil.setAlpha(180);
    overlay->setStyleSheet(QStringLiteral(
        "QFrame#ExportSuccessOverlay { background: %1; }"
        "QFrame#ExportSuccessCard { background: %2; border: 1px solid %3; border-radius: 8px; }")
        .arg(veil.name(QColor::HexArgb), themeColor(ThemeColor::Surface).name(), accent.name()));
    auto* heading = new QHBoxLayout;
    auto* icon = new QLabel(card);
    icon->setPixmap(toolGlyph(ToolGlyph::CheckCircle, accent).pixmap(QSize(32, 32), devicePixelRatioF()));
    heading->addWidget(icon);
    auto* title = new QLabel(QStringLiteral("<b style=\"color:%1\">%2</b>")
            .arg(accent.name(), tr("Successfully exported").toHtmlEscaped()));
    heading->addWidget(title, 1);
    auto* close = new QPushButton(toolGlyph(ToolGlyph::Close), QString(), card);
    close->setObjectName(QStringLiteral("ExportSuccessClose"));
    close->setToolTip(tr("Close"));
    close->setAccessibleName(tr("Close export confirmation"));
    close->setAutoDefault(false);
    close->setFixedSize(28, 28);
    heading->addWidget(close);
    content->addLayout(heading);
    auto* detail = new QLabel(pdf?tr("%1\n%2 page(s) · %3").arg(QFileInfo(destination).fileName()).arg(impl_->pdfPages).arg(bytesLabel(bytes)):tr("%1\n%2 × %3 px · %4")
            .arg(QFileInfo(destination).fileName())
            .arg(size.width()).arg(size.height()).arg(bytesLabel(bytes)), card);
    detail->setTextFormat(Qt::PlainText);
    detail->setWordWrap(true);
    detail->setToolTip(QDir::toNativeSeparators(destination));
    content->addWidget(detail);
    auto* buttons = new QHBoxLayout;
    auto* openImage = new QPushButton(pdf?tr("Open PDF"):tr("Open image"), card);
    openImage->setObjectName(QStringLiteral("ExportSuccessOpenImage"));
    openImage->setAutoDefault(false);
    auto* browse = new QPushButton(tr("Browse location"), card);
    browse->setObjectName(QStringLiteral("ExportSuccessBrowse"));
    browse->setAutoDefault(false);
    buttons->addWidget(openImage);
    buttons->addWidget(browse);
    content->addLayout(buttons);
    row->addWidget(card, 1);
    row->addStretch();
    outer->addLayout(row);
    outer->addStretch();
    connect(close, &QPushButton::clicked, this, &ExportDialog::closeSuccessOverlay);
    connect(openImage, &QPushButton::clicked, this, [this] { openExportedLocation(true); });
    connect(browse, &QPushButton::clicked, this, [this] { openExportedLocation(false); });
    overlay->show();
    overlay->raise();
    close->setFocus(Qt::OtherFocusReason);
}

void ExportDialog::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    if (impl_->successOverlay)
        impl_->successOverlay->setGeometry(rect());
}

void ExportDialog::closeSuccessOverlay()
{
    if (!impl_->successOverlay)
        return;
    impl_->successOverlay->hide();
    impl_->successOverlay->deleteLater();
    impl_->successOverlay = nullptr;
    for (const auto& child : impl_->successDisabled)
        if (child)
            child->setEnabled(true);
    impl_->successDisabled.clear();
    impl_->exportButton->setFocus(Qt::OtherFocusReason);
}

void ExportDialog::openExportedLocation(bool image)
{
    const auto path = image ? QFileInfo(impl_->exportedPath).absoluteFilePath()
                            : QFileInfo(impl_->exportedPath).absolutePath();
    closeSuccessOverlay();
    // Restore the owning surface before asking the desktop/portal to activate
    // another application. Never use a disappearing native confirmation as
    // the activation parent. QUrl handles spaces and non-ASCII filenames.
    QTimer::singleShot(0, this, [this, path] {
        if (!QFileInfo::exists(path) || !QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
            impl_->status->setText(tr("Export succeeded. Could not open %1.")
                                      .arg(QDir::toNativeSeparators(path)));
    });
}
} // namespace imageeditor::ui
