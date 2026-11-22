#include "clip.hpp"
#include "weight_shape.hpp"

#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>

namespace layerdiff {
namespace {

using json = nlohmann::json;
using Node = ggml_tensor*;
constexpr int tokensPerTag = 77;

json readJson(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file)
        throw std::runtime_error("Could not open the See-through component " + path.filename().string());
    return json::parse(file);
}

struct Tokens {
    Tensor ids, pooledIds;
};

Tokens tokenize(const std::filesystem::path& directory, const std::vector<std::string>& tags) {
    const auto vocab = readJson(directory / "vocab.json");
    const auto config = readJson(directory / "tokenizer_config.json");
    std::ifstream merges(directory / "merges.txt");
    if (!merges)
        throw std::runtime_error("Missing See-through CLIP tokenizer merges");
    using Pair = std::pair<std::string, std::string>;
    std::map<Pair, size_t> ranks;
    std::string line;
    std::getline(merges, line); // CLIP BPE version header
    while (std::getline(merges, line)) {
        std::istringstream row(line);
        std::string left, right;
        if (row >> left >> right)
            ranks.emplace(Pair{left, right}, ranks.size());
    }
    const auto id = [&](const std::string& symbol) {
        const int token = vocab.at(symbol).get<int>();
        if (token < 0 || token >= 49408)
            throw std::runtime_error("Invalid See-through CLIP token id");
        return float(token);
    };
    const int64_t n = tags.size();
    Tokens token{{{n * tokensPerTag}, {}}, {{n}, {}}};
    const float pad = id(config.at("pad_token").get<std::string>());
    for (const auto& tag : tags) {
        const auto start = token.ids.values.size();
        token.ids.values.push_back(id("<|startoftext|>"));
        // See-through v3's trained semantic labels are lowercase ASCII words.
        // No free-form user prompt is interpreted by this profile.
        std::istringstream words(tag);
        std::string word;
        while (words >> word) {
            std::vector<std::string> symbols;
            for (char c : word) {
                if (c < 'a' || c > 'z')
                    throw std::invalid_argument("Unexpected See-through tag");
                symbols.emplace_back(1, c);
            }
            symbols.back() += "</w>";
            while (symbols.size() > 1) {
                auto bestRank = std::numeric_limits<size_t>::max();
                Pair best;
                for (size_t i = 1; i < symbols.size(); ++i) {
                    Pair pair{symbols[i - 1], symbols[i]};
                    auto rank = ranks.find(pair);
                    if (rank != ranks.end() && rank->second < bestRank) {
                        bestRank = rank->second;
                        best = std::move(pair);
                    }
                }
                if (bestRank == std::numeric_limits<size_t>::max())
                    break;
                std::vector<std::string> next;
                for (size_t i = 0; i < symbols.size(); ++i) {
                    if (i + 1 < symbols.size() && symbols[i] == best.first && symbols[i + 1] == best.second) {
                        next.push_back(symbols[i] + symbols[i + 1]);
                        ++i;
                    } else
                        next.push_back(symbols[i]);
                }
                symbols = std::move(next);
            }
            for (const auto& symbol : symbols)
                token.ids.values.push_back(id(symbol));
        }
        token.pooledIds.values.push_back(float(token.ids.values.size()));
        token.ids.values.push_back(id("<|endoftext|>"));
        if (token.ids.values.size() - start > tokensPerTag)
            throw std::runtime_error("See-through tag exceeds CLIP token limit");
        token.ids.values.resize(start + tokensPerTag, pad);
    }
    return token;
}

Tensor encodeOne(Backend& backend, const std::filesystem::path& root, const std::vector<std::string>& tags,
                 bool large) {
    const std::string suffix = large ? "_2" : "";
    const auto path = root / ("text_encoder" + suffix);
    const auto cfg = readJson(path / "config.json");
    const int64_t width = large ? 1280 : 768, heads = width / 64, n = tags.size();
    const int depth = large ? 32 : 12;
    if (cfg.at("hidden_size") != width || cfg.at("num_attention_heads") != heads ||
        cfg.at("num_hidden_layers") != depth || cfg.at("max_position_embeddings") != tokensPerTag ||
        cfg.at("hidden_act") != (large ? "gelu" : "quick_gelu"))
        throw std::runtime_error("Unexpected See-through CLIP encoder architecture");

    Weights weights(backend, path / "model.safetensors");
    auto token = tokenize(root / ("tokenizer" + suffix), tags);
    Graph graph(backend);
    auto ctx = graph.ctx;
    auto embedding = checkedWeight(weights, "text_model.embeddings.token_embedding.weight", width, 49408);
    auto position =
        checkedWeight(weights, "text_model.embeddings.position_embedding.weight", width, tokensPerTag);
    auto ids = ggml_cast(ctx, graph.input(token.ids), GGML_TYPE_I32);
    auto x = ggml_reshape_3d(ctx, ggml_get_rows(ctx, embedding, ids), width, tokensPerTag, n);
    x = ggml_add(ctx, x, ggml_cast(ctx, position, GGML_TYPE_F32));

    auto linear = [&](Node v, const std::string& key, int64_t outputs) {
        auto w = checkedWeight(weights, key + ".weight", v->ne[0], outputs);
        auto b = checkedWeight(weights, key + ".bias", outputs);
        return ggml_add(ctx, ggml_mul_mat(ctx, w, v), b);
    };
    auto norm = [&](Node v, const std::string& key) {
        auto w = checkedWeight(weights, key + ".weight", width);
        auto b = checkedWeight(weights, key + ".bias", width);
        return ggml_add(ctx, ggml_mul(ctx, ggml_norm(ctx, v, 1.e-5f), w), b);
    };
    Node penultimate{};
    for (int i = 0; i < depth; ++i) {
        const auto key = "text_model.encoder.layers." + std::to_string(i) + ".";
        const auto z = norm(x, key + "layer_norm1");
        auto project = [&](const char* name) {
            auto p = linear(z, key + "self_attn." + name, width);
            p = ggml_reshape_4d(ctx, p, 64, heads, tokensPerTag, n);
            return ggml_cont(ctx, ggml_permute(ctx, p, 0, 2, 1, 3));
        };
        auto q = project("q_proj"), k = project("k_proj"), v = project("v_proj");
        auto scores = ggml_mul_mat(ctx, ggml_cast(ctx, k, GGML_TYPE_F16), q);
        scores = ggml_diag_mask_inf(ctx, ggml_scale(ctx, scores, 1.f / 8.f), 0);
        auto probs = ggml_soft_max(ctx, scores);
        auto vt = ggml_cast(ctx, ggml_cont(ctx, ggml_transpose(ctx, v)), GGML_TYPE_F16);
        auto attn = ggml_mul_mat(ctx, vt, probs);
        attn =
            ggml_reshape_3d(ctx, ggml_cont(ctx, ggml_permute(ctx, attn, 0, 2, 1, 3)), width, tokensPerTag, n);
        x = ggml_add(ctx, x, linear(attn, key + "self_attn.out_proj", width));
        auto f = linear(norm(x, key + "layer_norm2"), key + "mlp.fc1", width * 4);
        f = large ? ggml_gelu(ctx, f) : ggml_gelu_quick(ctx, f);
        x = ggml_add(ctx, x, linear(f, key + "mlp.fc2", width));
        if (i == depth - 2) {
            penultimate = x;
            if (!large)
                break;
        }
    }
    if (!large)
        return graph.evaluate(penultimate, {n, tokensPerTag, width});

    auto eot = ggml_cast(ctx, graph.input(token.pooledIds), GGML_TYPE_I32);
    auto last = ggml_reshape_2d(ctx, norm(x, "text_model.final_layer_norm"), width, n * tokensPerTag);
    auto projection = checkedWeight(weights, "text_projection.weight", width, width);
    auto pooled = ggml_mul_mat(ctx, projection, ggml_get_rows(ctx, last, eot));
    auto both = ggml_concat(ctx, penultimate, ggml_reshape_3d(ctx, pooled, width, 1, n), 1);
    return graph.evaluate(both, {n, tokensPerTag + 1, width});
}
} // namespace

Conditioning encodeParts(Backend& backend, const std::filesystem::path& root,
                         const std::vector<std::string>& tags) {
    if (tags.empty() || tags.size() > 24)
        throw std::invalid_argument("Invalid See-through semantic group");
    auto small = encodeOne(backend, root, tags, false);
    auto big = encodeOne(backend, root, tags, true);
    const int64_t n = tags.size();
    Conditioning result{{{n, tokensPerTag, 2048}, {}}, {{n, 1280}, {}}};
    result.context.values.reserve(n * tokensPerTag * 2048);
    result.pooled.values.reserve(n * 1280);
    for (int64_t i = 0; i < n; ++i) {
        for (int j = 0; j < tokensPerTag; ++j) {
            auto s = small.values.begin() + (i * tokensPerTag + j) * 768;
            auto b = big.values.begin() + (i * (tokensPerTag + 1) + j) * 1280;
            result.context.values.insert(result.context.values.end(), s, s + 768);
            result.context.values.insert(result.context.values.end(), b, b + 1280);
        }
        auto p = big.values.begin() + (i * (tokensPerTag + 1) + tokensPerTag) * 1280;
        result.pooled.values.insert(result.pooled.values.end(), p, p + 1280);
    }
    return result;
}

} // namespace layerdiff
