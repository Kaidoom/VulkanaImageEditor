#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/EditorColors.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/ColorPanel.hpp"
#include "imageeditor/ui/ColorSelector.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/NewDocumentDialog.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/PropertiesPanel.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/WorkspacePanel.hpp"

#include <QAction>
#include <QAbstractItemView>
#include <QApplication>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFocusEvent>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QTabletEvent>
#include <QTest>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVulkanInstance>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

namespace core = imageeditor::core;
namespace render = imageeditor::render;
namespace ui = imageeditor::ui;

int failures = 0;

void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

bool near(core::Vec2d left, core::Vec2d right)
{
    return std::abs(left.x - right.x) < 1.0e-8
        && std::abs(left.y - right.y) < 1.0e-8;
}

void sendMouse(render::CanvasWindow& window, QEvent::Type type,
    QPointF position, Qt::MouseButton button, Qt::MouseButtons buttons,
    Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QMouseEvent event(type, position, position, position,
        button, buttons, modifiers);
    QCoreApplication::sendEvent(&window, &event);
}

void sendKey(render::CanvasWindow& window, QEvent::Type type, int key,
    Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QKeyEvent event(type, key, modifiers);
    QCoreApplication::sendEvent(&window, &event);
}

void sendTablet(render::CanvasWindow& window, const QPointingDevice& device,
    QEvent::Type type, QPointF position, Qt::KeyboardModifiers modifiers)
{
    const bool release = type == QEvent::TabletRelease;
    QTabletEvent event(type, &device, position, position,
        release ? 0.0 : 0.6, 12.0F, -18.0F, 0.0F, 25.0, 0.0F,
        modifiers, type == QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton,
        release ? Qt::NoButton : Qt::LeftButton);
    QCoreApplication::sendEvent(&window, &event);
}

struct CanvasFixture {
    core::Document document {core::CanvasSpec {{200, 120}, 96.0}};
    render::CanvasWindow window;
    std::vector<core::Vec2d> samplePositions;
    std::vector<core::Rgba8> pickedColors;
    int brushBegins {0};
    int brushMoves {0};
    int brushEnds {0};
    int brushCancels {0};
    core::Rgba8 working {12, 34, 56, 78};

    CanvasFixture()
    {
        window.resize(600, 400);
        window.setDocument(document.snapshot(), false);
        window.resetTo100Percent();
        window.setWorkingColor(working);
        window.onColorSampleRequested = [this](core::Vec2d point) {
            samplePositions.push_back(point);
            if (point.x < 0.0 || point.y < 0.0
                || point.x >= 200.0 || point.y >= 120.0) {
                return core::ColorSample {};
            }
            return core::ColorSample {
                .status = core::ColorSampleStatus::Available,
                .color = {static_cast<std::uint8_t>(std::floor(point.x)),
                    static_cast<std::uint8_t>(std::floor(point.y)), 77, 128},
                .layersVisited = 1,
                .texelsRead = 1,
            };
        };
        window.onColorPicked = [this](core::Rgba8 color) {
            pickedColors.push_back(color);
            working = color;
            // Mirror the application's one-way working-color synchronization.
            // It must not overwrite the reference while a pick owns input.
            window.setWorkingColor(color);
        };
        window.onBrushStrokeBegan = [this](const core::NormalizedPointerSample&) {
            ++brushBegins;
            return true;
        };
        window.onBrushStrokeMoved = [this](const core::NormalizedPointerSample&) {
            ++brushMoves;
            return true;
        };
        window.onBrushStrokeEnded = [this](const core::NormalizedPointerSample&) {
            ++brushEnds;
            return true;
        };
        window.onBrushStrokeCancelled = [this] { ++brushCancels; };
    }

    [[nodiscard]] QPointF logical(core::Vec2d documentPoint) const
    {
        // Independent centered-canvas mapping for a viewport with zero pan.
        return {300.0 + (documentPoint.x - 100.0) * window.zoom(),
            200.0 + (documentPoint.y - 60.0) * window.zoom()};
    }
};

void altPickingKeepsItsOwnerAndReferenceUntilRelease()
{
    for (const auto tool : {core::ToolId::Brush, core::ToolId::Eraser}) {
        CanvasFixture fixture;
        auto& canvas = fixture.window;
        canvas.setActiveTool(tool);
        const auto original = fixture.working;
        const auto first = fixture.logical({20.25, 30.75});
        const auto second = fixture.logical({81.5, 65.25});

        // The modifier on a pointer event is enough: no native-canvas Alt
        // key event is required after keyboard focus was in a QWidget panel.
        sendMouse(canvas, QEvent::MouseMove, first,
            Qt::NoButton, Qt::NoButton, Qt::AltModifier);
        CHECK(canvas.scene().eyedropperActive);
        CHECK(canvas.scene().eyedropperSampleValid);
        CHECK(canvas.scene().eyedropperReference == original);
        CHECK(fixture.pickedColors.empty());
        CHECK(canvas.scene().activeTool == tool);

        sendMouse(canvas, QEvent::MouseButtonPress, first,
            Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
        CHECK(fixture.pickedColors.size() == 1);
        CHECK(fixture.working == core::Rgba8({20, 30, 77, 128}));
        CHECK(canvas.scene().eyedropperReference == original);
        sendKey(canvas, QEvent::KeyRelease, Qt::Key_Alt);
        CHECK(canvas.scene().eyedropperActive);
        sendMouse(canvas, QEvent::MouseMove, second,
            Qt::NoButton, Qt::LeftButton);
        CHECK(fixture.pickedColors.size() == 2);
        CHECK(canvas.scene().eyedropperCandidate == core::Rgba8({81, 65, 77, 128}));
        CHECK(canvas.scene().eyedropperReference == original);
        CHECK(fixture.brushBegins == 0);
        CHECK(fixture.brushMoves == 0);

        sendMouse(canvas, QEvent::MouseButtonRelease, second,
            Qt::LeftButton, Qt::NoButton);
        CHECK(!canvas.scene().eyedropperActive);
        CHECK(canvas.scene().eyedropperReference == fixture.working);
        CHECK(canvas.scene().activeTool == tool);
        CHECK(fixture.brushEnds == 0);
        const auto picks = fixture.pickedColors.size();
        sendMouse(canvas, QEvent::MouseMove, first, Qt::NoButton, Qt::NoButton);
        CHECK(fixture.pickedColors.size() == picks);
        sendMouse(canvas, QEvent::MouseButtonPress, first,
            Qt::LeftButton, Qt::LeftButton);
        sendMouse(canvas, QEvent::MouseButtonRelease, first,
            Qt::LeftButton, Qt::NoButton);
        CHECK(fixture.brushBegins == 1);
        CHECK(fixture.brushEnds == 1);
    }
}

void altPressedDuringAStrokeNeverStealsBrushOwnership()
{
    CanvasFixture fixture;
    auto& canvas = fixture.window;
    canvas.setActiveTool(core::ToolId::Brush);
    const auto point = fixture.logical({31.0, 44.0});
    sendMouse(canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
    CHECK(fixture.brushBegins == 1);
    sendKey(canvas, QEvent::KeyPress, Qt::Key_Alt, Qt::AltModifier);
    CHECK(!canvas.scene().eyedropperActive);
    sendMouse(canvas, QEvent::MouseMove, point + QPointF(20, 10),
        Qt::NoButton, Qt::LeftButton, Qt::AltModifier);
    CHECK(fixture.brushMoves == 1);
    CHECK(fixture.pickedColors.empty());
    CHECK(fixture.samplePositions.empty());
    sendMouse(canvas, QEvent::MouseButtonRelease, point,
        Qt::LeftButton, Qt::NoButton, Qt::AltModifier);
    CHECK(fixture.brushEnds == 1);
    CHECK(fixture.brushCancels == 0);
    sendMouse(canvas, QEvent::MouseMove, point,
        Qt::NoButton, Qt::NoButton, Qt::AltModifier);
    CHECK(canvas.scene().eyedropperActive);
    CHECK(fixture.pickedColors.empty());
    sendKey(canvas, QEvent::KeyRelease, Qt::Key_Alt);
    CHECK(!canvas.scene().eyedropperActive);
}

void escapeCancelsBrushEvenWhenAltWasPressedMidStroke()
{
    CanvasFixture fixture;
    auto& canvas = fixture.window;
    canvas.setActiveTool(core::ToolId::Brush);
    const auto point = fixture.logical({31.0, 44.0});
    sendMouse(canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
    sendKey(canvas, QEvent::KeyPress, Qt::Key_Alt, Qt::AltModifier);
    sendKey(canvas, QEvent::KeyPress, Qt::Key_Escape, Qt::AltModifier);
    CHECK(fixture.brushCancels == 1);
    sendMouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
    CHECK(fixture.brushEnds == 0);
    CHECK(fixture.pickedColors.empty());
}

void pickerCancellationStopsTheGestureWithoutCreatingPaint()
{
    for (const auto terminal : {QEvent::KeyPress, QEvent::FocusOut,
             QEvent::WindowDeactivate, QEvent::TouchCancel, QEvent::Hide}) {
        CanvasFixture fixture;
        auto& canvas = fixture.window;
        canvas.setActiveTool(core::ToolId::Brush);
        const auto point = fixture.logical({30.0, 45.0});
        sendMouse(canvas, QEvent::MouseButtonPress, point,
            Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
        CHECK(canvas.scene().eyedropperActive);
        if (terminal == QEvent::KeyPress) {
            sendKey(canvas, QEvent::KeyPress, Qt::Key_Escape);
        } else if (terminal == QEvent::FocusOut) {
            QFocusEvent event(QEvent::FocusOut, Qt::OtherFocusReason);
            QCoreApplication::sendEvent(&canvas, &event);
        } else {
            QEvent event(terminal);
            QCoreApplication::sendEvent(&canvas, &event);
        }
        CHECK(!canvas.scene().eyedropperActive);
        const auto picks = fixture.pickedColors.size();
        sendMouse(canvas, QEvent::MouseMove, point + QPointF(10, 0),
            Qt::NoButton, Qt::LeftButton);
        sendMouse(canvas, QEvent::MouseButtonRelease, point,
            Qt::LeftButton, Qt::NoButton);
        CHECK(fixture.pickedColors.size() == picks);
        CHECK(fixture.brushBegins == 0);
        CHECK(fixture.brushMoves == 0);
        CHECK(fixture.brushEnds == 0);
        CHECK(canvas.scene().activeTool == core::ToolId::Brush);
    }
}

void permanentPickerHoverIsReadOnlyAndInvalidSamplesAreIgnored()
{
    CanvasFixture fixture;
    auto& canvas = fixture.window;
    canvas.setActiveTool(core::ToolId::Eyedropper);
    const auto first = fixture.logical({30.0, 45.0});
    const auto second = fixture.logical({40.0, 55.0});
    sendMouse(canvas, QEvent::MouseMove, first, Qt::NoButton, Qt::NoButton);
    CHECK(canvas.scene().eyedropperActive);
    CHECK(canvas.scene().eyedropperSampleValid);
    CHECK(fixture.pickedColors.empty());
    CHECK(canvas.scene().eyedropperReference == fixture.working);
    sendMouse(canvas, QEvent::MouseButtonPress, first, Qt::LeftButton, Qt::LeftButton);
    const auto picks = fixture.pickedColors.size();
    sendKey(canvas, QEvent::KeyPress, Qt::Key_Escape);
    CHECK(canvas.scene().activeTool == core::ToolId::Eyedropper);
    sendMouse(canvas, QEvent::MouseMove, second, Qt::NoButton, Qt::LeftButton);
    CHECK(fixture.pickedColors.size() == picks);
    CHECK(canvas.scene().eyedropperCandidate != fixture.working);
    CHECK(canvas.scene().eyedropperReference == fixture.working);
    sendMouse(canvas, QEvent::MouseButtonRelease, second, Qt::LeftButton, Qt::NoButton);
    CHECK(fixture.pickedColors.size() == picks);

    const auto outside = fixture.logical({-1.0, 10.0});
    sendMouse(canvas, QEvent::MouseMove, outside, Qt::NoButton, Qt::NoButton);
    CHECK(!canvas.scene().eyedropperSampleValid);
    sendMouse(canvas, QEvent::MouseButtonPress, outside, Qt::LeftButton, Qt::LeftButton);
    sendMouse(canvas, QEvent::MouseButtonRelease, outside, Qt::LeftButton, Qt::NoButton);
    CHECK(fixture.pickedColors.size() == picks);
    CHECK(fixture.brushBegins == 0);
}

void tabletSamplingUsesTheSameLockedGesture()
{
    const QPointingDevice pen(QStringLiteral("Deterministic test pen"), 9127,
        QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen,
        QInputDevice::Capability::Position | QInputDevice::Capability::Pressure
            | QInputDevice::Capability::XTilt | QInputDevice::Capability::YTilt
            | QInputDevice::Capability::Rotation,
        1, 2);
    CanvasFixture fixture;
    auto& canvas = fixture.window;
    canvas.setActiveTool(core::ToolId::Eraser);
    const auto original = fixture.working;
    sendTablet(canvas, pen, QEvent::TabletPress,
        fixture.logical({19.25, 35.0}), Qt::AltModifier);
    CHECK(fixture.pickedColors.size() == 1);
    CHECK(canvas.scene().eyedropperReference == original);
    sendTablet(canvas, pen, QEvent::TabletMove,
        fixture.logical({69.75, 75.25}), Qt::NoModifier);
    CHECK(canvas.scene().eyedropperActive);
    CHECK(fixture.working == core::Rgba8({69, 75, 77, 128}));
    CHECK(canvas.scene().eyedropperReference == original);
    sendTablet(canvas, pen, QEvent::TabletRelease,
        fixture.logical({69.75, 75.25}), Qt::NoModifier);
    CHECK(!canvas.scene().eyedropperActive);
    CHECK(canvas.scene().activeTool == core::ToolId::Eraser);
    CHECK(fixture.brushBegins == 0);
    CHECK(fixture.brushEnds == 0);

    sendTablet(canvas, pen, QEvent::TabletPress, fixture.logical({19.25, 35.0}), Qt::AltModifier);
    sendKey(canvas, QEvent::KeyPress, Qt::Key_Space);
    CHECK(!canvas.scene().eyedropperActive);
    const auto picks = fixture.pickedColors.size();
    sendTablet(canvas, pen, QEvent::TabletRelease, fixture.logical({69.75, 75.25}), Qt::NoModifier);
    sendKey(canvas, QEvent::KeyRelease, Qt::Key_Space);
    sendTablet(canvas, pen, QEvent::TabletMove, fixture.logical({100.0, 80.0}), Qt::NoModifier);
    CHECK(!canvas.scene().eyedropperActive);
    CHECK(fixture.pickedColors.size() == picks);

    sendTablet(canvas, pen, QEvent::TabletPress, fixture.logical({19.25, 35.0}), Qt::NoModifier);
    sendKey(canvas, QEvent::KeyPress, Qt::Key_Alt, Qt::AltModifier);
    sendKey(canvas, QEvent::KeyPress, Qt::Key_Escape, Qt::AltModifier);
    sendTablet(canvas, pen, QEvent::TabletRelease, fixture.logical({69.75, 75.25}), Qt::NoModifier);
    CHECK(fixture.brushCancels == 1);
    CHECK(fixture.brushEnds == 0);

    fixture.pickedColors.clear();
    sendTablet(canvas, pen, QEvent::TabletPress,
        fixture.logical({20, 30}), Qt::NoModifier);
    CHECK(fixture.brushBegins == 2);
    sendTablet(canvas, pen, QEvent::TabletMove,
        fixture.logical({40, 50}), Qt::AltModifier);
    CHECK(fixture.brushMoves == 1);
    CHECK(!canvas.scene().eyedropperActive);
    CHECK(fixture.pickedColors.empty());
    sendTablet(canvas, pen, QEvent::TabletRelease,
        fixture.logical({40, 50}), Qt::AltModifier);
    CHECK(fixture.brushEnds == 1);
    CHECK(fixture.brushCancels == 1);
}

void samplingCoordinatesAreIndependentOfZoomAndPanWinsOverAlt()
{
    CanvasFixture fixture;
    auto& canvas = fixture.window;
    canvas.setActiveTool(core::ToolId::Eyedropper);
    const core::Vec2d documentPoint {37.25, 19.75};
    for (const int steps : {0, 5, -8, 3}) {
        if (steps != 0) {
            QWheelEvent wheel({300, 200}, {300, 200}, {}, {0, steps * 120},
                Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(&canvas, &wheel);
        }
        sendMouse(canvas, QEvent::MouseMove, fixture.logical(documentPoint),
            Qt::NoButton, Qt::NoButton);
        CHECK(!fixture.samplePositions.empty());
        CHECK(near(fixture.samplePositions.back(), documentPoint));
        CHECK(canvas.scene().eyedropperCandidate == core::Rgba8({37, 19, 77, 128}));
    }
    CHECK(fixture.pickedColors.empty());

    canvas.setActiveTool(core::ToolId::Brush);
    sendKey(canvas, QEvent::KeyPress, Qt::Key_Space);
    sendMouse(canvas, QEvent::MouseButtonPress, {300, 200},
        Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
    CHECK(!canvas.scene().eyedropperActive);
    sendMouse(canvas, QEvent::MouseMove, {310, 220},
        Qt::NoButton, Qt::LeftButton, Qt::AltModifier);
    CHECK(near(canvas.scene().viewport.pan(), {10, 20}));
    sendMouse(canvas, QEvent::MouseButtonRelease, {310, 220},
        Qt::LeftButton, Qt::NoButton);
    sendKey(canvas, QEvent::KeyRelease, Qt::Key_Space);
    CHECK(fixture.pickedColors.empty());
    CHECK(fixture.brushBegins == 0);
}

void colorSelectorsShareSlotAndSwapSemanticsWithoutSetterFeedback()
{
    for (const auto presentation : {ui::ColorSelector::Presentation::Detailed,
             ui::ColorSelector::Presentation::Compact}) {
        const bool compact = presentation == ui::ColorSelector::Presentation::Compact;
        ui::ColorSelector selector(presentation);
        if (!compact) {
            selector.resize(260, 50);
        }
        selector.show();
        QCoreApplication::processEvents();
        auto* primary = selector.findChild<QPushButton*>(compact
                ? QStringLiteral("RailPrimaryColor") : QStringLiteral("ForegroundColorButton"));
        auto* secondary = selector.findChild<QPushButton*>(compact
                ? QStringLiteral("RailSecondaryColor") : QStringLiteral("SecondaryColorButton"));
        auto* swap = selector.findChild<QToolButton*>(compact
                ? QStringLiteral("RailSwapColors") : QStringLiteral("PanelSwapColors"));
        CHECK(primary && secondary && swap);
        if (!primary || !secondary || !swap) {
            continue;
        }
        int changes = 0;
        core::EditorColors published;
        selector.onColorsChanged = [&](core::EditorColors colors) {
            ++changes;
            published = colors;
        };
        const core::EditorColors initial {{12, 34, 56, 78}, {200, 120, 33, 255},
            core::ColorSlot::Primary};
        selector.setColors(initial);
        selector.setColors(initial);
        CHECK(changes == 0);
        CHECK(selector.colors() == initial);
        CHECK(primary->isChecked());
        CHECK(!secondary->isChecked());
        CHECK(primary->text() == QStringLiteral("#0C22384E"));
        CHECK(secondary->text() == QStringLiteral("#C87821"));
        CHECK(primary->accessibleName().contains(QStringLiteral("#0C22384E")));
        CHECK(secondary->accessibleName().contains(QStringLiteral("Secondary")));

        secondary->click();
        CHECK(changes == 1);
        CHECK(published.active == core::ColorSlot::Secondary);
        CHECK(published.primary == initial.primary);
        CHECK(published.secondary == initial.secondary);
        CHECK(published.foreground() == initial.secondary);
        if (compact) {
            auto* animation = selector.findChild<QVariantAnimation*>();
            CHECK(animation && animation->state() == QAbstractAnimation::Running);
            if (animation) animation->setCurrentTime(animation->duration());
        }
        CHECK(secondary->isChecked());
        CHECK(!primary->isChecked());
        swap->click();
        CHECK(changes == 2);
        CHECK(published.active == core::ColorSlot::Primary);
        CHECK(published.primary == initial.primary);
        CHECK(published.secondary == initial.secondary);
        CHECK(published.foreground() == initial.primary);
        secondary->click();
        CHECK(changes == 3);
        CHECK(published.active == core::ColorSlot::Secondary);
        CHECK(published.foreground() == initial.secondary);
        for (const auto* child : std::array<QWidget*, 3> {primary, secondary, swap}) {
            CHECK(selector.rect().contains(child->geometry()));
        }
        if (compact) {
            CHECK(selector.width() <= 40);
            CHECK(selector.height() <= 40);
            CHECK(primary->width() == primary->height());
            CHECK(secondary->width() == secondary->height());
        }
    }

    ui::ColorSelector detailed(ui::ColorSelector::Presentation::Detailed);
    ui::ColorSelector compact(ui::ColorSelector::Presentation::Compact);
    int detailedChanges = 0;
    int compactChanges = 0;
    detailed.onColorsChanged = [&](core::EditorColors colors) {
        ++detailedChanges;
        compact.setColors(colors);
    };
    compact.onColorsChanged = [&](core::EditorColors colors) {
        ++compactChanges;
        detailed.setColors(colors);
    };
    compact.findChild<QPushButton*>(QStringLiteral("RailSecondaryColor"))->click();
    CHECK(compactChanges == 1);
    CHECK(detailedChanges == 0);
    CHECK(detailed.colors() == compact.colors());
    detailed.findChild<QToolButton*>(QStringLiteral("PanelSwapColors"))->click();
    CHECK(detailedChanges == 1);
    CHECK(compactChanges == 1);
    CHECK(detailed.colors() == compact.colors());
}

void colorAnimationKeepsIdentityAndSettlesAfterRapidSwitches()
{
    ui::ColorSelector selector(ui::ColorSelector::Presentation::Compact);
    selector.show();
    QCoreApplication::processEvents();
    auto* primary = selector.findChild<QPushButton*>(QStringLiteral("RailPrimaryColor"));
    auto* secondary = selector.findChild<QPushButton*>(QStringLiteral("RailSecondaryColor"));
    auto* change = selector.findChild<QToolButton*>(QStringLiteral("RailSwapColors"));
    auto* animation = selector.findChild<QVariantAnimation*>();
    CHECK(primary && secondary && change && animation);
    if (!primary || !secondary || !change || !animation) return;
    const auto a = primary->geometry();
    const auto b = secondary->geometry();
    const auto overlap = a.intersected(b).center();
    const auto original = selector.colors();
    change->click();
    CHECK(selector.colors().foreground() == original.secondary);
    animation->setCurrentTime(45);
    CHECK(primary->geometry() != a && secondary->geometry() != b);
    CHECK(primary->isChecked());
    animation->setCurrentTime(100);
    CHECK(secondary->isChecked() && !primary->isChecked());
    animation->setCurrentTime(animation->duration());
    CHECK(selector.childAt(overlap) == secondary);
    CHECK(primary->geometry() == a && secondary->geometry() == b);
    for (int i = 0; i < 7; ++i) {
        change->click();
        animation->setCurrentTime(45);
        // The application echoes state synchronously; that must not restart it.
        selector.setColors(selector.colors());
        CHECK(animation->currentTime() == 45);
        CHECK(selector.rect().contains(primary->geometry()));
        CHECK(selector.rect().contains(secondary->geometry()));
    }
    animation->setCurrentTime(animation->duration());
    CHECK(selector.colors() == original);
    CHECK(selector.childAt(overlap) == primary);
    CHECK(primary->geometry() == a && secondary->geometry() == b);
    change->click();
    animation->setCurrentTime(45);
    selector.hide();
    CHECK(animation->state() == QAbstractAnimation::Stopped);
    CHECK(primary->geometry() == a && secondary->geometry() == b);
    CHECK(secondary->isChecked());
}

void colorDialogBelongsToEditorAcrossPanelPresentations()
{
    ui::MainWindow window(nullptr, false, false);
    window.setUnsavedPromptEnabled(false);
    window.show();
    QCoreApplication::processEvents();
    const auto original = window.editorSession().colors();
    auto* workspace = dynamic_cast<ui::OverlayDockWorkspace*>(
        window.findChild<QWidget*>(QStringLiteral("CanvasWorkspace")));
    auto* panelShell = dynamic_cast<ui::WorkspacePanel*>(
        window.findChild<QWidget*>(QStringLiteral("ColorPanelShell")));
    CHECK(workspace && panelShell);
    if (!workspace || !panelShell) return;
    for (const auto* buttonName : {"RailPrimaryColor", "ForegroundColorButton"}) {
        if (QString::fromLatin1(buttonName) == QStringLiteral("ForegroundColorButton")) {
            workspace->floatPanel(panelShell, QRect(200, 180, 330, 110));
        }
        auto* button = window.findChild<QPushButton*>(QString::fromLatin1(buttonName));
        CHECK(button);
        if (!button) continue;
        button->click();
        QCoreApplication::processEvents();
        QPointer<QColorDialog> dialog = window.findChild<QColorDialog*>(QStringLiteral("WorkingColorDialog"));
        CHECK(dialog && dialog->isVisible());
        if (!dialog) continue;
        CHECK(dialog->parentWidget() == &window);
        CHECK(dialog->windowType() == Qt::Dialog);
        CHECK(dialog->windowModality() == Qt::ApplicationModal);
        CHECK(QApplication::activeModalWidget() == dialog);
        CHECK(dialog->windowHandle()->transientParent() == window.windowHandle());
        CHECK(dialog->windowHandle()->transientParent() != workspace->panelOverlay()->windowHandle());
        CHECK(dialog->testOption(QColorDialog::DontUseNativeDialog));
        dialog->setCurrentColor(QColor(2, 4, 6, 80));
        dialog->reject();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        CHECK(!dialog);
        CHECK(window.editorSession().colors() == original);
    }
    auto* button = window.findChild<QPushButton*>(QStringLiteral("ForegroundColorButton"));
    button->click();
    QCoreApplication::processEvents();
    QPointer<QColorDialog> dialog = window.findChild<QColorDialog*>(QStringLiteral("WorkingColorDialog"));
    CHECK(dialog);
    if (dialog) {
        dialog->setCurrentColor(QColor(2, 4, 6, 80));
        dialog->accept();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        CHECK(window.editorSession().colors().primary == core::Rgba8({2, 4, 6, 80}));
        CHECK(window.editorSession().history().undoDepth() == 0);
    }
    button->click();
    QCoreApplication::processEvents();
    dialog = window.findChild<QColorDialog*>(QStringLiteral("WorkingColorDialog"));
    CHECK(dialog);
    window.close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(!dialog);
}

render::CanvasWindow* findCanvas()
{
    for (auto* window : QGuiApplication::allWindows()) {
        if (window->objectName() == QStringLiteral("VulkanCanvasWindow")) {
            if (auto* canvas = dynamic_cast<render::CanvasWindow*>(window)) {
                return canvas;
            }
        }
    }
    return nullptr;
}

ui::ColorSelector* selectorIn(ui::MainWindow& window, const QString& name)
{
    return dynamic_cast<ui::ColorSelector*>(window.findChild<QWidget*>(name));
}

void applicationSamplingSynchronizesTheActiveSlotWithoutHistory()
{
    QTemporaryDir imageDirectory;
    CHECK(imageDirectory.isValid());
    const auto lowerPath = imageDirectory.filePath(QStringLiteral("lower.png"));
    const auto upperPath = imageDirectory.filePath(QStringLiteral("upper.png"));
    QImage lower(4, 4, QImage::Format_RGBA8888);
    QImage upper(4, 4, QImage::Format_RGBA8888);
    lower.fill(QColor(220, 25, 45, 255));
    upper.fill(QColor(25, 80, 230, 128));
    CHECK(lower.save(lowerPath));
    CHECK(upper.save(upperPath));

    ui::MainWindow window(nullptr, false, false);
    window.setUnsavedPromptEnabled(false);
    // This check requires every horizontal/vertical rail item to be visible. The
    // approved default now has occupied columns on both sides (~840 px), so
    // leave enough central width and height for the full rail (including Local Blur)
    // rather than testing QToolBar overflow here.
    window.resize(1640, 900);
    window.show();
    CHECK(window.openImageFromPath(lowerPath));
    CHECK(window.importImageAsLayerFromPath(upperPath));
    QCoreApplication::processEvents();
    auto* canvas = findCanvas();
    auto* picker = window.findChild<QAction*>(QStringLiteral("ToolAction_eyedropper"));
    auto* swapAction = window.findChild<QAction*>(QStringLiteral("SwapColorsAction"));
    auto* mergedSource = window.findChild<QToolButton*>(QStringLiteral("SampleMergedVisible"));
    auto* activeSource = window.findChild<QToolButton*>(QStringLiteral("SampleActiveLayer"));
    auto* railSecondary = window.findChild<QPushButton*>(QStringLiteral("RailSecondaryColor"));
    auto* panelSwap = window.findChild<QToolButton*>(QStringLiteral("PanelSwapColors"));
    auto* rail = selectorIn(window, QStringLiteral("ToolRailColors"));
    auto* panel = selectorIn(window, QStringLiteral("ColorPanelColors"));
    ui::PropertiesPanel* properties = nullptr;
    for (auto* widget : window.findChildren<QWidget*>()) {
        if (auto* candidate = dynamic_cast<ui::PropertiesPanel*>(widget)) {
            properties = candidate;
            break;
        }
    }
    auto* workspace = dynamic_cast<ui::OverlayDockWorkspace*>(
        window.findChild<QWidget*>(QStringLiteral("CanvasWorkspace")));
    CHECK(canvas && picker && swapAction && mergedSource && activeSource && railSecondary && panelSwap
        && rail && panel && workspace && properties);
    if (!canvas || !picker || !swapAction || !mergedSource || !activeSource || !railSecondary || !panelSwap
        || !rail || !panel || !workspace || !properties) {
        return;
    }
    const auto& session = window.editorSession();
    CHECK(session.document() != nullptr);
    if (!session.document()) {
        return;
    }
    const auto revision = session.document()->revision();
    const auto undoDepth = session.history().undoDepth();
    const auto redoDepth = session.history().redoDepth();
    const auto memory = session.history().memoryUsed();
    std::vector<core::Revision> surfaceRevisions;
    for (const auto& layer : session.document()->layers()) {
        surfaceRevisions.push_back(std::get<core::RasterLayer>(layer.payload).surface->revision());
    }
    const auto originalPrimary = session.colors().primary;
    railSecondary->click();
    CHECK(session.colors().active == core::ColorSlot::Secondary);
    CHECK(rail->colors() == panel->colors());
    CHECK(rail->colors() == session.colors());

    CHECK(picker->shortcutContext() == Qt::WindowShortcut);
    CHECK(swapAction->associatedObjects().contains(&window));
    CHECK(swapAction->associatedObjects().contains(workspace->panelOverlay()));
    canvas->requestActivate();
    QCoreApplication::processEvents();
    QTest::keyClick(canvas, Qt::Key_I);
    QCoreApplication::processEvents();
    CHECK(picker->isChecked());
    CHECK(session.activeTool() == core::ToolId::Eyedropper);
    CHECK(!window.findChild<QComboBox*>(QStringLiteral("EyedropperSourceCombo")));
    CHECK(mergedSource->isChecked() && !activeSource->isChecked());
    for (auto* source : {mergedSource, activeSource}) {
        CHECK(source->property("toolOptionsButton").toBool());
        CHECK(source->isCheckable());
        CHECK(source->toolButtonStyle() == Qt::ToolButtonIconOnly);
        CHECK(!source->icon().isNull());
        CHECK(!source->toolTip().isEmpty());
        CHECK(!source->accessibleName().isEmpty());
    }
    for (auto* source : {activeSource, mergedSource, activeSource, mergedSource}) {
        QTest::mouseClick(source, Qt::LeftButton);
        CHECK(source->isChecked());
        CHECK(mergedSource->isChecked() != activeSource->isChecked());
        CHECK(session.colorSampleSource() == (source == mergedSource
            ? core::ColorSampleSource::MergedVisible : core::ColorSampleSource::ActiveLayer));
    }

    const core::Vec2d documentPoint {1.5, 1.5};
    const auto viewPoint = canvas->scene().viewport.documentToViewport(documentPoint,
        {4.0, 4.0}, {static_cast<double>(canvas->width()), static_cast<double>(canvas->height())});
    const QPointF point(viewPoint.x, viewPoint.y);
    for (const auto sourceKind : {core::ColorSampleSource::MergedVisible,
             core::ColorSampleSource::ActiveLayer}) {
        (sourceKind == core::ColorSampleSource::MergedVisible ? mergedSource : activeSource)->click();
        CHECK(session.colorSampleSource() == sourceKind);
        const auto expected = core::sampleDocumentColor(*session.document(),
            session.activeLayer(), documentPoint, sourceKind);
        CHECK(expected.available());
        const auto previous = session.foregroundColor();
        sendMouse(*canvas, QEvent::MouseMove, point, Qt::NoButton, Qt::NoButton);
        CHECK(session.foregroundColor() == previous);
        CHECK(canvas->scene().eyedropperCandidate == expected.color);
        CHECK(canvas->scene().eyedropperReference == previous);
        sendMouse(*canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
        CHECK(session.colors().secondary == expected.color);
        CHECK(session.colors().primary == originalPrimary);
        CHECK(canvas->scene().eyedropperReference == previous);
        sendMouse(*canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
        CHECK(rail->colors() == session.colors());
        CHECK(panel->colors() == session.colors());
    }
    CHECK(session.colors().secondary == core::Rgba8({25, 80, 230, 128}));
    const auto beforeSwap = session.colors();
    panelSwap->click();
    CHECK(session.colors().primary == beforeSwap.primary);
    CHECK(session.colors().secondary == beforeSwap.secondary);
    CHECK(session.colors().active != beforeSwap.active);
    CHECK(rail->colors() == panel->colors());

    canvas->requestActivate();
    QCoreApplication::processEvents();
    QTest::keyClick(canvas, Qt::Key_X);
    CHECK(session.colors() == beforeSwap);
    workspace->panelOverlay()->activateWindow();
    QLineEdit input(workspace->panelOverlay());
    input.setGeometry(80, 80, 160, 32);
    input.show();
    input.setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();
    const auto beforeTyping = session.colors();
    QTest::keyClick(&input, Qt::Key_X);
    QTest::keyClick(&input, Qt::Key_I);
    CHECK(input.text() == QStringLiteral("xi"));
    CHECK(session.colors() == beforeTyping);
    CHECK(session.activeTool() == core::ToolId::Eyedropper);
    CHECK(session.document()->revision() == revision);
    CHECK(session.history().undoDepth() == undoDepth);
    CHECK(session.history().redoDepth() == redoDepth);
    CHECK(session.history().memoryUsed() == memory);
    for (std::size_t index = 0; index < surfaceRevisions.size(); ++index) {
        CHECK(std::get<core::RasterLayer>(session.document()->layers()[index].payload)
                  .surface->revision() == surfaceRevisions[index]);
    }
    CHECK(!properties->brushPresetModified());

    input.hide();
    auto* brush = window.findChild<QAction*>(QStringLiteral("ToolAction_brush"));
    auto* size = dynamic_cast<ui::CompactValueControl*>(
        window.findChild<QDoubleSpinBox*>(QStringLiteral("BrushSizeControl")));
    CHECK(brush && size);
    if (brush && size) {
        brush->trigger();
        sendMouse(*canvas, QEvent::MouseMove, point, Qt::NoButton, Qt::NoButton);
        window.activateWindow();
        size->setFocus(Qt::OtherFocusReason);
        QCoreApplication::processEvents();
        QTest::keyClick(size, Qt::Key_6);
        CHECK(size->isManualEntryActive());
        CHECK(size->value() == 6.0);
        QTest::keyPress(size, Qt::Key_Alt);
        CHECK(!canvas->scene().eyedropperActive);
        CHECK(size->isManualEntryActive());
        QTest::keyRelease(size, Qt::Key_Alt);
        // Qt may activate menu mnemonics and commit on focus loss. The color
        // override must not consume Alt or change the typed numeric value.
        CHECK(size->value() == 6.0);
        CHECK(!canvas->scene().eyedropperActive);
        QTest::keyClick(size, Qt::Key_Escape);
        CHECK(!size->isManualEntryActive());
        // The same focused control in Slide mode is no longer a text editor.
        QTest::keyPress(size, Qt::Key_Alt);
        CHECK(canvas->scene().eyedropperActive);
        QTest::keyRelease(size, Qt::Key_Alt);
        CHECK(!canvas->scene().eyedropperActive);
    }
    auto* toolbar = window.findChild<QToolBar*>(QStringLiteral("ToolRail"));
    CHECK(toolbar != nullptr);
    // This fixture tests the unobstructed color control, not Qt's intentional
    // overflow menu. Reserve the actual current rail size (new tools/default
    // dock widths must not turn an unrelated eyedropper test into overflow).
    window.findChild<QAction*>("ToolRailDockTopAction")->trigger();
    QCoreApplication::processEvents();
    if (toolbar && toolbar->width() < toolbar->sizeHint().width()) {
        window.resize(window.width() + toolbar->sizeHint().width() - toolbar->width() + 16, window.height());
        QCoreApplication::processEvents();
    }
    // Also reserve vertical space before the stationary-canvas baseline. Tabs
    // reduce the available rail height without changing its normal overflow.
    window.findChild<QAction*>("ToolRailDockLeftAction")->trigger();
    QCoreApplication::processEvents();
    if (toolbar && toolbar->height() < toolbar->sizeHint().height()) {
        window.resize(window.width(), window.height() + toolbar->sizeHint().height() - toolbar->height() + 16);
        QCoreApplication::processEvents();
    }
    const auto canvasGeometry = workspace->canvasContainer()->geometry();
    const auto workspaceGeometry = workspace->geometry();
    for (const auto* actionName : {"ToolRailDockTopAction", "ToolRailDockBottomAction",
             "ToolRailDockRightAction", "ToolRailDockLeftAction"}) {
        auto* dock = window.findChild<QAction*>(QString::fromLatin1(actionName));
        CHECK(dock != nullptr);
        if (!dock || !toolbar) {
            continue;
        }
        dock->trigger();
        QCoreApplication::processEvents();
        CHECK(rail->isVisible());
        for (auto* ancestor = rail->parentWidget(); ancestor;
             ancestor = ancestor->parentWidget()) {
            CHECK(ancestor->contentsRect().contains(
                QRect(rail->mapTo(ancestor, QPoint {}), rail->size())));
            if (ancestor == toolbar) {
                break;
            }
        }
        CHECK(workspace->canvasContainer()->geometry() == canvasGeometry);
        CHECK(workspace->geometry() == workspaceGeometry);
    }
}

void colorPairPersistsAndNewDocumentDoesNotResetWorkingColors()
{
    const core::EditorColors initial {{12, 34, 56, 78}, {201, 98, 37, 112},
        core::ColorSlot::Secondary};
    {
        QSettings settings;
        settings.clear();
        settings.setValue(QStringLiteral("editor/colors-v2/primary"), QColor(12, 34, 56, 78));
        settings.setValue(QStringLiteral("editor/colors-v2/secondary"), QColor(201, 98, 37, 112));
        settings.setValue(QStringLiteral("editor/colors-v2/active"), 1);
        settings.sync();
    }
    auto saved = initial;
    {
        ui::MainWindow window(nullptr, true, false);
        window.setUnsavedPromptEnabled(false);
        window.show();
        QCoreApplication::processEvents();
        CHECK(window.editorSession().colors() == initial);
        auto* primary = window.findChild<QPushButton*>(QStringLiteral("RailPrimaryColor"));
        auto* swap = window.findChild<QToolButton*>(QStringLiteral("PanelSwapColors"));
        CHECK(primary && swap);
        if (!primary || !swap) {
            return;
        }
        primary->click();
        swap->click();
        saved.active = core::ColorSlot::Primary;
        saved.switchActive();
        CHECK(window.editorSession().colors() == saved);
        CHECK(window.editorSession().history().undoDepth() == 0);
        window.close();
    }
    {
        ui::MainWindow window(nullptr, true, false);
        window.setUnsavedPromptEnabled(false);
        window.show();
        QCoreApplication::processEvents();
        CHECK(window.editorSession().colors() == saved);
        auto* rail = selectorIn(window, QStringLiteral("ToolRailColors"));
        auto* panel = selectorIn(window, QStringLiteral("ColorPanelColors"));
        CHECK(rail && panel);
        if (rail && panel) {
            CHECK(rail->colors() == saved);
            CHECK(panel->colors() == saved);
        }
        QAction* newDocument = nullptr;
        for (auto* action : window.findChildren<QAction*>()) {
            if (action->shortcut() == QKeySequence::New) {
                newDocument = action;
                break;
            }
        }
        CHECK(newDocument != nullptr);
        if (newDocument) {
            bool accepted = false;
            QTimer::singleShot(0, &window, [&accepted, &window] {
                auto* dialog = dynamic_cast<ui::NewDocumentDialog*>(
                    window.findChild<QDialog*>(QStringLiteral("NewDocumentDialog")));
                CHECK(dialog != nullptr);
                if (!dialog) {
                    return;
                }
                auto* width = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("CanvasWidthSpinBox"));
                auto* height = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("CanvasHeightSpinBox"));
                CHECK(width && height);
                if (width && height) {
                    width->setValue(64);
                    height->setValue(48);
                    accepted = true;
                    dialog->accept();
                } else {
                    dialog->reject();
                }
            });
            newDocument->trigger();
            CHECK(accepted);
            CHECK(window.editorSession().document()->canvas().extent == core::Extent2u({64, 48}));
            CHECK(window.editorSession().colors() == saved);
            CHECK(window.editorSession().history().undoDepth() == 0);
            CHECK(window.editorSession().history().redoDepth() == 0);
        }
        window.close();
    }
}

void detailedColorPanelKeepsFullSwatchesAndScrollsWhenShort()
{
    ui::ColorPanel panel;
    panel.setColors({{34, 119, 230, 255}, {205, 85, 40, 112}, core::ColorSlot::Secondary});
    panel.resize(360, 112);
    panel.show();
    QCoreApplication::processEvents();
    auto* selector = panel.findChild<QWidget*>(QStringLiteral("ColorPanelColors"));
    auto* primary = panel.findChild<QPushButton*>(QStringLiteral("ForegroundColorButton"));
    auto* secondary = panel.findChild<QPushButton*>(QStringLiteral("SecondaryColorButton"));
    auto* scroll = panel.findChild<QScrollArea*>();
    CHECK(selector && primary && secondary && scroll);
    if (selector && primary && secondary && scroll) {
        const auto checkSwatches = [&] {
            CHECK(primary->height() == 46);
            CHECK(secondary->height() == 46);
            CHECK(selector->height() >= 46);
            CHECK(selector->contentsRect().contains(primary->geometry()));
            CHECK(selector->contentsRect().contains(secondary->geometry()));
            CHECK(scroll->widget()->rect().contains(
                QRect(selector->mapTo(scroll->widget(), QPoint {}), selector->size())));
        };
        checkSwatches();
        const auto output = qEnvironmentVariable("IMAGEEDITOR_TEST_COLOR_PREVIEW");
        if (!output.isEmpty()) {
            CHECK(panel.grab().save(output));
        }
        panel.resize(360, 40);
        QCoreApplication::processEvents();
        CHECK(panel.height() == 40);
        checkSwatches();
        CHECK(scroll->verticalScrollBar()->maximum() > 0);
    }
}

int captureNativeVulkanColorPreview(const QString& output)
{
    const bool dialogPreview = qEnvironmentVariableIsSet("IMAGEEDITOR_TEST_COLOR_DIALOG");
    // Explicit opt-in only. The regular target remains an offscreen, CPU-only
    // interaction test and never opens a real desktop window or takes a capture.
    if (QGuiApplication::platformName() != QStringLiteral("wayland")) {
        std::cerr << "Native color preview requires QT_QPA_PLATFORM=wayland\n";
        return 77;
    }
    const auto spectacle = QStandardPaths::findExecutable(QStringLiteral("spectacle"));
    if (spectacle.isEmpty()) {
        std::cerr << "Native color preview requires Spectacle\n";
        return EXIT_FAILURE;
    }
    QVulkanInstance instance;
    instance.setApiVersion(QVersionNumber(1, 2));
    if (instance.supportedLayers().contains(QByteArrayLiteral("VK_LAYER_KHRONOS_validation"))) {
        instance.setLayers({QByteArrayLiteral("VK_LAYER_KHRONOS_validation")});
    }
    if (!instance.create()) {
        std::cerr << "Native color preview could not create a Vulkan instance\n";
        return EXIT_FAILURE;
    }
    QTemporaryDir assets;
    if (!assets.isValid()) {
        return EXIT_FAILURE;
    }
    QImage reference(512, 320, QImage::Format_RGBA8888);
    reference.fill(QColor(25, 42, 65, 255));
    {
        QPainter painter(&reference);
        painter.fillRect(0, 0, 256, 160, QColor(230, 88, 43, 255));
        painter.fillRect(256, 0, 256, 160, QColor(238, 193, 60, 255));
        painter.fillRect(0, 160, 256, 160, QColor(62, 174, 134, 255));
        painter.fillRect(256, 160, 256, 160, QColor(125, 89, 205, 255));
    }
    const auto referencePath = assets.filePath(QStringLiteral("color-quadrants.png"));
    if (!reference.save(referencePath)) {
        return EXIT_FAILURE;
    }
    ui::MainWindow window(&instance, false, false);
    window.setUnsavedPromptEnabled(false);
    window.resize(1200, 800);
    if (!window.openImageFromPath(referencePath)) {
        return EXIT_FAILURE;
    }
    window.show();
    window.activateWindow();

    QEventLoop loop;
    QProcess capture;
    QTimer framePoll;
    framePoll.setInterval(40);
    render::CanvasWindow* canvas = nullptr;
    std::uint64_t beforeHoverFrames = 0;
    bool captureStarted = false;
    QPointer<QColorDialog> reviewDialog;
    bool passed = false;
    QString failure = QStringLiteral("native preview timed out");
    const auto complete = [&](bool success, const QString& detail) {
        passed = success;
        failure = detail;
        loop.quit();
    };
    QObject::connect(&capture, &QProcess::finished, &loop,
        [&](int exitCode, QProcess::ExitStatus status) {
            if (status != QProcess::NormalExit || exitCode != 0) {
                complete(false, QStringLiteral("Spectacle failed: %1")
                    .arg(QString::fromUtf8(capture.readAllStandardError())));
                return;
            }
            const QImage screenshot(output);
            const QWidget* target = dialogPreview ? static_cast<QWidget*>(reviewDialog.data()) : &window;
            if (!target) {
                complete(false, QStringLiteral("Color dialog closed before capture"));
                return;
            }
            const QSize expected {
                static_cast<int>(std::lround(target->width() * target->devicePixelRatioF())),
                static_cast<int>(std::lround(target->height() * target->devicePixelRatioF())),
            };
            if (screenshot.isNull() || screenshot.size() != expected) {
                complete(false, QStringLiteral("Spectacle captured another window or no image"));
                return;
            }
            complete(true, {});
        });
    QObject::connect(&capture, &QProcess::errorOccurred, &loop,
        [&](QProcess::ProcessError) { complete(false, capture.errorString()); });
    QObject::connect(&framePoll, &QTimer::timeout, &loop, [&] {
        if (!canvas || captureStarted
            || (!dialogPreview && (!canvas->scene().eyedropperActive || !canvas->scene().eyedropperSampleValid))
            || canvas->rendererStats().framesSubmitted <= beforeHoverFrames) {
            return;
        }
        captureStarted = true;
        framePoll.stop();
        window.activateWindow();
        QTimer::singleShot(120, &capture, [&] {
            if (dialogPreview && (!reviewDialog || !reviewDialog->isVisible()
                    || QApplication::activeModalWidget() != reviewDialog
                    || reviewDialog->windowHandle()->transientParent() != window.windowHandle()
                    || !reviewDialog->isActiveWindow())) {
                complete(false, QStringLiteral("Color dialog lost editor ownership or focus after parent activation"));
                return;
            }
            capture.start(spectacle, {QStringLiteral("--background"),
                QStringLiteral("--nonotify"), QStringLiteral("--activewindow"),
                QStringLiteral("--no-decoration"), QStringLiteral("--no-shadow"),
                QStringLiteral("--output"), output});
        });
    });
    QTimer::singleShot(650, &window, [&] {
        canvas = findCanvas();
        auto* picker = window.findChild<QAction*>(QStringLiteral("ToolAction_eyedropper"));
        if (!canvas || !picker) {
            complete(false, QStringLiteral("Missing canvas or picker action"));
            return;
        }
        picker->trigger();
        canvas->fitDocumentToView();
        const auto point = canvas->scene().viewport.documentToViewport({168.5, 93.5},
            {512.0, 320.0}, {static_cast<double>(canvas->width()), static_cast<double>(canvas->height())});
        beforeHoverFrames = canvas->rendererStats().framesSubmitted;
        sendMouse(*canvas, QEvent::MouseMove, {point.x, point.y}, Qt::NoButton, Qt::NoButton);
        if (dialogPreview) {
            window.findChild<QPushButton*>(QStringLiteral("RailPrimaryColor"))->click();
            reviewDialog = window.findChild<QColorDialog*>(QStringLiteral("WorkingColorDialog"));
            if (!reviewDialog || reviewDialog->parentWidget() != &window) {
                complete(false, QStringLiteral("Color dialog does not belong to main editor"));
                return;
            }
        }
        framePoll.start();
    });
    QTimer::singleShot(12000, &loop, &QEventLoop::quit);
    loop.exec();
    framePoll.stop();
    if (capture.state() != QProcess::NotRunning) {
        capture.kill();
        capture.waitForFinished(1000);
    }
    window.logRendererDiagnostics();
    window.close();
    if (!passed) {
        std::cerr << failure.toStdString() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "Native Vulkan eyedropper preview: " << output.toStdString() << '\n';
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ImageEditorTests"));
    QCoreApplication::setApplicationName(QStringLiteral("EyedropperInteractions"));
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settingsDirectory;
    CHECK(settingsDirectory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    ui::applyEditorTheme(application);
    const auto nativePreview = qEnvironmentVariable("IMAGEEDITOR_TEST_COLOR_PREVIEW_NATIVE");
    if (!nativePreview.isEmpty()) {
        return captureNativeVulkanColorPreview(nativePreview);
    }
    altPickingKeepsItsOwnerAndReferenceUntilRelease();
    altPressedDuringAStrokeNeverStealsBrushOwnership();
    escapeCancelsBrushEvenWhenAltWasPressedMidStroke();
    pickerCancellationStopsTheGestureWithoutCreatingPaint();
    permanentPickerHoverIsReadOnlyAndInvalidSamplesAreIgnored();
    tabletSamplingUsesTheSameLockedGesture();
    samplingCoordinatesAreIndependentOfZoomAndPanWinsOverAlt();
    colorSelectorsShareSlotAndSwapSemanticsWithoutSetterFeedback();
    colorAnimationKeepsIdentityAndSettlesAfterRapidSwitches();
    colorDialogBelongsToEditorAcrossPanelPresentations();
    applicationSamplingSynchronizesTheActiveSlotWithoutHistory();
    colorPairPersistsAndNewDocumentDoesNotResetWorkingColors();
    detailedColorPanelKeepsFullSwatchesAndScrollsWhenShort();
    if (failures != 0) {
        std::cerr << failures << " eyedropper interaction assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All eyedropper interaction tests passed\n";
    return EXIT_SUCCESS;
}
