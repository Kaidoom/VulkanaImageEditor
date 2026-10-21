#include "imageeditor/ui/BackgroundRemoval.hpp"
#include "imageeditor/platform/ApplicationPaths.hpp"
#include "imageeditor/platform/PerformanceSettings.hpp"
#include "imageeditor/ui/Localization.hpp"
#include "../third_party/onnxruntime/onnxruntime_c_api.h"

#include <QDir>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QScopeGuard>
#include <QStringList>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <span>
#include <stdexcept>
#include <thread>

namespace imageeditor::ui {
namespace {
enum class InputScale { ImageMaximum, UnitRange };
enum class OutputScale { MinMax, Sigmoid };
enum class Filter { Bilinear, Lanczos };

struct Profile {
    const char* id;
    const char* name;
    const char* path;
    const char* sha256;
    int dynamicSize;
    InputScale input;
    Filter resize;
    OutputScale output;
};

// The adapters intentionally differ: rembg divides by the resized image's
// maximum, while BiRefNet expects ImageNet-normalized /255 and emits logits.
// The INT8 graph keeps floating-point inputs/outputs; quantization is internal.
constexpr std::array profiles {
    Profile { "u2netp", "U²-NetP", "u2netp.onnx",
        "309c8469258dda742793dce0ebea8e6dd393174f89934733ecc8b14c76f4ddd8", 320, InputScale::ImageMaximum,
        Filter::Lanczos, OutputScale::MinMax },
    Profile { "birefnet-lite-int8", "BiRefNet-lite INT8", "birefnet-lite-512/onnx/model_int8.onnx",
        "aec1e9eca3ec401f82a32911d9e8369a24bae8ae490168308f768ddd5a936da2",
        512, InputScale::UnitRange, Filter::Bilinear, OutputScale::Sigmoid }
};

void require(bool condition, const QString& message)
{
    if (!condition)
        throw std::runtime_error(message.toStdString());
}

struct Cancelled { };
void checkCancellation(const std::atomic_bool& cancel)
{
    if (cancel.load(std::memory_order_relaxed))
        throw Cancelled { };
}

void verifyChecksum(QFile& file, const QByteArray& expected, const std::atomic_bool& cancel)
{
    require(expected.size() == 64 && file.seek(0), QStringLiteral("Invalid bundled model/runtime checksum record."));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        checkCancellation(cancel);
        hash.addData(file.read(1024 * 1024));
        require(file.error() == QFileDevice::NoError, QStringLiteral("Could not read bundled model/runtime data."));
    }
    require(hash.result().toHex() == expected,
        QStringLiteral("Bundled file checksum mismatch: %1. Repair the Vulkana installation.").arg(QFileInfo(file).fileName()));
}

double milliseconds(const QElapsedTimer& timer)
{
    return double(timer.nsecsElapsed()) / 1.0e6;
}

std::size_t pixelIndex(int x, int y, int width)
{
    return std::size_t(y) * std::size_t(width) + std::size_t(x);
}

struct Weights {
    int start { };
    std::vector<float> values;
};

std::vector<Weights> resizeWeights(int source, int destination, Filter filter)
{
    std::vector<Weights> result(static_cast<std::size_t>(destination));
    const double scale = double(source) / destination;
    const double filterScale = std::max(1.0, scale);
    const double support = (filter == Filter::Lanczos ? 3.0 : 1.0) * filterScale;
    auto kernel = [filter](double value) {
        value = std::abs(value);
        if (filter == Filter::Bilinear)
            return std::max(0.0, 1.0 - value);
        if (value >= 3.0)
            return 0.0;
        if (value < 1.0e-12)
            return 1.0;
        const double x = value * std::numbers::pi;
        return std::sin(x) * std::sin(x / 3.0) / (x * x / 3.0);
    };
    for (int x = 0; x < destination; ++x) {
        auto& weights = result[static_cast<std::size_t>(x)];
        const double center = (x + .5) * scale;
        weights.start = std::max(0, int(std::floor(center - support + .5)));
        const int end = std::min(source, int(std::floor(center + support + .5)));
        weights.values.resize(static_cast<std::size_t>(end - weights.start));
        double sum = 0;
        for (int i = weights.start; i < end; ++i) {
            const double value = kernel((i + .5 - center) / filterScale);
            weights.values[static_cast<std::size_t>(i - weights.start)] = float(value);
            sum += value;
        }
        for (auto& weight : weights.values)
            weight = float(weight / sum);
    }
    return result;
}

// Separable, antialiased RGB/coverage resampling with the Pillow conventions
// used by the reference preprocessors (pixel centres, truncated edge support).
QImage resizeBytes(const QImage& source, QSize size, Filter filter, const std::atomic_bool& cancel)
{
    if (source.size() == size)
        return source;
    const int channels = source.format() == QImage::Format_Grayscale8 ? 1 : 3;
    const auto horizontal = resizeWeights(source.width(), size.width(), filter);
    const auto vertical = resizeWeights(source.height(), size.height(), filter);
    QImage temp(size.width(), source.height(), source.format());
    QImage result(size, source.format());
    require(!temp.isNull() && !result.isNull(), QStringLiteral("Not enough memory to resize the model input."));
    for (int y = 0; y < source.height(); ++y) {
        checkCancellation(cancel);
        const auto* input = source.constScanLine(y);
        auto* output = temp.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            const auto& weights = horizontal[static_cast<std::size_t>(x)];
            for (int channel = 0; channel < channels; ++channel) {
                float value = 0;
                for (std::size_t i = 0; i < weights.values.size(); ++i)
                    value += input[(weights.start + int(i)) * channels + channel] * weights.values[i];
                output[x * channels + channel] = static_cast<uchar>(std::clamp(std::lround(value), 0L, 255L));
            }
        }
    }
    for (int y = 0; y < size.height(); ++y) {
        checkCancellation(cancel);
        const auto& weights = vertical[static_cast<std::size_t>(y)];
        auto* output = result.scanLine(y);
        for (int x = 0; x < size.width() * channels; ++x) {
            float value = 0;
            for (std::size_t i = 0; i < weights.values.size(); ++i)
                value += temp.constScanLine(weights.start + int(i))[x] * weights.values[i];
            output[x] = static_cast<uchar>(std::clamp(std::lround(value), 0L, 255L));
        }
    }
    return result;
}

std::vector<float> resizeCoverage(std::span<const float> source, QSize from, QSize to,
    const std::atomic_bool& cancel)
{
    if (from == to)
        return { source.begin(), source.end() };
    std::vector<float> result(std::size_t(to.width()) * std::size_t(to.height()));
    for (int y = 0; y < to.height(); ++y) {
        checkCancellation(cancel);
        const double fy = std::max(0.0, (y + .5) * from.height() / to.height() - .5);
        const int y0 = std::min(int(fy), from.height() - 1);
        const int y1 = std::min(y0 + 1, from.height() - 1);
        const float wy = float(fy - y0);
        for (int x = 0; x < to.width(); ++x) {
            const double fx = std::max(0.0, (x + .5) * from.width() / to.width() - .5);
            const int x0 = std::min(int(fx), from.width() - 1);
            const int x1 = std::min(x0 + 1, from.width() - 1);
            const float wx = float(fx - x0);
            const auto at = [&](int sx, int sy) { return source[pixelIndex(sx, sy, from.width())]; };
            result[pixelIndex(x, y, to.width())]
                = std::lerp(std::lerp(at(x0, y0), at(x1, y0), wx),
                    std::lerp(at(x0, y1), at(x1, y1), wx), wy);
        }
    }
    return result;
}

void minMaxNormalize(std::vector<float>& values)
{
    const auto [low, high] = std::minmax_element(values.begin(), values.end());
    const float minimum = *low;
    const float range = *high - minimum;
    // A constant map has no foreground evidence: avoid divide-by-zero/NaNs
    // and leave Apply disabled through the empty-result safeguard.
    if (!(range > 0.f)) {
        std::fill(values.begin(), values.end(), 0.f);
        return;
    }
    for (auto& value : values)
        value = std::clamp((value - minimum) / range, 0.f, 1.f);
}

QString shapeText(std::span<const int64_t> dimensions)
{
    QStringList text;
    for (const auto dimension : dimensions)
        text.push_back(QString::number(dimension));
    return QStringLiteral("[") + text.join(QStringLiteral(", ")) + QStringLiteral("]");
}

QString typeText(ONNXTensorElementDataType type)
{
    if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
        return QStringLiteral("float32");
    return QStringLiteral("type %1").arg(int(type));
}
}

struct BackgroundRemovalEngine::Impl {
    QString directory;
    bool verifyIntegrity;
    QLibrary library;
    const OrtApi* api { };
    OrtEnv* env { };
    OrtMemoryInfo* memory { };
    OrtSession* session { };
    const Profile* profile { };
    QByteArray inputName;
    QByteArray outputName;
    std::array<int64_t, 4> inputShape { };
    ONNXTensorElementDataType inputType { };
    QString metadata;
    int threads { };

    explicit Impl(QString path, bool verify) : directory(std::move(path)), verifyIntegrity(verify) { }
    ~Impl()
    {
        if (!api)
            return;
        if (session)
            api->ReleaseSession(session);
        if (memory)
            api->ReleaseMemoryInfo(memory);
        if (env)
            api->ReleaseEnv(env);
    }

    void check(OrtStatus* status) const
    {
        if (!status)
            return;
        const QString message = QString::fromUtf8(api->GetErrorMessage(status));
        api->ReleaseStatus(status);
        throw std::runtime_error(message.toStdString());
    }

    void runtime(const std::atomic_bool& cancel)
    {
        if (env && memory)
            return;
#ifdef Q_OS_WIN
        const auto runtimeName = QStringLiteral("onnxruntime.dll");
#else
        const auto runtimeName = QStringLiteral("libonnxruntime.so");
#endif
        const QDir runtimeDirectory(platform::objectSelectionBundlePath());
        QFile manifest(runtimeDirectory.filePath(QStringLiteral("vulkana-mobilesam-v1.json")));
        require(manifest.open(QIODevice::ReadOnly) && manifest.size() < 16384,
            QStringLiteral("Bundled ONNX runtime metadata is missing; repair the Vulkana installation."));
        const auto record = QJsonDocument::fromJson(manifest.readAll()).object();
        require(record[QStringLiteral("format")].toInt() == 1,
            QStringLiteral("Unsupported bundled ONNX runtime metadata."));
        QFile runtimeFile(runtimeDirectory.filePath(runtimeName));
        require(runtimeFile.open(QIODevice::ReadOnly) && runtimeFile.size() > 0 && runtimeFile.size() < 128 * 1024 * 1024,
            QStringLiteral("Bundled ONNX runtime is missing or invalid; repair the Vulkana installation."));
        verifyChecksum(runtimeFile, record[runtimeName].toString().toLatin1(), cancel);
        runtimeFile.close();
        library.setFileName(runtimeDirectory.filePath(runtimeName));
        require(library.load(), QStringLiteral("Could not load Vulkana's bundled CPU ONNX runtime: %1")
            .arg(library.errorString()));
        const auto getApi = reinterpret_cast<const OrtApiBase*(ORT_API_CALL*)()>(library.resolve("OrtGetApiBase"));
        require(getApi, QStringLiteral("The bundled ONNX runtime has no OrtGetApiBase entry point."));
        api = getApi()->GetApi(ORT_API_VERSION);
        require(api, QStringLiteral("The bundled ONNX runtime does not support the required C API."));
        if (!env)
            check(api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "Vulkana Background Removal", &env));
        check(api->DisableTelemetryEvents(env));
        check(api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory));
        metadata = QStringLiteral("ONNX Runtime %1 · CPUExecutionProvider")
                       .arg(QString::fromUtf8(getApi()->GetVersionString()));
    }

    void load(const Profile& requested, const std::atomic_bool& cancel)
    {
        if (profile == &requested && session)
            return;
        // Release the previous graph before loading another large model.
        if (session) {
            api->ReleaseSession(session);
            session = nullptr;
        }
        profile = nullptr;
        checkCancellation(cancel);
        const auto path = QDir(directory).filePath(QString::fromUtf8(requested.path));
        QFile file(path);
        require(file.open(QIODevice::ReadOnly), QStringLiteral("Cannot read model file: %1").arg(path));
        const auto prefix = file.read(256);
        require(!prefix.startsWith("version https://git-lfs.github.com/spec/"),
            QStringLiteral("This ONNX file is only a Git LFS pointer, not downloaded model data: %1").arg(path));
        require(file.size() > 256, QStringLiteral("The ONNX model file is empty or incomplete: %1").arg(path));
        if (verifyIntegrity)
            verifyChecksum(file, QByteArray(requested.sha256), cancel);
        file.close();
        runtime(cancel);
        OrtSessionOptions* options { };
        check(api->CreateSessionOptions(&options));
        const auto release = qScopeGuard([&] { api->ReleaseSessionOptions(options); });
        threads = int(platform::resolvedObjectSelectionThreads(platform::startupPerformanceSettings()));
        check(api->SetIntraOpNumThreads(options, threads));
        check(api->SetInterOpNumThreads(options, 1));
        check(api->SetSessionExecutionMode(options, ORT_SEQUENTIAL));
        check(api->DisableCpuMemArena(options));
        check(api->DisableMemPattern(options));
        check(api->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL));
        checkCancellation(cancel);
        // Session construction is synchronous in this C API, so it must stay on
        // the worker. Cancellation is checked before and immediately afterward;
        // Run itself is interruptible below.
        OrtSession* loaded { };
        const auto releaseLoaded = qScopeGuard([&] { if (loaded) api->ReleaseSession(loaded); });
#ifdef Q_OS_WIN
        check(api->CreateSession(env, path.toStdWString().c_str(), options, &loaded));
#else
        check(api->CreateSession(env, path.toLocal8Bit().constData(), options, &loaded));
#endif
        checkCancellation(cancel);
        size_t inputs { }, outputs { };
        check(api->SessionGetInputCount(loaded, &inputs));
        check(api->SessionGetOutputCount(loaded, &outputs));
        require(inputs == 1 && outputs >= 1,
            QStringLiteral("This adapter requires one image input and at least one mask output; graph has %1 / %2.")
                .arg(inputs).arg(outputs));
        OrtAllocator* allocator { };
        check(api->GetAllocatorWithDefaultOptions(&allocator));
        char* name { };
        check(api->SessionGetInputName(loaded, 0, allocator, &name));
        inputName = name;
        allocator->Free(allocator, name);
        check(api->SessionGetOutputName(loaded, 0, allocator, &name));
        outputName = name;
        allocator->Free(allocator, name);
        OrtTypeInfo* inputInfo { };
        check(api->SessionGetInputTypeInfo(loaded, 0, &inputInfo));
        const auto releaseInputInfo = qScopeGuard([&] { api->ReleaseTypeInfo(inputInfo); });
        const OrtTensorTypeAndShapeInfo* shape { };
        check(api->CastTypeInfoToTensorInfo(inputInfo, &shape));
        require(shape, QStringLiteral("The model input is not an image tensor."));
        size_t rank { };
        check(api->GetDimensionsCount(shape, &rank));
        require(rank == 4, QStringLiteral("The model input is not a four-dimensional NCHW image."));
        check(api->GetDimensions(shape, inputShape.data(), inputShape.size()));
        check(api->GetTensorElementType(shape, &inputType));
        require((inputShape[0] == 1 || inputShape[0] < 0) && inputShape[1] == 3,
            QStringLiteral("Unsupported model input layout: %1; expected [1, 3, height, width].")
                .arg(shapeText(inputShape)));
        require(inputType == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
            QStringLiteral("Unsupported model input type: %1.").arg(typeText(inputType)));
        inputShape[0] = 1;
        for (std::size_t i : { 2u, 3u }) {
            if (inputShape[i] < 0)
                inputShape[i] = requested.dynamicSize;
            require(inputShape[i] > 0 && inputShape[i] <= 4096,
                QStringLiteral("Unsupported model input resolution: %1.").arg(shapeText(inputShape)));
        }
        OrtTypeInfo* outputInfo { };
        check(api->SessionGetOutputTypeInfo(loaded, 0, &outputInfo));
        const auto releaseOutputInfo = qScopeGuard([&] { api->ReleaseTypeInfo(outputInfo); });
        const OrtTensorTypeAndShapeInfo* outputShape { };
        check(api->CastTypeInfoToTensorInfo(outputInfo, &outputShape));
        require(outputShape, QStringLiteral("The first model output is not a tensor."));
        check(api->GetDimensionsCount(outputShape, &rank));
        std::vector<int64_t> dimensions(rank);
        check(api->GetDimensions(outputShape, dimensions.data(), rank));
        ONNXTensorElementDataType outputType { };
        check(api->GetTensorElementType(outputShape, &outputType));
        metadata = QStringLiteral("CPU · %1 threads · %2 %3 %4 → %5 %6 %7 · %8 output(s)")
            .arg(threads).arg(QString::fromUtf8(inputName), typeText(inputType), shapeText(inputShape),
                QString::fromUtf8(outputName), typeText(outputType), shapeText(dimensions)).arg(outputs);
        session = loaded;
        loaded = nullptr;
        profile = &requested;
    }

    void run(const OrtValue* input, OrtValue** output, const std::atomic_bool& cancel)
    {
        OrtRunOptions* options { };
        check(api->CreateRunOptions(&options));
        const auto release = qScopeGuard([&] { api->ReleaseRunOptions(options); });
        checkCancellation(cancel);
        std::jthread monitor([&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                if (cancel.load(std::memory_order_relaxed)) {
                    if (auto* status = api->RunOptionsSetTerminate(options))
                        api->ReleaseStatus(status);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
        });
        const char* inputNames[] { inputName.constData() };
        const char* outputNames[] { outputName.constData() };
        auto* status = api->Run(session, options, inputNames, &input, 1, outputNames, 1, output);
        monitor.request_stop();
        monitor.join();
        if (cancel.load(std::memory_order_relaxed)) {
            if (status)
                api->ReleaseStatus(status);
            throw Cancelled { };
        }
        check(status);
    }

    std::pair<std::vector<float>, QSize> coverage(OrtValue* output)
    {
        require(output, QStringLiteral("The model returned no foreground mask."));
        OrtTensorTypeAndShapeInfo* info { };
        check(api->GetTensorTypeAndShape(output, &info));
        const auto release = qScopeGuard([&] { api->ReleaseTensorTypeAndShapeInfo(info); });
        size_t rank { }, count { };
        check(api->GetDimensionsCount(info, &rank));
        std::vector<int64_t> dimensions(rank);
        check(api->GetDimensions(info, dimensions.data(), rank));
        require(rank >= 2 && rank <= 4,
            QStringLiteral("Unsupported mask output shape: %1.").arg(shapeText(dimensions)));
        for (size_t i = 0; i + 2 < rank; ++i)
            require(dimensions[i] == 1, QStringLiteral("The model returned multiple masks/channels: %1.")
                .arg(shapeText(dimensions)));
        const auto height = dimensions[rank - 2], width = dimensions[rank - 1];
        require(width > 0 && height > 0 && width <= 8192 && height <= 8192,
            QStringLiteral("Invalid mask output resolution: %1.").arg(shapeText(dimensions)));
        check(api->GetTensorShapeElementCount(info, &count));
        require(count == std::size_t(width) * std::size_t(height),
            QStringLiteral("The model returned inconsistent mask dimensions."));
        ONNXTensorElementDataType type { };
        check(api->GetTensorElementType(info, &type));
        require(type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
            QStringLiteral("Unsupported mask output type: %1.").arg(typeText(type)));
        void* data { };
        check(api->GetTensorMutableData(output, &data));
        std::vector<float> values(count);
        std::copy_n(static_cast<const float*>(data), count, values.begin());
        require(std::all_of(values.begin(), values.end(), [](float v) { return std::isfinite(v); }),
            QStringLiteral("The model returned non-finite mask values; no preview can be applied."));
        return { std::move(values), QSize(int(width), int(height)) };
    }
};

BackgroundRemovalEngine::BackgroundRemovalEngine()
    : BackgroundRemovalEngine(platform::backgroundRemovalBundlePath(), true)
{
}

BackgroundRemovalEngine::BackgroundRemovalEngine(QString modelDirectory, bool verifyIntegrity)
    : impl_(std::make_unique<Impl>(std::move(modelDirectory), verifyIntegrity))
{
}
BackgroundRemovalEngine::~BackgroundRemovalEngine() = default;

std::vector<BackgroundRemovalModel> BackgroundRemovalEngine::models()
{
    std::vector<BackgroundRemovalModel> result;
    for (const auto& profile : profiles) {
        result.push_back({ QString::fromUtf8(profile.id), QString::fromUtf8(profile.name),
            QString::fromUtf8(profile.path) });
    }
    return result;
}

BackgroundRemovalResult BackgroundRemovalEngine::evaluate(
    const QString& id, const QImage& source, const std::atomic_bool& cancel)
{
    BackgroundRemovalResult result;
    QElapsedTimer total;
    total.start();
    try {
        checkCancellation(cancel);
        require(!source.isNull(), QStringLiteral("The selected layer has no raster image."));
        require(std::uint64_t(source.width()) * std::uint64_t(source.height()) <= 64u * 1024u * 1024u,
            i18n::text("background.removal.limit"));
        const auto found = std::find_if(profiles.begin(), profiles.end(),
            [&](const auto& profile) { return id == QString::fromUtf8(profile.id); });
        require(found != profiles.end(), QStringLiteral("Unknown background-removal model: %1").arg(id));
        const auto& profile = *found;
        auto& p = *impl_;
        QElapsedTimer stage;
        stage.start();
        p.load(profile, cancel);
        result.timings.initializationMs = milliseconds(stage);
        result.details = p.metadata;
        stage.restart();
        const int height = int(p.inputShape[2]), width = int(p.inputShape[3]);
        const auto rgb = resizeBytes(source.convertToFormat(QImage::Format_RGB888),
            QSize(width, height), profile.resize, cancel);
        float divisor = 255.f;
        if (profile.input == InputScale::ImageMaximum) {
            divisor = 0.f;
            for (int y = 0; y < rgb.height(); ++y) {
                checkCancellation(cancel);
                const auto* row = rgb.constScanLine(y);
                for (int x = 0; x < rgb.width() * 3; ++x)
                    divisor = std::max(divisor, float(row[x]));
            }
            divisor = std::max(divisor, 1.0e-6f);
        }
        const std::size_t plane = std::size_t(width) * std::size_t(height);
        std::vector<float> input(plane * 3);
        constexpr std::array<float, 3> mean { .485f, .456f, .406f }, deviation { .229f, .224f, .225f };
        for (int y = 0; y < height; ++y) {
            checkCancellation(cancel);
            const auto* row = rgb.constScanLine(y);
            for (int x = 0; x < width; ++x)
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    const float value = row[std::size_t(x) * 3 + channel] / divisor;
                    input[channel * plane + pixelIndex(x, y, width)]
                        = (value - mean[channel]) / deviation[channel];
                }
        }
        OrtValue* inputTensor { };
        OrtValue* outputTensor { };
        const auto release = qScopeGuard([&] {
            if (inputTensor)
                p.api->ReleaseValue(inputTensor);
            if (outputTensor)
                p.api->ReleaseValue(outputTensor);
        });
        p.check(p.api->CreateTensorWithDataAsOrtValue(p.memory, input.data(), input.size() * sizeof(float), p.inputShape.data(),
            p.inputShape.size(), p.inputType, &inputTensor));
        result.timings.preprocessingMs = milliseconds(stage);
        stage.restart();
        p.run(inputTensor, &outputTensor, cancel);
        result.timings.inferenceMs = milliseconds(stage);
        stage.restart();
        auto [values, size] = p.coverage(outputTensor);
        checkCancellation(cancel);
        if (profile.output == OutputScale::MinMax) {
            minMaxNormalize(values);
            QImage small(size, QImage::Format_Grayscale8);
            require(!small.isNull(), QStringLiteral("Not enough memory for the foreground mask."));
            for (int y = 0; y < size.height(); ++y) {
                auto* row = small.scanLine(y);
                for (int x = 0; x < size.width(); ++x)
                    row[x] = static_cast<uchar>(values[pixelIndex(x, y, size.width())] * 255.f);
            }
            result.mask = resizeBytes(small, source.size(), Filter::Lanczos, cancel);
        } else {
            if (profile.output == OutputScale::Sigmoid)
                for (auto& value : values)
                    value = value >= 0.f ? 1.f / (1.f + std::exp(-value))
                                        : std::exp(value) / (1.f + std::exp(value));
            // Tensor dimensions are H,W; QImage is W,H.
            values = resizeCoverage(values, size, source.size(), cancel);
            result.mask = QImage(source.size(), QImage::Format_Grayscale8);
            require(!result.mask.isNull(), QStringLiteral("Not enough memory for the foreground mask."));
            for (int y = 0; y < source.height(); ++y) {
                checkCancellation(cancel);
                auto* row = result.mask.scanLine(y);
                for (int x = 0; x < source.width(); ++x)
                    row[x] = static_cast<uchar>(std::clamp(values[pixelIndex(x, y, source.width())], 0.f, 1.f) * 255.f);
            }
        }
        bool nonempty = false;
        for (int y = 0; y < result.mask.height(); ++y) {
            checkCancellation(cancel);
            const auto* row = result.mask.constScanLine(y);
            nonempty |= std::any_of(row, row + result.mask.width(), [](uchar v) { return v != 0; });
        }
        require(nonempty, QStringLiteral("This model produced an empty foreground mask. The layer was not changed."));
        result.timings.postprocessingMs = milliseconds(stage);
    } catch (const Cancelled&) {
        result.mask = { };
        result.cancelled = true;
    } catch (const std::exception& exception) {
        result.mask = { };
        result.error = QString::fromUtf8(exception.what());
    }
    result.timings.totalMs = milliseconds(total);
    return result;
}

}
