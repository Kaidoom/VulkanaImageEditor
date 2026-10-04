#include "imageeditor/ui/AbsolutePositionSlider.hpp"
#include "imageeditor/ui/BrushOptionsPage.hpp"
#include "imageeditor/ui/BrushPresetGrid.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/LayerListView.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/ToolOptionsBar.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/WorkspacePanel.hpp"
#include "imageeditor/render/CanvasWindow.hpp"

#include <QAction>
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QCoreApplication>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMainWindow>
#include <QMap>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSpinBox>
#include <QSettings>
#include <QSplitter>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QStyleOptionSpinBox>
#include <QTest>
#include <QTemporaryDir>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <tuple>

namespace {

int failures = 0;

class ResizeCounter final : public QObject {
public:
    int count {0};

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched && event && event->type() == QEvent::Resize) {
            ++count;
        }
        return false;
    }
};

void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

void sendMouse(QWidget& receiver, QEvent::Type type, QPointF local,
    Qt::MouseButton button, Qt::MouseButtons buttons)
{
    const auto global = receiver.mapToGlobal(local);
    QMouseEvent event(type, local, local, global, button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(&receiver, &event);
}

void sendMouse(QWindow& receiver, QEvent::Type type, const QPointF& global,
    Qt::MouseButton button, Qt::MouseButtons buttons)
{
    const auto local = receiver.mapFromGlobal(global);
    QMouseEvent event(type, local, local, global, button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(&receiver, &event);
}

QRect grooveRect(const QSlider& slider)
{
    QStyleOptionSlider option;
    option.initFrom(&slider);
    option.orientation = slider.orientation();
    option.minimum = slider.minimum();
    option.maximum = slider.maximum();
    option.sliderPosition = slider.sliderPosition();
    option.sliderValue = slider.value();
    option.upsideDown = slider.invertedAppearance();
    return slider.style()->subControlRect(
        QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, &slider);
}

void groovePressSetsAbsoluteValueAndContinuesDragging()
{
    imageeditor::ui::AbsolutePositionSlider slider(Qt::Horizontal);
    slider.setRange(0, 100);
    slider.setValue(100);
    slider.resize(260, 40);
    slider.show();
    QCoreApplication::processEvents();

    const auto groove = grooveRect(slider);
    const QPointF midpoint {groove.center()};
    sendMouse(slider, QEvent::MouseButtonPress, midpoint,
        Qt::LeftButton, Qt::LeftButton);
    CHECK(slider.value() >= 47 && slider.value() <= 53);
    CHECK(slider.isSliderDown());

    const QPointF quarter {
        static_cast<double>(groove.left()) + static_cast<double>(groove.width()) * 0.25,
        static_cast<double>(groove.center().y()),
    };
    sendMouse(slider, QEvent::MouseMove, quarter,
        Qt::NoButton, Qt::LeftButton);
    CHECK(slider.value() >= 22 && slider.value() <= 28);
    sendMouse(slider, QEvent::MouseButtonRelease, quarter,
        Qt::LeftButton, Qt::NoButton);
    CHECK(!slider.isSliderDown());
}

void absolutePositionHonorsInvertedAppearance()
{
    imageeditor::ui::AbsolutePositionSlider slider(Qt::Horizontal);
    slider.setRange(0, 100);
    slider.setValue(0);
    slider.setInvertedAppearance(true);
    slider.resize(260, 40);
    slider.show();
    QCoreApplication::processEvents();

    const auto groove = grooveRect(slider);
    const QPointF quarter {
        static_cast<double>(groove.left()) + static_cast<double>(groove.width()) * 0.25,
        static_cast<double>(groove.center().y()),
    };
    sendMouse(slider, QEvent::MouseButtonPress, quarter,
        Qt::LeftButton, Qt::LeftButton);
    CHECK(slider.value() >= 72 && slider.value() <= 78);
    CHECK(slider.isSliderDown());
    sendMouse(slider, QEvent::MouseButtonRelease, quarter,
        Qt::LeftButton, Qt::NoButton);
}

void sliderWheelInputIsLeftForPanelScrolling()
{
    QScrollArea scrollArea;
    scrollArea.setWidgetResizable(true);
    scrollArea.resize(320, 180);
    auto* content = new QWidget;
    content->setMinimumHeight(900);
    auto* layout = new QVBoxLayout(content);
    layout->addSpacing(300);
    auto* slider = new imageeditor::ui::AbsolutePositionSlider(Qt::Horizontal);
    slider->setRange(0, 100);
    slider->setValue(50);
    layout->addWidget(slider);
    layout->addStretch(1);
    scrollArea.setWidget(content);
    scrollArea.show();
    slider->setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();

    int valueChanges = 0;
    QObject::connect(slider, &QSlider::valueChanged,
        [&valueChanges] { ++valueChanges; });
    auto* scrollBar = scrollArea.verticalScrollBar();
    CHECK(scrollBar->maximum() > 0);
    scrollArea.ensureWidgetVisible(slider, 0, 24);
    QCoreApplication::processEvents();
    const int scrollBefore = scrollBar->value();
    const QPoint local = slider->rect().center();
    QWheelEvent wheel(local, slider->mapToGlobal(local),
        QPoint {}, QPoint {0, -120}, Qt::NoButton, Qt::NoModifier,
        Qt::ScrollUpdate, false);
    QCoreApplication::sendEvent(slider, &wheel);

    CHECK(slider->value() == 50);
    CHECK(valueChanges == 0);
    CHECK(!wheel.isAccepted());

    // Directly injected QWidget events do not perform the platform dispatcher's
    // parent-propagation pass. Confirm the ignored event's intended recipient
    // independently: the enclosing viewport consumes the same wheel input and
    // moves the page.
    const QPoint viewportLocal = slider->mapTo(
        scrollArea.viewport(), slider->rect().center());
    QWheelEvent viewportWheel(viewportLocal,
        scrollArea.viewport()->mapToGlobal(viewportLocal), QPoint {},
        QPoint {0, -120}, Qt::NoButton, Qt::NoModifier,
        Qt::ScrollUpdate, false);
    QCoreApplication::sendEvent(scrollArea.viewport(), &viewportWheel);
    CHECK(scrollBar->value() != scrollBefore);
}

void compactValueControlCombinesSliderTextAndSteps()
{
    imageeditor::ui::CompactValueControl control;
    control.setRange(-180.0, 180.0);
    control.setDecimals(0);
    control.setSingleStep(1.0);
    control.setPrefix(QStringLiteral("Angle: "));
    control.setSuffix(QStringLiteral("°"));
    control.setValue(180.0);
    control.resize(180, 30);
    control.show();
    QCoreApplication::processEvents();

    auto* editor = control.findChild<QLineEdit*>(
        QStringLiteral("CompactValueEditor"));
    CHECK(editor != nullptr);
    CHECK(editor && editor->isReadOnly());
    CHECK(!control.isManualEntryActive());

    const auto field = control.valueFieldRect();
    const auto track = control.progressTrackRect();
    CHECK(field.isValid());
    CHECK(track.isValid());
    CHECK(track.left() <= field.left());
    CHECK(track.right() >= control.width() - 20);
    CHECK(track.right() - field.right() <= 3);
    const QPointF midpoint {field.center()};
    const auto midpointGlobal = control.mapToGlobal(midpoint);
    sendMouse(*control.windowHandle(), QEvent::MouseButtonPress, midpointGlobal,
        Qt::LeftButton, Qt::LeftButton);
    CHECK(std::abs(control.value()) <= 2.0);
    CHECK(control.isSliding());
    CHECK(editor && !editor->hasSelectedText());

    const QPointF quarter {
        static_cast<double>(field.left())
            + static_cast<double>(field.width() - 1) * 0.25,
        static_cast<double>(field.center().y()),
    };
    const auto quarterGlobal = control.mapToGlobal(quarter);
    sendMouse(*control.windowHandle(), QEvent::MouseMove, quarterGlobal,
        Qt::NoButton, Qt::LeftButton);
    CHECK(control.value() >= -92.0 && control.value() <= -88.0);
    CHECK(editor && !editor->hasSelectedText());
    sendMouse(*control.windowHandle(), QEvent::MouseButtonRelease, quarterGlobal,
        Qt::LeftButton, Qt::NoButton);
    CHECK(!control.isSliding());
    CHECK(editor && !editor->hasSelectedText());
    CHECK(editor && editor->isReadOnly());

    if (editor) {
        control.setValue(23.0);
        editor->setFocus(Qt::OtherFocusReason);
        QTest::keyClick(editor, Qt::Key_X);
        CHECK(!control.isManualEntryActive());
        CHECK(editor->isReadOnly());
        CHECK(control.value() == 23.0);

        QTest::keyClick(editor, Qt::Key_6);
        CHECK(control.isManualEntryActive());
        CHECK(!editor->isReadOnly());
        CHECK(control.value() == 6.0);
        QTest::keyClick(editor, Qt::Key_3);
        CHECK(control.value() == 63.0);
        CHECK(editor->text().startsWith(QStringLiteral("Angle: ")));
        CHECK(editor->text().endsWith(QStringLiteral("°")));
        CHECK(!editor->hasSelectedText());

        const auto manualValue = control.value();
        const auto editorCenter = QPointF(editor->rect().center());
        sendMouse(*editor, QEvent::MouseButtonPress, editorCenter,
            Qt::LeftButton, Qt::LeftButton);
        sendMouse(*editor, QEvent::MouseMove,
            QPointF(editor->rect().topLeft()), Qt::NoButton, Qt::LeftButton);
        sendMouse(*editor, QEvent::MouseButtonRelease,
            QPointF(editor->rect().topLeft()), Qt::LeftButton, Qt::NoButton);
        CHECK(!control.isSliding());
        CHECK(control.value() == manualValue);

        QTest::keyClick(editor, Qt::Key_A, Qt::ControlModifier);
        CHECK(editor->selectedText() == control.cleanText());
        CHECK(editor->selectionStart()
            >= static_cast<int>(control.prefix().size()));
        CHECK(editor->selectionStart()
                + static_cast<int>(editor->selectedText().size())
            <= static_cast<int>(editor->text().size()
                - control.suffix().size()));
        QTest::keyClick(editor, Qt::Key_Return);
        QCoreApplication::processEvents();
        CHECK(!control.isManualEntryActive());
        CHECK(editor->isReadOnly());
        CHECK(!editor->hasSelectedText());

        // Qt delivers a normal first click, then a double-click second press.
        sendMouse(*control.windowHandle(), QEvent::MouseButtonPress,
            midpointGlobal, Qt::LeftButton, Qt::LeftButton);
        sendMouse(*control.windowHandle(), QEvent::MouseButtonRelease,
            midpointGlobal, Qt::LeftButton, Qt::NoButton);
        const auto beforeDoubleClick = control.value();
        sendMouse(*control.windowHandle(), QEvent::MouseButtonDblClick,
            midpointGlobal, Qt::LeftButton, Qt::LeftButton);
        CHECK(!control.isSliding());
        CHECK(control.isManualEntryActive());
        CHECK(!editor->isReadOnly());
        CHECK(editor->selectedText() == control.cleanText());
        CHECK(control.value() == beforeDoubleClick);
        sendMouse(*control.windowHandle(), QEvent::MouseButtonRelease,
            midpointGlobal, Qt::LeftButton, Qt::NoButton);
        CHECK(control.isManualEntryActive());
        CHECK(editor->selectedText() == control.cleanText());
        CHECK(control.value() == beforeDoubleClick);

        QTest::keyClick(editor, Qt::Key_4);
        CHECK(control.isManualEntryActive());
        CHECK(control.value() == 4.0);
        QTest::keyClick(editor, Qt::Key_5);
        CHECK(control.value() == 45.0);
        QTest::keyClick(editor, Qt::Key_Escape);
        QCoreApplication::processEvents();
        CHECK(!control.isManualEntryActive());
        CHECK(editor->isReadOnly());
        CHECK(control.value() == 45.0);

        sendMouse(*control.windowHandle(), QEvent::MouseButtonDblClick,
            midpointGlobal, Qt::LeftButton, Qt::LeftButton);
        sendMouse(*control.windowHandle(), QEvent::MouseButtonRelease,
            midpointGlobal, Qt::LeftButton, Qt::NoButton);
        QTest::keyClick(editor, Qt::Key_7);
        CHECK(control.isManualEntryActive());
        QFocusEvent focusOut(QEvent::FocusOut, Qt::OtherFocusReason);
        QCoreApplication::sendEvent(editor, &focusOut);
        QCoreApplication::processEvents();
        CHECK(!control.isManualEntryActive());
        CHECK(editor->isReadOnly());
        CHECK(control.value() == 7.0);

        // Both step buttons commit manual entry and resume ordinary stepping.
        QStyleOptionSpinBox option;
        option.initFrom(&control);
        option.frame = true;
        option.buttonSymbols = QAbstractSpinBox::UpDownArrows;
        option.stepEnabled = QAbstractSpinBox::StepUpEnabled | QAbstractSpinBox::StepDownEnabled;
        for (const auto subControl : {QStyle::SC_SpinBoxUp, QStyle::SC_SpinBoxDown}) {
            QTest::mouseDClick(&control, Qt::LeftButton, {}, field.center());
            QTest::mouseRelease(&control, Qt::LeftButton, {}, field.center());
            CHECK(control.isManualEntryActive());
            QTest::keyClicks(editor, "-12");
            const auto button = control.style()->subControlRect(
                QStyle::CC_SpinBox, &option, subControl, &control);
            QTest::mouseClick(&control, Qt::LeftButton, {}, button.center());
            CHECK(!control.isManualEntryActive() && !control.interactionActive());
            CHECK(editor->isReadOnly() && !editor->hasSelectedText());
            CHECK(control.value() == (subControl == QStyle::SC_SpinBoxUp ? -11 : -13));
        }
    }

    control.setValue(23.0);
    control.setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();
    QKeyEvent shortcutProbe(QEvent::ShortcutOverride, Qt::Key_6,
        Qt::NoModifier, QStringLiteral("6"));
    QCoreApplication::sendEvent(&control, &shortcutProbe);
    CHECK(shortcutProbe.isAccepted());
    CHECK(!control.isManualEntryActive());
    CHECK(control.value() == 23.0);

    QTest::keyClick(&control, Qt::Key_6);
    CHECK(control.isManualEntryActive());
    CHECK(control.value() == 6.0);
    QTest::keyClick(&control, Qt::Key_3, Qt::KeypadModifier);
    CHECK(control.value() == 63.0);
    QTest::keyClick(&control, Qt::Key_Escape);
    QCoreApplication::processEvents();
    CHECK(!control.isManualEntryActive());
    CHECK(control.value() == 63.0);

    control.setValue(23.0);
    control.setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();
    QTest::keyClick(control.windowHandle(), Qt::Key_6, Qt::KeypadModifier);
    CHECK(control.isManualEntryActive());
    CHECK(control.value() == 6.0);
    QTest::keyClick(control.windowHandle(), Qt::Key_3);
    CHECK(control.value() == 63.0);
    QTest::keyClick(control.windowHandle(), Qt::Key_Return);
    QCoreApplication::processEvents();
    CHECK(!control.isManualEntryActive());

    control.setValue(7.0);
    control.setFocus(Qt::OtherFocusReason);
    QTest::keyClick(&control, Qt::Key_Up);
    CHECK(control.value() == 8.0);

    int changes = 0;
    QObject::connect(&control, &QDoubleSpinBox::valueChanged,
        [&changes] { ++changes; });
    const auto valueBeforeWheel = control.value();
    const QPoint local = control.rect().center();
    QWheelEvent wheel(local, control.mapToGlobal(local), QPoint {},
        QPoint {0, 120}, Qt::NoButton, Qt::NoModifier,
        Qt::ScrollUpdate, false);
    QCoreApplication::sendEvent(&control, &wheel);
    CHECK(control.value() == valueBeforeWheel);
    CHECK(changes == 0);
    CHECK(!wheel.isAccepted());

    sendMouse(*control.windowHandle(), QEvent::MouseButtonPress, midpointGlobal,
        Qt::LeftButton, Qt::LeftButton);
    CHECK(control.isSliding());
    CHECK(editor && !editor->hasSelectedText());
    QEvent cancel(QEvent::TouchCancel);
    QCoreApplication::sendEvent(&control, &cancel);
    CHECK(!control.isSliding());
    CHECK(editor && !editor->hasSelectedText());
}

void rapidLayerEyeClicksToggleEveryTime()
{
    QStandardItemModel model;
    auto* item = new QStandardItem(QStringLiteral("Layer 1"));
    item->setCheckable(true);
    item->setCheckState(Qt::Checked);
    model.appendRow(item);

    imageeditor::ui::LayerListView view;
    view.setModel(&model);
    view.setDragEnabled(true);
    view.resize(320, 120);
    view.show();
    QCoreApplication::processEvents();

    const QModelIndex index = model.index(0, 0);
    const QRect eye = view.visibilityIndicatorRect(index);
    CHECK(eye.isValid());
    CHECK(view.indexAt(eye.center()) == index);
    int visibilityChanges = 0;
    QObject::connect(&model, &QAbstractItemModel::dataChanged,
        [&visibilityChanges](const QModelIndex&, const QModelIndex&,
            const QList<int>& roles) {
            if (roles.isEmpty() || roles.contains(Qt::CheckStateRole)) {
                ++visibilityChanges;
            }
        });

    sendMouse(*view.viewport(), QEvent::MouseButtonPress, eye.center(),
        Qt::LeftButton, Qt::LeftButton);
    CHECK(item->checkState() == Qt::Unchecked);
    sendMouse(*view.viewport(), QEvent::MouseButtonRelease, eye.center(),
        Qt::LeftButton, Qt::NoButton);
    CHECK(item->checkState() == Qt::Unchecked);

    // This is Qt's actual second-click event type inside the platform double-
    // click interval. It must count as another eye activation.
    sendMouse(*view.viewport(), QEvent::MouseButtonDblClick, eye.center(),
        Qt::LeftButton, Qt::LeftButton);
    CHECK(item->checkState() == Qt::Checked);
    sendMouse(*view.viewport(), QEvent::MouseButtonRelease, eye.center(),
        Qt::LeftButton, Qt::NoButton);
    CHECK(item->checkState() == Qt::Checked);
    CHECK(visibilityChanges == 2);

    sendMouse(*view.viewport(), QEvent::MouseButtonPress, eye.center(),
        Qt::LeftButton, Qt::LeftButton);
    sendMouse(*view.viewport(), QEvent::MouseButtonRelease, eye.center(),
        Qt::LeftButton, Qt::NoButton);
    CHECK(item->checkState() == Qt::Unchecked);
    CHECK(visibilityChanges == 3);

    const QPoint rowBody = view.visualRect(index).center();
    CHECK(!eye.contains(rowBody));
    sendMouse(*view.viewport(), QEvent::MouseButtonPress, rowBody,
        Qt::LeftButton, Qt::LeftButton);
    sendMouse(*view.viewport(), QEvent::MouseButtonRelease, rowBody,
        Qt::LeftButton, Qt::NoButton);
    CHECK(view.currentIndex() == index);
    CHECK(visibilityChanges == 3);
}

void layerOpacityUsesCompactAdjustmentsAndOneUndoPerInteraction()
{
    imageeditor::ui::MainWindow window(nullptr, false);
    window.setUnsavedPromptEnabled(false);
    window.resize(1200, 800); window.show();
    QCoreApplication::processEvents();
    auto* opacity = dynamic_cast<imageeditor::ui::CompactValueControl*>(
        window.findChild<QDoubleSpinBox*>(QStringLiteral("LayerOpacitySlider")));
    auto* controls = window.findChild<QWidget*>(QStringLiteral("LayerControls"));
    CHECK(opacity && controls);
    if (!opacity || !controls) return;
    CHECK(opacity->prefix() == QStringLiteral("Opacity: ") && opacity->suffix() == QStringLiteral("%"));
    CHECK(opacity->width() == controls->width() - 20);
    for (auto* label : controls->findChildren<QLabel*>()) CHECK(label->text() != QStringLiteral("Opacity"));
    const auto& session = window.editorSession();
    const auto layerId = session.activeLayer().value();
    const auto original = session.document()->layer(layerId)->opacity;
    const auto undoDepth = session.history().undoDepth();
    auto* editor = opacity->findChild<QLineEdit*>();
    CHECK(editor); if (!editor) return;
    const auto undo = [&] {
        for (auto* action : window.findChildren<QAction*>())
            if (action->shortcuts().contains(QKeySequence::Undo)) { action->trigger(); break; }
        QCoreApplication::processEvents();
    };
    const auto track = opacity->progressTrackRect();
    const auto point = [&](double fraction) { return QPoint(track.left() + qRound((track.width() - 1) * fraction), track.center().y()); };
    QTest::mousePress(opacity, Qt::LeftButton, Qt::NoModifier, point(.7));
    CHECK(opacity->isSliding() && opacity->interactionActive());
    sendMouse(*opacity, QEvent::MouseMove, point(.3), Qt::NoButton, Qt::LeftButton);
    QTest::mouseRelease(opacity, Qt::LeftButton, Qt::NoModifier, point(.4));
    CHECK(!opacity->isSliding() && !opacity->interactionActive());
    CHECK(session.history().undoDepth() == undoDepth + 1);
    CHECK(std::abs(session.document()->layer(layerId)->opacity - .4F) < .011F);
    undo();
    CHECK(session.document()->layer(layerId)->opacity == original);
    CHECK(session.history().undoDepth() == undoDepth);

    QTest::mouseDClick(opacity, Qt::LeftButton, {}, opacity->valueFieldRect().center());
    QTest::mouseRelease(opacity, Qt::LeftButton, {}, opacity->valueFieldRect().center());
    CHECK(opacity->isManualEntryActive() && !opacity->isSliding());
    CHECK(editor->selectedText() == opacity->cleanText());
    CHECK(session.history().undoDepth() == undoDepth);
    QTest::keyClicks(opacity, QStringLiteral("63"));
    CHECK(opacity->isManualEntryActive());
    CHECK(opacity->value() == 63);
    QTest::keyClick(editor, Qt::Key_Return);
    CHECK(!opacity->isManualEntryActive() && !opacity->interactionActive());
    CHECK(session.history().undoDepth() == undoDepth + 1);
    QTest::keyClick(opacity, Qt::Key_Z, Qt::ControlModifier);
    QCoreApplication::processEvents();
    CHECK(session.history().undoDepth() == undoDepth);
    CHECK(session.document()->layer(layerId)->opacity == original);
    const auto numericRedoDepth = session.history().redoDepth();
    QTest::mouseDClick(opacity, Qt::LeftButton, {}, opacity->valueFieldRect().center());
    QTest::mouseRelease(opacity, Qt::LeftButton, {}, opacity->valueFieldRect().center());
    QTest::keyClick(editor, Qt::Key_Return);
    CHECK(!opacity->isManualEntryActive() && !opacity->interactionActive());
    CHECK(session.history().undoDepth() == undoDepth);
    CHECK(session.history().redoDepth() == numericRedoDepth);

    QStyleOptionSpinBox option;
    option.initFrom(opacity); option.frame = true; option.buttonSymbols = QAbstractSpinBox::UpDownArrows;
    option.stepEnabled = QAbstractSpinBox::StepUpEnabled | QAbstractSpinBox::StepDownEnabled;
    const auto down = opacity->style()->subControlRect(QStyle::CC_SpinBox, &option, QStyle::SC_SpinBoxDown, opacity);
    QTest::mousePress(opacity, Qt::LeftButton, Qt::NoModifier, down.center());
    QTest::qWait(750);
    QTest::mouseRelease(opacity, Qt::LeftButton, Qt::NoModifier, down.center());
    CHECK(opacity->value() < 99);
    CHECK(!opacity->interactionActive() && !opacity->isManualEntryActive());
    CHECK(!editor->hasSelectedText() && editor->isReadOnly());
    CHECK(session.history().undoDepth() == undoDepth + 1);
    QTest::keyClick(opacity, Qt::Key_Z, Qt::ControlModifier);
    QCoreApplication::processEvents();
    CHECK(session.history().undoDepth() == undoDepth);
    CHECK(session.document()->layer(layerId)->opacity == original);
    // A no-effect step at the upper endpoint must retain the redo branch.
    const auto redoDepth = session.history().redoDepth();
    const auto up = opacity->style()->subControlRect(QStyle::CC_SpinBox, &option, QStyle::SC_SpinBoxUp, opacity);
    QTest::mouseClick(opacity, Qt::LeftButton, Qt::NoModifier, up.center());
    CHECK(session.history().undoDepth() == undoDepth && session.history().redoDepth() == redoDepth);
}

void toolShortcutsSpanCanvasAndOverlayWindows()
{
    imageeditor::ui::MainWindow window(nullptr, false, true);
    window.setUnsavedPromptEnabled(false);
    window.resize(1200, 760);
    window.show();
    QCoreApplication::processEvents();

    auto* brushAction = window.findChild<QAction*>(QStringLiteral("ToolAction_brush"));
    auto* eraserAction = window.findChild<QAction*>(QStringLiteral("ToolAction_eraser"));
    auto* moveAction = window.findChild<QAction*>(QStringLiteral("ToolAction_move"));
    imageeditor::render::CanvasWindow* canvas = nullptr;
    for (auto* candidate : QGuiApplication::allWindows()) {
        if (candidate->objectName() == QStringLiteral("VulkanCanvasWindow")) {
            canvas = dynamic_cast<imageeditor::render::CanvasWindow*>(candidate);
            break;
        }
    }
    auto* workspace = dynamic_cast<imageeditor::ui::OverlayDockWorkspace*>(
        window.findChild<QWidget*>(QStringLiteral("CanvasWorkspace")));
    auto* opacity = dynamic_cast<imageeditor::ui::CompactValueControl*>(
        window.findChild<QDoubleSpinBox*>(QStringLiteral("LayerOpacitySlider")));
    auto* brushSize = window.findChild<QDoubleSpinBox*>(
        QStringLiteral("BrushSizeControl"));
    CHECK(brushAction != nullptr);
    CHECK(eraserAction != nullptr);
    CHECK(moveAction != nullptr);
    CHECK(canvas != nullptr);
    CHECK(workspace != nullptr);
    CHECK(opacity != nullptr);
    CHECK(brushSize != nullptr);
    if (!brushAction || !eraserAction || !moveAction || !canvas || !workspace || !opacity
        || !brushSize) {
        return;
    }

    CHECK(brushAction->shortcutContext() == Qt::WindowShortcut);
    CHECK(eraserAction->shortcutContext() == Qt::WindowShortcut);
    CHECK(brushAction->associatedObjects().contains(&window));
    CHECK(brushAction->associatedObjects().contains(workspace->panelOverlay()));

    canvas->requestActivate();
    QCoreApplication::processEvents();
    QTest::keyClick(canvas, Qt::Key_B);
    QCoreApplication::processEvents();
    CHECK(brushAction->isChecked());
    QTest::keyClick(canvas, Qt::Key_E);
    QCoreApplication::processEvents();
    CHECK(eraserAction->isChecked());
    CHECK(!brushAction->isChecked());
    QTest::keyClick(canvas, Qt::Key_E);
    QCoreApplication::processEvents();
    CHECK(brushAction->isChecked());
    CHECK(!eraserAction->isChecked());
    const auto sizeBeforeShortcut = brushSize->value();
    QTest::keyClick(canvas, Qt::Key_BracketRight);
    QCoreApplication::processEvents();
    CHECK(brushSize->value() > sizeBeforeShortcut);

    workspace->panelOverlay()->activateWindow();
    opacity->setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();
    QTest::keyClick(opacity, Qt::Key_V);
    QCoreApplication::processEvents();
    CHECK(moveAction->isChecked());

    QLineEdit textInput(workspace->panelOverlay());
    textInput.setGeometry(80, 80, 160, 32);
    textInput.show();
    textInput.setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();
    QTest::keyClick(&textInput, Qt::Key_B);
    QTest::keyClick(&textInput, Qt::Key_E);
    QCoreApplication::processEvents();
    CHECK(textInput.text() == QStringLiteral("be"));
    CHECK(moveAction->isChecked());
    CHECK(!eraserAction->isChecked());
}

void brushWorkspaceUsesOneStableTopOptionsBar()
{
    imageeditor::ui::MainWindow window(nullptr, false, false);
    window.setUnsavedPromptEnabled(false);
    window.resize(1500, 900);
    window.show();
    QCoreApplication::processEvents();

    auto* bar = dynamic_cast<imageeditor::ui::ToolOptionsBar*>(
        window.findChild<QToolBar*>(QStringLiteral("ToolOptionsBar")));
    auto* page = dynamic_cast<imageeditor::ui::BrushOptionsPage*>(
        window.findChild<QWidget*>(QStringLiteral("BrushOptionsPage")));
    auto* grid = dynamic_cast<imageeditor::ui::BrushPresetGrid*>(
        window.findChild<QListWidget*>(QStringLiteral("BrushPresetGrid")));
    auto* size = dynamic_cast<imageeditor::ui::CompactValueControl*>(
        window.findChild<QDoubleSpinBox*>(QStringLiteral("BrushSizeControl")));
    auto* opacity = dynamic_cast<imageeditor::ui::CompactValueControl*>(
        window.findChild<QDoubleSpinBox*>(QStringLiteral("BrushOpacityControl")));
    auto* hardness = dynamic_cast<imageeditor::ui::CompactValueControl*>(
        window.findChild<QDoubleSpinBox*>(QStringLiteral("BrushHardnessControl")));
    auto* rotation = window.findChild<QToolButton*>(
        QStringLiteral("BrushDirectionButton"));
    auto* angle = dynamic_cast<imageeditor::ui::CompactValueControl*>(
        window.findChild<QDoubleSpinBox*>(QStringLiteral("BrushAngleControl")));
    auto* context = window.findChild<QLabel*>(
        QStringLiteral("ToolOptionsContext"));
    auto* verticalScale = dynamic_cast<imageeditor::ui::CompactValueControl*>(
        window.findChild<QDoubleSpinBox*>(QStringLiteral("BrushScaleControl")));
    auto* flow = dynamic_cast<imageeditor::ui::CompactValueControl*>(
        window.findChild<QDoubleSpinBox*>(QStringLiteral("BrushFlowControl")));
    auto* spacing = dynamic_cast<imageeditor::ui::CompactValueControl*>(
        window.findChild<QDoubleSpinBox*>(QStringLiteral("BrushSpacingControl")));
    auto* viewport = window.findChild<QScrollArea*>(QStringLiteral("ToolOptionsViewport"));
    auto* previous = window.findChild<QToolButton*>(QStringLiteral("PreviousToolOptions"));
    auto* next = window.findChild<QToolButton*>(QStringLiteral("NextToolOptions"));
    auto* workspace = dynamic_cast<imageeditor::ui::OverlayDockWorkspace*>(
        window.findChild<QWidget*>(QStringLiteral("CanvasWorkspace")));
    auto* brushAction = window.findChild<QAction*>(QStringLiteral("ToolAction_brush"));
    auto* eraserAction = window.findChild<QAction*>(QStringLiteral("ToolAction_eraser"));
    auto* moveAction = window.findChild<QAction*>(QStringLiteral("ToolAction_move"));
    auto* eraseModeButton = window.findChild<QToolButton*>(
        QStringLiteral("BrushEraseModeButton"));
    auto* sizeEditor = size ? size->findChild<QLineEdit*>(
        QStringLiteral("CompactValueEditor")) : nullptr;
    imageeditor::render::CanvasWindow* canvas = nullptr;
    for (auto* candidate : QGuiApplication::allWindows()) {
        if (candidate->objectName() == QStringLiteral("VulkanCanvasWindow")) {
            canvas = dynamic_cast<imageeditor::render::CanvasWindow*>(candidate);
            if (canvas) {
                break;
            }
        }
    }
    CHECK(bar != nullptr);
    CHECK(page != nullptr);
    CHECK(grid != nullptr);
    CHECK(size != nullptr);
    CHECK(opacity != nullptr);
    CHECK(hardness != nullptr);
    CHECK(rotation != nullptr);
    CHECK(angle != nullptr);
    CHECK(context != nullptr);
    CHECK(verticalScale != nullptr);
    CHECK(flow != nullptr);
    CHECK(spacing != nullptr);
    CHECK(viewport != nullptr);
    CHECK(previous != nullptr);
    CHECK(next != nullptr);
    CHECK(workspace != nullptr);
    CHECK(brushAction != nullptr);
    CHECK(eraserAction != nullptr);
    CHECK(moveAction != nullptr);
    CHECK(eraseModeButton != nullptr);
    CHECK(sizeEditor != nullptr);
    CHECK(canvas != nullptr);
    if (!bar || !page || !grid || !size || !opacity || !hardness || !rotation
        || !angle || !context || !verticalScale || !workspace || !brushAction
        || !eraserAction || !moveAction || !eraseModeButton || !sizeEditor
        || !canvas || !flow || !spacing || !viewport || !previous || !next) {
        return;
    }

    CHECK(window.toolBarArea(bar) == Qt::TopToolBarArea);
    CHECK(!bar->isMovable());
    CHECK(!bar->isFloatable());
    CHECK(bar->minimumHeight() == bar->maximumHeight());
    CHECK(!bar->isWindow());
    CHECK(bar->parentWidget() == &window);
    CHECK(!workspace->panelOverlay()->isAncestorOf(bar));
    CHECK(bar->pageForTool(imageeditor::core::ToolId::Brush) == page);
    CHECK(eraseModeButton->defaultAction() == eraserAction);
    CHECK(!page->isVisible());
    CHECK(context->text() == QStringLiteral("MOVE"));
    CHECK(rotation->isCheckable());
    CHECK(rotation->text() == QStringLiteral("Direction"));
    CHECK(rotation->isChecked());
    CHECK(grid->count() >= 10);
    for (int row = 0; row < grid->count(); ++row) {
        CHECK(!grid->item(row)->icon().isNull());
    }
    const auto directionRight = rotation->mapTo(
        &window, rotation->rect().bottomRight()).x();
    CHECK(directionRight < window.width());

    const auto selectPreset = [grid](const QString& id) {
        for (int row = 0; row < grid->count(); ++row) {
            auto* item = grid->item(row);
            if (item->data(Qt::UserRole).toString() == id) {
                grid->setCurrentItem(item);
                return true;
            }
        }
        return false;
    };

    const QRect workspaceGeometry = workspace->geometry();
    const QRect canvasGeometry = workspace->canvasContainer()->geometry();
    const QRect barGeometry = bar->geometry();
    brushAction->trigger();
    QCoreApplication::processEvents();
    CHECK(page->isVisible());
    CHECK(context->text() == QStringLiteral("BRUSH"));
    CHECK(eraseModeButton->isVisible());
    CHECK(!eraseModeButton->isChecked());
    const auto checkControlContainment = [bar](QWidget* control) {
        for (auto* ancestor = control->parentWidget(); ancestor;
             ancestor = ancestor->parentWidget()) {
            const QRect mapped(control->mapTo(ancestor, QPoint {}), control->size());
            if (!ancestor->contentsRect().contains(mapped)) {
                std::cerr << control->objectName().toStdString() << " clipped by "
                          << ancestor->objectName().toStdString() << " control="
                          << mapped.x() << ',' << mapped.y() << ','
                          << mapped.width() << ',' << mapped.height()
                          << " ancestor=" << ancestor->width() << ','
                          << ancestor->height() << '\n';
            }
            CHECK(ancestor->contentsRect().contains(mapped));
            if (ancestor == bar) {
                break;
            }
        }
    };
    const auto checkControls = [&] {
        for (auto* control : std::array<QWidget*, 9> {
                 eraseModeButton, size, opacity, hardness, flow, verticalScale,
                 spacing, angle, rotation}) {
            // Wide: all controls visible together. Narrow: each control must
            // remain fully reachable through the generic horizontal viewport.
            control->setFocus(Qt::TabFocusReason);
            QCoreApplication::processEvents();
            CHECK(control->height() == ((control == eraseModeButton || control == rotation) ? 28 : 30));
            checkControlContainment(control);
        }
        CHECK(bar->height() >= bar->minimumSizeHint().height());
        // Five logical pixels on each side of the 30 px row, plus the separate
        // bottom separator. Icon-only buttons are centered within that row.
        CHECK(size->mapTo(bar, QPoint{}).y() == 5);
        CHECK(bar->height() - size->mapTo(bar, QPoint{}).y() - size->height() - 1 == 5);
    };
    checkControls();
    CHECK(!previous->isVisible());
    CHECK(!next->isVisible());
    CHECK(hardness->geometry().right() < flow->geometry().left());
    CHECK(flow->geometry().right() < verticalScale->geometry().left());
    CHECK(verticalScale->geometry().right() < spacing->geometry().left());
    CHECK(spacing->geometry().right() < angle->geometry().left());
    CHECK(angle->geometry().right() < rotation->geometry().left());
    const auto controlsLeft = size->mapTo(bar, size->rect().topLeft()).x();
    const auto controlsRight = rotation->mapTo(
        bar, rotation->rect().topRight()).x();
    const auto eraseRight = eraseModeButton->mapTo(
        bar, eraseModeButton->rect().topRight()).x();
    CHECK(eraseRight < controlsLeft);
    CHECK(std::abs((controlsLeft + controlsRight) / 2
              - bar->rect().center().x())
        <= 3);
    // Match each authored recipe: decorative stamps and chisel pens deliberately
    // keep a fixed angle. The original five and registered bitmap entries still
    // follow the stroke. Selecting a preset must update the toolbar accordingly.
    for (int row = 0; row < grid->count(); ++row) {
        auto* item = grid->item(row);
        if (item->data(Qt::UserRole).toString().startsWith(QStringLiteral("builtin."))) {
            grid->setCurrentItem(item);
            const auto id = item->data(Qt::UserRole).toString().toStdString();
            const auto* preset = imageeditor::core::findBuiltinBrushPreset(id);
            const auto mode = preset ? preset->settings.tip.rotationMode
                : imageeditor::core::BrushTipRotationMode::FollowStrokeDirection;
            CHECK(rotation->isChecked()
                == (mode == imageeditor::core::BrushTipRotationMode::FollowStrokeDirection));
            CHECK(page->brushSettings().tip.rotationMode == mode);
        }
    }
    CHECK(selectPreset(QStringLiteral("builtin.preset.ink-pen.v1")));
    QCoreApplication::processEvents();
    CHECK(size->value() == 24.0);
    CHECK(size->singleStep() == 1.0);
    CHECK(opacity->value() == 100.0);
    CHECK(hardness->value() == 94.0);
    CHECK(rotation->isChecked());
    CHECK(angle->value() == -38.0);
    CHECK(verticalScale->value() == 24);
    const auto ink = imageeditor::core::proceduralBrushPreset(
        imageeditor::core::ProceduralBrushPreset::InkPen);
    CHECK(flow->value() == std::round(ink.flow * 100.0));
    CHECK(spacing->value() == std::round(ink.spacingPercent));

    // Rail, shortcut, and compact top presentation are the same exclusive
    // Brush/Eraser state. Switching mode keeps one settings page and does not
    // disturb its values or the canvas topology.
    QTest::mouseClick(eraseModeButton, Qt::LeftButton);
    QCoreApplication::processEvents();
    CHECK(eraserAction->isChecked());
    CHECK(!brushAction->isChecked());
    CHECK(page->isVisible());
    CHECK(context->text() == QStringLiteral("BRUSH"));
    CHECK(size->value() == 24.0);
    checkControls();
    QTest::mouseClick(eraseModeButton, Qt::LeftButton);
    QCoreApplication::processEvents();
    CHECK(brushAction->isChecked());
    CHECK(!eraserAction->isChecked());
    CHECK(page->isVisible());

    size->setValue(23.0);
    QTest::mouseDClick(size, Qt::LeftButton, {}, size->valueFieldRect().center());
    QTest::mouseRelease(size, Qt::LeftButton, {}, size->valueFieldRect().center());
    CHECK(size->isManualEntryActive() && size->value() == 23.0);
    CHECK(sizeEditor->selectedText() == size->cleanText());
    QCoreApplication::processEvents();
    QTest::keyClick(size, Qt::Key_6);
    CHECK(size->isManualEntryActive());
    CHECK(size->value() == 6.0);
    CHECK(sizeEditor->text() == QStringLiteral("Size: 6 px"));
    CHECK(page->brushSettings().sizePixels == 6.0);
    QTest::keyClick(size, Qt::Key_Period);
    CHECK(sizeEditor->text() == QStringLiteral("Size: 6. px"));
    QTest::keyClick(size, Qt::Key_5);
    CHECK(size->value() == 6.5);
    CHECK(sizeEditor->text() == QStringLiteral("Size: 6.5 px"));
    CHECK(page->brushSettings().sizePixels == 6.5);
    QTest::keyClick(size, Qt::Key_Escape);
    QCoreApplication::processEvents();
    CHECK(!size->isManualEntryActive());
    CHECK(sizeEditor->isReadOnly());
    CHECK(size->value() == 6.5);

    size->setValue(23.0);
    QTest::keyClick(size, Qt::Key_7);
    QTest::keyClick(size, Qt::Key_Period);
    CHECK(size->isManualEntryActive());
    CHECK(sizeEditor->text() == QStringLiteral("Size: 7. px"));
    const QPointF canvasPosition {24.0, 24.0};
    QMouseEvent canvasPress(QEvent::MouseButtonPress, canvasPosition,
        canvasPosition, QPointF(canvas->mapToGlobal(canvasPosition.toPoint())),
        Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(canvas, &canvasPress);
    QMouseEvent canvasRelease(QEvent::MouseButtonRelease, canvasPosition,
        canvasPosition, QPointF(canvas->mapToGlobal(canvasPosition.toPoint())),
        Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(canvas, &canvasRelease);
    QCoreApplication::processEvents();
    CHECK(!size->isManualEntryActive());
    CHECK(sizeEditor->isReadOnly());
    CHECK(!size->hasFocus());
    CHECK(!sizeEditor->hasFocus());
    CHECK(size->value() == 7.0);
    CHECK(sizeEditor->text() == QStringLiteral("Size: 7.0 px"));

    CHECK(selectPreset(QStringLiteral("builtin.preset.hard-round.v1")));
    CHECK(selectPreset(QStringLiteral("builtin.preset.ink-pen.v1")));
    CHECK(!grid->currentItem()->text().endsWith(QStringLiteral(" *")));
    CHECK(!grid->currentItem()->toolTip().contains(QStringLiteral("Modified")));
    const auto beforeRotation = page->brushSettings();
    QTest::mouseClick(rotation, Qt::LeftButton);
    CHECK(!rotation->isChecked());
    CHECK(page->brushSettings().tip.rotationMode
        == imageeditor::core::BrushTipRotationMode::Fixed);
    CHECK(angle->value() == -38.0);
    CHECK(grid->currentItem()->text().endsWith(QStringLiteral(" *")));
    CHECK(grid->currentItem()->toolTip().contains(QStringLiteral("Modified")));
    // Icon-only toolbar toggles intentionally leave keyboard ownership with
    // the canvas/field (Space pans); Tab navigation is disabled app-wide.
    CHECK(rotation->focusPolicy() == Qt::NoFocus);
    QTest::mouseClick(rotation, Qt::LeftButton);
    CHECK(rotation->isChecked());
    CHECK(page->brushSettings().tip.rotationMode
        == imageeditor::core::BrushTipRotationMode::FollowStrokeDirection);
    CHECK(page->brushSettings() == beforeRotation);
    CHECK(!grid->currentItem()->text().endsWith(QStringLiteral(" *")));
    CHECK(!grid->currentItem()->toolTip().contains(QStringLiteral("Modified")));

    size->setValue(37.5);
    verticalScale->setValue(51);
    flow->setValue(31);
    spacing->setValue(18);
    QCoreApplication::processEvents();
    CHECK(page->brushSettings().sizePixels == 37.5);
    CHECK(std::abs(page->brushSettings().tip.aspectRatio - 0.51) < 1.0e-9);
    CHECK(std::abs(page->brushSettings().flow - 0.31) < 1.0e-9);
    CHECK(page->brushSettings().spacingPercent == 18.0);
    // A remaining Properties control must preserve all three moved settings.
    auto* pressureSize = window.findChild<QCheckBox*>(QStringLiteral("BrushPressureSize"));
    CHECK(pressureSize != nullptr);
    if (pressureSize) {
        pressureSize->setChecked(!pressureSize->isChecked());
        CHECK(verticalScale->value() == 51);
        CHECK(flow->value() == 31);
        CHECK(spacing->value() == 18);
    }
    CHECK(grid->currentPresetId() == "builtin.preset.ink-pen.v1");
    CHECK(grid->currentItem()
        && grid->currentItem()->text().endsWith(QStringLiteral(" *")));
    CHECK(grid->currentItem()->toolTip().contains(QStringLiteral("Modified")));

    moveAction->trigger();
    QCoreApplication::processEvents();
    CHECK(!page->isVisible());
    CHECK(context->text() == QStringLiteral("MOVE"));
    brushAction->trigger();
    QCoreApplication::processEvents();
    CHECK(page->isVisible());
    CHECK(size->value() == 37.5);
    CHECK(workspace->geometry() == workspaceGeometry);
    CHECK(workspace->canvasContainer()->geometry() == canvasGeometry);
    CHECK(bar->geometry() == barGeometry);
    checkControls();
    window.resize(980, 720);
    QCoreApplication::processEvents();
    CHECK(window.width() == 980);
    CHECK(previous->isVisible());
    CHECK(next->isVisible());
    viewport->horizontalScrollBar()->setValue(0);
    CHECK(!previous->isEnabled());
    CHECK(next->isEnabled());
    QTest::mouseClick(next, Qt::LeftButton);
    CHECK(viewport->horizontalScrollBar()->value() > 0);
    QTest::mouseClick(previous, Qt::LeftButton);
    CHECK(viewport->horizontalScrollBar()->value() == 0);
    angle->setFocus(Qt::TabFocusReason);
    QCoreApplication::processEvents();
    CHECK(viewport->horizontalScrollBar()->value() > 0);
    checkControls();
    const auto narrowCanvas = workspace->canvasContainer()->geometry();
    moveAction->trigger();
    QCoreApplication::processEvents();
    // Move now includes its Transform entry button. The same overflow system
    // applies if those controls exceed this deliberately narrow viewport.
    const bool moveOverflow = viewport->horizontalScrollBar()->maximum() > 0;
    CHECK(previous->isVisible() == moveOverflow);
    CHECK(next->isVisible() == moveOverflow);
    brushAction->trigger();
    QCoreApplication::processEvents();
    CHECK(workspace->canvasContainer()->geometry() == narrowCanvas);
    CHECK(bar->height() == barGeometry.height());
    window.resize(1500, 900);
    QCoreApplication::processEvents();
    CHECK(!previous->isVisible());
    CHECK(!next->isVisible());
    checkControls();
    const auto previewPath = qEnvironmentVariable("IMAGEEDITOR_TEST_OPTIONS_PREVIEW");
    if (!previewPath.isEmpty()) {
        CHECK(bar->grab().save(previewPath));
    }
}

void layerControlsAndToolRailDockingStayCoherent()
{
    imageeditor::ui::MainWindow window(nullptr, false, true);
    window.setUnsavedPromptEnabled(false);
    window.resize(1200, 760);
    window.show();
    QCoreApplication::processEvents();
    auto* layerControls = window.findChild<QWidget*>(
        QStringLiteral("LayerControls"));
    auto* layerList = window.findChild<QListView*>(
        QStringLiteral("LayerList"));
    auto* blendMode = window.findChild<QComboBox*>(
        QStringLiteral("LayerBlendModeCombo"));
    auto* deleteButton = window.findChild<QPushButton*>(
        QStringLiteral("DeleteLayerButton"));
    auto* canvasFps = window.findChild<QLabel*>(
        QStringLiteral("CanvasFpsStatus"));
    CHECK(layerControls != nullptr);
    CHECK(layerList != nullptr);
    CHECK(blendMode != nullptr);
    CHECK(deleteButton != nullptr);
    CHECK(canvasFps != nullptr);
    CHECK(canvasFps && canvasFps->text().endsWith(QStringLiteral(" FPS")));
    CHECK(blendMode && blendMode->currentText() == QStringLiteral("Normal"));
    CHECK(blendMode && blendMode->isEnabled());
    CHECK(blendMode && blendMode->count() == int(imageeditor::core::allBlendModes.size()));
    CHECK(deleteButton && !deleteButton->isEnabled());
    if (layerControls && layerList
        && layerControls->parentWidget() == layerList->parentWidget()) {
        auto* layout = qobject_cast<QVBoxLayout*>(
            layerControls->parentWidget()->layout());
        CHECK(layout != nullptr);
        CHECK(layout && layout->indexOf(layerControls) < layout->indexOf(layerList));
    } else {
        CHECK(false);
    }

    auto* toolRail = window.findChild<QToolBar*>(QStringLiteral("ToolRail"));
    auto* workspace = dynamic_cast<imageeditor::ui::OverlayDockWorkspace*>(
        window.findChild<QWidget*>(QStringLiteral("CanvasWorkspace")));
    auto* layersPanel = dynamic_cast<imageeditor::ui::WorkspacePanel*>(
        window.findChild<QWidget*>(QStringLiteral("LayersPanel")));
    auto* propertiesPanel = dynamic_cast<imageeditor::ui::WorkspacePanel*>(
        window.findChild<QWidget*>(QStringLiteral("PropertiesPanelShell")));
    auto* colorPanel = dynamic_cast<imageeditor::ui::WorkspacePanel*>(
        window.findChild<QWidget*>(QStringLiteral("ColorPanelShell")));
    auto* adjustmentsPanel = dynamic_cast<imageeditor::ui::WorkspacePanel*>(
        window.findChild<QWidget*>(QStringLiteral("AdjustmentsPanelShell")));
    auto* colorButton = window.findChild<QPushButton*>(
        QStringLiteral("ForegroundColorButton"));
    auto* colorPanelAction = window.findChild<QAction*>(
        QStringLiteral("ColorPanelVisibilityAction"));
    auto* leftAction = window.findChild<QAction*>(
        QStringLiteral("ToolRailDockLeftAction"));
    auto* rightAction = window.findChild<QAction*>(
        QStringLiteral("ToolRailDockRightAction"));
    auto* topAction = window.findChild<QAction*>(
        QStringLiteral("ToolRailDockTopAction"));
    auto* bottomAction = window.findChild<QAction*>(
        QStringLiteral("ToolRailDockBottomAction"));
    CHECK(toolRail != nullptr);
    CHECK(workspace != nullptr);
    CHECK(layersPanel != nullptr);
    CHECK(propertiesPanel != nullptr);
    CHECK(colorPanel != nullptr);
    CHECK(adjustmentsPanel != nullptr);
    CHECK(colorButton != nullptr);
    CHECK(colorPanelAction != nullptr);
    CHECK(leftAction != nullptr);
    CHECK(rightAction != nullptr);
    CHECK(topAction != nullptr);
    CHECK(bottomAction != nullptr);
    if (!toolRail || !workspace || !layersPanel || !propertiesPanel
        || !colorPanel || !adjustmentsPanel || !colorButton || !colorPanelAction
        || !leftAction || !rightAction || !topAction || !bottomAction) {
        return;
    }

    CHECK(colorPanelAction->isChecked());
    CHECK(colorButton->text().contains(QStringLiteral("#4F73FF")));
    CHECK(workspace->panelPlacement(colorPanel)
        == imageeditor::ui::OverlayDockWorkspace::PanelPlacement::DockedRight);
    CHECK(workspace->dockedPanelIndex(colorPanel) == 0);
    CHECK(colorPanel->minimumHeight() == 120);
    CHECK(colorPanel->maximumHeight() == QWIDGETSIZE_MAX);
    CHECK(layersPanel->minimumHeight() == 260);
    CHECK(propertiesPanel->minimumHeight() == 200);

    const QRect fixedWorkspaceGeometry = workspace->geometry();
    const QRect fixedCanvasGeometry = workspace->canvasContainer()->geometry();
    ResizeCounter canvasResizeCounter;
    workspace->canvasContainer()->installEventFilter(&canvasResizeCounter);

    const auto verifyToolRailPlacement = [&](QAction* action,
                                             imageeditor::ui::OverlayDockWorkspace::ToolRailPlacement placement,
                                             Qt::Orientation orientation,
                                             QWidget* expectedParent) {
        action->trigger();
        QCoreApplication::processEvents();
        CHECK(workspace->hasToolRail(toolRail));
        CHECK(workspace->toolRailPlacement() == placement);
        CHECK(toolRail->parentWidget() == expectedParent);
        CHECK(toolRail->orientation() == orientation);
        CHECK(action->isChecked());
        CHECK(window.toolBarArea(toolRail) == Qt::NoToolBarArea);
        CHECK(!toolRail->isWindow());
        CHECK(workspace->panelOverlay()->isAncestorOf(toolRail));
        CHECK(workspace->geometry() == fixedWorkspaceGeometry);
        CHECK(workspace->canvasContainer()->geometry() == fixedCanvasGeometry);
        CHECK(canvasResizeCounter.count == 0);
        const QPoint railCenter = toolRail->mapTo(
            workspace->panelOverlay(), toolRail->rect().center());
        CHECK(workspace->panelOverlay()->windowHandle()
            && workspace->panelOverlay()->windowHandle()->mask().contains(
                railCenter));
    };

    verifyToolRailPlacement(rightAction,
        imageeditor::ui::OverlayDockWorkspace::ToolRailPlacement::Right,
        Qt::Vertical, workspace->rightPanelCard());

    workspace->floatPanel(layersPanel, QRect {160, 80, 300, 260});
    workspace->floatPanel(propertiesPanel, QRect {480, 110, 320, 300});
    workspace->floatPanel(colorPanel, QRect {820, 140, 300, 220});
    // The narrow-right-column assertion below means every regular panel has
    // been detached, including the newly added Adjustments workspace panel.
    workspace->floatPanel(adjustmentsPanel, QRect {320, 380, 360, 310});
    QCoreApplication::processEvents();
    CHECK(!layersPanel->isWindow());
    CHECK(!propertiesPanel->isWindow());
    CHECK(!colorPanel->isWindow());
    CHECK(!adjustmentsPanel->isWindow());
    CHECK(layersPanel->parentWidget() == workspace->panelOverlay());
    CHECK(propertiesPanel->parentWidget() == workspace->panelOverlay());
    CHECK(colorPanel->parentWidget() == workspace->panelOverlay());
    CHECK(adjustmentsPanel->parentWidget() == workspace->panelOverlay());
    CHECK(colorPanel->minimumHeight() == 120);
    CHECK(colorPanel->maximumHeight() == QWIDGETSIZE_MAX);
    CHECK(workspace->leftPanelCard()->isHidden());
    CHECK(!workspace->rightPanelCard()->isHidden());
    CHECK(workspace->rightPanelCard()->width() == toolRail->width());

    workspace->dockPanel(
        layersPanel, imageeditor::ui::OverlayDockWorkspace::PanelDockSide::Left);
    workspace->dockPanel(colorPanel,
        imageeditor::ui::OverlayDockWorkspace::PanelDockSide::Right, 0);
    workspace->dockPanel(propertiesPanel,
        imageeditor::ui::OverlayDockWorkspace::PanelDockSide::Right, 1);
    workspace->dockPanel(adjustmentsPanel,
        imageeditor::ui::OverlayDockWorkspace::PanelDockSide::Right, 2);
    QCoreApplication::processEvents();
    CHECK(workspace->panelPlacement(layersPanel)
        == imageeditor::ui::OverlayDockWorkspace::PanelPlacement::DockedLeft);
    CHECK(workspace->panelPlacement(propertiesPanel)
        == imageeditor::ui::OverlayDockWorkspace::PanelPlacement::DockedRight);
    CHECK(workspace->panelPlacement(colorPanel)
        == imageeditor::ui::OverlayDockWorkspace::PanelPlacement::DockedRight);
    CHECK(workspace->dockedPanelIndex(colorPanel) == 0);
    CHECK(workspace->dockedPanelIndex(propertiesPanel) == 1);
    CHECK(colorPanel->minimumHeight() == 120);
    CHECK(colorPanel->maximumHeight() == QWIDGETSIZE_MAX);

    colorPanelAction->trigger();
    QCoreApplication::processEvents();
    CHECK(!workspace->panelVisible(colorPanel));
    CHECK(!colorPanelAction->isChecked());
    CHECK(workspace->canvasContainer()->geometry() == fixedCanvasGeometry);
    CHECK(canvasResizeCounter.count == 0);
    colorPanelAction->trigger();
    QCoreApplication::processEvents();
    CHECK(workspace->panelVisible(colorPanel));
    CHECK(colorPanelAction->isChecked());
    CHECK(workspace->canvasContainer()->geometry() == fixedCanvasGeometry);
    CHECK(canvasResizeCounter.count == 0);

    const std::array transitions {
        std::tuple {topAction,
            imageeditor::ui::OverlayDockWorkspace::ToolRailPlacement::Top,
            Qt::Horizontal, workspace->panelOverlay()},
        std::tuple {leftAction,
            imageeditor::ui::OverlayDockWorkspace::ToolRailPlacement::Left,
            Qt::Vertical, workspace->leftPanelCard()},
        std::tuple {bottomAction,
            imageeditor::ui::OverlayDockWorkspace::ToolRailPlacement::Bottom,
            Qt::Horizontal, workspace->panelOverlay()},
        std::tuple {rightAction,
            imageeditor::ui::OverlayDockWorkspace::ToolRailPlacement::Right,
            Qt::Vertical, workspace->rightPanelCard()},
        std::tuple {topAction,
            imageeditor::ui::OverlayDockWorkspace::ToolRailPlacement::Top,
            Qt::Horizontal, workspace->panelOverlay()},
        std::tuple {bottomAction,
            imageeditor::ui::OverlayDockWorkspace::ToolRailPlacement::Bottom,
            Qt::Horizontal, workspace->panelOverlay()},
        std::tuple {leftAction,
            imageeditor::ui::OverlayDockWorkspace::ToolRailPlacement::Left,
            Qt::Vertical, workspace->leftPanelCard()},
    };
    for (const auto& [action, placement, orientation, parent] : transitions) {
        verifyToolRailPlacement(action, placement, orientation, parent);
    }

    workspace->canvasContainer()->removeEventFilter(&canvasResizeCounter);
}

void dockedPanelSizesPersistAcrossWindows()
{
    QTemporaryDir settingsDirectory;
    CHECK(settingsDirectory.isValid());
    if (!settingsDirectory.isValid()) {
        return;
    }
    QCoreApplication::setOrganizationName(
        QStringLiteral("ImageEditorPanelPersistenceTests"));
    QCoreApplication::setApplicationName(
        QStringLiteral("ImageEditorPanelPersistenceTests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
        settingsDirectory.path());

    imageeditor::ui::UiLayoutConfig config;
    config.color = {100, 120};
    int savedColorHeight = 0;
    {
        imageeditor::ui::MainWindow window(nullptr, true, false, config);
        window.setUnsavedPromptEnabled(false);
        window.resize(1200, 900);
        window.show();
        QCoreApplication::processEvents();
        auto* workspace = dynamic_cast<imageeditor::ui::OverlayDockWorkspace*>(
            window.findChild<QWidget*>(QStringLiteral("CanvasWorkspace")));
        auto* splitter = workspace
            ? workspace->rightPanelCard()->findChild<QSplitter*>(
                  QStringLiteral("RightPanelSplitter"))
            : nullptr;
        auto* colorPanel = dynamic_cast<imageeditor::ui::WorkspacePanel*>(
            window.findChild<QWidget*>(QStringLiteral("ColorPanelShell")));
        CHECK(splitter != nullptr);
        CHECK(colorPanel != nullptr);
        if (splitter && colorPanel) {
            // Set an actual pixel height without assuming the historical
            // three-panel right column. QSplitter rescales setSizes weights
            // to fill its available space; an obsolete extra entry made the
            // requested 100 pixels grow to Color's 120-pixel maximum once
            // Layers moved to the left column.
            auto sizes = splitter->sizes();
            const auto colorIndex = splitter->indexOf(colorPanel);
            CHECK(colorIndex >= 0 && sizes.size() >= 2);
            if (colorIndex >= 0 && sizes.size() >= 2) {
                const auto recipient = colorIndex == 0 ? 1 : 0;
                sizes[recipient] += sizes[colorIndex] - 100;
                sizes[colorIndex] = 100;
                splitter->setSizes(sizes);
            }
            QCoreApplication::processEvents();
            savedColorHeight = colorPanel->height();
            CHECK(savedColorHeight == 100);
        }
        window.close();
        QCoreApplication::processEvents();
    }

    {
        imageeditor::ui::MainWindow window(nullptr, true, false, config);
        window.setUnsavedPromptEnabled(false);
        window.show();
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        auto* colorPanel = dynamic_cast<imageeditor::ui::WorkspacePanel*>(
            window.findChild<QWidget*>(QStringLiteral("ColorPanelShell")));
        CHECK(colorPanel != nullptr);
        CHECK(colorPanel && colorPanel->height() == savedColorHeight);
        window.close();
        QCoreApplication::processEvents();
    }
}

void defaultPanelLayoutIsSessionOnly()
{
    using Window = imageeditor::ui::MainWindow;
    using Workspace = imageeditor::ui::OverlayDockWorkspace;
    using Panel = imageeditor::ui::WorkspacePanel;
    QTemporaryDir directory;
    CHECK(directory.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("VulkanaPanelDefaultTests"));
    QCoreApplication::setApplicationName(QStringLiteral("VulkanaPanelDefaultTests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    const auto settle = [] {
        for (int i = 0; i < 3; ++i) QCoreApplication::processEvents();
    };
    const auto workspace = [](Window& w) {
        return dynamic_cast<Workspace*>(w.findChild<QWidget*>(QStringLiteral("CanvasWorkspace")));
    };
    const auto panel = [](Window& w, const char* name) {
        return dynamic_cast<Panel*>(w.findChild<QWidget*>(QString::fromLatin1(name)));
    };
    const auto layoutSettings = [] {
        QSettings settings;
        QMap<QString, QVariant> result;
        for (const auto& key : settings.allKeys()) {
            if ((key.startsWith("window/") && key != "window/geometry")
                || key.startsWith("view/rulers/")) result.insert(key, settings.value(key));
        }
        return result;
    };
    imageeditor::ui::UiLayoutConfig config;
    config.color = {100, QWIDGETSIZE_MAX};
    QSettings().setValue("editor/colors-v2/primary", QColor(14, 25, 36));
    {
        Window saved(nullptr, true, false, config);
        saved.setUnsavedPromptEnabled(false);
        saved.resize(1400, 850);
        saved.show();settle();
        auto* ws = workspace(saved);
        ws->setPanelWidth(480);
        ws->setLeftPanelWidth(350);
        ws->floatPanel(panel(saved, "ColorPanelShell"), {380, 100, 370, 210});
        ws->dockPanel(panel(saved, "LayersPanel"), Workspace::PanelDockSide::Right, 0);
        ws->setPanelVisible(panel(saved, "PropertiesPanelShell"), false);
        ws->setRulerFarEdge(Qt::Horizontal, true);
        ws->setRulerVisible(Qt::Vertical, false);
        saved.findChild<QAction*>(QStringLiteral("ToolRailDockTopAction"))->trigger();
        settle();saved.close();settle();
    }
    const auto original = layoutSettings();
    CHECK(!original.isEmpty());
    QSize defaultColorSize, restoredWindowSize;
    int defaultRightWidth = 0, defaultLeftWidth = 0;
    {
        Window fresh(nullptr, false, false, config);
        fresh.setUnsavedPromptEnabled(false);
        // Qt constrains restored geometry to the available screen, including
        // the smaller offscreen test display. Compare the same outer size.
        CHECK(fresh.restoreGeometry(QSettings().value("window/geometry").toByteArray()));
        fresh.show();settle();
        restoredWindowSize = fresh.size();
        defaultColorSize = panel(fresh, "ColorPanelShell")->size();
        defaultRightWidth = workspace(fresh)->panelWidth();
        defaultLeftWidth = workspace(fresh)->leftPanelWidth();
        fresh.close();settle();
    }
    {
        Window trial(nullptr, true, false, config, nullptr, Window::PanelLayoutMode::SessionDefaults);
        trial.setUnsavedPromptEnabled(false);
        trial.show();settle();
        auto* ws = workspace(trial);
        CHECK(trial.size() == restoredWindowSize);
        CHECK(ws->panelWidth() == defaultRightWidth && ws->leftPanelWidth() == defaultLeftWidth);
        CHECK(ws->panelPlacement(panel(trial, "LayersPanel")) == Workspace::PanelPlacement::DockedLeft);
        for (const auto& [name, order] : std::array{
                 std::pair{"ColorPanelShell", 0}, std::pair{"PropertiesPanelShell", 1},
                 std::pair{"AdjustmentsPanelShell", 2}}) {
            auto* item = panel(trial, name);
            CHECK(ws->panelVisible(item));
            CHECK(ws->panelPlacement(item) == Workspace::PanelPlacement::DockedRight);
            CHECK(ws->dockedPanelIndex(item) == order);
        }
        CHECK(panel(trial, "ColorPanelShell")->size() == defaultColorSize);
        CHECK(panel(trial, "ColorPanelShell")->minimumHeight() == 100);
        CHECK(panel(trial, "ColorPanelShell")->maximumHeight() == QWIDGETSIZE_MAX);
        CHECK(ws->toolRailPlacement() == Workspace::ToolRailPlacement::Left);
        CHECK(ws->rulerVisible(Qt::Vertical) && !ws->rulerFarEdge(Qt::Horizontal));
        CHECK(trial.editorSession().colors().primary == imageeditor::core::Rgba8(14, 25, 36, 255));
        trial.resize(1500, 940);settle();
        CHECK(ws->panelWidth() == 556 && ws->leftPanelWidth() == 305);
        ws->setPanelWidth(400);
        ws->floatPanel(panel(trial, "LayersPanel"), {100, 200, 320, 310});
        trial.findChild<QAction*>(QStringLiteral("ToolRailDockBottomAction"))->trigger();
        settle();trial.close();settle();
    }
    CHECK(layoutSettings() == original);
    {
        Window restored(nullptr, true, false, config);
        restored.setUnsavedPromptEnabled(false);
        restored.show();settle();
        auto* ws = workspace(restored);
        CHECK(ws->panelWidth() == 480 && ws->leftPanelWidth() == 350);
        CHECK(ws->panelPlacement(panel(restored, "ColorPanelShell")) == Workspace::PanelPlacement::Floating);
        CHECK(ws->floatingPanelGeometry(panel(restored, "ColorPanelShell")) == QRect(380, 100, 370, 210));
        CHECK(!ws->panelVisible(panel(restored, "PropertiesPanelShell")));
        CHECK(ws->panelPlacement(panel(restored, "LayersPanel")) == Workspace::PanelPlacement::DockedRight);
        CHECK(ws->toolRailPlacement() == Workspace::ToolRailPlacement::Top);
        CHECK(!ws->rulerVisible(Qt::Vertical) && ws->rulerFarEdge(Qt::Horizontal));
        restored.close();settle();
    }
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    groovePressSetsAbsoluteValueAndContinuesDragging();
    absolutePositionHonorsInvertedAppearance();
    sliderWheelInputIsLeftForPanelScrolling();
    imageeditor::ui::applyEditorTheme(application);
    compactValueControlCombinesSliderTextAndSteps();
    layerOpacityUsesCompactAdjustmentsAndOneUndoPerInteraction();
    rapidLayerEyeClicksToggleEveryTime();
    toolShortcutsSpanCanvasAndOverlayWindows();
    brushWorkspaceUsesOneStableTopOptionsBar();
    layerControlsAndToolRailDockingStayCoherent();
    dockedPanelSizesPersistAcrossWindows();
    defaultPanelLayoutIsSessionOnly();

    if (failures != 0) {
        std::cerr << failures << " UI interaction assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All UI interaction tests passed\n";
    return EXIT_SUCCESS;
}
