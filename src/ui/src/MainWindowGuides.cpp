#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/PropertiesPanel.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/ui/ToolOptionsBar.hpp"
#include "imageeditor/ui/ToolOptionsButton.hpp"
#include "imageeditor/ui/ToolOptionsNumber.hpp"
#include <QAction>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QMenu>
#include <QSettings>
#include <QSignalBlocker>
#include <QStatusBar>

namespace imageeditor::ui {
void MainWindow::createGuideControls()
{
    auto* page = new QWidget;
    auto* row = new QHBoxLayout(page);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);
    row->addStretch();
    auto* orientations = new QButtonGroup(page);
    auto* horizontal = new ToolOptionsButton("GuideHorizontal", i18n::text("guides.horizontal"),
        i18n::text("guides.horizontal"), ToolOptionsButton::Kind::Toggle, page);
    auto* vertical = new ToolOptionsButton("GuideVertical", i18n::text("guides.vertical"),
        i18n::text("guides.vertical"), ToolOptionsButton::Kind::Toggle, page);
    orientations->addButton(horizontal);
    orientations->addButton(vertical);
    horizontal->setChecked(true);
    row->addWidget(horizontal);
    row->addWidget(vertical);
    guideCountControl_ = new ToolOptionsNumber(page);
    guideCountControl_->setObjectName("GuideCount");
    guideCountControl_->setRange(1, 1000);
    guideCountControl_->setDecimals(0);
    guideCountControl_->setValue(1);
    guideCountControl_->setFixedWidth(130);
    i18n::setPrefix(guideCountControl_, i18n::text("guides.count"));
    i18n::setToolTip(guideCountControl_, i18n::text("guides.count.hint"));
    row->addWidget(guideCountControl_);
    clearGuidesButton_ = new ToolOptionsButton("ClearGuidesButton", i18n::text("guides.clear"),
        i18n::text("guides.clear"), ToolOptionsButton::Kind::Action, page);
    clearGuidesButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    row->addWidget(clearGuidesButton_);
    row->addStretch();
    const auto changed = [this, horizontal] {
        canvasWindow_->setGuideOptions(
            horizontal->isChecked() ? core::GuideOrientation::Horizontal : core::GuideOrientation::Vertical,
            unsigned(guideCountControl_->value()));
    };
    connect(horizontal, &QToolButton::toggled, this, [changed](bool on) {
        if (on)
            changed();
    });
    connect(vertical, &QToolButton::toggled, this, [changed](bool on) {
        if (on)
            changed();
    });
    connect(guideCountControl_, &QDoubleSpinBox::valueChanged, this, [changed](double) { changed(); });
    canvasWindow_->onGuideCountChanged = [this](unsigned count) {
        const QSignalBlocker block(guideCountControl_);
        guideCountControl_->setValue(count);
    };
    canvasWindow_->onGuideDragBegan = [this](const core::DocumentGuide&) {
        guideSnapping_.clear();
        if (const auto* doc = session().document()) {
            const auto extent = doc->canvas().extent;
            guideSnapping_.begin(*doc, {{0,0}, {double(extent.width),double(extent.height)}}, {});
        }
    };
    canvasWindow_->onGuideDragged = [this](const core::DocumentGuide& guide, Qt::KeyboardModifiers modifiers) {
        const bool horizontal = guide.orientation == core::GuideOrientation::Horizontal;
        const core::Vec2d position = horizontal ? core::Vec2d{0,guide.position} : core::Vec2d{guide.position,0};
        if (!session().document()) return core::HandleSnapResult{position,{}};
        auto options = snappingOptions_;
        options.logicalScale = canvasWindow_->zoom();
        options.bypass |= modifiers.testFlag(Qt::ControlModifier);
        // Align the moving line with visible layer bounds and canvas anchors,
        // without attracting it back to its own saved position.
        options.guides = false;
        const auto result = guideSnapping_.resolveHandle(*session().document(), position,
            horizontal ? core::Vec2d{0,1} : core::Vec2d{1,0}, options);
        const auto extent = session().document()->canvas().extent;
        const auto coordinate = horizontal ? result.position.y : result.position.x;
        if (coordinate < 0 || coordinate > (horizontal ? extent.height : extent.width))
            return core::HandleSnapResult{position,{}};
        return result;
    };
    canvasWindow_->onGuidesChanged = [this](const core::DocumentGuides& before, core::DocumentGuides after,
                                         const char* label) {
        if (!session().document() || fileBusy_ || workspaceDialog_ || canvasWindow_->scene().repairProcessing
            || session().activeTool() != core::ToolId::Guides)
            return false;
        try {
            if (!session().execute(
                    std::make_unique<core::ChangeDocumentGuidesCommand>(before, std::move(after), label)))
                return false;
            fileState().untouched = false;
            synchronizeUi(false, false);
            return true;
        } catch (const std::exception& error) {
            statusBar()->showMessage(QString::fromUtf8(error.what()), 5000);
            return false;
        }
    };
    toolOptionsBar_->registerToolPage(core::ToolId::Guides, i18n::text("guides.tool"), page);
    QVBoxLayout* properties { };
    propertiesPanel_->addToolPage(core::ToolId::Guides, i18n::text("guides.tool"), { }, properties);
}
void MainWindow::createGuideActions(QMenu* view)
{
    auto* menu = i18n::addMenu(view, i18n::text("guides.tool"));
    menu->setIcon(toolGlyph(ToolGlyph::Guides));
    showGuidesAction_ = i18n::addAction(menu, toolGlyph(ToolGlyph::Visible), i18n::text("guides.show"));
    showGuidesAction_->setObjectName("ShowGuidesAction");
    showGuidesAction_->setCheckable(true);
    showGuidesAction_->setChecked(
        !persistWindowState_ || QSettings().value("view/guides/visible", true).toBool());
    connect(showGuidesAction_, &QAction::toggled, this, [this](bool shown) {
        canvasWindow_->setDocumentGuidesVisible(shown);
        if (persistWindowState_)
            QSettings().setValue("view/guides/visible", shown);
    });
    canvasWindow_->setDocumentGuidesVisible(showGuidesAction_->isChecked());
    clearGuidesAction_ = i18n::addAction(menu, toolGlyph(ToolGlyph::Trash), i18n::text("guides.clear"));
    clearGuidesAction_->setObjectName("ClearGuidesAction");
    clearGuidesButton_->setDefaultAction(clearGuidesAction_);
    connect(clearGuidesAction_, &QAction::triggered, this, [this] {
        if (!session().document() || fileBusy_ || workspaceDialog_ || canvasWindow_->scene().repairProcessing)
            return;
        canvasWindow_->cancelGuideInput();
        try {
            const auto before = session().document()->guides();
            auto after = before;
            after.items.clear();
            if (session().execute(std::make_unique<core::ChangeDocumentGuidesCommand>(
                    before, std::move(after), "Clear guides"))) {
                fileState().untouched = false;
                synchronizeUi(false, false);
            }
        } catch (const std::exception& error) {
            statusBar()->showMessage(QString::fromUtf8(error.what()), 5000);
        }
    });
    refreshGuideActions();
}
void MainWindow::refreshGuideActions()
{
    if (clearGuidesAction_)
        clearGuidesAction_->setEnabled(session().document() && !session().document()->guides().items.empty()
            && !fileBusy_ && !workspaceDialog_);
}
}
