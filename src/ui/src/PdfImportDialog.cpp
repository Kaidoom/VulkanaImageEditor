#include "imageeditor/ui/PdfImportDialog.hpp"
#include "imageeditor/ui/ToolOptionsNumber.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>

namespace imageeditor::ui {
namespace {
// Reserve the complete tile before its lazy thumbnail arrives. In IconMode,
// changing an item's size on DecorationRole updates can leave Qt's spatial
// hit-test index describing the old, text-only rectangle.
class PdfPageDelegate final : public QStyledItemDelegate {
public:
    explicit PdfPageDelegate(QListWidget* view) : QStyledItemDelegate(view), view_(view) {}
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return view_->gridSize() - QSize(8,8);
    }
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        // Keep the full tile clickable; only inset its visual/selection area.
        auto visual=option;
        visual.rect.adjust(8,0,-8,0);
        QStyledItemDelegate::paint(painter,visual,index);
    }
protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override {
        QStyledItemDelegate::initStyleOption(option,index);
        option->features |= QStyleOptionViewItem::HasDecoration;
        option->decorationSize = view_->iconSize();
    }
private:
    QListWidget* view_;
};
template<class T> bool ready(std::future<T>& future) {
    return future.valid() && future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}
QIcon thumbnailIcon(const QImage& image) {
    QPixmap pixmap(image.size()); pixmap.fill(QColor(215,215,215));
    QPainter painter(&pixmap);
    for (int y=0; y<image.height(); y+=10) for (int x=0; x<image.width(); x+=10)
        if ((x/10+y/10)%2) painter.fillRect(x,y,10,10,QColor(165,165,165));
    painter.drawImage(0,0,image); return QIcon(pixmap);
}
}
struct PdfImportDialog::State {
    PdfImportDialog& dialog;
    QString path;
    PdfLimits limits;
    PdfMetadata metadata;
    PdfOptions settings;
    PdfRenderedPages output;
    QListWidget* pages;
    QLineEdit *range, *password;
    QWidget *passwordRow, *controls;
    ToolOptionsNumber* ppi;
    QComboBox *background, *destination;
    QCheckBox* annotations;
    QLabel *status, *estimate;
    QProgressBar* progress;
    QPushButton *import, *all;
    QTimer timer;
    std::shared_ptr<PdfJobState> loadState, thumbState, renderState;
    std::future<PdfMetadata> loadJob;
    std::future<PdfThumbnail> thumbJob;
    std::future<PdfRenderedPages> renderJob;
    // At most 32 displayed icons + one pending thumbnail. Old page icons are
    // evicted too, not merely the bookkeeping entry.
    std::map<int, QIcon> thumbnails;
    std::vector<int> thumbnailOrder;
    int thumbnailPage {-1};
    unsigned generation {0}, thumbnailGeneration {0};
    QString rangeError;
    bool importing {false};

    State(PdfImportDialog& owner, QString input, bool currentAvailable, bool intoCurrent, PdfLimits budget)
        : dialog(owner), path(std::move(input)), limits(budget), settings(pdfImportPreferences())
    {
        dialog.setObjectName(QStringLiteral("PdfImportDialog"));
        dialog.setWindowTitle(QStringLiteral("Import PDF"));
        dialog.setWindowFlags(Qt::SubWindow);
        dialog.resize(660, 640);
        dialog.setMinimumWidth(620);
        auto* layout = new QVBoxLayout(&dialog); layout->setContentsMargins(18,18,18,18); layout->setSpacing(10);
        auto* title = new QLabel(QStringLiteral("Import PDF · %1").arg(QFileInfo(path).fileName()), &dialog);
        title->setTextFormat(Qt::PlainText); title->setWordWrap(true); layout->addWidget(title);
        pages = new QListWidget(&dialog); pages->setObjectName(QStringLiteral("PdfPages"));
        pages->setViewMode(QListView::IconMode); pages->setResizeMode(QListView::Adjust);
        pages->setMovement(QListView::Static); pages->setSelectionMode(QAbstractItemView::ExtendedSelection);
        pages->setIconSize(QSize(128,144)); pages->setSpacing(0);
        pages->setUniformItemSizes(true);
        pages->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        pages->setItemDelegate(new PdfPageDelegate(pages));
        pages->setWordWrap(false); pages->setMinimumHeight(240); pages->setDragEnabled(false);
        updatePageGrid();
        layout->addWidget(pages,1);
        controls = new QWidget(&dialog); auto* fields = new QVBoxLayout(controls); fields->setContentsMargins(0,0,0,0);
        auto* selectRow = new QHBoxLayout;
        selectRow->addWidget(new QLabel(QStringLiteral("Pages"), controls));
        range = new QLineEdit(controls); range->setObjectName(QStringLiteral("PdfRange")); range->setMaxLength(32768);
        range->setPlaceholderText(QStringLiteral("1, 3-5, 8")); selectRow->addWidget(range,1);
        all = new QPushButton(QStringLiteral("Select All"), controls); all->setAutoDefault(false); selectRow->addWidget(all);
        fields->addLayout(selectRow);
        auto* rasterRow = new QHBoxLayout;
        rasterRow->addWidget(new QLabel(QStringLiteral("Resolution"), controls));
        ppi = new ToolOptionsNumber(controls); ppi->setObjectName(QStringLiteral("PdfPpi"));
        ppi->setRange(1,2400); ppi->setDecimals(1); ppi->setSuffix(QStringLiteral(" PPI")); ppi->setValue(settings.ppi);
        rasterRow->addWidget(ppi,1);
        auto* presets = new QComboBox(controls); presets->setObjectName(QStringLiteral("PdfPpiPresets"));
        presets->addItem(QStringLiteral("Presets…"),0);
        for (int value : {72,150,300,600}) presets->addItem(QString::number(value), value);
        rasterRow->addWidget(presets);
        background = new QComboBox(controls); background->setObjectName(QStringLiteral("PdfBackground"));
        background->addItems({QStringLiteral("White paper"),QStringLiteral("Transparent")});
        background->setCurrentIndex(settings.whitePaper ? 0 : 1); rasterRow->addWidget(background,1);
        fields->addLayout(rasterRow);
        auto* destinationRow = new QHBoxLayout;
        destination = new QComboBox(controls); destination->setObjectName(QStringLiteral("PdfDestination"));
        destination->addItems({QStringLiteral("Layers in a new document"),QStringLiteral("Separate document tabs"),QStringLiteral("Layers in the current document")});
        if (!currentAvailable) static_cast<QStandardItemModel*>(destination->model())->item(2)->setEnabled(false);
        destination->setCurrentIndex(currentAvailable && intoCurrent ? 2 : 0); destinationRow->addWidget(destination,1);
        annotations = new QCheckBox(QStringLiteral("Render annotations"),controls); annotations->setChecked(settings.annotations);
        annotations->setObjectName(QStringLiteral("PdfAnnotations")); destinationRow->addWidget(annotations);
        fields->addLayout(destinationRow); layout->addWidget(controls);
        estimate = new QLabel(&dialog); estimate->setObjectName(QStringLiteral("PdfEstimate")); estimate->setWordWrap(true); layout->addWidget(estimate);
        auto* policy = new QLabel(QStringLiteral("Cropped page area, including existing print marks. No automatic trim. "
            "Annotation appearances are optional. Interactive form widgets are not rendered."), &dialog);
        policy->setWordWrap(true); policy->setProperty("muted",true); layout->addWidget(policy);
        passwordRow = new QWidget(&dialog); auto* unlockRow = new QHBoxLayout(passwordRow); unlockRow->setContentsMargins(0,0,0,0);
        password = new QLineEdit(passwordRow); password->setObjectName(QStringLiteral("PdfPassword"));
        password->setEchoMode(QLineEdit::Password); password->setPlaceholderText(QStringLiteral("PDF password"));
        password->setMaxLength(1024); unlockRow->addWidget(password,1);
        auto* unlock = new QPushButton(QStringLiteral("Unlock"),passwordRow); unlock->setAutoDefault(false); unlockRow->addWidget(unlock);
        layout->addWidget(passwordRow); passwordRow->hide();
        status = new QLabel(QStringLiteral("Reading PDF…"),&dialog); status->setObjectName(QStringLiteral("PdfStatus"));
        status->setWordWrap(true); layout->addWidget(status);
        progress = new QProgressBar(&dialog); progress->setObjectName(QStringLiteral("PdfProgress")); progress->hide(); layout->addWidget(progress);
        auto* buttons = new QHBoxLayout; buttons->addStretch();
        auto* cancel = new QPushButton(QStringLiteral("Cancel"),&dialog); cancel->setAutoDefault(false);
        buttons->addWidget(cancel);
        import = new QPushButton(QStringLiteral("Import"),&dialog); import->setObjectName(QStringLiteral("PdfImport"));
        import->setAutoDefault(false); import->setEnabled(false); buttons->addWidget(import); layout->addLayout(buttons);
        QObject::connect(cancel,&QPushButton::clicked,&dialog,&PdfImportDialog::reject);
        QObject::connect(import,&QPushButton::clicked,&dialog,[this] { startImport(); });
        QObject::connect(unlock,&QPushButton::clicked,&dialog,[this] { load(); });
        QObject::connect(password,&QLineEdit::returnPressed,&dialog,[this] { load(); });
        QObject::connect(all,&QPushButton::clicked,&dialog,[this] { pages->selectAll(); });
        QObject::connect(pages,&QListWidget::itemSelectionChanged,&dialog,[this] {
            settings.pages.clear();
            for (int i=0; i<pages->count(); ++i) if (pages->item(i)->isSelected()) settings.pages.push_back(i);
            rangeError.clear(); QSignalBlocker guard(range); range->setText(formatPdfPageRange(settings.pages)); refresh();
        });
        QObject::connect(range,&QLineEdit::textChanged,&dialog,[this](const QString& text) {
            settings.pages = parsePdfPageRange(text, int(metadata.pages.size()), rangeError);
            QSignalBlocker guard(pages);
            for (int i=0; i<pages->count(); ++i) pages->item(i)->setSelected(std::ranges::binary_search(settings.pages,i));
            refresh();
        });
        QObject::connect(ppi,&QDoubleSpinBox::valueChanged,&dialog,[this](double value) { settings.ppi=value; refresh(); });
        QObject::connect(presets,&QComboBox::activated,&dialog,[this,presets](int i) {
            if (i) ppi->setValue(presets->itemData(i).toDouble());
        });
        QObject::connect(destination,&QComboBox::currentIndexChanged,&dialog,[this](int i) {
            settings.destination=PdfDestination(i); refresh();
        });
        settings.destination=PdfDestination(destination->currentIndex());
        QObject::connect(background,&QComboBox::currentIndexChanged,&dialog,[this](int i) {
            settings.whitePaper=i==0; invalidateThumbnails(); refresh();
        });
        QObject::connect(annotations,&QCheckBox::toggled,&dialog,[this](bool value) {
            settings.annotations=value; invalidateThumbnails(); refresh();
        });
        QObject::connect(pages->verticalScrollBar(),&QScrollBar::valueChanged,&dialog,[this] { tickThumbnails(); });
        timer.setInterval(30); QObject::connect(&timer,&QTimer::timeout,&dialog,[this] { poll(); });
        QObject::connect(qApp,&QCoreApplication::aboutToQuit,&dialog,[this] { cancelWork(); dialog.reject(); });
        controls->setEnabled(false); load();
    }
    void cancelWork() {
        for (auto state : {loadState,thumbState,renderState}) if (state) state->cancelled=true;
        timer.stop();
    }
    void load() {
        if (loadJob.valid()) return;
        loadState=std::make_shared<PdfJobState>();
        loadJob=readPdfMetadata(path,password->text(),loadState); password->clear();
        passwordRow->setEnabled(false); status->setText(QStringLiteral("Reading PDF…")); timer.start();
    }
    PdfLimits applicableLimits() const {
        auto result=limits;
        if (settings.destination != PdfDestination::CurrentDocument) {
            result.availableLayers=4096; result.rasterBytes=256ULL*1024*1024;
        }
        return result;
    }
    void updatePageGrid() {
        // IconMode reserves scrollbar space even while it is hidden. Match its
        // styled layout allowance rather than sizing from the wider viewport.
        auto* style=pages->style();
        auto* bar=pages->verticalScrollBar();
        int width=pages->maximumViewportSize().width();
        if (!style->pixelMetric(QStyle::PM_ScrollView_ScrollBarOverlap,nullptr,bar)) {
            width-=style->pixelMetric(QStyle::PM_ScrollBarExtent,nullptr,bar);
            if (style->styleHint(QStyle::SH_ScrollView_FrameOnlyAroundContents,nullptr,pages)) {
                QStyleOption option; option.initFrom(pages);
                width-=2*style->pixelMetric(QStyle::PM_DefaultFrameWidth,&option,pages);
            }
        }
        // The wrapping boundary is an inclusive right edge, hence minus one.
        const QSize grid(std::max(9,(std::min(width,pages->viewport()->width())-1)/3),
            pages->iconSize().height()+3*pages->fontMetrics().lineSpacing()+16);
        if (pages->gridSize()!=grid) pages->setGridSize(grid);
    }
    void refresh() {
        if (!metadata.source || importing) return;
        for (std::size_t i=0; i<metadata.pages.size(); ++i) {
            const auto& info=metadata.pages[i];
            const auto w=std::max(1.0,std::floor(info.points.width()*settings.ppi/72+0.5));
            const auto h=std::max(1.0,std::floor(info.points.height()*settings.ppi/72+0.5));
            auto label=QStringLiteral("Page %1").arg(i+1);
            if (!info.label.isEmpty() && info.label!=QString::number(i+1)) label+=QStringLiteral(" · %1").arg(info.label);
            pages->item(int(i))->setText(label+QStringLiteral("\n%1 × %2 pt\n%3 × %4 px")
                .arg(info.points.width(),0,'f',2).arg(info.points.height(),0,'f',2).arg(w,0,'f',0).arg(h,0,'f',0));
        }
        const auto plan=planPdfImport(metadata,settings,applicableLimits());
        estimate->setText(QStringLiteral("%1 selected · %2 MiB raster pixels · %3 MiB estimated working memory")
            .arg(settings.pages.size()).arg(double(plan.rasterBytes)/1048576,0,'f',1)
            .arg(double(plan.estimatedWorkingBytes)/1048576,0,'f',1));
        status->setText(rangeError.isEmpty()?plan.error:rangeError);
        import->setEnabled(bool(plan)&&rangeError.isEmpty());
    }
    void invalidateThumbnails() {
        ++generation; if (thumbState) thumbState->cancelled=true;
        for (auto [i,icon]:thumbnails) { Q_UNUSED(icon); pages->item(i)->setIcon({}); }
        thumbnails.clear(); thumbnailOrder.clear(); timer.start();
    }
    void tickThumbnails() {
        if (!metadata.source || importing || thumbJob.valid()) return;
        for (int i=0; i<pages->count(); ++i) {
            if (thumbnails.contains(i) || !pages->visualItemRect(pages->item(i)).intersects(pages->viewport()->rect())) continue;
            thumbnailPage=i; thumbnailGeneration=generation; thumbState=std::make_shared<PdfJobState>();
            const auto ratio=metadata.pages[std::size_t(i)].points;
            auto size=ratio.scaled(QSizeF(128,144),Qt::KeepAspectRatio).toSize();
            size.setWidth(std::max(1,size.width())); size.setHeight(std::max(1,size.height()));
            thumbJob=renderPdfThumbnail(metadata.source,i,size,settings,thumbState); timer.start(); return;
        }
    }
    void startImport() {
        ppi->interpretText();
        if (importing || !rangeError.isEmpty() || !planPdfImport(metadata,settings,applicableLimits())) return;
        importing=true; if (thumbState) thumbState->cancelled=true;
        controls->setEnabled(false); pages->setEnabled(false); import->setEnabled(false);
        renderState=std::make_shared<PdfJobState>();
        renderJob=renderPdfPages(metadata,settings,applicableLimits(),renderState);
        progress->setRange(0,int(settings.pages.size())); progress->setValue(0); progress->show(); timer.start();
    }
    void poll() {
        if (ready(loadJob)) {
            metadata=loadJob.get(); passwordRow->setEnabled(true);
            passwordRow->setVisible(metadata.passwordRequired);
            if (metadata.source) {
                pages->clear();
                updatePageGrid();
                for (std::size_t i=0; i<metadata.pages.size(); ++i) pages->addItem(new QListWidgetItem);
                controls->setEnabled(true); pages->item(0)->setSelected(true); refresh();
            } else status->setText(metadata.error);
        }
        if (ready(thumbJob)) {
            const auto result=thumbJob.get();
            if (thumbnailGeneration==generation && !thumbState->cancelled) {
                const auto icon=result.image.isNull()?QIcon():thumbnailIcon(result.image);
                thumbnails.emplace(thumbnailPage,icon); thumbnailOrder.push_back(thumbnailPage);
                pages->item(thumbnailPage)->setIcon(icon);
                if (thumbnailOrder.size()>32) {
                    const auto oldest=thumbnailOrder.front(); thumbnailOrder.erase(thumbnailOrder.begin());
                    thumbnails.erase(oldest); pages->item(oldest)->setIcon({});
                }
                if (!result.error.isEmpty()) pages->item(thumbnailPage)->setToolTip(result.error);
            }
        }
        if (importing) {
            progress->setValue(renderState->completed);
            status->setText(QStringLiteral("Rendering page %1… Cancel discards the result when this page finishes.")
                .arg(renderState->physicalPage.load()));
        }
        if (ready(renderJob)) {
            output=renderJob.get(); importing=false; progress->hide();
            if (!output.cancelled && output.error.isEmpty() && output.pages.size()==settings.pages.size()) {
                timer.stop(); dialog.accept(); return;
            }
            controls->setEnabled(true); pages->setEnabled(true); refresh(); status->setText(output.error);
        }
        tickThumbnails();
        if (!loadJob.valid() && !thumbJob.valid() && !renderJob.valid()) timer.stop();
    }
};
PdfImportDialog::PdfImportDialog(QString path, bool current, bool intoCurrent, PdfLimits limits, QWidget* parent)
    : QDialog(parent),state_(std::make_unique<State>(*this,std::move(path),current,intoCurrent,limits)) {
    state_->pages->viewport()->installEventFilter(this);
}
PdfImportDialog::~PdfImportDialog() { state_->cancelWork(); }
PdfOptions PdfImportDialog::options() const { return state_->settings; }
PdfPlan PdfImportDialog::plan() const { return planPdfImport(state_->metadata,state_->settings,state_->applicableLimits()); }
PdfRenderedPages PdfImportDialog::takePages() { return std::move(state_->output); }
void PdfImportDialog::reject() { state_->cancelWork(); QDialog::reject(); }
bool PdfImportDialog::eventFilter(QObject* object, QEvent* event) {
    if (object==state_->pages->viewport() && (event->type()==QEvent::Resize || event->type()==QEvent::Show))
        QTimer::singleShot(0,this,[this] { state_->updatePageGrid(); state_->tickThumbnails(); });
    return QDialog::eventFilter(object,event);
}
} // namespace imageeditor::ui
