#include "model/diffusion/flux3.hpp"
#include "model_loader.h"
#include "ggml-cpu.h"
#include <iostream>
#include <stdexcept>

static void check(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}

static void detection(bool fused, bool flowMap)
{
    ModelLoader loader;
    auto& tensors = loader.get_tensor_storage_map();
    const std::string prefix = "model.diffusion_model";
    const auto add = [&](const std::string& key, int64_t in, int64_t out) {
        const int64_t shape[] = {in, out};
        const auto name = prefix + "." + key;
        tensors[name] = TensorStorage(name, GGML_TYPE_F32, shape, 2, 0);
    };
    add("emb_in.image.weight", 128, 256);
    add("txt_in.weight", 512, 256);
    add(fused ? "txt_mode_blocks.0.qkv.weight" : "txt_mode_blocks.0.q_proj.weight",
        256, fused ? 768 : 256);
    add("single_blocks.0.mlp_out.weight", 768, 256);
    if (flowMap) add("ref_time_in.in_layer.weight", 256, 256);
    check(loader.get_sd_version() == VERSION_FLUX3, "FLUX.3 layout was not detected");
    const auto config = Flux3::Flux3Config::detect_from_weights(tensors, prefix);
    check(config.fused_qkv == fused, "incorrect QKV layout");
    check(config.flow_map == flowMap, "incorrect flow-map detection");
    check(config.hidden_size == 256 && config.context_dim == 512 && config.heads == 2,
        "incorrect model dimensions");
    check(config.depth == 1 && config.single_depth == 1 && config.mlp_dim == 768,
        "incorrect block dimensions");
    tensors.erase(prefix + (fused ? ".txt_mode_blocks.0.qkv.weight" : ".txt_mode_blocks.0.q_proj.weight"));
    check(loader.get_sd_version() != VERSION_FLUX3, "incomplete marker set detected as FLUX.3");
}

static void projectionViews()
{
    // Distinct channel/token/batch values expose any wrong axis, pitch or offset.
    constexpr int hidden = 8, heads = 2, tokens = 3, batch = 2;
    auto* ctx = ggml_init({1024 * 1024, nullptr, false});
    check(ctx, "context allocation failed");
    auto* qkv = ggml_new_tensor_3d(ctx, GGML_TYPE_F32, 3 * hidden, tokens, batch);
    for (int b = 0; b < batch; ++b)
        for (int t = 0; t < tokens; ++t)
            for (int c = 0; c < 3 * hidden; ++c)
                static_cast<float*>(qkv->data)[(b * tokens + t) * 3 * hidden + c] = b * 10000 + t * 100 + c;
    auto backend = ggml_backend_cpu_init();
    check(backend, "CPU backend initialization failed");
    for (int part = 0; part < 3; ++part) {
        auto* view = Flux3::qkv_heads(ctx, qkv, heads, part);
        check(view->ne[0] == hidden / heads && view->ne[1] == heads
            && view->ne[2] == tokens && view->ne[3] == batch, "incorrect QKV head dimensions");
        auto* contiguous = ggml_cont(ctx, view);
        auto* graph = ggml_new_graph(ctx);
        ggml_build_forward_expand(graph, contiguous);
        check(ggml_backend_graph_compute(backend, graph) == GGML_STATUS_SUCCESS, "view graph failed");
        const auto* data = static_cast<const float*>(contiguous->data);
        for (int b = 0; b < batch; ++b)
            for (int t = 0; t < tokens; ++t)
                for (int c = 0; c < hidden; ++c)
                    check(data[(b * tokens + t) * hidden + c] == b * 10000 + t * 100 + part * hidden + c,
                        "QKV channels crossed a token or batch boundary");
    }
    ggml_backend_free(backend);
    ggml_free(ctx);
}

int main()
{
    try {
        for (bool fused : {false, true})
            for (bool flowMap : {false, true}) detection(fused, flowMap);
        projectionViews();
        std::cout << "FLUX.3 fused/separate detection and QKV views passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
