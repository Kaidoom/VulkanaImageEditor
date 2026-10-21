// Relocated, offline production-path check. No model-directory argument is
// accepted: the normal engine must find and verify the installed bundle.
#include "imageeditor/platform/ApplicationPaths.hpp"
#include "imageeditor/ui/BackgroundRemoval.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextStream>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <stdexcept>

namespace {
void expect(bool condition, const QString& message)
{
    if (!condition)
        throw std::runtime_error(message.toStdString());
}

QImage sample()
{
    QImage image(128, 128, QImage::Format_RGBA8888);
    for (int y = 0; y < image.height(); ++y) {
        auto* row = image.scanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            const bool foreground = x >= 35 && x < 93 && y >= 20 && y < 108;
            row[x * 4] = foreground ? 220 : 235;
            row[x * 4 + 1] = foreground ? uchar(55 + y / 4) : 240;
            row[x * 4 + 2] = foreground ? 40 : 248;
            row[x * 4 + 3] = 255;
        }
    }
    return image;
}

void verify(const imageeditor::ui::BackgroundRemovalResult& result, const QImage& source,
    const QString& id)
{
    expect(result.error.isEmpty(), id + QStringLiteral(": ") + result.error);
    expect(!result.cancelled && !result.mask.isNull(), id + QStringLiteral(": missing mask"));
    expect(result.mask.size() == source.size() && result.mask.format() == QImage::Format_Grayscale8,
        id + QStringLiteral(": wrong mask dimensions/format"));
    bool nonempty = false;
    for (int y = 0; y < result.mask.height(); ++y) {
        const auto* row = result.mask.constScanLine(y);
        nonempty |= std::any_of(row, row + result.mask.width(), [](uchar value) { return value != 0; });
    }
    expect(nonempty, id + QStringLiteral(": empty foreground mask"));
    for (const double time : { result.timings.initializationMs, result.timings.preprocessingMs,
             result.timings.inferenceMs, result.timings.postprocessingMs, result.timings.totalMs })
        expect(std::isfinite(time) && time >= 0, id + QStringLiteral(": invalid timing"));
    expect(!result.details.isEmpty(), id + QStringLiteral(": missing runtime metadata"));
    QTextStream(stdout) << "PASS " << id << " initialization=" << result.timings.initializationMs
        << " ms inference=" << result.timings.inferenceMs << " ms\n";
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("VulkanaTests"));
    QCoreApplication::setApplicationName(QStringLiteral("BundledBackgroundRemoval"));
    QStandardPaths::setTestModeEnabled(true);
    try {
        const auto arguments = app.arguments();
        const bool runtimeError = arguments.size() == 2
            && (arguments[1] == QStringLiteral("--expect-runtime-error")
                || arguments[1] == QStringLiteral("--expect-runtime-checksum-error"));
        const bool modelError = arguments.size() == 3
            && (arguments[1] == QStringLiteral("--expect-model-error")
                || arguments[1] == QStringLiteral("--expect-model-checksum-error"));
        expect(arguments.size() == 1 || runtimeError || modelError, QStringLiteral("Invalid probe arguments"));
        const bool checksumError = arguments.size() > 1 && arguments[1].contains(QStringLiteral("checksum"));
        const QString failedModel = modelError ? arguments[2] : QString{};
        const auto installed = QDir(QCoreApplication::applicationDirPath())
            .absoluteFilePath(QStringLiteral("../share/vulkana-editor"));
        const auto expectedModels = QDir::cleanPath(installed + QStringLiteral("/background-removal/v1"));
        const auto expectedRuntime = QDir::cleanPath(installed + QStringLiteral("/object-selection/mobilesam-v1"));
        // Obsolete research overrides must not rescue absent packaged assets.
        qputenv("VULKANA_BACKGROUND_REMOVAL_MODELS", "/not-a-background-removal-bundle");
        qputenv("VULKANA_OBJECT_SELECTION_PACK", "/not-an-object-selection-bundle");
        expect(imageeditor::platform::backgroundRemovalBundlePath() == expectedModels,
            QStringLiteral("Model lookup escaped the relocated installation"));
        expect(imageeditor::platform::objectSelectionBundlePath() == expectedRuntime,
            QStringLiteral("Runtime lookup escaped the relocated installation"));
        for (const auto* name : { "encoder.onnx", "decoder.onnx" })
            expect(!QFileInfo::exists(QDir(expectedRuntime).filePath(QString::fromLatin1(name))),
                QStringLiteral("Background removal unexpectedly includes MobileSAM weights"));

        const auto models = imageeditor::ui::BackgroundRemovalEngine::models();
        const std::array ids { QStringLiteral("u2netp"), QStringLiteral("birefnet-lite-int8") };
        expect(models.size() == ids.size(), QStringLiteral("Incorrect bundled model count"));
        for (std::size_t index = 0; index < ids.size(); ++index)
            expect(models[index].id == ids[index], QStringLiteral("Incorrect default or model order"));
        expect(!modelError || std::find(ids.begin(), ids.end(), failedModel) != ids.end(),
            QStringLiteral("Unknown expected model failure"));

        auto order = ids;
        // Fail first, then load the healthy graph with the same engine. This
        // catches an initialization failure poisoning subsequent model changes.
        if (modelError && failedModel == order[1])
            std::swap(order[0], order[1]);
        const auto image = sample();
        const auto original = image.copy();
        std::atomic_bool cancel { false };
        imageeditor::ui::BackgroundRemovalEngine engine;
        for (const auto& id : order) {
            const auto result = engine.evaluate(id, image, cancel);
            if (runtimeError || (modelError && id == failedModel)) {
                expect(!result.error.isEmpty() && result.mask.isNull() && !result.cancelled,
                    id + QStringLiteral(": missing/corrupt bundle data was accepted"));
                expect(!checksumError || result.error.contains(QStringLiteral("checksum mismatch")),
                    id + QStringLiteral(": damaged data was not rejected by the integrity check: ") + result.error);
                QTextStream(stdout) << "PASS rejected " << id << ": " << result.error << '\n';
            } else {
                verify(result, image, id);
            }
            expect(image == original, QStringLiteral("Inference modified the source image"));
        }
        QTextStream(stdout) << "PASS relocated offline background removal\n";
        return 0;
    } catch (const std::exception& error) {
        QTextStream(stderr) << error.what() << '\n';
        return 1;
    }
}
