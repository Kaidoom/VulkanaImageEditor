// LayerDiff 3D v3. Architecture reference: See-through (Apache-2.0),
// common/modules/layerdiffuse/{layerdiff3d,transformer3d}.py.
#include "denoiser.hpp"
#include "projections.hpp"

#include "ggml.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace layerdiff {
namespace {
using Node = ggml_tensor*;

void requireShape(bool condition, const std::string& what) {
    if (!condition)
        throw std::runtime_error("Invalid LayerDiff " + what);
}

Tensor partSlice(const Tensor& t, int first, int count) {
    const auto stride = t.values.size() / t.shape.front();
    Tensor out;
    out.shape = t.shape;
    out.shape.front() = count;
    out.values.assign(t.values.begin() + first * stride, t.values.begin() + (first + count) * stride);
    return out;
}

// Self-attention across semantic parts belongs in its own operation: it cannot
// be approximated by evaluating one text prompt and image at a time.
class Blocks {
  public:
    Blocks(Graph& graph, Weights& weights) : ctx(graph.ctx), w(weights), kernels(graph.projectionKernels()) {}

    Node linear(const std::string& key, Node x, bool bias = true) const {
        Node weight = w(key + ".weight");
        const int64_t tokens = x->ne[1], parts = x->ne[2];
        requireShape(x->ne[0] == weight->ne[0] && x->ne[3] == 1 && weight->ne[2] == 1 && weight->ne[3] == 1, key);
        Node flat = ggml_reshape_2d(ctx, contiguous(x), x->ne[0], tokens * parts);
        Node result = kernels.halfProducts && usesHalfProjection(flat->ne[1])
                    ? kernels.halfProducts(ctx, weight, flat) : nullptr;
        if (!result) result = ggml_mul_mat(ctx, weight, flat);
        if (bias) {
            auto b = w(key + ".bias");
            requireShape(b->type == GGML_TYPE_F32 && ggml_is_vector(b) && b->ne[0] == result->ne[0], key + ".bias");
            // Widen during the bias addition so bias and downstream residuals stay in FP32.
            result = result->type == GGML_TYPE_F16 ? ggml_add_cast(ctx, result, b, GGML_TYPE_F32)
                                                   : ggml_add(ctx, result, b);
        }
        return ggml_reshape_3d(ctx, result, result->ne[0], tokens, parts);
    }

    Node groupEmbedding(const std::string& key, Node x, int parts, bool tokens) const {
        Node p = f32(w(key + ".params"));
        requireShape(p->ne[0] == x->ne[0] && p->ne[1] == parts, key);
        if (tokens)
            p = ggml_reshape_3d(ctx, p, p->ne[0], 1, parts);
        return ggml_add(ctx, x, linear(key + ".linear", ggml_add(ctx, x, p)));
    }

    Node mlp(const std::string& key, Node x) const {
        return linear(key + ".linear_2", ggml_silu(ctx, linear(key + ".linear_1", x)));
    }

    Node conv(const std::string& key, Node x, int stride = 1, int padding = 1) const {
        Node weight = w(key + ".weight");
        requireShape(weight->ne[2] == x->ne[2], key);
        Node out = ggml_conv_2d_direct(ctx, weight, contiguous(x), stride, stride, padding, padding, 1, 1);
        Node bias = w(key + ".bias");
        requireShape(ggml_nelements(bias) == out->ne[2], key + ".bias");
        return ggml_add(ctx, out, ggml_reshape_4d(ctx, bias, 1, 1, bias->ne[0], 1));
    }

    Node groupNorm(const std::string& key, Node x, float eps = 1e-5f) const {
        requireShape(x->ne[2] % 32 == 0 && w(key + ".weight")->ne[0] == x->ne[2], key);
        auto channel = [&](const char* suffix) {
            auto tensor = f32(w(key + suffix));
            requireShape(ggml_nelements(tensor) == x->ne[2], key + suffix);
            return ggml_reshape_4d(ctx, tensor, 1, 1, x->ne[2], 1);
        };
        Node norm = ggml_group_norm(ctx, contiguous(x), 32, eps);
        return ggml_add(ctx, ggml_mul(ctx, norm, channel(".weight")), channel(".bias"));
    }

    Node resnet(const std::string& key, Node x, Node embedding) const {
        Node h = conv(key + ".conv1", ggml_silu(ctx, groupNorm(key + ".norm1", x)));
        Node e = linear(key + ".time_emb_proj", embedding);
        h = ggml_add(ctx, h, ggml_reshape_4d(ctx, e, 1, 1, e->ne[0], x->ne[3]));
        h = conv(key + ".conv2", ggml_silu(ctx, groupNorm(key + ".norm2", h)));
        if (x->ne[2] != h->ne[2])
            x = conv(key + ".conv_shortcut", x, 1, 0);
        return ggml_add(ctx, h, x);
    }

    Node transform(const std::string& key, Node image, Node context, int depth) const {
        const int64_t width = image->ne[0], height = image->ne[1], channels = image->ne[2], parts = image->ne[3];
        Node h = groupNorm(key + ".norm", image, 1e-6f);
        h = ggml_reshape_3d(ctx, h, width * height, channels, parts);
        h = ggml_cont(ctx, ggml_permute(ctx, h, 1, 0, 2, 3));
        h = linear(key + ".proj_in", h);
        const int temporalStride = depth >= 3 ? 2 : 1;
        for (int index = 0; index < depth; ++index) {
            h = spatialBlock(key + ".transformer_blocks." + std::to_string(index), h, context);
            if ((index + 1) % temporalStride == 0)
                h = ggml_add(ctx, h,
                             temporalDelta(key + ".temporal_transformer_blocks." +
                                               std::to_string((index + 1) / temporalStride - 1),
                                           h));
        }
        h = linear(key + ".proj_out", h);
        h = ggml_cont(ctx, ggml_permute(ctx, h, 1, 0, 2, 3));
        return ggml_add(ctx, image, ggml_reshape_4d(ctx, h, width, height, channels, parts));
    }

  private:
    ggml_context* ctx;
    Weights& w;
    ProjectionKernels kernels;
    static constexpr int64_t chunkRows = 8192;

    Node contiguous(Node t) const {
        return ggml_is_contiguous(t) ? t : ggml_cont(ctx, t);
    }
    Node f32(Node t) const {
        return t->type == GGML_TYPE_F32 ? t : ggml_cast(ctx, t, GGML_TYPE_F32);
    }

    Node norm(const std::string& key, Node x) const {
        auto weight = f32(w(key + ".weight")), bias = f32(w(key + ".bias"));
        requireShape(ggml_nelements(weight) == x->ne[0] && ggml_nelements(bias) == x->ne[0], key);
        auto normalized = ggml_norm(ctx, x, 1e-5f);
        // Preserve FP32 for any tail that will use ggml's small-batch path.
        if (kernels.affineInput && usesHalfProjection(1 + (ggml_nrows(x) - 1) % chunkRows)) {
            if (auto packed = kernels.affineInput(ctx, normalized, weight, bias)) return packed;
        }
        return ggml_add(ctx, ggml_mul(ctx, normalized, weight), bias);
    }

    Node attention(const std::string& key, Node input, Node context) const {
        const int64_t channels = input->ne[0], parts = input->ne[2], heads = channels / 64;
        requireShape(channels % 64 == 0 && context->ne[2] == parts, key);
        auto project = [&](const char* suffix, Node src, bool half) {
            Node z = linear(key + suffix, src, false);
            requireShape(z->ne[0] == channels, key);
            z = ggml_reshape_4d(ctx, z, 64, heads, src->ne[1], parts);
            z = ggml_permute(ctx, z, 0, 2, 1, 3);
            // Flash attention uses float queries and half keys/values.
            auto type = half ? GGML_TYPE_F16 : GGML_TYPE_F32;
            return z->type == type ? contiguous(z) : ggml_cast(ctx, z, type);
        };
        auto projectionInput = [&](Node src) {
            return src->type != GGML_TYPE_F16 && kernels.halfProducts && usesHalfProjection(ggml_nrows(src))
                ? ggml_cast(ctx, src, GGML_TYPE_F16) : src;
        };
        Node qInput = projectionInput(input);
        context = context == input ? qInput : projectionInput(context);
        Node q = project(".to_q", qInput, false);
        Node k = project(".to_k", context, true);
        Node v = project(".to_v", context, true);
        Node out = ggml_flash_attn_ext(ctx, q, k, v, nullptr, 1.f / 8.f, 0.f, 0.f);
        ggml_prec_set_acc(out, GGML_PREC_F32);
        return linear(key + ".to_out.0", ggml_reshape_3d(ctx, out, channels, input->ne[1], parts));
    }

    Node ffn(const std::string& key, Node x) const {
        const int64_t width = x->ne[0], rows = x->ne[1] * x->ne[2];
        Node flat = ggml_reshape_2d(ctx, contiguous(x), width, rows);
        // GEGLU expands each independent token row eightfold. Chunk it so the
        // graph allocator can reuse temporary buffers across the widest grids.
        Node result = nullptr;
        for (int64_t offset = 0; offset < rows; offset += chunkRows) {
            const int64_t count = std::min(rows - offset, chunkRows);
            Node slice = ggml_view_2d(ctx, flat, width, count, flat->nb[1], offset * flat->nb[1]);
            Node both = linear(key + ".net.0.proj", slice, false);
            Node bias = w(key + ".net.0.proj.bias");
            requireShape(both->ne[0] % 2 == 0 && ggml_is_vector(bias) && bias->ne[0] == both->ne[0], key);
            Node h = linear(key + ".net.2", biasedGeglu(ctx, both, bias, kernels.biasedGate));
            result = result ? ggml_concat(ctx, result, h, 1) : h;
        }
        return ggml_reshape_3d(ctx, result, width, x->ne[1], x->ne[2]);
    }

    Node spatialBlock(const std::string& key, Node h, Node context) const {
        Node n = norm(key + ".norm1", h);
        h = ggml_add(ctx, h, attention(key + ".attn1", n, n));
        h = ggml_add(ctx, h, attention(key + ".attn2", norm(key + ".norm2", h), context));
        return ggml_add(ctx, h, ffn(key + ".ff", norm(key + ".norm3", h)));
    }

    Node temporalDelta(const std::string& key, Node x) const {
        // [C,pixels,parts] -> [C,parts,pixels]. No pixel borrows data from a
        // neighboring location, but all parts attend at the same pixel.
        Node h = ggml_cont(ctx, ggml_permute(ctx, x, 0, 2, 1, 3));
        h = ggml_add(ctx, h, ffn(key + ".ff_in", norm(key + ".norm_in", h)));
        Node n = norm(key + ".norm1", h);
        h = ggml_add(ctx, h, attention(key + ".attn1", n, n));
        h = ffn(key + ".ff", norm(key + ".norm3", h));
        return ggml_cont(ctx, ggml_permute(ctx, h, 0, 2, 1, 3));
    }
};

Tensor positional(const Tensor& values, int width) {
    Tensor out;
    out.shape = {values.shape.front(), static_cast<int64_t>(values.values.size() / values.shape.front()) * width};
    out.values.reserve(values.values.size() * width);
    for (float t : values.values) {
        const size_t begin = out.values.size();
        out.values.resize(begin + width);
        for (int i = 0; i < width / 2; ++i) {
            const float omega = std::exp(-std::log(10000.f) * i / (width / 2));
            out.values[begin + i] = std::cos(t * omega);
            out.values[begin + i + width / 2] = std::sin(t * omega);
        }
    }
    return out;
}

// Keep the per-convolution batch shape identical on both paths. The bounded
// groups also limit the size of the temporary convolution inputs.
int64_t frameChunk(int64_t pixels, int64_t channels, int64_t count) {
    return std::clamp<int64_t>((48 * 1024 * 1024) / (pixels * channels), 1, count);
}

struct HostStages {
    Backend& backend;
    Weights& weights;
    template <class Build>
    Tensor frameBlocks(const Tensor& x, const Tensor* emb, const Tensor* skip, Build build) const {
        Tensor out;
        int64_t count = x.shape.front();
        const auto chunk = frameChunk(x.shape[2] * x.shape[3], x.shape[1] + (skip ? skip->shape[1] : 0), count);
        for (int64_t first = 0; first < count; first += chunk) {
            int64_t n = std::min(chunk, count - first);
            Graph g(backend);
            auto node = [&](const Tensor* src) -> Node {
                if (!src)
                    return nullptr;
                if (n == count)
                    return g.inputRef(*src);
                return g.input(partSlice(*src, first, n));
            };
            Blocks block(g, weights);
            Node v = build(g, block, node(&x), node(emb), node(skip));
            auto step = g.evaluate(v, {n, v->ne[2], v->ne[1], v->ne[0]});
            if (out.shape.empty()) {
                if (n == count)
                    return step;
                out.shape = step.shape;
                out.shape[0] = count;
                out.values.reserve(step.values.size() * count / n);
            }
            out.values.insert(out.values.end(), step.values.begin(), step.values.end());
        }
        return out;
    }

    Tensor transform(const std::string& key, const Tensor& x, const Tensor& context, int depth) const {
        Graph g(backend);
        Blocks b(g, weights);
        return g.evaluate(b.transform(key, g.inputRef(x), g.inputRef(context), depth), x.shape);
    }
};

struct DeviceStages {
    Graph& graph;
    Blocks& block;
    template <class Build>
    Node frameBlocks(Node x, Node const* emb, Node const* skip, Build build) const {
        const auto count = x->ne[3];
        const auto chunk = frameChunk(x->ne[0] * x->ne[1], x->ne[2] + (skip ? (*skip)->ne[2] : 0), count);
        Node result = nullptr;
        for (int64_t first = 0; first < count; first += chunk) {
            const auto n = std::min(chunk, count - first);
            auto slice = [&](Node v) {
                return n == count ? v : ggml_view_4d(graph.ctx, v, v->ne[0], v->ne[1], v->ne[2], n,
                                                      v->nb[1], v->nb[2], v->nb[3], first * v->nb[3]);
            };
            Node e = emb ? *emb : nullptr;
            if (e && n < count)
                e = ggml_view_2d(graph.ctx, e, e->ne[0], n, e->nb[1], first * e->nb[1]);
            Node step = build(graph, block, slice(x), e, skip ? slice(*skip) : nullptr);
            result = result ? ggml_concat(graph.ctx, result, step, 3) : step;
        }
        return result;
    }
    Node transform(const std::string& key, Node x, Node context, int depth) const {
        return block.transform(key, x, context, depth);
    }
};

template<class Stages, class Value>
Value network(Stages& stages, const Value& input, const Value& emb, const Value& context) {
    auto convFrames = [&](const std::string& key, const Value& x, int stride, int pad, bool up = false) {
        return stages.frameBlocks(x, nullptr, nullptr, [&](Graph& g, Blocks& b, Node h, Node, Node) {
            if (up)
                h = ggml_interpolate(g.ctx, h, h->ne[0] * 2, h->ne[1] * 2, h->ne[2], h->ne[3],
                                     GGML_SCALE_MODE_NEAREST);
            return b.conv(key, h, stride, pad);
        });
    };
    auto residual = [&](const std::string& key, const Value& x, const Value* skip = nullptr) {
        return stages.frameBlocks(x, &emb, skip, [&](Graph& g, Blocks& b, Node h, Node e, Node s) {
            if (s)
                h = ggml_concat(g.ctx, h, s, 2);
            return b.resnet(key, h, e);
        });
    };
    std::vector<Value> skips;
    skips.reserve(9);
    skips.push_back(convFrames("conv_in", input, 1, 1));
    const std::array downDepth{0, 2, 10};
    for (int i = 0; i < 3; ++i) {
        const auto block = "down_blocks." + std::to_string(i);
        for (int j = 0; j < 2; ++j) {
            const auto ix = std::to_string(j);
            Value next = residual(block + ".resnets." + ix, skips.back());
            if (downDepth[i])
                next = stages.transform(block + ".attentions." + ix, next, context, downDepth[i]);
            skips.push_back(std::move(next));
        }
        if (i < 2)
            skips.push_back(convFrames(block + ".downsamplers.0.conv", skips.back(), 2, 1));
    }
    Value x = residual("mid_block.resnets.0", skips.back());
    x = stages.transform("mid_block.attentions.0", x, context, 10);
    x = residual("mid_block.resnets.1", x);
    const std::array upDepth{10, 2, 0};
    for (int i = 0; i < 3; ++i) {
        const auto block = "up_blocks." + std::to_string(i);
        for (int j = 0; j < 3; ++j) {
            const auto ix = std::to_string(j);
            x = residual(block + ".resnets." + ix, x, &skips.back());
            skips.pop_back();
            if (upDepth[i])
                x = stages.transform(block + ".attentions." + ix, x, context, upDepth[i]);
        }
        if (i < 2)
            x = convFrames(block + ".upsamplers.0.conv", x, 1, 1, true);
    }
    return stages.frameBlocks(x, nullptr, nullptr, [&](Graph& g, Blocks& b, Node h, Node, Node) {
        return b.conv("conv_out", ggml_silu(g.ctx, b.groupNorm("conv_norm_out", h)));
    });
}
} // namespace

Tensor denoise(Backend& backend, Weights& weights, const Tensor& input, double time, const Tensor& tokens,
               const Tensor& pooled, const Tensor& timeIds, int group, bool& tryResident) {
    const int parts = group == 0 ? 13 : 11;
    requireShape((group == 0 || group == 1) && input.shape.size() == 4 && input.shape[0] == parts &&
                     input.shape[1] == 8 && input.shape[2] > 0 && input.shape[3] > 0 &&
                     input.shape[2] % 4 == 0 && input.shape[3] % 4 == 0,
                 "latent grid");
    requireShape(tokens.shape == std::vector<int64_t>{parts, 77, 2048} &&
                     pooled.shape == std::vector<int64_t>{parts, 1280} &&
                     timeIds.shape == std::vector<int64_t>{parts, 6},
                 "conditioning shapes");
    Tensor timesteps;
    timesteps.shape = {parts};
    timesteps.values.assign(parts, static_cast<float>(time));
    auto pos = positional(timesteps, 320);
    auto size = positional(timeIds, 256);
    Tensor emb;
    const std::string groupName = std::to_string(group);
    {
        Graph g(backend);
        Blocks b(g, weights);
        Node pooledGroup = b.groupEmbedding("group_embeds." + groupName, g.inputRef(pooled), parts, false);
        Node joint = ggml_concat(g.ctx, pooledGroup, g.inputRef(size), 0);
        Node result = ggml_add(g.ctx, b.mlp("time_embedding", g.inputRef(pos)), b.mlp("add_embedding", joint));
        emb = g.evaluate(ggml_silu(g.ctx, result), {parts, 1280});
    }
    Tensor context;
    {
        Graph g(backend);
        Blocks b(g, weights);
        context = g.evaluate(b.groupEmbedding("group_embeds2." + groupName, g.inputRef(tokens), parts, true),
                             tokens.shape);
    }
    if (tryResident) {
        // A complete step keeps skip connections on the GPU. Narrower devices can still
        // run the identical network with host-staged skips for the rest of the pass.
        Graph graph(backend, 24576);
        Blocks block(graph, weights);
        DeviceStages device{graph, block};
        auto output = network(device, graph.inputRef(input), graph.inputRef(emb), graph.inputRef(context));
        if (auto result = graph.evaluateIfFits(output, {parts, 4, input.shape[2], input.shape[3]}))
            return std::move(*result);
        tryResident = false;
    }
    HostStages host{backend, weights};
    return network(host, input, emb, context);
}
} // namespace layerdiff
