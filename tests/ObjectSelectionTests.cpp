#include "imageeditor/platform/ApplicationPaths.hpp"
#include "imageeditor/ui/ObjectSelection.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QTemporaryDir>
#include <iostream>
#include <sys/resource.h>
#include <thread>
namespace c = imageeditor::core;
namespace u = imageeditor::ui;
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    int failures = 0;
    auto require = [&](bool value, const char* text) {
        if (!value) {
            ++failures;
            std::cerr << text << '\n';
        }
    };
    std::atomic_bool cancelled { true };
    QTemporaryDir missing;
    u::ObjectSelectionEngine absent(missing.path());
    auto dummy = std::make_shared<c::SmartReferenceImage>();
    require(!absent.evaluate(dummy, { }, cancelled), "Pre-cancel must not load runtime");
    dummy->extent = { 4, 4 };
    dummy->pixels.assign(16, { 20, 30, 40, 255 });
    dummy->valid.assign(16, 1);
    cancelled = false;
    bool rejected = false;
    try {
        (void)absent.evaluate(dummy, { { 0, 0, 4, 4 }, { } }, cancelled);
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "Missing model pack must fail clearly");
    rejected = false;
    try {
        (void)absent.evaluate({ }, { { 0, 0, 4, 4 }, { } }, cancelled);
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "Missing reference must be rejected safely");
    rejected = false;
    try {
        (void)absent.evaluate(dummy, { { 10, 10, 4, 4 }, { } }, cancelled);
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "Off-canvas box must be rejected safely");
    if (argc < 4) {
        // No configuration, environment override, Python or first-use download.
        qputenv("VULKANA_OBJECT_SELECTION_PACK", "/not-a-model-directory");
        auto fixture = std::make_shared<c::SmartReferenceImage>();
        fixture->extent = { 128, 128 };
        fixture->valid.assign(128 * 128, 1);
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 128; ++x)
                fixture->pixels.push_back(x >= 32 && x < 96 && y >= 24 && y < 104
                        ? c::Rgba8 { 225, 45, 35, 255 }
                        : c::Rgba8 { 30, 110, 185, 255 });
        u::ObjectSelectionEngine bundled;
        const u::ObjectSelectionPrompt rectangle { { 24, 16, 80, 96 }, { } };
        const auto result = bundled.evaluate(fixture, rectangle, cancelled);
        require(result && result->coverageAtDocumentPixel(64, 64) == 255
                && result->coverageAtDocumentPixel(2, 2) == 0,
            "Bundled model must select the prompted generated object offline");
        const auto repeated = bundled.evaluate(fixture, rectangle, cancelled);
        require(result && repeated && result->equivalent(*repeated) && bundled.encodedImageCount() == 1,
            "Bundled prompts must reuse image features");
        std::cout << "Bundled CPU inference passed: "
                  << imageeditor::platform::objectSelectionBundlePath().toStdString() << '\n';
        return failures ? 1 : 0;
    }
    QImage image(argv[2]);
    require(!image.isNull(), "Reference missing");
    if (image.isNull())
        return 1;
    image = image.convertToFormat(QImage::Format_RGBA8888);
    auto source = std::make_shared<c::SmartReferenceImage>();
    source->extent = { unsigned(image.width()), unsigned(image.height()) };
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const auto* p = image.constScanLine(y) + 4 * x;
            source->pixels.push_back({ p[0], p[1], p[2], p[3] });
            source->valid.push_back(1);
        }
    u::ObjectSelectionEngine engine(QByteArray(argv[1]) == "--bundled"
            ? imageeditor::platform::objectSelectionBundlePath()
            : QString::fromLocal8Bit(argv[1]));
    u::ObjectSelectionPrompt prompt { { 80, 400, 690, double(image.height() - 400) }, { } };
    QElapsedTimer timer;
    timer.start();
    auto mask = engine.evaluate(source, prompt, cancelled);
    std::cout << "cold_total_ms=" << timer.elapsed() << '\n';
    require(bool(mask), "Missing object mask");
    QDir output(QString::fromLocal8Bit(argv[3]));
    output.mkpath(".");
    const auto save = [&](const char* name, const c::SelectionState& m) {
        QImage gray(image.size(), QImage::Format_Grayscale8), overlay = image.copy();
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                auto a = m ? m->coverageAtDocumentPixel(x, y) : 0;
                gray.scanLine(y)[x] = std::uint8_t(a);
                if (a < 128)
                    for (int k = 0; k < 3; ++k)
                        overlay.scanLine(y)[4 * x + k] /= 5;
            }
        gray.save(output.filePath(QString::fromLatin1(name) + "-mask.png"));
        overlay.save(output.filePath(QString::fromLatin1(name) + "-overlay.png"));
    };
    save("native-object", mask);
    timer.restart();
    auto repeat = engine.evaluate(source, prompt, cancelled);
    std::cout << "cached_prompt_ms=" << timer.elapsed() << '\n';
    require(mask && repeat && mask->equivalent(*repeat), "Repeated prompts differ");
    require(engine.encodedImageCount() == 1, "Prompt should reuse features");
    prompt.corrections = { { { 490, 1510 }, true }, { { 787, 1270 }, false } };
    timer.restart();
    auto corrected = engine.evaluate(source, prompt, cancelled);
    save("native-corrected", corrected);
    std::cout << "corrected_prompt_ms=" << timer.elapsed() << '\n';
    require(engine.encodedImageCount() == 1, "Correction encoded image again");
    require(corrected && corrected->coverageAtDocumentPixel(490, 1510) > 127
            && corrected->coverageAtDocumentPixel(787, 1270) == 0,
        "Corrections ignored");
    cancelled = true;
    require(!engine.evaluate(source, prompt, cancelled), "Cancelled prompt published result");
    cancelled = false;
    auto changed = std::make_shared<c::SmartReferenceImage>(*source);
    (void)engine.evaluate(changed, prompt, cancelled);
    require(engine.encodedImageCount() == 2, "New source did not invalidate features");
    auto next = std::make_shared<c::SmartReferenceImage>(*source);
    std::jthread cancel([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        cancelled = true;
    });
    timer.restart();
    require(!engine.evaluate(next, prompt, cancelled), "In-flight cancellation published");
    std::cout << "cancel_ms=" << timer.elapsed() << '\n';
    cancel.join();
    rusage usage { };
    getrusage(RUSAGE_SELF, &usage);
    std::cout << "peak_rss_mib=" << double(usage.ru_maxrss) / 1024 << '\n';
    return failures ? 1 : 0;
}
