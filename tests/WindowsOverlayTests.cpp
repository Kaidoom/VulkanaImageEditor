#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/WorkspaceDialog.hpp"
#include "imageeditor/ui/ColorDialog.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "NativeGraphicsStartup.hpp"

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QImage>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QVulkanInstance>
#include <QWindow>
#include <windows.h>
#include <cstdlib>
#include <iostream>

namespace ui = imageeditor::ui;
int failures = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { ++failures; std::cerr << __LINE__ << ": " << #__VA_ARGS__ << '\n'; } } while (false)

// Use desktop pixels, not QWidget::grab(): the latter hides native compositor
// failures by reading the correctly transparent Qt backing store directly.
QImage capture(QWidget& window, const QString& name)
{
    QTest::qWait(250);
    const QPoint global = window.mapToGlobal(QPoint());
    const auto image = window.screen()->grabWindow(0, global.x(), global.y(),
        window.width(), window.height()).toImage();
    const auto output = qEnvironmentVariable("VULKANA_OVERLAY_CAPTURE_DIR");
    if (!output.isEmpty()) {
        QDir().mkpath(output);
        CHECK(image.save(output + '/' + name + ".png"));
    }
    CHECK(!image.isNull());
    return image;
}

QColor pixel(const QImage& image, const QPoint& point)
{
    return image.pixelColor(point * image.devicePixelRatio());
}

bool closeColor(QColor actual, QColor expected, int tolerance = 3)
{
    return std::abs(actual.red() - expected.red()) <= tolerance
        && std::abs(actual.green() - expected.green()) <= tolerance
        && std::abs(actual.blue() - expected.blue()) <= tolerance;
}

HWND hitWindow(QWidget& host, QPoint point)
{
    POINT native {qRound(point.x() * host.devicePixelRatioF()), qRound(point.y() * host.devicePixelRatioF())};
    ClientToScreen(reinterpret_cast<HWND>(host.winId()), &native);
    return WindowFromPoint(native);
}

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("ImageEditorTests");
    QCoreApplication::setApplicationName("WindowsOverlayTests");
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir temp;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path());
    ui::applyEditorTheme(app);
    QVulkanInstance vulkan;
    vulkan.setApiVersion(QVersionNumber(1, 2));
    vulkan.setLayers({"VK_LAYER_KHRONOS_validation"});
    if (!vulkan.create()) return EXIT_FAILURE;
    vulkan.installDebugOutputFilter([](auto severity, auto types, const void*) {
        if (types.testFlag(QVulkanInstance::ValidationMessage)
            && (severity.testFlag(QVulkanInstance::WarningSeverity)
                || severity.testFlag(QVulkanInstance::ErrorSeverity))) ++failures;
        return false;
    });
    QImage source(800, 600, QImage::Format_RGB32);
    source.fill(QColor(214, 132, 62));
    const auto path = temp.filePath("reference.png");
    CHECK(source.save(path));
    ui::MainWindow window(&vulkan, false, false);
    window.setUnsavedPromptEnabled(false);
    window.resize(1200, 800);
    window.show();
    window.raise();
    window.activateWindow();
    CHECK(imageeditor::test::waitForGraphicsStartup(window));
    CHECK(window.openImageFromPath(path));
    auto* workspace = dynamic_cast<ui::OverlayDockWorkspace*>(window.findChild<QWidget*>("CanvasWorkspace"));
    if (!workspace) return EXIT_FAILURE;
    auto* plane = workspace->panelOverlay();
    auto* native = plane->windowHandle();
    if (!native) return EXIT_FAILURE;
    const auto hwnd = reinterpret_cast<HWND>(native->winId());
    std::cout << "Overlay alpha=" << native->format().alphaBufferSize()
        << " layered=" << bool(GetWindowLongPtr(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED)
        << " child=" << bool(GetWindowLongPtr(hwnd, GWL_STYLE) & WS_CHILD) << '\n';
    CHECK(GetWindowLongPtr(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED);
    CHECK(GetWindowLongPtr(hwnd, GWL_STYLE) & WS_CHILD);

    const auto normal = capture(window, "panels");
    for (const char* name : {"OverlayLeftPanelResizeHandle", "OverlayPanelResizeHandle"}) {
        auto* handle = workspace->findChild<QWidget*>(name);
        CHECK(handle && handle->isVisible());
        if (!handle || !handle->isVisible()) continue;
        // Canvas-facing edge of the transparent handle, away from its grip.
        const bool right = QByteArray(name) == "OverlayPanelResizeHandle";
        const int x = right ? 0 : handle->width() - 1;
        const auto point = window.mapFromGlobal(handle->mapToGlobal(QPoint(x, handle->height() / 2)));
        // The uniform fixture extends beneath both handles at mid-height.
        // Hiding the plane is not a valid reference: the
        // workspace automatically restores that native surface on layout.
        const QColor expected(214, 132, 62);
        std::cout << name << " at " << point.x() << ',' << point.y()
            << " " << pixel(normal, point).name().toStdString()
            << " underlying=" << expected.name().toStdString() << '\n';
        CHECK(closeColor(pixel(normal, point), expected));
        CHECK(hitWindow(window, point) == hwnd);
    }

    // Exercise the shared shield independently of card layout, including full
    // input coverage, repeated opening, and restoration on dismissal.
    const QPoint probe = window.mapFromGlobal(workspace->mapToGlobal(QPoint(workspace->width() / 2, 70)));
    const QColor original = pixel(normal, probe);
    CHECK(original.red() > 20 || original.green() > 20 || original.blue() > 20);
    for (int cycle = 0; cycle < 2; ++cycle) {
        ui::WorkspaceDialog shield(*workspace, window);
        QDialog card(&shield, Qt::Widget);
        card.setFixedSize(260, 160);
        shield.open(card);
        const auto dimmed = capture(window, "modal-" + QString::number(cycle));
        const QColor expected(qRound(original.red() * 143.0 / 255.0),
            qRound(original.green() * 143.0 / 255.0), qRound(original.blue() * 143.0 / 255.0));
        std::cout << "Modal " << pixel(dimmed, probe).name().toStdString()
            << " expected=" << expected.name().toStdString() << '\n';
        CHECK(closeColor(pixel(dimmed, probe), expected));
        CHECK(native->mask().contains(workspace->rect()));
        CHECK(hitWindow(window, probe) == hwnd);
        card.reject();
        const auto restored = capture(window, "restored-" + QString::number(cycle));
        CHECK(closeColor(pixel(restored, probe), original));
        CHECK(!workspace->hasModalOverlay());
    }
    auto* picker = window.findChild<QPushButton*>("RailPrimaryColor");
    CHECK(picker);
    if (picker) {
        picker->click();
        auto* dialog = window.findChild<ui::ColorDialog*>("WorkingColorDialog");
        CHECK(dialog && dialog->isVisible() && !dialog->isWindow());
        const auto colorCard = capture(window, "color-picker");
        CHECK(pixel(colorCard, probe).red() > 10);
        if (dialog) dialog->reject();
    }
    window.logRendererDiagnostics();
    window.close();
    std::cout << "Native overlay pixels: " << (failures ? "FAILED" : "passed") << '\n';
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
