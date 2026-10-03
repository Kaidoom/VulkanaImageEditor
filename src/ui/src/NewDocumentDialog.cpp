#include "imageeditor/ui/NewDocumentDialog.hpp"
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include "imageeditor/ui/RasterLimits.hpp"
#include "imageeditor/ui/RecentFiles.hpp"
#include "imageeditor/ui/ToolOptionsNumber.hpp"
#include "imageeditor/ui/PopupOwnership.hpp"

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
#include <QStyleOptionButton>
#include <QKeyEvent>
#include <QVBoxLayout>
#include <array>

namespace imageeditor::ui {
namespace {
class DocumentMenuButton final : public QPushButton {
public:
    using QPushButton::QPushButton;
    QSize sizeHint() const override
    {
        auto size = QPushButton::sizeHint();
        QStyleOptionButton option;
        initStyleOption(&option);
        size.rwidth() += style()->pixelMetric(QStyle::PM_MenuButtonIndicator, &option, this);
        return size;
    }
protected:
    void initStyleOption(QStyleOptionButton* option) const override
    {
        QPushButton::initStyleOption(option);
        option->features |= QStyleOptionButton::HasMenu;
    }
    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Down) { click(); event->accept(); }
        else QPushButton::keyPressEvent(event);
    }
};
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
    setWindowTitle(changingCanvas ? QStringLiteral("Change canvas size") : QStringLiteral("New document"));
    setSizeGripEnabled(false);
    setFixedWidth(changingCanvas ? 500 : 820);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(22, 20, 22, 20);
    layout->setSpacing(15);
    layout->setSizeConstraint(QLayout::SetFixedSize);
    auto* heading = new QLabel(changingCanvas ? QStringLiteral("Change the canvas boundary")
                                              : QStringLiteral("Create a new canvas"));
    heading->setObjectName(QStringLiteral("ToolTitle"));
    auto* help = new QLabel(changingCanvas
            ? QStringLiteral("The top-left document origin stays fixed. Layers keep their pixels and "
                             "transforms; content outside the new boundary is clipped, not deleted.")
            : QStringLiteral("V1 documents use 8-bit RGBA in the sRGB color space."));
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
    presets->addItem(QStringLiteral("Custom"));
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
    form->addRow(QStringLiteral("Preset"), presets);
    width_->setDecimals(0);
    height_->setDecimals(0);
    width_->setRange(1, static_cast<int>(kMaximumRasterDimension));
    width_->setObjectName(QStringLiteral("CanvasWidthSpinBox"));
    width_->setValue(static_cast<int>(initialSpec.extent.width));
    width_->setSuffix(QStringLiteral(" px"));
    height_->setRange(1, static_cast<int>(kMaximumRasterDimension));
    height_->setObjectName(QStringLiteral("CanvasHeightSpinBox"));
    height_->setValue(static_cast<int>(initialSpec.extent.height));
    height_->setSuffix(QStringLiteral(" px"));
    dpi_->setRange(1.0, 1200.0);
    dpi_->setObjectName(QStringLiteral("CanvasDpiSpinBox"));
    dpi_->setValue(initialSpec.dotsPerInch);
    dpi_->setSuffix(QStringLiteral(" ppi"));
    form->addRow(QStringLiteral("Width"), width_);
    form->addRow(QStringLiteral("Height"), height_);
    form->addRow(QStringLiteral("Resolution"), dpi_);
    if (!changingCanvas) {
        background_ = new QComboBox(this);
        background_->setObjectName(QStringLiteral("CanvasBackgroundCombo"));
        background_->setItemDelegate(new QStyledItemDelegate(background_));
        background_->addItems(
            { QStringLiteral("Transparent"), QStringLiteral("White"), QStringLiteral("Black") });
        form->addRow(QStringLiteral("Background"), background_);
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
        auto* recentTitle = new QLabel(QStringLiteral("Recent files"), recentSection_);
        recentTitle->setObjectName(QStringLiteral("SectionLabel"));
        recentHeader->addWidget(recentTitle);
        recentHeader->addStretch();
        clearRecentButton_ = new QPushButton(QStringLiteral("Clear recent"), recentSection_);
        clearRecentButton_->setObjectName(QStringLiteral("ClearRecentFilesButton"));
        clearRecentButton_->setToolTip(QStringLiteral("Remove the recent list. Files are not deleted."));
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
        recentList_->setToolTip(QStringLiteral("Activate a file or double-click to open. Right-click to remove it from this list."));
        recentLayout->addWidget(recentList_);
        recentEmptyLabel_ = new QLabel(QStringLiteral("No recent files yet."), recentSection_);
        recentEmptyLabel_->setObjectName(QStringLiteral("MutedLabel"));
        recentLayout->addWidget(recentEmptyLabel_);
        recentLayout->addStretch();
        connect(recentList_, &QListWidget::itemActivated, this, &NewDocumentDialog::activateRecentFile);
        connect(recentList_, &QListWidget::itemDoubleClicked, this, &NewDocumentDialog::activateRecentFile);

        auto* recentMenu = new QMenu(recentList_);
        recentMenu->setObjectName(QStringLiteral("RecentFileContextMenu"));
        auto* remove = recentMenu->addAction(QStringLiteral("Remove from recent files"));
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

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok);
    buttons->setObjectName(QStringLiteral("CanvasDialogButtons"));
    buttons->button(QDialogButtonBox::Ok)
        ->setText(changingCanvas ? QStringLiteral("Apply") : QStringLiteral("Create"));
    if (!changingCanvas) {
        auto* open = new DocumentMenuButton(QStringLiteral("Open document…"), buttons);
        buttons->addButton(open, QDialogButtonBox::ActionRole);
        open->setObjectName(QStringLiteral("OpenDocumentButton"));
        auto* menu = new QMenu(open);
        auto* image = menu->addAction(QStringLiteral("Open image…"));
        image->setObjectName(QStringLiteral("OpenImageFromNewAction"));
        connect(image, &QAction::triggered, this, [this] { done(OpenImage); });
        auto* project = menu->addAction(QStringLiteral("Open project…"));
        project->setObjectName(QStringLiteral("OpenProjectFromNewAction"));
        connect(project, &QAction::triggered, this, [this] { done(OpenProject); });
        connect(open, &QPushButton::clicked, menu, [open, menu] { showOwnedPopupMenu(menu, open); });
        connect(menu, &QMenu::aboutToShow, open, [open] { open->setDown(true); });
        connect(menu, &QMenu::aboutToHide, open, [open] { open->setDown(false); });
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
                sizeHint->setText(QStringLiteral("Existing layer storage will not be resized or resampled."));
            } else {
                const auto mebibytes
                    = static_cast<double>(width) * static_cast<double>(height) * 4.0 / (1024.0 * 1024.0);
                sizeHint->setText(
                    QStringLiteral("Base layer memory: approximately %1 MiB").arg(mebibytes, 0, 'f', 1));
            }
            sizeHint->setObjectName(QStringLiteral("MutedLabel"));
        } else {
            sizeHint->setText(QStringLiteral("Canvas must not exceed 40 megapixels."));
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
