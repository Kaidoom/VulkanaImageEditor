#include "vae.hpp"
#include "weight_shape.hpp"
#include <array>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace layerdiff {
namespace {

using Node = ggml_tensor*;
constexpr float latentScale = 0.13025f;

std::string block(const std::string& parent, const char* child, int index) {
    return parent + "." + child + "." + std::to_string(index);
}

class VaeGraph {
  public:
    VaeGraph(ggml_context* context, Weights& weights, int groupCount, float epsilon)
        : ctx(context), weights_(weights), groupCount_(groupCount), epsilon_(epsilon) {}

    Node conv(Node x, const std::string& key, int64_t outputs, int kernel = 3, int stride = 1,
              int padding = 1) {
        auto w = checkedWeight(weights_, key + ".weight", kernel, kernel, x->ne[2], outputs);
        auto b = checkedWeight(weights_, key + ".bias", outputs);
        auto y = ggml_conv_2d_direct(ctx, w, ggml_cont(ctx, x), stride, stride, padding, padding, 1, 1);
        return ggml_add(ctx, y, ggml_reshape_4d(ctx, b, 1, 1, b->ne[0], 1));
    }

    Node norm(Node x, const std::string& key) {
        const int64_t c = x->ne[2];
        auto w = checkedWeight(weights_, key + ".weight", c);
        auto b = checkedWeight(weights_, key + ".bias", c);
        auto normalized = ggml_group_norm(ctx, ggml_cont(ctx, x), groupCount_, epsilon_);
        return ggml_add(ctx, ggml_mul(ctx, normalized, ggml_reshape_4d(ctx, w, 1, 1, c, 1)),
                        ggml_reshape_4d(ctx, b, 1, 1, c, 1));
    }

    Node residual(Node x, const std::string& key, int64_t channels) {
        auto h = conv(ggml_silu(ctx, norm(x, key + ".norm1")), key + ".conv1", channels);
        h = conv(ggml_silu(ctx, norm(h, key + ".norm2")), key + ".conv2", channels);
        if (x->ne[2] != h->ne[2])
            x = conv(x, key + ".conv_shortcut", channels, 1, 1, 0);
        return ggml_add(ctx, x, h);
    }

    Node attention(Node x, const std::string& key, int headSize) {
        const int64_t w = x->ne[0], h = x->ne[1], c = x->ne[2], n = w * h;
        if (x->ne[3] != 1 || c % headSize)
            throw std::runtime_error("Invalid VAE attention dimensions");
        const int64_t heads = c / headSize;
        auto normed = norm(x, key + ".group_norm");
        auto hidden = ggml_cont(ctx, ggml_transpose(ctx, ggml_reshape_2d(ctx, normed, n, c)));
        auto project = [&](const char* suffix) {
            auto p = linear(hidden, key + suffix);
            auto split = ggml_reshape_3d(ctx, p, headSize, heads, n);
            return ggml_cont(ctx, ggml_permute(ctx, split, 0, 2, 1, 3)); // [D,T,H]
        };
        auto q = project(".to_q"), k = project(".to_k"), v = project(".to_v");
        Node out;
        if (headSize == 8) {
            // ggml CUDA flash attention supports 64 features per head. Padding
            // with zeros preserves the original eight-feature dot product.
            auto q64 = ggml_pad(ctx, q, 56, 0, 0, 0);
            auto k64 = ggml_cast(ctx, ggml_pad(ctx, k, 56, 0, 0, 0), GGML_TYPE_F16);
            auto v64 = ggml_cast(ctx, ggml_pad(ctx, v, 56, 0, 0, 0), GGML_TYPE_F16);
            auto attention = ggml_flash_attn_ext(ctx, q64, k64, v64, nullptr, 1.f / std::sqrt(8.f), 0, 0);
            auto selected = ggml_view_3d(ctx, attention, 8, heads, n, attention->nb[1], attention->nb[2], 0);
            out = ggml_reshape_2d(ctx, ggml_cont(ctx, selected), c, n);
        } else {
            if (heads != 1 || headSize != 512)
                throw std::runtime_error("Unsupported VAE attention head");
#ifdef VULKANA_LAYERDIFF_VULKAN
            // Vulkan supports the RGB VAE's 512-wide head directly. Use online
            // softmax with FP32 accumulation across the entire spatial grid.
            auto attn = ggml_flash_attn_ext(ctx, q, ggml_cast(ctx, k, GGML_TYPE_F16),
                                            ggml_cast(ctx, v, GGML_TYPE_F16), nullptr,
                                            1.f / std::sqrt(float(headSize)), 0.f, 0.f);
            ggml_prec_set_acc(attn, GGML_PREC_F32);
            out = ggml_reshape_2d(ctx, attn, c, n);
#else
            // CUDA evaluates these queries in chunks, each attending to every key.
            auto kt = ggml_cast(ctx, k, GGML_TYPE_F16);
            auto vt = ggml_cast(ctx, ggml_cont(ctx, ggml_transpose(ctx, v)), GGML_TYPE_F16);
            constexpr int64_t chunk = 256;
            std::vector<Node> chunks;
            for (int64_t start = 0; start < n; start += chunk) {
                auto count = std::min(chunk, n - start);
                auto slice = ggml_view_2d(ctx, q, headSize, count, q->nb[1], start * q->nb[1]);
                auto scores = ggml_mul_mat(ctx, kt, slice);
                auto prob = ggml_soft_max_ext(ctx, scores, nullptr, 1.f / std::sqrt(float(headSize)), 0.f);
                chunks.push_back(ggml_mul_mat(ctx, vt, prob));
            }
            while (chunks.size() > 1) {
                std::vector<Node> next;
                for (size_t i = 0; i < chunks.size(); i += 2)
                    next.push_back(i + 1 == chunks.size() ? chunks[i]
                                                          : ggml_concat(ctx, chunks[i], chunks[i + 1], 1));
                chunks = std::move(next);
            }
            out = chunks.front();
#endif
        }
        out = linear(out, key + ".to_out.0");
        out = ggml_cont(ctx, ggml_transpose(ctx, out));
        return ggml_add(ctx, x, ggml_reshape_4d(ctx, out, w, h, c, 1));
    }

    Node mid(Node x, const std::string& key, int headSize) {
        x = residual(x, key + ".resnets.0", x->ne[2]);
        x = attention(x, key + ".attentions.0", headSize);
        return residual(x, key + ".resnets.1", x->ne[2]);
    }

    Node up(Node x, const std::string& key) {
        return conv(ggml_upscale(ctx, x, 2, GGML_SCALE_MODE_NEAREST), key, x->ne[2]);
    }

    ggml_context* ctx;

  private:
    Node linear(Node x, const std::string& key) {
        auto w = checkedWeight(weights_, key + ".weight", x->ne[0], x->ne[0]);
        auto b = checkedWeight(weights_, key + ".bias", x->ne[0]);
        return ggml_add(ctx, ggml_mul_mat(ctx, w, x), b);
    }

    Weights& weights_;
    int groupCount_;
    float epsilon_;
};

Node encodeMoments(VaeGraph& g, Node x) {
    constexpr std::array<int, 4> widths{128, 256, 512, 512};
    x = g.conv(x, "encoder.conv_in", widths.front());
    for (int i = 0; i < 4; ++i) {
        auto key = block("encoder", "down_blocks", i);
        for (int j = 0; j < 2; ++j)
            x = g.residual(x, block(key, "resnets", j), widths[i]);
        if (i < 3) {
            // Diffusers AutoencoderKL's stride-two convolution adds its single
            // padding pixel on the right/bottom, keeping the source grid aligned.
            x = ggml_pad(g.ctx, x, 1, 1, 0, 0);
            x = g.conv(x, key + ".downsamplers.0.conv", widths[i], 3, 2, 0);
        }
    }
    x = g.mid(x, "encoder.mid_block", 512);
    x = g.conv(ggml_silu(g.ctx, g.norm(x, "encoder.conv_norm_out")), "encoder.conv_out", 8);
    return g.conv(x, "quant_conv", 8, 1, 1, 0);
}

Node decodeRgb(VaeGraph& g, Node x) {
    constexpr std::array<int, 4> widths{512, 512, 256, 128};
    x = g.conv(x, "post_quant_conv", 4, 1, 1, 0);
    x = g.conv(x, "decoder.conv_in", widths.front());
    x = g.mid(x, "decoder.mid_block", 512);
    for (int i = 0; i < 4; ++i) {
        const auto key = block("decoder", "up_blocks", i);
        for (int j = 0; j < 3; ++j)
            x = g.residual(x, block(key, "resnets", j), widths[i]);
        if (i < 3)
            x = g.up(x, key + ".upsamplers.0.conv");
    }
    return g.conv(ggml_silu(g.ctx, g.norm(x, "decoder.conv_norm_out")), "decoder.conv_out", 3);
}

Node decodeArgb(VaeGraph& g, Node rgb, Node latent) {
    const std::string prefix = "decoder.model";
    constexpr std::array<int, 7> widths{32, 32, 64, 128, 256, 512, 512};
    auto x = g.conv(rgb, prefix + ".conv_in", widths.front());
    std::vector<Node> skips{x};
    for (int i = 0; i < 7; ++i) {
        auto key = block(prefix, "down_blocks", i);
        if (i == 3)
            x = ggml_add(g.ctx, x, g.conv(latent, prefix + ".latent_conv_in", x->ne[2], 1, 1, 0));
        for (int j = 0; j < 2; ++j) {
            x = g.residual(x, block(key, "resnets", j), widths[i]);
            if (i >= 4)
                x = g.attention(x, block(key, "attentions", j), 8);
            skips.push_back(x);
        }
        if (i < 6) {
            x = g.conv(x, key + ".downsamplers.0.conv", widths[i], 3, 2, 1);
            skips.push_back(x);
        }
    }
    x = g.mid(x, prefix + ".mid_block", 8);
    for (int i = 0; i < 7; ++i) {
        auto key = block(prefix, "up_blocks", i);
        for (int j = 0; j < 3; ++j) {
            x = ggml_concat(g.ctx, x, skips.back(), 2);
            skips.pop_back();
            x = g.residual(x, block(key, "resnets", j), widths[6 - i]);
            if (i < 3)
                x = g.attention(x, block(key, "attentions", j), 8);
        }
        if (i < 6)
            x = g.up(x, key + ".upsamplers.0.conv");
    }
    return g.conv(ggml_silu(g.ctx, g.norm(x, prefix + ".conv_norm_out")), prefix + ".conv_out", 4);
}

void checkImage(const Tensor& image, int channels, int multiple) {
    if (image.shape.size() != 4 || image.shape[0] != 1 || image.shape[1] != channels || image.shape[2] <= 0 ||
        image.shape[3] <= 0 || image.shape[2] % multiple || image.shape[3] % multiple)
        throw std::invalid_argument("Invalid See-through VAE input geometry");
}

} // namespace

Vae::Vae(Backend& backend, const std::filesystem::path& directory)
    : backend_(backend), rgb_(backend, directory / "vae/diffusion_pytorch_model.safetensors"),
      transparency_(backend, directory / "trans_vae/diffusion_pytorch_model.safetensors") {
    // Build a minimal graph without evaluating it. A damaged transparent
    // decoder must fail at load, before spending two diffusion passes on it.
    Graph graph(backend_);
    VaeGraph rgb(graph.ctx, rgb_, 32, 1.e-6f);
    VaeGraph transparent(graph.ctx, transparency_, 4, 1.e-5f);
    auto image = ggml_new_tensor_4d(graph.ctx, GGML_TYPE_F32, 64, 64, 3, 1);
    auto latent = ggml_new_tensor_4d(graph.ctx, GGML_TYPE_F32, 8, 8, 4, 1);
    encodeMoments(rgb, image);
    decodeArgb(transparent, decodeRgb(rgb, latent), latent);
}

Tensor Vae::encode(const Tensor& rgb01, const Tensor& noise) {
    checkImage(rgb01, 3, 8);
    const auto h = rgb01.shape[2] / 8, w = rgb01.shape[3] / 8;
    if (noise.shape != std::vector<int64_t>{1, 4, h, w} || noise.values.size() != size_t(4 * h * w))
        throw std::invalid_argument("Invalid See-through VAE posterior noise");
    Graph builder(backend_);
    VaeGraph g(builder.ctx, rgb_, 32, 1.e-6f);
    auto rgb = builder.input(rgb01);
    auto moments = builder.evaluate(encodeMoments(g, ggml_scale_bias(g.ctx, rgb, 2.f, -1.f)), {1, 8, h, w});
    const auto count = noise.values.size();
    Tensor result{{1, 4, h, w}, std::vector<float>(count)};
    for (size_t i = 0; i < count; ++i) {
        const float sigma = std::exp(.5f * std::clamp(moments.values[count + i], -30.f, 20.f));
        result.values[i] = latentScale * (moments.values[i] + sigma * noise.values[i]);
    }
    return result;
}

Tensor Vae::decode(const Tensor& scaledLatent, const Tensor& alpha01) {
    checkImage(scaledLatent, 4, 8);
    const int64_t h = scaledLatent.shape[2] * 8, w = scaledLatent.shape[3] * 8;
    if (alpha01.shape != std::vector<int64_t>{1, 1, h, w} || alpha01.values.size() != size_t(h * w))
        throw std::invalid_argument("Invalid See-through VAE alpha mask");
    Graph builder(backend_);
    VaeGraph g(builder.ctx, rgb_, 32, 1.e-6f);
    auto latent = ggml_scale(g.ctx, builder.input(scaledLatent), 1.f / latentScale);
    auto rgb = ggml_scale_bias(g.ctx, decodeRgb(g, latent), .5f, .5f);
    VaeGraph trans(builder.ctx, transparency_, 4, 1.e-5f);
    auto argb = builder.evaluate(decodeArgb(trans, rgb, latent), {1, 4, h, w});
    const auto plane = size_t(h * w);
    Tensor result{{1, 4, h, w}, std::vector<float>(4 * plane)};
    for (size_t i = 0; i < plane; ++i) {
        for (size_t c = 0; c < 3; ++c)
            result.values[c * plane + i] = std::clamp(argb.values[(c + 1) * plane + i], 0.f, 1.f);
        result.values[3 * plane + i] = std::clamp(argb.values[i], 0.f, 1.f) * alpha01.values[i];
    }
    return result;
}

} // namespace layerdiff
