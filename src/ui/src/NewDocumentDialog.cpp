#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/NewDocumentDialog.hpp"
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include "imageeditor/ui/RasterLimits.hpp"
#include "imageeditor/ui/RecentFiles.hpp"
#include "imageeditor/ui/ToolOptionsNumber.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <array>

namespace imageeditor::ui {
namespace {
struct CanvasPreset { const char* name; core::CanvasSpec spec; };
constexpr std::array<CanvasPreset,25> canvasPresets {{
    {"Icon · 16 × 16",{{16,16},96}}, {"Icon · 32 × 32",{{32,32},96}},
    {"Icon · 48 × 48",{{48,48},96}}, {"Icon · 64 × 64",{{64,64},96}},
    {"Icon · 128 × 128",{{128,128},96}}, {"Icon · 256 × 256",{{256,256},96}},
    {"Texture · 512 × 512",{{512,512},96}}, {"Texture · 1024 × 1024",{{1024,1024},96}},
    {"Texture · 2048 × 2048",{{2048,2048},96}}, {"Texture · 4096 × 4096",{{4096,4096},96}},
    {"HD · 1280 × 720",{{1280,720},96}}, {"Full HD · 1920 × 1080",{{1920,1080},96}},
    {"4K · 3840 × 2160",{{3840,2160},96}},
    {"Social media square · 1080 × 1080",{{1080,1080},96}},
    {"Social media story · 1080 × 1920",{{1080,1920},96}},
    {"A4 · 210 × 297 mm",{{2480,3508},300}}, {"US Letter · 8.5 × 11 in",{{2550,3300},300}},
    {"Portrait · 2 × 3 in",{{600,900},300}}, {"Portrait · 4 × 6 in",{{1200,1800},300}},
    {"Portrait · 5 × 7 in",{{1500,2100},300}}, {"Portrait · 8 × 10 in",{{2400,3000},300}},
    {"Landscape · 3 × 2 in",{{900,600},300}}, {"Landscape · 6 × 4 in",{{1800,1200},300}},
    {"Landscape · 7 × 5 in",{{2100,1500},300}}, {"Landscape · 10 × 8 in",{{3000,2400},300}}
}};
}

NewDocumentDialog::NewDocumentDialog(QWidget* parent)
    : NewDocumentDialog(
          Mode::CreateDocument, core::CanvasSpec { .extent = { 1600, 900 }, .dotsPerInch = 96.0 }, parent)
{
}

NewDocumentDialog::NewDocumentDialog(Mode mode, core::CanvasSpec initialSpec, QWidget* parent)
    : QDialog(parent, parent ? Qt::SubWindow : Qt::Dialog)
    , mode_(mode)
    , width_(new ToolOptionsNumber(this))
    , height_(new ToolOptionsNumber(this))
    , dpi_(new ToolOptionsNumber(this))
{
    const bool changingCanvas = mode_ == Mode::ChangeCanvasSize;
    setObjectName(QStringLiteral("NewDocumentDialog"));
    // This form is born on the panel plane, never converted from a toplevel:
    // reparenting an active dialog corrupts Qt's native-widget focus chain.
    if (parent) setAttribute(Qt::WA_ShowWithoutActivating);
    i18n::setWindowTitle(this, changingCanvas ? i18n::text("new.document.dialog.change.canvas.size") : i18n::text("new.document.dialog.new.document"));
    setSizeGripEnabled(false);
    setFixedWidth(changingCanvas ? 500 : 820);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(22, 20, 22, 20);
    layout->setSpacing(15);
    layout->setSizeConstraint(QLayout::SetFixedSize);
    auto* heading = i18n::make<QLabel>(changingCanvas ? i18n::text("new.document.dialog.change.the.canvas.boundary")
                                              : i18n::text("new.document.dialog.create.a.new.canvas"));
    heading->setObjectName(QStringLiteral("ToolTitle"));
    auto* help = i18n::make<QLabel>(changingCanvas
            ? i18n::text("new.document.dialog.the.top.left.document.origin.stays.fixed.layers.keep")
            : i18n::text("new.document.dialog.v1.documents.use.8.bit.rgba.in.the.srgb"));
    help->setObjectName(QStringLiteral("MutedLabel"));
    help->setWordWrap(true);
    layout->addWidget(heading);
    layout->addWidget(help);

    // Keep five readable recent rows alongside the form, rather than making
    // a tall dialog that no longer fits the supported minimum app height.
    auto* body = new QHBoxLayout;
    body->setSpacing(24);
    auto* settings = new QVBoxLayout;
    settings->setSpacing(15);
    body->addLayout(settings, 1);
    layout->addLayout(body);

    auto* form = new QFormLayout;
    form->setVerticalSpacing(12);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto* presets = new QComboBox(this);
    presets->setObjectName(QStringLiteral("CanvasPresetCombo"));
    presets->setItemDelegate(new QStyledItemDelegate(presets));
    i18n::addItem(presets, i18n::text("new.document.dialog.custom"));
    for (const auto& preset : canvasPresets) presets->addItem(QString::fromUtf8(preset.name));
    presets->setMaxVisibleItems(14);
    if (changingCanvas) {
        // Identify the existing canvas without applying a preset or rounding
        // its resolution. Creation intentionally continues to start at Custom.
        for (std::size_t index = 0; index < canvasPresets.size(); ++index) {
            if (canvasPresets[index].spec == initialSpec) {
                presets->setCurrentIndex(static_cast<int>(index + 1));
                break;
            }
        }
    }
    i18n::addRow(form, i18n::text("new.document.dialog.preset"), presets);
    width_->setDecimals(0);
    height_->setDecimals(0);
    width_->setRange(1, static_cast<int>(kMaximumRasterDimension));
    width_->setObjectName(QStringLiteral("CanvasWidthSpinBox"));
    width_->setValue(static_cast<int>(initialSpec.extent.width));
    i18n::setSuffix(width_, i18n::text("new.document.dialog.px"));
    height_->setRange(1, static_cast<int>(kMaximumRasterDimension));
    height_->setObjectName(QStringLiteral("CanvasHeightSpinBox"));
    height_->setValue(static_cast<int>(initialSpec.extent.height));
    i18n::setSuffix(height_, i18n::text("new.document.dialog.px"));
    dpi_->setRange(1.0, 1200.0);
    dpi_->setObjectName(QStringLiteral("CanvasDpiSpinBox"));
    dpi_->setValue(initialSpec.dotsPerInch);
    i18n::setSuffix(dpi_, i18n::text("new.document.dialog.ppi"));
    i18n::addRow(form, i18n::text("new.document.dialog.width"), width_);
    i18n::addRow(form, i18n::text("new.document.dialog.height"), height_);
    i18n::addRow(form, i18n::text("new.document.dialog.resolution"), dpi_);
    if (!changingCanvas) {
        background_ = new QComboBox(this);
        background_->setObjectName(QStringLiteral("CanvasBackgroundCombo"));
        background_->setItemDelegate(new QStyledItemDelegate(background_));
        i18n::addItems(background_, { i18n::text("new.document.dialog.transparent"), i18n::text("new.document.dialog.white"), i18n::text("new.document.dialog.black") });
        i18n::addRow(form, i18n::text("new.document.dialog.background"), background_);
    }
    settings->addLayout(form);

    auto* sizeHint = new QLabel;
    sizeHint->setObjectName(QStringLiteral("MutedLabel"));
    sizeHint->setWordWrap(true);
    settings->addWidget(sizeHint);
    settings->addStretch();

    if (!changingCanvas) {
        recentSection_ = new QWidget(this);
        recentSection_->setObjectName(QStringLiteral("RecentFilesSection"));
        auto* recentLayout = new QVBoxLayout(recentSection_);
        recentLayout->setContentsMargins(0, 0, 0, 0);
        recentLayout->setSpacing(6);
        auto* recentHeader = new QHBoxLayout;
        auto* recentTitle = i18n::make<QLabel>(i18n::text("new.document.dialog.recent.files"), recentSection_);
        recentTitle->setObjectName(QStringLiteral("SectionLabel"));
        recentHeader->addWidget(recentTitle);
        recentHeader->addStretch();
        clearRecentButton_ = i18n::make<QPushButton>(i18n::text("new.document.dialog.clear.recent"), recentSection_);
        clearRecentButton_->setObjectName(QStringLiteral("ClearRecentFilesButton"));
        i18n::setToolTip(clearRecentButton_, i18n::text("new.document.dialog.remove.the.recent.list.files.are.not.deleted"));
        clearRecentButton_->setAutoDefault(false);
        recentHeader->addWidget(clearRecentButton_);
        recentLayout->addLayout(recentHeader);
        recentList_ = new QListWidget(recentSection_);
        recentList_->setObjectName(QStringLiteral("RecentFilesList"));
        recentList_->setUniformItemSizes(true);
        recentList_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        recentList_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        recentList_->setSelectionMode(QAbstractItemView::SingleSelection);
        recentList_->setTextElideMode(Qt::ElideMiddle);
        recentList_->setWordWrap(false);
        recentList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        recentList_->setContextMenuPolicy(Qt::CustomContextMenu);
        i18n::setToolTip(recentList_, i18n::text("new.document.dialog.activate.a.file.or.double.click.to.open.right"));
        recentLayout->addWidget(recentList_);
        recentEmptyLabel_ = i18n::make<QLabel>(i18n::text("new.document.dialog.no.recent.files.yet"), recentSection_);
        recentEmptyLabel_->setObjectName(QStringLiteral("MutedLabel"));
        recentLayout->addWidget(recentEmptyLabel_);
        recentLayout->addStretch();
        connect(recentList_, &QListWidget::itemActivated, this, &NewDocumentDialog::activateRecentFile);
        connect(recentList_, &QListWidget::itemDoubleClicked, this, &NewDocumentDialog::activateRecentFile);

        auto* recentMenu = new QMenu(recentList_);
        recentMenu->setObjectName(QStringLiteral("RecentFileContextMenu"));
        auto* remove = i18n::addAction(recentMenu, i18n::text("new.document.dialog.remove.from.recent.files"));
        remove->setObjectName(QStringLiteral("RemoveRecentFileAction"));
        connect(recentList_, &QListWidget::customContextMenuRequested, this,
            [this, recentMenu, remove](const QPoint& position) {
                auto* item = recentList_->itemAt(position);
                if (!item || !recentFiles_)
                    return;
                recentList_->setCurrentItem(item);
                remove->setData(item->data(Qt::UserRole));
                recentMenu->popup(recentList_->viewport()->mapToGlobal(position));
            });
        connect(remove, &QAction::triggered, this, [this, remove] {
            if (!recentFiles_)
                return;
            recentFiles_->remove(remove->data().toString());
            refreshRecentFiles();
        });
        connect(clearRecentButton_, &QPushButton::clicked, this, [this] {
            if (!recentFiles_)
                return;
            recentFiles_->clear();
            refreshRecentFiles();
        });
        recentSection_->hide(); // Shown only when the caller supplies the store.
        body->addWidget(recentSection_, 1);
    }

    auto* buttons = new i18n::ButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok);
    buttons->setObjectName(QStringLiteral("CanvasDialogButtons"));
    i18n::setText(buttons->button(QDialogButtonBox::Ok),
        changingCanvas ? i18n::text("dialog.button.apply") : i18n::text("dialog.button.create"));
    if (!changingCanvas) {
        auto* open = i18n::make<QPushButton>(i18n::text("new.document.dialog.open.document"), buttons);
        buttons->addButton(open, QDialogButtonBox::ActionRole);
        open->setObjectName(QStringLiteral("OpenDocumentButton"));
        open->setAutoDefault(false);
        connect(open, &QPushButton::clicked, this, [this] { done(OpenDocument); });
    }
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    const auto updateSizeValidity = [this, buttons, sizeHint, changingCanvas] {
        const auto width = static_cast<std::uint32_t>(width_->value());
        const auto height = static_cast<std::uint32_t>(height_->value());
        const auto valid = rasterExtentWithinLimits(width, height);
        buttons->button(QDialogButtonBox::Ok)->setEnabled(valid);
        if (valid) {
            if (changingCanvas) {
                i18n::setText(sizeHint, i18n::text("new.document.dialog.existing.layer.storage.will.not.be.resized.or.resampled"));
            } else {
                const auto mebibytes
                    = static_cast<double>(width) * static_cast<double>(height) * 4.0 / (1024.0 * 1024.0);
                i18n::setText(sizeHint, i18n::text("new.document.dialog.base.layer.memory.approximately.1.mib").arg(mebibytes, 0, 'f', 1));
            }
            sizeHint->setObjectName(QStringLiteral("MutedLabel"));
        } else {
            i18n::setText(sizeHint, i18n::text("new.document.dialog.canvas.must.not.exceed.40.megapixels"));
            sizeHint->setObjectName(QStringLiteral("ErrorLabel"));
        }
        sizeHint->style()->unpolish(sizeHint);
        sizeHint->style()->polish(sizeHint);
    };
    connect(width_, &QDoubleSpinBox::valueChanged, this, updateSizeValidity);
    connect(height_, &QDoubleSpinBox::valueChanged, this, updateSizeValidity);
    for (auto* field : { width_, height_, dpi_ })
        connect(field, &QDoubleSpinBox::valueChanged, this, [presets] {
            const QSignalBlocker block(presets);
            presets->setCurrentIndex(0);
        });
    connect(presets, &QComboBox::currentIndexChanged, this, [this, updateSizeValidity](int index) {
        if (index <= 0)
            return;
        const auto spec = canvasPresets.at(std::size_t(index - 1)).spec;
        const QSignalBlocker w(width_), h(height_), dpi(dpi_);
        width_->setValue(spec.extent.width);
        height_->setValue(spec.extent.height);
        dpi_->setValue(spec.dotsPerInch);
        updateSizeValidity();
    });
    updateSizeValidity();
}

void NewDocumentDialog::setRecentFiles(RecentFiles* recentFiles)
{
    recentFiles_ = recentFiles;
    refreshRecentFiles();
}

void NewDocumentDialog::refreshRecentFiles()
{
    if (!recentSection_)
        return; // Canvas resize has no open/recent controls.
    QString selected;
    if (const auto* item = recentList_->currentItem())
        selected = item->data(Qt::UserRole).toString();
    recentList_->clear();
    selectedRecentPath_.clear();
    openingRecent_ = false;
    if (recentFiles_) {
        for (const auto& entry : recentFiles_->entries()) {
            const QFileInfo file(entry.path);
            const auto kind = entry.kind == RecentFileKind::Project ? QStringLiteral("Project")
                : entry.kind == RecentFileKind::Pdf ? QStringLiteral("PDF")
                : entry.kind == RecentFileKind::Psd ? QStringLiteral("PSD") : QStringLiteral("Image");
            auto* item = new QListWidgetItem(file.fileName(), recentList_);
            item->setData(Qt::UserRole, entry.path);
            item->setData(Qt::UserRole + 1, int(entry.kind));
            item->setToolTip(QStringLiteral("%1\n%2").arg(entry.path, kind));
            item->setSizeHint(QSize(0, recentList_->fontMetrics().height() + 8));
            if (entry.path == selected)
                recentList_->setCurrentItem(item);
        }
    }
    const bool hasEntries = recentList_->count() != 0;
    const int rowHeight = recentList_->fontMetrics().height() + 8;
    recentList_->setFixedHeight(std::min(5, recentList_->count()) * rowHeight
        + 2 * recentList_->frameWidth());
    recentList_->setVisible(hasEntries);
    recentEmptyLabel_->setVisible(!hasEntries);
    clearRecentButton_->setEnabled(hasEntries);
    recentSection_->setVisible(recentFiles_ != nullptr);
}

void NewDocumentDialog::activateRecentFile(QListWidgetItem* item)
{
    if (openingRecent_ || !recentFiles_ || !item)
        return;
    const auto path = item->data(Qt::UserRole).toString();
    if (path.isEmpty())
        return;
    openingRecent_ = true;
    selectedRecentPath_ = path;
    done(OpenRecent);
}

core::Rgba8 NewDocumentDialog::backgroundColor() const
{
    if (!background_ || background_->currentIndex() == 0)
        return { 0, 0, 0, 0 };
    return background_->currentIndex() == 1 ? core::Rgba8 { 255, 255, 255, 255 }
                                            : core::Rgba8 { 0, 0, 0, 255 };
}

core::CanvasSpec NewDocumentDialog::canvasSpec() const
{
    return {
        .extent = {
            static_cast<std::uint32_t>(width_->value()),
            static_cast<std::uint32_t>(height_->value()),
        },
        .dotsPerInch = dpi_->value(),
    };
}

void NewDocumentDialog::dragEnterEvent(QDragEnterEvent* event)
{
    if (mode_ != Mode::CreateDocument || !event->possibleActions().testFlag(Qt::CopyAction)) return;
    for (const auto& url : event->mimeData()->urls()) if (url.isLocalFile()) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
        return;
    }
}
void NewDocumentDialog::dropEvent(QDropEvent* event)
{
    if (mode_ != Mode::CreateDocument || !event->possibleActions().testFlag(Qt::CopyAction)) return;
    QStringList paths;
    for (const auto& url : event->mimeData()->urls()) if (url.isLocalFile()) paths.push_back(url.toLocalFile());
    if (!paths.empty()) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
        openDroppedFiles(paths);
    }
}
void NewDocumentDialog::openDroppedFiles(const QStringList& paths)
{
    if (mode_ != Mode::CreateDocument || paths.empty()) return;
    droppedFiles_ = paths;
    done(OpenDropped);
}
} // namespace imageeditor::ui
