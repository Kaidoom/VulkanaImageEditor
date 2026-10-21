#include "imageeditor/ui/BackgroundRemoval.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>
#include <array>
#include <atomic>
#include <cmath>
#include <stdexcept>

namespace {
void expect(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

QByteArray varint(quint64 value)
{
    QByteArray result;
    do {
        result.append(char((value & 127) | (value > 127 ? 128 : 0)));
        value >>= 7;
    } while (value);
    return result;
}

QByteArray number(int field, quint64 value)
{
    return varint(quint64(field) << 3) + varint(value);
}

QByteArray bytes(int field, const QByteArray& value)
{
    return varint((quint64(field) << 3) | 2) + varint(quint64(value.size())) + value;
}

QByteArray tensorInfo(const QByteArray& name, std::array<int, 4> shape)
{
    QByteArray dimensions;
    for (const auto dimension : shape)
        dimensions += bytes(1, number(1, quint64(dimension)));
    return bytes(1, name) + bytes(2, bytes(1, number(1, 1) + bytes(2, dimensions)));
}

// A tiny, self-contained ONNX ReduceMean graph. Fixture inputs deliberately use
// non-square metadata instead of the adapters' dynamic-shape defaults. No
// research weights, Python, protobuf library or network access is required.
QByteArray testModel(int height = 2, int width = 3)
{
    const auto axes = bytes(1, "axes") + number(8, 1) + number(20, 7);
    const auto keepdims = bytes(1, "keepdims") + number(3, 1) + number(20, 2);
    const auto node = bytes(1, "image") + bytes(2, "coverage") + bytes(4, "ReduceMean")
        + bytes(5, axes) + bytes(5, keepdims);
    const auto graph = bytes(1, node) + bytes(2, "background-removal-adapter-test")
        + bytes(11, tensorInfo("image", { 1, 3, height, width }))
        + bytes(12, tensorInfo("coverage", { 1, 1, height, width }));
    return number(1, 9) + bytes(6, QByteArray(256, ' ')) + bytes(7, graph) + bytes(8, number(2, 13));
}

void writeFile(const QString& root, const QString& relative, const QByteArray& content)
{
    const auto path = QDir(root).filePath(relative);
    expect(QDir().mkpath(QFileInfo(path).absolutePath()), "Cannot create test fixture directory");
    QFile file(path);
    expect(file.open(QIODevice::WriteOnly) && file.write(content) == content.size(), "Cannot write test model");
}

int at(const QImage& image, int x, int y) { return image.constScanLine(y)[x]; }

QImage ramp(int width, int height)
{
    QImage image(width, height, QImage::Format_RGBA8888);
    for (int y = 0; y < height; ++y) {
        auto* row = image.scanLine(y);
        for (int x = 0; x < width; ++x) {
            const auto value = uchar((y * width + x) * 255 / (width * height - 1));
            row[4 * x] = row[4 * x + 1] = row[4 * x + 2] = value;
            row[4 * x + 3] = 255;
        }
    }
    return image;
}
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    using imageeditor::ui::BackgroundRemovalEngine;
    try {
        QTemporaryDir fixture;
        expect(fixture.isValid(), "Cannot create test directory");
        std::atomic_bool cancel { false };
        const auto source = ramp(3, 2);
        const auto savedSource = source.copy();
        expect(BackgroundRemovalEngine::models().front().id == QStringLiteral("u2netp"),
            "Missing files must not change the default model");
        BackgroundRemovalEngine engine(fixture.path());
        expect(!engine.evaluate("u2netp", source, cancel).error.isEmpty(), "Missing model accepted");
        expect(!engine.evaluate("unknown", source, cancel).error.isEmpty(), "Unknown model accepted");
        expect(!engine.evaluate("u2netp", {}, cancel).error.isEmpty(), "Null raster accepted");
        writeFile(fixture.path(), "u2netp.onnx",
            "version https://git-lfs.github.com/spec/v1\noid sha256:012345\nsize 999999\n");
        const auto pointer = engine.evaluate("u2netp", source, cancel);
        expect(pointer.mask.isNull() && pointer.error.contains("Git LFS pointer"), "LFS pointer not identified");
        cancel.store(true);
        const auto canceled = engine.evaluate("u2netp", source, cancel);
        expect(canceled.cancelled && canceled.error.isEmpty() && canceled.mask.isNull(), "Cancellation failed");
        cancel.store(false);
        writeFile(fixture.path(), "u2netp.onnx", testModel());
        const auto result = engine.evaluate("u2netp", source, cancel);
        if (!result.error.isEmpty())
            throw std::runtime_error(result.error.toStdString());
        expect(result.mask.size() == source.size() && result.mask.format() == QImage::Format_Grayscale8,
            "Incorrect output mask format or dimensions");
        expect(result.details.contains("[1, 3, 2, 3]"), "Graph dimensions were not used");
        expect(source == savedSource, "Inference changed the input pixels");
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 3; ++x)
                expect(std::abs(at(result.mask, x, y) - (y * 3 + x) * 51) <= 1, "U2 min/max mismatch");
        const auto repeated = engine.evaluate("u2netp", source, cancel);
        expect(repeated.mask == result.mask, "Reused model session changed output");
        QImage black(source.size(), source.format());
        black.fill(Qt::black);
        const auto empty = engine.evaluate("u2netp", black, cancel);
        expect(empty.mask.isNull() && empty.error.contains("empty foreground"), "Constant map was applied");
        writeFile(fixture.path(), "birefnet-lite-512/onnx/model_int8.onnx", QByteArray(1024, 'x'));
        const auto invalid = engine.evaluate("birefnet-lite-int8", source, cancel);
        expect(invalid.mask.isNull() && !invalid.error.isEmpty(), "Corrupt graph accepted");
        expect(engine.evaluate("u2netp", source, cancel).mask == result.mask,
            "Failed model prevented subsequent successful model");
        writeFile(fixture.path(), "birefnet-lite-512/onnx/model_int8.onnx", testModel());
        const auto biref = engine.evaluate("birefnet-lite-int8", source, cancel);
        expect(biref.error.isEmpty(), "BiRefNet adapter failed");
        constexpr std::array<float, 3> means { .485f, .456f, .406f }, stds { .229f, .224f, .225f };
        for (int y = 0; y < 2; ++y) {
            for (int x = 0; x < 3; ++x) {
                const float value = float((y * 3 + x) * 51) / 255.f;
                float normalized = 0;
                for (size_t c = 0; c < 3; ++c)
                    normalized += (value - means[c]) / stds[c] / 3.f;
                const int expected = int(255.f / (1.f + std::exp(-normalized)));
                expect(std::abs(at(biref.mask, x, y) - expected) <= 1, "BiRefNet sigmoid/normalization mismatch");
            }
        }
        expect(at(biref.mask, 0, 0) > 0 && at(biref.mask, 2, 1) < 255, "BiRefNet unexpectedly min/max stretched");
        for (const auto size : { QSize(3, 2), QSize(3, 13), QSize(17, 2) }) {
            const auto portrait = ramp(size.width(), size.height());
            const auto int8 = engine.evaluate("birefnet-lite-int8", portrait, cancel);
            expect(int8.error.isEmpty() && int8.mask.size() == size, "INT8 portrait/landscape dimensions mismatch");
            expect(int8.mask == engine.evaluate("birefnet-lite-int8", portrait, cancel).mask,
                "Repeated INT8 profile evaluation changed the mask");
            expect(at(int8.mask, 0, size.height() - 1) > at(int8.mask, size.width() - 1, 0),
                "BiRefNet width/height transpose");
        }
        for (const auto* path : { "unregistered.onnx", "unused/model.onnx" })
            writeFile(fixture.path(), QString::fromUtf8(path), testModel());
        const auto models = BackgroundRemovalEngine::models();
        expect(models.size() == 2, "Unregistered models appear in the comparison");
        const std::array ids { "u2netp", "birefnet-lite-int8" };
        for (size_t i = 0; i < ids.size(); ++i)
            expect(models[i].id == QString::fromUtf8(ids[i]), "Incorrect default or comparison order");
        for (const auto* id : { "unregistered", "unused" })
            expect(engine.evaluate(QString::fromUtf8(id), source, cancel).error.contains("Unknown"),
                "Unregistered model can be invoked");
        QTextStream(stdout) << "Background-removal adapters: metadata, normalization, soft masks, "
                               "portrait/landscape, cancellation, empty/LFS/corrupt and retry tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        QTextStream(stderr) << error.what() << '\n';
        return 1;
    }
}
