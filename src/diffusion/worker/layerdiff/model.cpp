#include "model.hpp"
#include "clip.hpp"
#include "core/rng_philox.hpp"
#include "denoiser.hpp"
#include "vae.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <json.hpp>
#include <map>

namespace layerdiff {
namespace fs = std::filesystem;
namespace {
using Json = nlohmann::json;
const std::vector<std::string> bodyTags{"front hair", "back hair", "head",       "neck",    "neckwear",
                                        "topwear",    "handwear",  "bottomwear", "legwear", "footwear",
                                        "tail",       "wings",     "objects"};
const std::vector<std::string> faceTags{"headwear", "face", "irides",  "eyebrow", "eyewhite", "eyelash",
                                        "eyewear",  "ears", "earwear", "nose",    "mouth"};
// Bottom to top. Depth is intentionally outside this profile.
const std::vector<std::string> order{"wings",      "tail",    "back hair", "footwear", "legwear", "bottomwear",
                                     "neck",       "topwear", "neckwear",  "handwear", "ears",    "face",
                                     "eyewhite",   "irides",  "nose",      "mouth",    "eyelash", "eyebrow",
                                     "front hair", "eyewear", "earwear",   "headwear", "objects"};
Json config(const fs::path &p) {
    if (!fs::is_regular_file(p) || fs::file_size(p) > 4 * 1024 * 1024)
        throw std::invalid_argument("Missing or invalid LayerDiff component: " +
                                    p.parent_path().filename().string() + "/" + p.filename().string());
    std::ifstream f(p);
    auto obj = Json::parse(f, nullptr, false);
    if (!obj.is_object())
        throw std::invalid_argument("Invalid LayerDiff configuration in " + p.string());
    return obj;
}
struct Sampler {
    std::vector<int> time;
    std::vector<float> ratio, previous;
    Sampler(const Json &cfg, int steps) {
        if (cfg.value("algorithm_type", std::string{}) != "sde-dpmsolver++" ||
            cfg.value("solver_type", std::string{}) != "midpoint" || cfg.value("solver_order", 0) != 2 ||
            cfg.value("beta_schedule", std::string{}) != "scaled_linear" ||
            cfg.value("timestep_spacing", std::string{}) != "leading" ||
            cfg.value("final_sigmas_type", std::string{}) != "zero" ||
            cfg.value("prediction_type", std::string{}) != "epsilon" || cfg.value("use_karras_sigmas", false))
            throw std::invalid_argument("See-through v3 requires its DPM++ 2M SDE scheduler");
        int train = cfg.at("num_train_timesteps"), offset = cfg.at("steps_offset");
        double lo = cfg.at("beta_start"), hi = cfg.at("beta_end");
        if (train != 1000 || offset != 1 || !(lo > 0 && lo < hi && hi < 1) || steps < 1 || steps > 100)
            throw std::invalid_argument("Invalid See-through v3 noise schedule");
        std::vector<float> sigmas(train);
        double product = 1;
        for (int i = 0; i < train; ++i) {
            auto b = std::sqrt(lo) + (std::sqrt(hi) - std::sqrt(lo)) * i / (train - 1);
            product *= 1 - b * b;
            sigmas[i] = std::sqrt((1 - product) / product);
        }
        for (int i = steps; i > 0; --i) {
            time.push_back(i * (train / (steps + 1)) + offset);
            ratio.push_back(sigmas[time.back()]);
        }
        ratio.push_back(0);
    }
    void advance(int i, Tensor &x, const Tensor &eps, PhiloxRNG &rng) {
        if (eps.shape != x.shape)
            throw std::runtime_error("Invalid denoiser output dimensions");
        float s = ratio[i], t = ratio[i + 1], as = 1 / std::sqrt(1 + s * s), at = 1 / std::sqrt(1 + t * t);
        float decay = t / s, term = 1 - decay * decay,
              correction = i && t ? std::log(s / t) / std::log(ratio[i - 1] / s) : 0;
        auto noise = rng.randn(uint32_t(x.values.size()));
        if (previous.empty())
            previous.resize(x.values.size());
        for (size_t j = 0; j < x.values.size(); ++j) {
            float clean = (x.values[j] - s * as * eps.values[j]) / as;
            float derivative = (clean - previous[j]) * correction;
            float next = t ? (t * at / (s * as) * decay) * x.values[j] + at * term * (clean + derivative * .5f) +
                                 t * at * std::sqrt(term) * noise[j]
                           : clean;
            if (!std::isfinite(next))
                throw std::runtime_error("LayerDiff produced a non-finite latent");
            x.values[j] = next;
            previous[j] = clean;
        }
    }
};
Tensor inputPlanes(const Image &im, int first, int channels) {
    int area = im.w * im.h;
    Tensor x{{1, channels, im.h, im.w}, std::vector<float>(size_t(area) * channels)};
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < area; ++i)
            x.values[c * area + i] = im.rgba[4 * i + first + c] / 255.f;
    return x;
}
Image decoded(const Tensor &x, int size) {
    if (x.shape != std::vector<int64_t>{1, 4, size, size})
        throw std::runtime_error("Malformed LayerDiff RGBA output");
    Image image(size, size);
    int area = size * size;
    for (int i = 0; i < area; ++i)
        for (int c = 0; c < 4; ++c) {
            auto value = x.values[c * area + i];
            if (!std::isfinite(value))
                throw std::runtime_error("Invalid LayerDiff RGBA sample");
            image.rgba[4 * i + c] = uint8_t(std::clamp(std::round(value * 255.f), 0.f, 255.f));
        }
    return image;
}
} // namespace
void validateBundle(const fs::path &root) {
    if (!root.is_absolute() || !fs::is_directory(root))
        throw std::invalid_argument("Select the local See-through LayerDiff v3 model directory");
    const auto u = config(root / "unet/config.json");
    if (u.value("_class_name", std::string{}) != "UNetFrameConditionModel" ||
        u.value("tag_version", std::string{}) != "v3" || u.value("in_channels", 0) != 8)
        throw std::invalid_argument("Choose the See-through v0.0.2 LayerDiff v3 bundle with body and face groups");
    for (const auto *file : {"unet/diffusion_pytorch_model.safetensors", "vae/diffusion_pytorch_model.safetensors",
                             "trans_vae/diffusion_pytorch_model.safetensors", "text_encoder/model.safetensors",
                             "text_encoder_2/model.safetensors", "tokenizer/vocab.json", "tokenizer/merges.txt",
                             "tokenizer_2/vocab.json", "tokenizer_2/merges.txt"}) {
        auto part = root / file;
        if (!fs::is_regular_file(part) || fs::file_size(part) < 256)
            throw std::invalid_argument(std::string("Missing or incomplete LayerDiff component: ") + file);
    }
    for (const auto *component : {"text_encoder/config.json", "text_encoder_2/config.json",
                                  "tokenizer/tokenizer_config.json", "tokenizer_2/tokenizer_config.json"})
        (void)config(root / component);
    (void)Sampler(config(root / "scheduler/scheduler_config.json"), 30);
}
struct Model::Impl {
    Backend backend;
    Conditioning body, face;
    Weights unet;
    Vae vae;
    Json scheduler;
    Impl(const fs::path &root, int index, std::function<void()> check)
        : backend(index, std::move(check)), body(encodeParts(backend, root, bodyTags)),
          face(encodeParts(backend, root, faceTags)),
          unet(backend, root / "unet/diffusion_pytorch_model.safetensors"), vae(backend, root),
          scheduler(config(root / "scheduler/scheduler_config.json")) {}
    void pass(const Image &page, int steps, PhiloxRNG &initialRng, PhiloxRNG &rng, int group,
              std::map<std::string, Image> &images, const Progress &report) {
        const auto &tags = group ? faceTags : bodyTags;
        const auto &cond = group ? face : body;
        const auto name = group ? "Face parts" : "Body parts";
        int n = int(tags.size()), size = page.w, l = size / 8, frame = 4 * l * l;
        backend.check();
        report("vae_encode", name, 0, 1, group + 1);
        Tensor posterior{{1, 4, l, l}, rng.randn(frame)};
        auto source = vae.encode(inputPlanes(page, 0, 3), posterior);
        if (source.shape != posterior.shape)
            throw std::runtime_error("LayerDiff VAE source shape mismatch");
        auto initial = initialRng.randn(frame);
        Tensor x{{n, 4, l, l}, std::vector<float>(size_t(n) * frame)};
        Tensor input{{n, 8, l, l}, std::vector<float>(size_t(n) * 2 * frame)};
        Tensor timeIds{{n, 6}, {}};
        for (int part = 0; part < n; ++part) {
            std::copy(initial.begin(), initial.end(), x.values.begin() + part * frame);
            std::copy(source.values.begin(), source.values.end(), input.values.begin() + (part * 2 + 1) * frame);
            timeIds.values.insert(timeIds.values.end(),
                                  {float(size), float(size), 0.f, 0.f, float(size), float(size)});
        }
        Sampler sampler(scheduler, steps);
        bool tryResident = true;
        for (int i = 0; i < steps; ++i) {
            backend.check();
            report("sampling", name, i, steps, group + 1);
            for (int part = 0; part < n; ++part)
                std::copy_n(x.values.data() + part * frame, frame, input.values.data() + 2 * part * frame);
            auto eps = denoise(backend, unet, input, sampler.time[i], cond.context, cond.pooled, timeIds, group, tryResident);
            sampler.advance(i, x, eps, rng);
        }
        report("sampling", name, steps, steps, group + 1);
        backend.trim();
        auto mask = inputPlanes(page, 3, 1);
        for (int part = 0; part < n; ++part) {
            backend.check();
            report("vae_decode", name, part, n, group + 1);
            auto first = x.values.begin() + part * frame;
            Tensor latent{{1, 4, l, l}, std::vector<float>(first, first + frame)};
            images.emplace(tags[part], decoded(vae.decode(latent, mask), size));
        }
        report("vae_decode", name, n, n, group + 1);
        backend.trim();
    }
};
Model::Model(const fs::path &root, int device, std::function<void()> check) {
    validateBundle(root);
    impl_ = std::make_unique<Impl>(root, device, std::move(check));
}
Model::~Model() = default;
void Model::generate(const Image &source, int size, int steps, uint64_t seed, bool splitParts,
                     const Progress &report, const std::function<void(Part &&)> &writePart) {
    if (size < 256 || size > 1280 || size % 64 || steps < 1 || steps > 100)
        throw std::invalid_argument("Unsupported LayerDiff resolution or step count");
    if (support(source, 0).empty())
        throw std::invalid_argument("The source layer is fully transparent");
    // Cancellation or allocation failure can interrupt either pass. Keep the
    // reusable model weights, but release the interrupted graph's scratch pool.
    struct ReleaseScratch {
        Backend &backend;
        ~ReleaseScratch() { backend.trim(); }
    } releaseScratch{impl_->backend};
    PhiloxRNG initialRng(seed), rng(seed);
    std::map<std::string, Image> body, face;
    Rect full{0, 0, source.w, source.h};
    impl_->pass(modelPage(source, full, size), steps, initialRng, rng, 0, body, report);
    auto head = headRegion(body.at("head"), source.w, source.h);
    if (!head.empty())
        impl_->pass(modelPage(source, head, size), steps, initialRng, rng, 1, face, report);
    report("saving", "Character parts", 0, 0, 0);
    auto upper = assemble("topwear", std::move(body.at("topwear")), full, source, false);
    auto lower = assemble("bottomwear", std::move(body.at("bottomwear")), full, source, false);
    const bool raiseLower = !upper.empty() && !lower.empty() && lowerGarmentInFront(source, upper[0], lower[0]);
    for (const auto &tag : order) {
        impl_->backend.check();
        if (tag == "bottomwear") {
            if (!lower.empty() && !raiseLower)
                writePart(std::move(lower[0]));
            continue;
        }
        if (tag == "topwear") {
            if (!upper.empty())
                writePart(std::move(upper[0]));
            if (raiseLower)
                writePart(std::move(lower[0]));
            continue;
        }
        auto it = body.find(tag);
        bool headPart = it == body.end();
        if (headPart) {
            it = face.find(tag);
            if (it == face.end())
                continue;
        }
        for (auto &part : assemble(tag, std::move(it->second), headPart ? head : full, source, splitParts))
            writePart(std::move(part));
    }
}
} // namespace layerdiff
