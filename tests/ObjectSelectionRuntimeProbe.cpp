// Standalone package-baseline probe: full bundled encoder AND decoder, no Qt,
// Python, network, model conversion or source-image dependency.
#include "../src/ui/third_party/onnxruntime/onnxruntime_c_api.h"
#include <array>
#include <cmath>
#include <dlfcn.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

struct Runtime {
    void* library { };
    const OrtApi* api { };
    OrtEnv* env { };
    OrtSessionOptions* options { };
    OrtMemoryInfo* memory { };
    OrtSession* encoder { };
    OrtSession* decoder { };
    std::vector<OrtValue*> values;
    ~Runtime()
    {
        if (api) {
            for (auto* v : values)
                if (v)
                    api->ReleaseValue(v);
            if (encoder)
                api->ReleaseSession(encoder);
            if (decoder)
                api->ReleaseSession(decoder);
            if (memory)
                api->ReleaseMemoryInfo(memory);
            if (options)
                api->ReleaseSessionOptions(options);
            if (env)
                api->ReleaseEnv(env);
        }
        if (library)
            dlclose(library);
    }
    void check(OrtStatus* status)
    {
        if (!status)
            return;
        const std::string message = api->GetErrorMessage(status);
        api->ReleaseStatus(status);
        throw std::runtime_error(message);
    }
    template <std::size_t N> OrtValue* tensor(std::vector<float>& data, const std::array<int64_t, N>& shape)
    {
        OrtValue* v { };
        check(api->CreateTensorWithDataAsOrtValue(memory, data.data(), data.size() * sizeof(float),
            shape.data(), N, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &v));
        values.push_back(v);
        return v;
    }
    void verify(OrtValue* value, std::size_t expected)
    {
        OrtTensorTypeAndShapeInfo* info { };
        check(api->GetTensorTypeAndShape(value, &info));
        std::size_t count { };
        auto* status = api->GetTensorShapeElementCount(info, &count);
        api->ReleaseTensorTypeAndShapeInfo(info);
        check(status);
        if (count != expected)
            throw std::runtime_error("Incorrect output tensor shape");
        void* data { };
        check(api->GetTensorMutableData(value, &data));
        for (std::size_t i = 0; i < count; ++i)
            if (!std::isfinite(static_cast<float*>(data)[i]))
                throw std::runtime_error("Nonfinite output");
    }
};
int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;
    try {
        const std::filesystem::path directory(argv[1]);
        Runtime r;
        r.library = dlopen((directory / "libonnxruntime.so").c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!r.library)
            throw std::runtime_error(dlerror());
        const auto getApi
            = reinterpret_cast<const OrtApiBase*(ORT_API_CALL*)()>(dlsym(r.library, "OrtGetApiBase"));
        if (!getApi)
            throw std::runtime_error("Missing ONNX C API");
        r.api = getApi()->GetApi(ORT_API_VERSION);
        if (!r.api)
            throw std::runtime_error("Incompatible ONNX C API");
        r.check(r.api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "bundled-test", &r.env));
        r.check(r.api->DisableTelemetryEvents(r.env));
        r.check(r.api->CreateSessionOptions(&r.options));
        r.check(r.api->SetIntraOpNumThreads(r.options, 6));
        r.check(r.api->SetInterOpNumThreads(r.options, 1));
        r.check(r.api->DisableCpuMemArena(r.options));
        r.check(r.api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &r.memory));
        r.check(r.api->CreateSession(r.env, (directory / "encoder.onnx").c_str(), r.options, &r.encoder));
        r.check(r.api->CreateSession(r.env, (directory / "decoder.onnx").c_str(), r.options, &r.decoder));
        std::vector<float> image(3 * 1024 * 1024, 0);
        const OrtValue* input = r.tensor(image, std::array<int64_t, 4> { 1, 3, 1024, 1024 });
        OrtValue* features { };
        const char* name = "image";
        const char* output = "features";
        r.check(r.api->Run(r.encoder, nullptr, &name, &input, 1, &output, 1, &features));
        r.values.push_back(features);
        r.verify(features, 256 * 64 * 64);
        std::vector<float> coords { 64, 64, 960, 960 }, labels { 2, 3 }, mask(256 * 256, 0), has { 0 },
            size { 128, 128 };
        std::array<const OrtValue*, 6> inputs { features,
            r.tensor(coords, std::array<int64_t, 3> { 1, 2, 2 }),
            r.tensor(labels, std::array<int64_t, 2> { 1, 2 }),
            r.tensor(mask, std::array<int64_t, 4> { 1, 1, 256, 256 }),
            r.tensor(has, std::array<int64_t, 1> { 1 }), r.tensor(size, std::array<int64_t, 1> { 2 }) };
        const std::array<const char*, 6> names { "image_embeddings", "point_coords", "point_labels",
            "mask_input", "has_mask_input", "orig_im_size" };
        const std::array<const char*, 3> outputs { "masks", "scores", "logits" };
        std::array<OrtValue*, 3> results { };
        r.check(r.api->Run(
            r.decoder, nullptr, names.data(), inputs.data(), 6, outputs.data(), 3, results.data()));
        for (auto* v : results)
            r.values.push_back(v);
        r.verify(results[0], 128 * 128);
        std::cout << "PASS full offline encoder/decoder; ONNX Runtime " << getApi()->GetVersionString()
                  << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
