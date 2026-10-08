#include "imageeditor/ui/ObjectSelection.hpp"
#include "imageeditor/platform/ApplicationPaths.hpp"
#include "../third_party/onnxruntime/onnxruntime_c_api.h"
#include "imageeditor/core/ObjectSelectionBoundary.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
#include <QScopeGuard>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace imageeditor::ui {
bool ObjectSelectionEvidence::equivalent(const core::SelectionEvidence& value) const noexcept
{
    const auto* other = dynamic_cast<const ObjectSelectionEvidence*>(&value);
    if (!other || !identity.equivalent(other->identity) || operation != other->operation
        || prompt.box != other->prompt.box || prompt.corrections.size() != other->prompt.corrections.size())
        return false;
    for (std::size_t i = 0; i < prompt.corrections.size(); ++i)
        if (prompt.corrections[i].position != other->prompt.corrections[i].position
            || prompt.corrections[i].include != other->prompt.corrections[i].include)
            return false;
    return original == other->original
        || (original && other->original && original->equivalent(*other->original));
}
std::size_t ObjectSelectionEvidence::memoryCost() const noexcept
{
    return sizeof(*this) + identity.memoryCost() - sizeof(identity)
        + prompt.corrections.capacity() * sizeof(ObjectSelectionPoint)
        + (original ? original->memoryCost() : 0);
}
namespace {
    void require(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }
}
struct ObjectSelectionEngine::Impl {
    QString directory;
    QLibrary library;
    const OrtApi* api { };
    OrtEnv* env { };
    OrtSession* encoder { };
    OrtSession* decoder { };
    OrtMemoryInfo* memory { };
    std::weak_ptr<const core::SmartReferenceImage> source;
    std::vector<float> features;
    int resizedWidth { }, resizedHeight { };
    std::size_t encodes { };
    explicit Impl(QString path)
        : directory(std::move(path))
    {
    }
    ~Impl()
    {
        if (api) {
            if (encoder)
                api->ReleaseSession(encoder);
            if (decoder)
                api->ReleaseSession(decoder);
            if (memory)
                api->ReleaseMemoryInfo(memory);
            if (env)
                api->ReleaseEnv(env);
        }
    }
    void check(OrtStatus* status) const
    {
        if (!status)
            return;
        const std::string message = api->GetErrorMessage(status);
        api->ReleaseStatus(status);
        throw std::runtime_error("Object Selection: " + message);
    }
    void load(const std::atomic_bool& cancelled)
    {
        if (encoder && decoder)
            return;
        require(!api, "Bundled Object Selection runtime initialization failed; restart Vulkana to retry");
        const QDir dir(directory);
        QFile manifest(dir.filePath("vulkana-mobilesam-v1.json"));
        require(manifest.open(QIODevice::ReadOnly) && manifest.size() < 16384,
            "Bundled Object Selection files are missing or damaged; repair the Vulkana installation");
        const auto metadata = QJsonDocument::fromJson(manifest.readAll()).object();
        require(metadata["format"].toInt() == 1, "Unsupported Object Selection model pack version");
        for (const auto& name : { QStringLiteral("encoder.onnx"), QStringLiteral("decoder.onnx"),
                 QStringLiteral("libonnxruntime.so") }) {
            QFile file(dir.filePath(name));
            require(file.open(QIODevice::ReadOnly), "Object Selection model pack is incomplete");
            require(file.size() > 0 && file.size() < 128 * 1024 * 1024,
                "Object Selection model pack file exceeds the supported size");
            QCryptographicHash hash(QCryptographicHash::Sha256);
            while (!file.atEnd()) {
                if (cancelled)
                    return;
                const auto bytes = file.read(1024 * 1024);
                require(file.error() == QFileDevice::NoError, "Could not read Object Selection model pack");
                hash.addData(bytes);
            }
            require(hash.result().toHex() == metadata[name].toString().toLatin1(),
                "Object Selection model pack checksum mismatch");
        }
        library.setFileName(dir.absoluteFilePath("libonnxruntime.so"));
        require(library.load(), "Could not load the bundled Object Selection CPU runtime on this system");
        const auto getApi
            = reinterpret_cast<const OrtApiBase*(ORT_API_CALL*)()>(library.resolve("OrtGetApiBase"));
        require(getApi, "Invalid Object Selection runtime");
        api = getApi()->GetApi(ORT_API_VERSION);
        require(api, "Object Selection needs ONNX Runtime 1.20 or newer");
        check(api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "Vulkana Object Selection", &env));
        check(api->DisableTelemetryEvents(env));
        OrtSessionOptions* options { };
        check(api->CreateSessionOptions(&options));
        const auto release = qScopeGuard([&] { api->ReleaseSessionOptions(options); });
        check(api->SetIntraOpNumThreads(
            options, int(std::clamp(std::thread::hardware_concurrency() / 2, 1u, 6u))));
        check(api->SetInterOpNumThreads(options, 1));
        check(api->DisableCpuMemArena(options));
        check(api->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL));
        check(api->CreateSession(
            env, dir.filePath("encoder.onnx").toLocal8Bit().constData(), options, &encoder));
        check(api->CreateSession(
            env, dir.filePath("decoder.onnx").toLocal8Bit().constData(), options, &decoder));
        check(api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory));
    }
    OrtValue* tensor(std::span<float> values, std::span<const int64_t> shape)
    {
        OrtValue* result { };
        check(api->CreateTensorWithDataAsOrtValue(memory, values.data(), values.size_bytes(), shape.data(),
            shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &result));
        return result;
    }
    void run(OrtSession* session, std::span<const char* const> names, std::span<const OrtValue* const> inputs,
        std::span<const char* const> outputNames, std::span<OrtValue*> outputs,
        const std::atomic_bool& cancelled)
    {
        OrtRunOptions* options { };
        check(api->CreateRunOptions(&options));
        const auto release = qScopeGuard([&] { api->ReleaseRunOptions(options); });
        // RunOptions termination interrupts both the encoder and decoder. The
        // monitor is bounded to this one in-flight run and joins before release.
        std::jthread monitor([&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                if (cancelled) {
                    if (auto* status = api->RunOptionsSetTerminate(options))
                        api->ReleaseStatus(status);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
        });
        auto* status = api->Run(session, options, names.data(), inputs.data(), inputs.size(),
            outputNames.data(), outputs.size(), outputs.data());
        monitor.request_stop();
        monitor.join();
        if (cancelled) {
            if (status)
                api->ReleaseStatus(status);
            return;
        }
        check(status);
    }
    std::span<const float> data(OrtValue* value, std::size_t expected)
    {
        require(value, "Object Selection returned no tensor");
        OrtTensorTypeAndShapeInfo* info { };
        check(api->GetTensorTypeAndShape(value, &info));
        const auto release = qScopeGuard([&] { api->ReleaseTensorTypeAndShapeInfo(info); });
        size_t size { };
        ONNXTensorElementDataType type { };
        check(api->GetTensorShapeElementCount(info, &size));
        check(api->GetTensorElementType(info, &type));
        require(size == expected && type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
            "Object Selection model has incompatible output dimensions");
        void* pointer { };
        check(api->GetTensorMutableData(value, &pointer));
        return { static_cast<float*>(pointer), size };
    }
};
ObjectSelectionEngine::ObjectSelectionEngine()
    : ObjectSelectionEngine(platform::objectSelectionBundlePath())
{
}
ObjectSelectionEngine::ObjectSelectionEngine(QString directory)
    : impl_(std::make_unique<Impl>(std::move(directory)))
{
}
ObjectSelectionEngine::~ObjectSelectionEngine() = default;
std::size_t ObjectSelectionEngine::encodedImageCount() const { return impl_->encodes; }
core::SelectionState ObjectSelectionEngine::evaluate(std::shared_ptr<const core::SmartReferenceImage> image,
    const ObjectSelectionPrompt& prompt, const std::atomic_bool& cancelled)
{
    auto& p = *impl_;
    if (cancelled)
        return { };
    require(bool(image), "Missing Object Selection reference");
    const auto e = image->extent;
    const auto count = std::size_t(e.width) * e.height;
    require(count && count <= core::SmartSelectionReference::maximumPixels && image->pixels.size() == count
            && image->valid.size() == count,
        "Invalid Object Selection reference");
    require(double(std::min(e.width,e.height))*1024/std::max(e.width,e.height)>=.5,
        "This reference is too narrow for Object Selection; use Quick Selection instead");
    require(prompt.box.width > 1 && prompt.box.height > 1
            && std::isfinite(prompt.box.x + prompt.box.y + prompt.box.width + prompt.box.height),
        "Draw a rectangle around the intended object");
    require(std::min(prompt.box.right(), double(e.width)) - std::max(prompt.box.x, 0.0) > 1
            && std::min(prompt.box.bottom(), double(e.height)) - std::max(prompt.box.y, 0.0) > 1,
        "Draw an Object Selection rectangle inside the canvas");
    require(prompt.corrections.size() <= 128,
        "Object Selection supports 128 correction points; start a new rectangle");
    p.load(cancelled);
    if (cancelled)
        return { };
    if (p.source.lock() != image) {
        const double scale = 1024.0 / std::max(e.width, e.height);
        p.resizedWidth = std::max(1, int(std::floor(e.width * scale + .5)));
        p.resizedHeight = std::max(1, int(std::floor(e.height * scale + .5)));
        QImage rgb(int(e.width), int(e.height), QImage::Format_RGB888);
        for (unsigned y = 0; y < e.height; ++y) {
            if (cancelled)
                return { };
            auto* row = rgb.scanLine(int(y));
            for (unsigned x = 0; x < e.width; ++x) {
                const auto index = std::size_t(y) * e.width + x;
                const auto& c = image->pixels[index];
                const bool visible = image->valid[index] && c.alpha;
                row[3 * x] = visible ? c.red : 0;
                row[3 * x + 1] = visible ? c.green : 0;
                row[3 * x + 2] = visible ? c.blue : 0;
            }
        }
        const auto resized
            = rgb.scaled(p.resizedWidth, p.resizedHeight, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        std::vector<float> input(3 * 1024 * 1024, 0);
        constexpr std::array means { 123.675f, 116.28f, 103.53f }, deviations { 58.395f, 57.12f, 57.375f };
        for (int y = 0; y < p.resizedHeight; ++y)
            for (int x = 0; x < p.resizedWidth; ++x)
                for (int k = 0; k < 3; ++k)
                    input[std::size_t(k) * 1024 * 1024 + std::size_t(y) * 1024 + std::size_t(x)]
                        = (resized.constScanLine(y)[3 * x + k] - means[std::size_t(k)])
                        / deviations[std::size_t(k)];
        const std::array<int64_t, 4> shape { 1, 3, 1024, 1024 };
        auto* tensor = p.tensor(input, shape);
        OrtValue* output { };
        const auto release = qScopeGuard([&] {
            p.api->ReleaseValue(tensor);
            if (output)
                p.api->ReleaseValue(output);
        });
        const std::array<const OrtValue*, 1> inputs { tensor };
        const std::array<const char*, 1> names { "image" }, outputs { "features" };
        p.run(p.encoder, names, inputs, outputs, std::span(&output, 1), cancelled);
        if (cancelled)
            return { };
        const auto features = p.data(output, 256 * 64 * 64);
        p.features.assign(features.begin(), features.end());
        p.source = image;
        ++p.encodes;
    }
    std::vector<float> coordinates, labels;
    const auto point = [&](double x, double y, float label) {
        require(std::isfinite(x) && std::isfinite(y), "Invalid Object Selection correction");
        coordinates.push_back(float(std::clamp(x, 0.0, double(e.width)) * p.resizedWidth / e.width));
        coordinates.push_back(float(std::clamp(y, 0.0, double(e.height)) * p.resizedHeight / e.height));
        labels.push_back(label);
    };
    for (const auto& correction : prompt.corrections)
        point(correction.position.x, correction.position.y, correction.include ? 1 : 0);
    point(prompt.box.x, prompt.box.y, 2);
    point(prompt.box.right(), prompt.box.bottom(), 3);
    std::vector<float> emptyMask(256 * 256, 0);
    std::array<float, 1> hasMask { 0 };
    std::array<float, 2> originalSize { float(e.height), float(e.width) };
    const std::array<int64_t, 4> featureShape { 1, 256, 64, 64 }, maskShape { 1, 1, 256, 256 };
    const std::array<int64_t, 3> coordShape { 1, int64_t(labels.size()), 2 };
    const std::array<int64_t, 2> labelShape { 1, int64_t(labels.size()) };
    const std::array<int64_t, 1> one { 1 }, two { 2 };
    std::array<OrtValue*, 6> inputs { };
    std::array<OrtValue*, 3> outputs { };
    const auto release = qScopeGuard([&] {
        for (auto* t : inputs)
            if (t)
                p.api->ReleaseValue(t);
        for (auto* t : outputs)
            if (t)
                p.api->ReleaseValue(t);
    });
    inputs[0] = p.tensor(p.features, featureShape);
    inputs[1] = p.tensor(coordinates, coordShape);
    inputs[2] = p.tensor(labels, labelShape);
    inputs[3] = p.tensor(emptyMask, maskShape);
    inputs[4] = p.tensor(hasMask, one);
    inputs[5] = p.tensor(originalSize, two);
    const std::array<const char*, 6> names { "image_embeddings", "point_coords", "point_labels", "mask_input",
        "has_mask_input", "orig_im_size" };
    const std::array<const char*, 3> outputNames { "masks", "scores", "logits" };
    std::array<const OrtValue*, 6> values;
    std::copy(inputs.begin(), inputs.end(), values.begin());
    p.run(p.decoder, names, values, outputNames, outputs, cancelled);
    if (cancelled)
        return { };
    const auto logits = p.data(outputs[0], count);
    auto coverage = core::recoverObjectSelectionBoundary(*image, logits, cancelled);
    if (cancelled)
        return { };
    auto mask
        = core::SelectionMask::fromR8Region(e, { 0, 0, int(e.width), int(e.height) }, coverage, e.width);
    if (!core::prepareSelectionBoundary(mask, cancelled))
        return { };
    return mask;
}
}
