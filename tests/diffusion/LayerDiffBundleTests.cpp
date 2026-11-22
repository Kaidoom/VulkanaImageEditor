#include "layerdiff/cuda_projections.hpp"
#include "layerdiff/clip.hpp"
#include "layerdiff/model.hpp"
#include "layerdiff/vae.hpp"
#include <ggml-cuda.h>
#include <json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;

class Fixture {
  public:
    Fixture() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::temp_directory_path() / ("vulkana-layerdiff-test-" + std::to_string(stamp));
        if (!fs::create_directory(root))
            throw std::runtime_error("Cannot create test directory");
    }
    ~Fixture() {
        std::error_code error;
        fs::remove_all(root, error);
    }
    fs::path root;
    void text(const fs::path& relative, std::string contents) const {
        auto file = root / relative;
        fs::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << contents;
        if (!out)
            throw std::runtime_error("Could not write test component");
    }
    // A structurally valid safetensors file with deliberately wrong neural tensor
    // dimensions. This avoids embedding any model weights or requiring a download.
    void weights(const fs::path& file, const std::string& name) const {
        const std::vector<int> shape{8, 32};
        json header = {{name, {{"dtype", "F16"}, {"shape", shape}, {"data_offsets", {0, 512}}}}};
        auto s = header.dump();
        s.resize((s.size() + 7) / 8 * 8, ' ');
        uint64_t n = s.size();
        auto bytes = std::string(reinterpret_cast<const char*>(&n), 8) + s + std::string(512, '\0');
        text(file, bytes);
    }
    json scheduler() const {
        return {{"_class_name", "DPMSolverMultistepScheduler"},
                {"algorithm_type", "sde-dpmsolver++"},
                {"solver_type", "midpoint"},
                {"solver_order", 2},
                {"beta_schedule", "scaled_linear"},
                {"timestep_spacing", "leading"},
                {"final_sigmas_type", "zero"},
                {"prediction_type", "epsilon"},
                {"use_karras_sigmas", false},
                {"num_train_timesteps", 1000},
                {"steps_offset", 1},
                {"beta_start", 0.00085},
                {"beta_end", 0.012}};
    }
    void setup() const {
        text("unet/config.json", json({{"_class_name", "UNetFrameConditionModel"},
                                       {"tag_version", "v3"},
                                       {"in_channels", 8},
                                       {"out_channels", 4},
                                       {"cross_attention_dim", 2048}})
                                     .dump());
        text("scheduler/scheduler_config.json", scheduler().dump());
        text(
            "vae/config.json",
            R"({"_class_name":"AutoencoderKL","in_channels":3,"out_channels":3,"latent_channels":4,"scaling_factor":0.13025})");
        text("trans_vae/config.json", R"({"_class_name":"TransparentVAE"})");
        weights("unet/diffusion_pytorch_model.safetensors", "conv_in.weight");
        weights("vae/diffusion_pytorch_model.safetensors", "encoder.conv_in.weight");
        weights("trans_vae/diffusion_pytorch_model.safetensors", "decoder.model.conv_in.weight");
        for (bool large : {false, true}) {
            const auto enc = fs::path(large ? "text_encoder_2" : "text_encoder");
            const auto tok = fs::path(large ? "tokenizer_2" : "tokenizer");
            const auto width = large ? 1280 : 768;
            text(enc / "config.json", json({{"hidden_size", width},
                                            {"num_attention_heads", width / 64},
                                            {"num_hidden_layers", large ? 32 : 12},
                                            {"max_position_embeddings", 77},
                                            {"hidden_act", large ? "gelu" : "quick_gelu"}})
                                          .dump());
            weights(enc / "model.safetensors", "text_model.embeddings.token_embedding.weight");
            text(tok / "tokenizer_config.json", R"({"pad_token":"<|endoftext|>"})");
            text(tok / "vocab.json",
                 R"({"<|startoftext|>":49406,"<|endoftext|>":49407,"a</w>":320})" + std::string(256, ' '));
            text(tok / "merges.txt", "#version: 0.2\n" + std::string(256, '\n'));
        }
    }
};

template <class Call> void mustReject(Call call, const std::string& hint) {
    try {
        call();
    } catch (const std::exception& e) {
        if (std::string(e.what()).find(hint) == std::string::npos)
            throw std::runtime_error("Expected error naming '" + hint + "', got '" + e.what() + "'");
        return;
    }
    throw std::runtime_error("Malformed model accepted: " + hint);
}

void admission(Fixture& f) {
    using layerdiff::validateBundle;
    mustReject([] { validateBundle("relative-model-directory"); }, "directory");
    f.setup();
    auto check = [&] { validateBundle(f.root); };
    f.text("unet/config.json",
           R"({"_class_name":"UNetFrameConditionModel","tag_version":"v2","in_channels":12})");
    mustReject(check, "LayerDiff v3");
    f.setup();
    fs::remove(f.root / "trans_vae/diffusion_pytorch_model.safetensors");
    mustReject(check, "trans_vae/");
    f.text("trans_vae/diffusion_pytorch_model.safetensors", "version https://git-lfs.github.com/spec/v1\n");
    mustReject(check, "trans_vae/");
    f.setup();
    fs::remove(f.root / "tokenizer_2/tokenizer_config.json");
    mustReject(check, "tokenizer_2/tokenizer_config.json");
    f.setup();
    auto cfg = f.scheduler();
    cfg["use_karras_sigmas"] = true;
    f.text("scheduler/scheduler_config.json", cfg.dump());
    mustReject(check, "scheduler");
    cfg = f.scheduler();
    cfg["beta_end"] = 0.0001;
    f.text("scheduler/scheduler_config.json", cfg.dump());
    mustReject(check, "schedule");
    if (auto model = std::getenv("VULKANA_DIFFUSION_TEST_SEETHROUGH_MODEL"))
        validateBundle(fs::absolute(model));
}

void wrongShapes(Fixture& f) {
    using namespace layerdiff;
    f.setup();
    Backend gpu(0);
    mustReject([&] { encodeParts(gpu, f.root, {"a"}); }, "token_embedding.weight");
    mustReject([&] { Vae vae(gpu, f.root); }, "encoder.conv_in.weight");
}

int residentGraph() {
    using namespace layerdiff;
    int checkpoints = 0;
    bool cancel = false;
    Backend gpu(0, [&] {
        if (++checkpoints == 2 && cancel) throw std::runtime_error("cancelled at graph checkpoint");
    });
    auto evaluate = [&] {
        Graph graph(gpu);
        auto input = graph.input({{2}, {1.f, 2.f}});
        auto skip = ggml_mul(graph.ctx, input, input);
        auto x = skip;
        for (int i = 0; i < 600; ++i) x = ggml_add(graph.ctx, x, input);
        x = ggml_add(graph.ctx, x, skip);
        checkpoints = 0;
        return graph.evaluateIfFits(x, {2});
    };
    const auto first = evaluate();
    if (!first) return 77; // insufficient free CUDA memory for the resident path
    if (first->values != std::vector<float>{602.f, 1208.f} || checkpoints < 3)
        throw std::runtime_error("Resident graph lost a skip connection or cancellation checkpoint");
    cancel = true;
    mustReject(evaluate, "cancelled at graph checkpoint");
    cancel = false;
    const auto retry = evaluate();
    if (!retry || retry->values != first->values)
        throw std::runtime_error("Resident graph failed after cancellation");
    // The last CUDA block is partial, and distinct gate/content biases expose a swapped half.
    constexpr int width = 257;
    Tensor projection{{3, 2 * width}, {}}, bias{{2 * width}, {}};
    for (unsigned i = 0; i < 6 * width; ++i)
        projection.values.push_back((int(i % 79) - 39) * .125f);
    for (unsigned i = 0; i < 2 * width; ++i)
        bias.values.push_back((int(i % 13) - 6) * .0625f);
    auto gate = [&](bool fused) {
        Graph graph(gpu);
        if (fused && !graph.projectionKernels().biasedGate)
            throw std::runtime_error("CUDA feed-forward kernel registration failed");
        auto output = biasedGeglu(graph.ctx, graph.inputRef(projection), graph.inputRef(bias), fused);
        return graph.evaluate(output);
    };
    if (gate(true).values != gate(false).values)
        throw std::runtime_error("Fused feed-forward activation differs from the ggml operators");
    return 0;
}

int projectionPrecision() {
    using namespace layerdiff;
    Backend gpu(0);
    Graph capabilities(gpu);
    const auto kernels = capabilities.projectionKernels();
    if (std::getenv("GGML_CUDA_CUBLAS_COMPUTE_TYPE")) {
        if (kernels.halfProducts || kernels.affineInput)
            throw std::runtime_error("LayerDiff ignored the explicit cuBLAS compute type");
        return 0;
    }
    if (!kernels.halfProducts) return 77;
    if (!kernels.affineInput || usesHalfProjection(16) || usesHalfProjection(255))
        throw std::runtime_error("Incorrect half projection capabilities");

    constexpr int rows = 257, width = 64;
    auto samples = [](std::vector<int64_t> shape, unsigned seed, float amplitude) {
        Tensor tensor{std::move(shape), {}};
        tensor.values.resize(elements(tensor.shape));
        for (auto &value : tensor.values) {
            seed = seed * 1664525u + 1013904223u;
            value = (int(seed >> 16) - 32768) * amplitude;
        }
        return tensor;
    };
    auto x = samples({rows, width}, 3, .00011f);
    auto scale = samples({width}, 5, .00008f), shift = samples({width}, 7, .00002f);
    auto w1 = samples({width * 8, width}, 9, .0000041f), b1 = samples({width * 8}, 11, .000013f);
    auto w2 = samples({width, width * 4}, 13, .0000039f), b2 = samples({width}, 15, .000017f);
    auto evaluate = [&](bool optimized) {
        Graph g(gpu);
        auto input = g.inputRef(x), gain = g.inputRef(scale), offset = g.inputRef(shift);
        input = optimized ? affineProjectionInput(g.ctx, input, gain, offset)
                          : ggml_add(g.ctx, ggml_mul(g.ctx, input, gain), offset);
        auto weight1 = ggml_cast(g.ctx, g.inputRef(w1), GGML_TYPE_F16);
        auto weight2 = ggml_cast(g.ctx, g.inputRef(w2), GGML_TYPE_F16);
        auto wide = optimized ? halfProjection(g.ctx, weight1, input) : ggml_mul_mat(g.ctx, weight1, input);
        auto gate = biasedGeglu(g.ctx, wide, g.inputRef(b1), optimized);
        auto product = optimized ? halfProjection(g.ctx, weight2, gate) : ggml_mul_mat(g.ctx, weight2, gate);
        auto result = optimized ? ggml_add_cast(g.ctx, product, g.inputRef(b2), GGML_TYPE_F32)
                                : ggml_add(g.ctx, product, g.inputRef(b2));
        if (optimized && (input->type != GGML_TYPE_F16 || wide->type != GGML_TYPE_F16 ||
                          gate->type != GGML_TYPE_F16 || result->type != GGML_TYPE_F32))
            throw std::runtime_error("Incorrect projection storage or bias precision");
        return g.evaluate(result);
    };
    if (evaluate(true).values != evaluate(false).values)
        throw std::runtime_error("CUDA projection changed the reference feed-forward result");
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        Fixture fixture;
        if (argc == 2) {
            if (ggml_backend_cuda_get_device_count() < 1)
                return 77;
            if (std::string(argv[1]) == "--cuda-graph") return residentGraph();
            if (std::string(argv[1]) == "--cuda-projections") return projectionPrecision();
            if (std::string(argv[1]) != "--cuda-shapes") return 1;
            wrongShapes(fixture);
        } else
            admission(fixture);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
