#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/LayerListView.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/QtShapeRenderService.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "NativeGraphicsStartup.hpp"
#include <QApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QVulkanInstance>
#include <iostream>
namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace r = imageeditor::render;
namespace {
int failures = 0;
#define CHECK(value) do { if (!(value)) { ++failures; std::cerr << "FAIL " << __LINE__ << ": " << #value << '\n'; } } while(false)
QByteArray digest(const c::RasterSurface& source)
{
    const auto size = source.extent(); std::vector<std::byte> bytes(std::size_t(size.width) * size.height * 4);
    source.copyRgba8({0, 0, int(size.width), int(size.height)}, bytes, std::size_t(size.width) * 4);
    return QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char*>(bytes.data()), qsizetype(bytes.size())), QCryptographicHash::Sha256);
}
c::Layer shapeLayer(std::string name, double width, double height, bool styled = false)
{
    c::ShapeLayer shape; shape.kind = c::ShapeKind::RoundedRectangle; shape.size = {width, height};
    shape.cornerRadius = 28; shape.strokeEnabled = true; shape.strokeWidth = 1.5;
    auto layer = c::Layer::shape(std::move(name), shape);
    if (styled) {
        auto effects = std::make_shared<c::LayerEffectStack>();
        effects->items[std::size_t(c::LayerEffectType::GradientOverlay)].enabled = true;
        layer.effects = effects;
    }
    return layer;
}
struct Fixture {
    u::MainWindow window;
    r::CanvasWindow* canvas {};
    bool native {};
    QStringList warnings;
    explicit Fixture(QVulkanInstance* instance) : window(instance, false, false), native(instance != nullptr)
    {
        window.setUnsavedPromptEnabled(false); window.resize(1400, 900); window.show();
        CHECK(imageeditor::test::waitForGraphicsStartup(window));
        for (auto* w : QGuiApplication::allWindows())
            if (auto* found = dynamic_cast<r::CanvasWindow*>(w)) canvas = found;
        CHECK(canvas);
        QObject::connect(window.statusBar(), &QStatusBar::messageChanged, &window, [this](const QString& message) {
            if (message.contains("editable data is retained")) warnings.append(message);
        });
    }
    ~Fixture() { window.close(); QCoreApplication::processEvents(); }
    c::Document& doc() { return *const_cast<c::EditorSession&>(window.editorSession()).document(); }
    void zoom(double value)
    {
        canvas->setZoomAt(value, QPointF(canvas->width() * .5, canvas->height() * .5));
        // Offscreen fixtures have no drawable surface to publish the frame's
        // viewport callback. Drive that production hook explicitly there.
        if (!native) canvas->onViewportUpdated();
        QTest::qWait(120);
    }
    void select(int row)
    {
        auto* list = dynamic_cast<u::LayerListView*>(window.findChild<QListView*>("LayerList")); CHECK(list);
        if (list) list->onRowSelectionRequested(row, {});
        QCoreApplication::processEvents();
    }
    std::uint64_t residentPixels()
    {
        std::uint64_t sum = 0;
        for (const auto& layer : doc().layers()) if (std::holds_alternative<c::ShapeLayer>(layer.payload) && layer.renderCache) {
            const auto size = layer.renderCache->surface->extent(); sum += std::uint64_t(size.width) * size.height;
            CHECK(size.width <= 8192 && size.height <= 8192);
        }
        return sum;
    }
};
void verifyProject(const QString& path, QVulkanInstance* instance)
{
    QFile file(path); CHECK(file.open(QIODevice::ReadOnly)); const auto original = QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256); file.close();
    Fixture f(instance); CHECK(f.window.openImageFromPath(path));
    const auto content = f.doc().contentState(), revision = f.doc().revision();
    const auto history = f.window.editorSession().history().undoDepth();
    std::map<c::LayerId, QByteArray> sources;
    for (const auto& layer : f.doc().layers()) if (const auto* shape = std::get_if<c::ShapeLayer>(&layer.payload);
        shape && c::hasActiveLayerEffects(layer.effects)) {
        const auto exact = u::QtShapeRenderService{}.render({*shape, 1, 16ULL * 1024 * 1024});
        sources.emplace(layer.id, digest(*exact->surface));
    }
    CHECK(!sources.empty());
    for (double zoom : {.35, .78, 1.0, 1.2, 2.0, 4.0, .78}) {
        f.zoom(zoom); f.select(0); f.select(1);
        for (const auto& [id, expected] : sources) {
            const auto* layer = f.doc().layer(id); CHECK(layer->renderCache);
            if (!layer->renderCache) continue;
            CHECK(layer->renderCache->density == 1 && !layer->renderCache->rasterizedDocumentTransform);
            CHECK(layer->renderCache->contentRevision == layer->shapeRevision);
            CHECK(digest(*layer->renderCache->surface) == expected);
        }
        CHECK(f.residentPixels() <= 64ULL * 1024 * 1024);
    }
    CHECK(f.warnings.isEmpty());
    for (const auto& warning : f.warnings) std::cerr << warning.toStdString() << '\n';
    CHECK(f.doc().contentState() == content && f.doc().revision() == revision);
    CHECK(f.window.editorSession().history().undoDepth() == history && !f.doc().isModified());
    CHECK(file.open(QIODevice::ReadOnly)); CHECK(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256) == original);
    std::cout << "Verified project: " << f.doc().layers().size() << " layers, " << sources.size()
              << " full-resolution effect sources, " << f.warnings.size() << " warnings\n";
}
void syntheticRegression(QVulkanInstance* instance)
{
    QTemporaryDir dir; c::Document document({{1024,1024},96});
    CHECK(document.insertLayer(0, shapeLayer("Large styled frame", 928, 928, true)));
    for (int i = 0; i < 119; ++i) CHECK(document.insertLayer(document.layers().size(), shapeLayer("Small detail", 6, 6)));
    const auto path = dir.filePath("many-shapes.vulkana"); CHECK(u::saveProject(path, document));
    verifyProject(path, instance);

    // A low configured cache pool constrains previews without reducing effect
    // sources. The many small shapes do not receive wasteful equal reservations.
    QSettings settings; settings.setValue("preferences/performance/documentCacheMiB", 64);
    c::Document pressure({{1024,1024},96});
    CHECK(pressure.insertLayer(0, shapeLayer("Styled frame", 928, 928, true)));
    for (int i = 0; i < 5; ++i) CHECK(pressure.insertLayer(pressure.layers().size(), shapeLayer("Large preview", 1024, 1024)));
    const auto pressurePath = dir.filePath("pressure.vulkana"); CHECK(u::saveProject(pressurePath, pressure));
    {
        Fixture f(instance); CHECK(f.window.openImageFromPath(pressurePath)); f.zoom(4);
        CHECK(f.residentPixels() <= 16ULL * 1024 * 1024);
        const auto& styled = f.doc().layers().front(); CHECK(styled.renderCache && styled.renderCache->density == 1);
        for (const auto& layer : f.doc().layers()) CHECK(layer.renderCache);
        const auto before = f.doc().layers()[1].renderCache;
        f.select(0); f.select(1); f.zoom(3.5);
        if (f.doc().layers()[1].renderCache != before) {
            const auto& after = f.doc().layers()[1].renderCache;
            std::cerr << "Cache stability: " << before->requestedDensity << '/' << before->density << " "
                << before->surface->extent().width << 'x' << before->surface->extent().height << " -> "
                << after->requestedDensity << '/' << after->density << " " << after->surface->extent().width
                << 'x' << after->surface->extent().height << " zoom=" << f.canvas->zoom() << '\n';
        }
        CHECK(f.doc().layers()[1].renderCache == before);
        CHECK(f.warnings.isEmpty());
    }
    settings.remove("preferences/performance/documentCacheMiB");

    c::Document limits({{1024,1024},96});
    CHECK(limits.insertLayer(0, shapeLayer("Oversized styled banner", 20000, 100, true)));
    CHECK(limits.insertLayer(1, shapeLayer("Other shape", 20, 20)));
    const auto limitPath = dir.filePath("limit.vulkana"); CHECK(u::saveProject(limitPath, limits));
    Fixture f(instance); CHECK(f.window.openImageFromPath(limitPath));
    CHECK(f.warnings.size() == 1);
    if (!f.warnings.isEmpty()) {
        CHECK(f.warnings[0].contains("Oversized styled banner") && f.warnings[0].contains(QString::number(f.doc().layers().front().id)));
        CHECK(f.warnings[0].contains("preview limit") && !f.warnings[0].contains("Not enough memory"));
    }
    for (double zoom : {.5, 1.2, .78}) { f.zoom(zoom); f.select(0); f.select(1); }
    CHECK(f.warnings.size() == 1);
    const auto id = f.doc().layers().front().id;
    auto shape = std::get<c::ShapeLayer>(f.doc().layer(id)->payload); shape.size = {200,100};
    CHECK(f.doc().setLayerShape(id, shape)); f.zoom(1.2); f.select(0); f.select(1);
    CHECK(f.doc().layer(id)->renderCache && f.doc().layer(id)->renderCache->density == 1);
    shape.size = {20000,100}; CHECK(f.doc().setLayerShape(id, shape)); f.zoom(.78); f.select(0); f.select(1);
    CHECK(f.warnings.size() == 2); // Recovery resets suppression for a later failure.
}
}
int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv); app.setApplicationName("ShapeCacheBudgetTests"); app.setOrganizationName("ImageEditorTests");
    QStandardPaths::setTestModeEnabled(true); QTemporaryDir config;
    QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, config.path());
    u::applyEditorTheme(app);
    QVulkanInstance vulkan; const bool native = app.arguments().contains("--native");
    if (native) { vulkan.setApiVersion(QVersionNumber(1, 2)); CHECK(vulkan.create()); if (!vulkan.isValid()) return 1; }
    const auto project = app.arguments().indexOf("--project");
    if (project >= 0 && project + 1 < app.arguments().size()) verifyProject(app.arguments()[project + 1], native ? &vulkan : nullptr);
    else syntheticRegression(native ? &vulkan : nullptr);
    std::cout << "Shape cache budget: " << failures << " failures\n"; return failures ? 1 : 0;
}
