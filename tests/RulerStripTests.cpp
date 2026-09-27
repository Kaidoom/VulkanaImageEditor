#include "imageeditor/ui/RulerStrip.hpp"
#include "imageeditor/ui/Theme.hpp"

#include <QApplication>
#include <QFocusEvent>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTest>

#include <cstdlib>
#include <iostream>

namespace {
namespace core = imageeditor::core;
namespace ui = imageeditor::ui;
int failures = 0;
void check(bool condition, const char* expression, int line)
{
    if (!condition) { std::cerr << "FAIL " << line << ": " << expression << '\n'; ++failures; }
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

struct PaintCounter : QObject {
    int count {0};
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::Paint) ++count;
        return false;
    }
};

QImage render(QWidget& widget)
{
    QImage image(widget.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    widget.render(&image);
    return image;
}

void moveMouse(ui::RulerStrip& strip, QPoint local, Qt::MouseButtons buttons)
{
    QMouseEvent event(QEvent::MouseMove, QPointF(local), QPointF(strip.mapToGlobal(local)),
        Qt::NoButton, buttons, Qt::NoModifier);
    QApplication::sendEvent(&strip, &event);
}

void paintingTracksViewportInsteadOfStripPosition(QApplication& application)
{
    QWidget workspace;
    workspace.resize(800, 600);
    QWidget canvas(&workspace);
    canvas.setGeometry(workspace.rect());
    ui::RulerStrip horizontal(ui::RulerStrip::Axis::Horizontal, &workspace);
    ui::RulerStrip vertical(ui::RulerStrip::Axis::Vertical, &workspace);
    horizontal.setGeometry(100, 35, 600, ui::RulerStrip::thickness);
    vertical.setGeometry(45, 100, ui::RulerStrip::thickness, 450);
    const core::Extent2d document {400, 400}, viewport {800, 600};
    core::ViewportState view;
    horizontal.setView(view, document, viewport, {}, core::Vec2d {50, 50});
    vertical.setView(view, document, viewport, {}, core::Vec2d {50, 50});
    workspace.show();
    application.processEvents();
    const auto originalCanvas = canvas.geometry();
    const auto first = render(horizontal);
    CHECK(horizontal.focusPolicy() == Qt::NoFocus);
    CHECK(vertical.focusPolicy() == Qt::NoFocus);
    CHECK(horizontal.height() == ui::RulerStrip::thickness);
    CHECK(vertical.width() == ui::RulerStrip::thickness);
    CHECK(first.pixelColor(40, 17) == ui::themeColor(ui::ThemeColor::Surface));
    CHECK(first.pixelColor(180, 17) == ui::themeColor(ui::ThemeColor::Control));
    // Canvas origin x=200, marker doc x=50 -> workspace x=250 -> strip x=150.
    CHECK(first.pixelColor(150, 20) == ui::themeColor(ui::ThemeColor::Accent));
    horizontal.move(150, 35);
    const auto relocated = render(horizontal);
    CHECK(relocated.pixelColor(100, 20) == ui::themeColor(ui::ThemeColor::Accent));
    CHECK(canvas.geometry() == originalCanvas);
    CHECK(view.pan() == core::Vec2d());
    CHECK(view.zoom() == 1);

    // Bounds interval is independent of the pointer and normal tick labels.
    horizontal.setView(view, document, viewport,
        core::DocumentBounds {{60, 30}, {140, 160}}, core::Vec2d {50, 50});
    const auto interval = render(horizontal);
    CHECK(interval.pixelColor(130, 17) != relocated.pixelColor(130, 17));
    CHECK(interval.pixelColor(250, 17) == relocated.pixelColor(250, 17));

    horizontal.setTicksTowardStart(true);
    horizontal.move(100, 550);
    const auto bottom = render(horizontal);
    CHECK(bottom.pixelColor(150, 3) == ui::themeColor(ui::ThemeColor::Accent));
    vertical.setTicksTowardStart(true);
    const auto right = render(vertical);
    // Origin y=100, marker y=50 -> strip y=50.
    CHECK(right.pixelColor(3, 50) == ui::themeColor(ui::ThemeColor::Accent));
    CHECK(canvas.geometry() == originalCanvas);

    view.setPan({-700, -400});
    view.setZoom(1.25);
    horizontal.setView(view, document, viewport, {}, core::Vec2d {900, 400});
    vertical.setView(view, document, viewport);
    CHECK(!render(horizontal).isNull());
    CHECK(!render(vertical).isNull());
    horizontal.hide();
    vertical.hide();
    CHECK(canvas.geometry() == originalCanvas);

    // Optional actual-widget review image, no generated mock controls.
    const auto path = qEnvironmentVariable("IMAGEEDITOR_RULER_REVIEW");
    if (!path.isEmpty()) {
        QImage sheet(640, 135, QImage::Format_ARGB32_Premultiplied);
        sheet.fill(ui::themeColor(ui::ThemeColor::Background));
        QPainter painter(&sheet);
        painter.drawImage(20, 15, first);
        painter.drawImage(20, 50, interval);
        painter.drawImage(20, 85, bottom);
        painter.end();
        CHECK(sheet.save(path));
    }
}

void noIdlePaintOrUnchangedViewUpdates(QApplication& application)
{
    QWidget workspace;
    workspace.resize(800, 500);
    ui::RulerStrip strip(ui::RulerStrip::Axis::Horizontal, &workspace);
    strip.resize(600, ui::RulerStrip::thickness);
    core::ViewportState view;
    const core::Extent2d doc {400, 300}, viewport {800, 500};
    const core::DocumentBounds bounds {{10, 20}, {80, 120}};
    const core::Vec2d pointer {25, 70};
    strip.setView(view, doc, viewport, bounds, pointer);
    workspace.show();
    application.processEvents();
    PaintCounter paints;
    strip.installEventFilter(&paints);
    for (int n = 0; n != 50; ++n) strip.setView(view, doc, viewport, bounds, pointer);
    application.processEvents();
    CHECK(paints.count == 0);
    QTest::qWait(35);
    CHECK(paints.count == 0);
    view.panBy({1, 0});
    strip.setView(view, doc, viewport, bounds, pointer);
    application.processEvents();
    CHECK(paints.count == 1);
}

void onlyTheGripDragsAndEveryCancellationFinishesOnce(QApplication& application)
{
    QWidget workspace;
    workspace.resize(800, 600);
    ui::RulerStrip strip(ui::RulerStrip::Axis::Horizontal, &workspace);
    strip.setGeometry(30, 30, 600, ui::RulerStrip::thickness);
    workspace.show();
    application.processEvents();
    int starts = 0, moves = 0, finishes = 0, cancelled = 0;
    strip.onDragStarted = [&](QPoint position) {
        ++starts;
        CHECK(position == strip.mapToGlobal(QPoint(8, 10)));
    };
    strip.onDragMoved = [&](QPoint) { ++moves; };
    strip.onDragFinished = [&](QPoint, bool cancel) {
        ++finishes;
        if (cancel) ++cancelled;
        CHECK(!strip.isDragging());
    };
    const auto begin = [&] {
        QTest::mousePress(&strip, Qt::LeftButton, Qt::NoModifier, QPoint(8, 10));
        moveMouse(strip, {9, 10}, Qt::LeftButton);
        CHECK(!strip.isDragging());
        moveMouse(strip, {100, 100}, Qt::LeftButton);
        CHECK(strip.isDragging());
    };
    QTest::mousePress(&strip, Qt::LeftButton, Qt::NoModifier, QPoint(100, 10));
    moveMouse(strip, {200, 200}, Qt::LeftButton);
    QTest::mouseRelease(&strip, Qt::LeftButton, Qt::NoModifier, QPoint(200, 200));
    CHECK(starts == 0 && finishes == 0);
    begin();
    QTest::mouseRelease(&strip, Qt::LeftButton, Qt::NoModifier, QPoint(300, 300));
    CHECK(starts == 1 && moves == 1 && finishes == 1 && cancelled == 0);
    CHECK(!strip.isDragging());
    for (const auto type : {QEvent::FocusOut, QEvent::WindowDeactivate, QEvent::TouchCancel}) {
        begin();
        if (type == QEvent::FocusOut) {
            QFocusEvent event(QEvent::FocusOut);
            QApplication::sendEvent(&strip, &event);
        } else {
            QEvent event(type);
            QApplication::sendEvent(&strip, &event);
        }
        CHECK(!strip.isDragging());
        // A pending physical release cannot finish again.
        QTest::mouseRelease(&strip, Qt::LeftButton, Qt::NoModifier, QPoint(300, 300));
    }
    CHECK(finishes == 4 && cancelled == 3);
    begin();
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&strip, &escape);
    CHECK(!strip.isDragging());
    CHECK(escape.isAccepted());
    CHECK(finishes == 5 && cancelled == 4);
    begin();
    strip.hide();
    CHECK(!strip.isDragging());
    CHECK(finishes == 6 && cancelled == 5);
    CHECK(QWidget::mouseGrabber() != &strip);
}
}

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    ui::applyEditorTheme(application);
    paintingTracksViewportInsteadOfStripPosition(application);
    noIdlePaintOrUnchangedViewUpdates(application);
    onlyTheGripDragsAndEveryCancellationFinishesOnce(application);
    ui::ThemeSettings light;
    light.preset = ui::ThemePreset::Light;
    ui::applyEditorTheme(application, light);
    paintingTracksViewportInsteadOfStripPosition(application);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
