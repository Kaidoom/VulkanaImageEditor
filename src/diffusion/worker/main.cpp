// The engine is process-isolated: backend assertions and allocation failures must
// never bring down the editor. stdout is reserved exclusively for this protocol.
#include "Profiles.h"
#include "SpatialGuidance.h"
#include "FluxSpatial.h"
#include "LoraReport.h"
#include <imageeditor/diffusion/Resolution.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "ReferenceSizing.h"
#include <json.hpp>
#include <stable-diffusion.h>
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <stb_image.h>
#include <stb_image_write.h>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

using Json = nlohmann::json;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
static double millis(Clock::time_point from)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
}
struct Failure : std::runtime_error {
    std::string code;
    Failure(std::string c, std::string m)
        : std::runtime_error(std::move(m))
        , code(std::move(c))
    {
    }
};
static void require(bool ok, const std::string& text)
{
    if (!ok)
        throw Failure("invalid_arguments", text);
}
static void keys(const Json& j, std::initializer_list<const char*> allowed)
{
    require(j.is_object(), "Expected a JSON object");
    for (const auto& [key, unused] : j.items()) {
        (void)unused;
        require(std::find(allowed.begin(), allowed.end(), key) != allowed.end(), "Unknown argument: " + key);
    }
}
static std::string str(const Json& j, const char* key, std::string fallback = { })
{
    if (!j.contains(key))
        return fallback;
    require(j[key].is_string(), std::string(key) + " must be a string");
    auto value = j[key].get<std::string>();
    require(value.find('\0') == std::string::npos && value.size() <= 65536,
        std::string(key) + " is too long or contains NUL");
    return value;
}
static double number(const Json& j, const char* key, double fallback, double low, double high)
{
    if (!j.contains(key))
        return fallback;
    require(j[key].is_number(), std::string(key) + " must be a number");
    const auto value = j[key].get<double>();
    require(std::isfinite(value) && value >= low && value <= high,
        std::string(key) + " must be between " + std::to_string(low) + " and " + std::to_string(high));
    return value;
}
static int integer(const Json& j, const char* key, int fallback, int low, int high)
{
    const auto n = number(j, key, fallback, low, high);
    require(std::floor(n) == n, std::string(key) + " must be an integer");
    return static_cast<int>(n);
}
static bool flag(const Json& j, const char* key, bool fallback)
{
    if (!j.contains(key))
        return fallback;
    require(j[key].is_boolean(), std::string(key) + " must be boolean");
    return j[key].get<bool>();
}
static std::string choice(
    const Json& j, const char* key, const char* fallback, std::initializer_list<const char*> values)
{
    const auto value = str(j, key, fallback);
    std::string options;
    for (auto v : values) {
        if (value == v)
            return value;
        if (!options.empty())
            options += ", ";
        options += v;
    }
    throw Failure("invalid_arguments", std::string(key) + " must be one of: " + options);
}
static std::string inputFile(const Json& j, const char* key, bool required)
{
    const auto value = str(j, key);
    require(!required || !value.empty(), std::string(key) + " is required");
    if (value.empty())
        return value;
    std::error_code ec;
    auto path = fs::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
    require(path.is_absolute() && fs::is_regular_file(path, ec),
        std::string(key) + " must be an existing absolute file path");
    std::ifstream f(path, std::ios::binary);
    require(f.good(), std::string(key) + " cannot be read");
    char prefix[44] { };
    f.read(prefix, 43);
    require(std::string(prefix).find("version https://git-lfs.github.com/spec/") != 0,
        std::string(key) + " is a Git LFS pointer, not a model/image");
    return value;
}
static Json profiles()
{
    return Json::parse(profileJson);
}
static Json findProfile(const std::string& id)
{
    for (const auto& p : profiles())
        if (p["id"] == id)
            return p;
    throw Failure("invalid_arguments", "Unknown profile: " + id + "; see hello.profiles");
}
struct Image {
    sd_image_t view { };
    std::unique_ptr<unsigned char, decltype(&stbi_image_free)> pixels { nullptr, stbi_image_free };
    explicit Image(const std::string& file, int channels = 4)
    {
        int w = 0, h = 0, c = 0;
        require(stbi_info(file.c_str(), &w, &h, &c) && w > 0 && h > 0 && uint64_t(w) * h <= 67108864,
            "Input must be a supported image of at most 64 megapixels: " + file);
        pixels.reset(stbi_load(file.c_str(), &w, &h, &c, channels));
        require(bool(pixels), "Cannot decode image: " + file);
        view = { static_cast<uint32_t>(w), static_cast<uint32_t>(h), static_cast<uint32_t>(channels),
            pixels.get() };
    }
};
static void png(const fs::path& path, const sd_image_t& image)
{
    if (!image.data || image.width == 0 || image.height == 0 || image.channel < 1 || image.channel > 4)
        throw Failure("invalid_output", "Engine returned an invalid image");
    const auto temporary = path.string() + ".tmp";
    if (!stbi_write_png(temporary.c_str(), int(image.width), int(image.height), int(image.channel),
            image.data, int(image.width * image.channel)))
        throw Failure("output_failed", "Cannot write generated image");
    std::error_code ec;
#ifdef _WIN32
    fs::remove(path, ec);
    ec.clear();
#endif
    fs::rename(temporary, path, ec);
    if (ec)
        throw Failure("output_failed", "Cannot publish generated image: " + ec.message());
}

class Worker {
    FILE* protocol;
    fs::path root;
    std::mutex outputMutex, stateMutex, logMutex, watchdogMutex;
    std::condition_variable finished;
    std::thread job;
    std::atomic<bool> busy { false }, cancelled { false };
    sd_ctx_t* context = nullptr; // protected by stateMutex when accessed by the control thread
    Json loadedProfile, effectiveSettings, files;
    Json loadedModels = Json::array(); // Immutable snapshots, protected by stateMutex.
    std::string jobId, state = "idle", lastError;
    uint64_t generation = 0;
    bool oom = false, memoryLimited = false, exited = false;
    std::string memoryError;
    Json engineTimings = Json::object();
    vulkana::diffusion::LoraReport loraReport;
    Clock::time_point jobStart, firstStep, lastStep;
    bool stepped = false;
    fs::path jobDirectory;
    Json devices = Json::array();

public:
    Worker(FILE* stream, fs::path work)
        : protocol(stream)
        , root(std::move(work))
    {
        sd_set_log_callback(
            [](sd_log_level_t level, const char* text, void* user) {
                auto& w = *static_cast<Worker*>(user);
                // Callbacks can arrive from engine pool threads. Do not take stateMutex here.
                const std::string message = text ? text : "";
                std::fputs(message.c_str(), stderr);
                {
                    std::lock_guard lock(w.logMutex);
                    w.loraReport.observe(message);
                }
                // Timings are extracted from this pinned engine's stage reports;
                // overall worker timings remain independent of log formatting.
                {
                    static const std::regex stage(
                        R"((get_learned_condition|encode_first_stage|decode_first_stage|sampling) completed, taking ([0-9.]+)s)");
                    std::smatch match;
                    if (std::regex_search(message, match, stage)) {
                        std::lock_guard lock(w.logMutex);
                        const auto key = match[1].str();
                        w.engineTimings[key]
                            = w.engineTimings.value(key, 0.0) + std::stod(match[2].str()) * 1000.0;
                    }
                }
                if (level >= SD_LOG_WARN) {
                    std::lock_guard lock(w.logMutex);
                    w.lastError = message.substr(0, 4096);
                    std::string lower = message;
                    std::transform(lower.begin(), lower.end(), lower.begin(),
                        [](unsigned char c) { return char(std::tolower(c)); });
                    if (lower.find("cannot make enough memory available") != std::string::npos) {
                        w.memoryLimited = true;
                        w.memoryError = message.substr(0, 2048);
                    }
                    if (lower.find("out of memory") != std::string::npos
                        || lower.find("failed to allocate") != std::string::npos
                        || lower.find("cuda_error_out_of_memory") != std::string::npos) {
                        w.oom = true;
                        w.memoryError = message.substr(0, 2048);
                    }
                }
            },
            this);
        sd_set_progress_callback(
            [](int step, int steps, float seconds, void* user) {
                auto& w = *static_cast<Worker*>(user);
                if (!w.stepped) {
                    w.firstStep = Clock::now();
                    w.stepped = true;
                }
                w.lastStep = Clock::now();
                // Reassert cancellation after the engine's per-generation reset.
                if (w.cancelled.load()) {
                    std::lock_guard lock(w.stateMutex);
                    if (w.context)
                        sd_cancel_generation(w.context, SD_CANCEL_ALL);
                }
                w.send({ { "event", "progress" }, { "scope", "engineOperation" }, { "jobId", w.jobId },
                    { "step", step }, { "steps", steps }, { "stepSeconds", seconds },
                    { "elapsedMs", millis(w.jobStart) } });
            },
            this);
        const auto n = sd_list_devices(nullptr, 0);
        std::vector<char> names(n + 1);
        sd_list_devices(names.data(), names.size());
        std::istringstream input(names.data());
        std::string line;
        while (std::getline(input, line)) {
            auto split = line.find('\t');
            if (split != std::string::npos)
                devices.push_back({ { "id", line.substr(0, split) }, { "name", line.substr(split + 1) } });
        }
    }
    ~Worker()
    {
        if (job.joinable())
            job.join();
        if (context)
            free_sd_ctx(context);
        sd_set_log_callback(nullptr, nullptr);
        sd_set_progress_callback(nullptr, nullptr);
        sd_set_preview_callback(nullptr, PREVIEW_NONE, 0, false, false, nullptr);
    }
    void send(const Json& message)
    {
        const auto line = message.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n";
        std::lock_guard lock(outputMutex);
        if (std::fwrite(line.data(), 1, line.size(), protocol) != line.size() || std::fflush(protocol) != 0)
            std::_Exit(2);
    }
    void fail(const std::string& id, const std::string& code, const std::string& message)
    {
        send({ { "id", id }, { "ok", false }, { "error", { { "code", code }, { "message", message } } } });
    }
    void ok(const std::string& id, const Json& result)
    {
        send({ { "id", id }, { "ok", true }, { "result", result } });
    }
    Json status()
    {
        std::lock_guard lock(stateMutex);
        return { { "state", state }, { "loaded", context != nullptr }, { "jobId", busy.load() ? jobId : "" },
            { "contextGeneration", generation }, { "loadedModels", loadedModels } };
    }
    void announce()
    {
        Json samplers = Json::array(), schedulers = Json::array();
        for (int i = 0; i < SAMPLE_METHOD_COUNT; ++i)
            samplers.push_back(sample_method_to_str[i]);
        for (int i = 0; i < SCHEDULER_COUNT; ++i)
            schedulers.push_back(scheduler_to_str[i]);
        send({ { "samplers", samplers }, { "schedulers", schedulers }, { "event", "hello" },
            { "referenceSizeLimit", true },
            { "loraParameters", true },
            { "flux3NativeRegions", true },
            { "protocol", 1 }, { "spatialVersions", Json::array({1, 2, 3}) }, { "engineCommit", VULKANA_DIFFUSION_ENGINE_COMMIT },
            { "backend", VULKANA_DIFFUSION_BACKEND }, { "devices", devices }, { "profiles", profiles() } });
    }
    void stateEvent()
    {
        auto s = status();
        s["event"] = "state";
        send(s);
    }
    bool cancel()
    {
        std::lock_guard lock(stateMutex);
        if (!busy.load())
            return false;
        cancelled = true;
        state = "cancelling";
        if (context)
            sd_cancel_generation(context, SD_CANCEL_ALL);
        return true;
    }
    void shutdown()
    {
        // A constructor or backend kernel may not cooperate. The supervisor also
        // enforces a grace period, including on platforms without this watchdog.
        std::thread watchdog([this] {
            std::unique_lock lock(watchdogMutex);
            if (!finished.wait_for(lock, std::chrono::seconds(5), [this] { return exited; }))
                std::_Exit(2);
        });
        cancel();
        if (job.joinable())
            job.join();
        release();
        {
            std::lock_guard lock(watchdogMutex);
            exited = true;
        }
        finished.notify_all();
        watchdog.join();
    }
    void release()
    {
        sd_ctx_t* old = nullptr;
        {
            std::lock_guard lock(stateMutex);
            old = context;
            context = nullptr;
        }
        if (old) {
            free_sd_ctx(old);
            {
                std::lock_guard lock(stateMutex);
                loadedModels = Json::array();
            }
            stateEvent();
        }
    }
    void checkCancel()
    {
        if (cancelled.load())
            throw Failure("cancelled", "Operation cancelled; no result was published");
    }
    Json load(const Json& params);
    Json generate(const Json& params);
    bool handle(const Json& request)
    {
        std::string id;
        try {
            keys(request, { "protocol", "id", "method", "params" });
            id = str(request, "id");
            require(!id.empty() && id.size() <= 64
                    && std::all_of(id.begin(), id.end(),
                        [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; }),
                "id must contain 1–64 letters, numbers, '-' or '_'");
            require(request.contains("protocol") && request["protocol"] == 1, "protocol must be 1");
            if (busy.load()) {
                std::lock_guard lock(stateMutex);
                if (id == jobId) {
                    send({ { "event", "fatal" },
                        { "error",
                            { { "code", "duplicate_id" },
                                { "message",
                                    "A request id is already pending; closing the ambiguous protocol "
                                    "session" } } } });
                    return false;
                }
            }
            const auto method = str(request, "method");
            const auto params = request.value("params", Json::object());
            require(params.is_object(), "params must be an object");
            if (method == "ping" || method == "status") {
                keys(params, { });
                ok(id, status());
                return true;
            }
            if (method == "cancel") {
                keys(params, { });
                const bool active = cancel();
                ok(id, { { "requested", active } });
                stateEvent();
                return true;
            }
            if (method == "shutdown") {
                keys(params, { });
                cancel();
                ok(id, { { "stopping", true } });
                return false;
            }
            require(method == "load" || method == "generate" || method == "unload",
                "Unknown method; use ping, status, load, generate, cancel, unload or shutdown");
            if (busy.load())
                throw Failure("busy", "Another job is running; wait for its terminal response or cancel it");
            if (job.joinable())
                job.join();
            {
                std::lock_guard lock(stateMutex);
                jobId = id;
                state = method == "load" ? "loading" : method == "generate" ? "generating" : "unloading";
                cancelled = false;
                busy = true;
            }
            {
                std::lock_guard lock(logMutex);
                lastError.clear();
                memoryError.clear();
                oom = false;
                memoryLimited = false;
                engineTimings = Json::object();
            }
            jobStart = Clock::now();
            stepped = false;
            jobDirectory.clear();
            stateEvent();
            job = std::thread([this, id, method, params] {
                Json result;
                std::string code, message;
                bool fatal = false;
                try {
                    if (method == "load")
                        result = load(params);
                    else if (method == "generate")
                        result = generate(params);
                    else {
                        keys(params, { });
                        release();
                        result = { { "unloaded", true } };
                    }
                    checkCancel();
                } catch (const Failure& e) {
                    code = e.code;
                    message = e.what();
                } catch (const std::bad_alloc&) {
                    code = "out_of_memory";
                    message = "Not enough host memory; worker restart required. Retry with a smaller image "
                              "or different offloading settings";
                    fatal = true;
                } catch (const std::exception& e) {
                    code = "engine_error";
                    message = std::string(e.what()) + "; worker restart required";
                    fatal = true;
                }
                // Linearize cancellation and completion. A late Cancel cannot
                // retroactively invalidate a completed result or the next job.
                {
                    std::lock_guard lock(stateMutex);
                    if (cancelled.load()) {
                        code = "cancelled";
                        message = "Operation cancelled; no result was published";
                    }
                    if (!code.empty() && !jobDirectory.empty()) {
                        std::error_code ec;
                        fs::remove_all(jobDirectory, ec);
                    }
                    state = "idle";
                    busy = false;
                }
                if (code.empty())
                    ok(id, result);
                else
                    fail(id, code, message);
                if (fatal)
                    std::_Exit(3); // Unreturned engine constructors may own leaked allocations.
                stateEvent();
            });
        } catch (const Failure& e) {
            fail(id, e.code, e.what());
        } catch (const std::exception& e) {
            fail(id, "invalid_arguments", e.what());
        }
        return true;
    }
};

Json Worker::load(const Json& params)
{
    keys(params, { "profile", "files", "settings" });
    const auto profile = findProfile(str(params, "profile"));
    const auto f = params.value("files", Json::object());
    keys(f, { "diffusion", "textEncoder", "vae", "vision" });
    const bool checkpoint = profile.value("checkpoint", false);
    require(!checkpoint || (str(f, "textEncoder").empty() && str(f, "vision").empty()),
        "Stable Diffusion uses the checkpoint's embedded text encoders; only a VAE override is supported.");
    const auto diffusion = inputFile(f, "diffusion", true), te = inputFile(f, "textEncoder", !checkpoint),
               vae = inputFile(f, "vae", !checkpoint), vision = inputFile(f, "vision", false);
    const auto s = params.value("settings", Json::object());
    keys(s,
        { "device", "maxVramGiB", "threads", "weights", "placements", "textEncoderCompute", "vaeCompute",
            "flashAttention", "directConvolution", "sageAttention", "mmap", "prefetch", "segmentedCompute",
            "eagerLoad", "conditioningCacheSize", "prefixCache", "prefixCacheType" });
    std::string device = str(s, "device");
    if (device.empty())
        for (const auto& d : devices)
            if (d["id"].get<std::string>().starts_with("CUDA")) {
                device = d["id"];
                break;
            }
    if (device.empty() && !devices.empty())
        device = devices.front()["id"];
    require(std::any_of(devices.begin(), devices.end(), [&](const Json& d) { return d["id"] == device; }),
        "device must match an id in hello.devices");
    const auto weights = choice(s, "weights", "auto", { "auto", "gpu", "ram", "disk", "manual" });
    const auto teCompute = choice(s, "textEncoderCompute", "gpu", { "gpu", "cpu" }),
               vaeCompute = choice(s, "vaeCompute", "gpu", { "gpu", "cpu" });
    const auto flash = choice(s, "flashAttention", "auto", { "auto", "on", "off" }),
               conv = choice(s, "directConvolution", "auto", { "auto", "on", "off" });
    const auto prefix = choice(s, "prefixCacheType", "auto",
        { "auto", "f32", "f16", "bf16", "q8_0", "q4_0", "q4_1", "q5_0", "q5_1", "q4_K", "q6_K" });
    const double budget = number(s, "maxVramGiB", 10, 0, 1024);
    const int threads = integer(s, "threads", 0, 0, 1024);
    const auto cacheSize = integer(s, "conditioningCacheSize", 1, 0, 64);
    std::string backend = device + ",te=" + (teCompute == "cpu" ? "cpu" : device)
        + ",vae=" + (vaeCompute == "cpu" ? "cpu" : device);
    auto placement = [&](const std::string& v) {
        return v == "gpu" ? device : v == "ram" ? std::string("cpu") : v;
    };
    std::string storage;
    if (weights == "manual") {
        require(s.contains("placements"), "manual weights require diffusion, textEncoder and vae placements");
        const auto p = s["placements"];
        keys(p, { "diffusion", "textEncoder", "vae" });
        for (auto name : { "diffusion", "textEncoder", "vae" })
            require(p.contains(name), std::string("placements.") + name + " is required");
        storage = "diffusion=" + placement(choice(p, "diffusion", "", { "gpu", "ram", "disk" }))
            + ",te=" + placement(choice(p, "textEncoder", "", { "gpu", "ram", "disk" }))
            + ",vae=" + placement(choice(p, "vae", "", { "gpu", "ram", "disk" }));
    } else {
        require(!s.contains("placements"), "placements require weights=manual");
        if (weights != "auto")
            storage = placement(weights);
    }
    const bool prefixCache = flag(s, "prefixCache", true);
    require(profile["family"] == "qwen-image-2.1"
            || (!s.contains("prefixCache") && !s.contains("prefixCacheType")),
        "prefix cache settings apply only to Qwen Image 2.1");
    std::string modelArgs = profile["family"] == "qwen-image-2.1"
        ? std::string("qwen_image_2_1_prefix_cache=") + (prefixCache ? "true" : "false")
            + ",qwen_image_2_1_prefix_cache_type=" + prefix
        : "";
    std::string vram = std::to_string(budget);
    sd_ctx_params_t options;
    sd_ctx_params_init(&options);
    if (checkpoint)
        options.model_path = diffusion.c_str();
    else {
        options.diffusion_model_path = diffusion.c_str();
        options.llm_path = te.c_str();
        options.llm_vision_path = vision.c_str();
    }
    if (!vae.empty()) options.vae_path = vae.c_str();
    options.backend = backend.c_str();
    options.params_backend = storage.empty() ? nullptr : storage.c_str();
    options.auto_fit = weights == "auto";
    options.max_vram = vram.c_str();
    options.n_threads = threads == 0 ? sd_get_num_physical_cores() : threads;
    options.flash_attn_mode = flash.c_str();
    options.conv_direct_mode = conv.c_str();
    options.sage_attn = flag(s, "sageAttention", false);
    options.enable_mmap = flag(s, "mmap", true);
    options.disable_prefetch = !flag(s, "prefetch", true);
    options.disable_segmented_compute = !flag(s, "segmentedCompute", true);
    options.eager_load = flag(s, "eagerLoad", false);
    options.conditioning_cache_size = cacheSize;
    options.model_args = modelArgs.c_str();
    options.lora_apply_mode = LORA_APPLY_AT_RUNTIME;
    Json effective = { { "device", device }, { "maxVramGiB", budget }, { "threads", options.n_threads },
        { "weights", weights }, { "computeBackend", backend }, { "weightBackend", storage },
        { "autoFit", options.auto_fit }, { "flashAttention", flash }, { "directConvolution", conv },
        { "sageAttention", options.sage_attn }, { "mmap", options.enable_mmap },
        { "prefetch", !options.disable_prefetch }, { "segmentedCompute", !options.disable_segmented_compute },
        { "eagerLoad", options.eager_load }, { "conditioningCacheSize", cacheSize },
        { "modelArguments", modelArgs } };
    // Validate everything before relinquishing a working context. A new context
    // cannot coexist with the old model on many consumer GPUs.
    checkCancel();
    release();
    checkCancel();
    const auto start = Clock::now();
    sd_ctx_t* fresh = new_sd_ctx(&options);
    if (!fresh) {
        std::lock_guard lock(logMutex);
        throw Failure(oom       ? "out_of_memory"
                : memoryLimited ? "memory_limit"
                                : "model_load_failed",
            "Could not load the model. " + (memoryError.empty() ? lastError : memoryError));
    }
    const std::string actual = sd_get_model_version_name(fresh);
    const auto accepted = profile.contains("engineModels") ? profile["engineModels"]
        : Json::array({ profile["engineModel"] });
    if (std::find(accepted.begin(), accepted.end(), actual) == accepted.end()
        || !sd_ctx_supports_image_generation(fresh)) {
        free_sd_ctx(fresh);
        throw Failure("profile_mismatch",
            "Loaded " + actual + " but profile requires " + profile["name"].get<std::string>()
                + "; select the matching profile and reload");
    }
    effective["architecture"] = actual;
    {
        std::lock_guard lock(stateMutex);
        context = fresh;
        ++generation;
        loadedModels = Json::array({ { { "contextGeneration", generation },
            { "profile", profile }, { "files", f }, { "effectiveSettings", effective },
            { "backend", VULKANA_DIFFUSION_BACKEND } } });
    }
    loadedProfile = profile;
    effectiveSettings = effective;
    files = f;
    if (cancelled.load()) {
        release();
        checkCancel();
    }
    return { { "profile", profile }, { "contextGeneration", generation }, { "effectiveSettings", effective },
        { "timings", { { "modelLoadMs", millis(start) }, { "totalMs", millis(jobStart) } } } };
}

Json Worker::generate(const Json& p)
{
    keys(p,
        { "prompt", "negativePrompt", "width", "height", "steps", "guidance", "seed", "batch", "sampler",
            "scheduler", "schedule", "sigmas", "strength", "referenceImages", "initialImage", "mask",
            "vaeTiling", "vaeTileSize", "vaeTileOverlap", "preview", "previewInterval", "flowShift",
            "spatial", "eta", "cacheMode", "cacheReuseThreshold", "cacheStart", "cacheEnd", "referenceMaxSize", "loras" });
    if (!context)
        throw Failure("model_not_loaded", "Load a model before generating");
    struct LoraInput { std::string path, name; float strength; };
    std::vector<LoraInput> loraInputs;
    if (p.contains("loras")) {
        require(p["loras"].is_array(), "loras must be an array of path, name and strength objects");
        std::set<fs::path> paths;
        for (const auto& item : p["loras"]) {
            keys(item, {"path", "name", "strength"});
            const auto strength = number(item, "strength", 1, -4, 4);
            if (strength == 0) continue;
            const auto name = str(item, "name", str(item, "path"));
            try {
                const auto path = inputFile(item, "path", true);
                require(paths.insert(fs::canonical(fs::u8path(path))).second,
                    "The same file is enabled more than once. Use a single entry with the combined strength.");
                loraInputs.push_back({path, name, static_cast<float>(strength)});
            } catch (const std::exception& error) {
                throw Failure("invalid_arguments", "LoRA '" + name + "': " + error.what());
            }
        }
    }
    std::vector<sd_lora_t> loras;
    for (const auto& input : loraInputs)
        loras.push_back({false, input.strength, input.path.c_str()});
    const auto modes = loadedProfile.value("modes", Json::array({"generate", "edit", "fill"}));
    const auto supportsMode = [&](const char* mode) {
        return std::find(modes.begin(), modes.end(), mode) != modes.end();
    };
    const std::string modelName = loadedProfile["name"];
    require(loadedProfile.value("referenceImages", true) || !p.contains("referenceImages"),
        modelName + " generates from text only; reference images are unavailable");
    require(supportsMode("edit") || supportsMode("fill") || (!p.contains("initialImage") && !p.contains("strength")),
        modelName + " does not support image-to-image denoising");
    require(supportsMode("fill") || !p.contains("mask"), modelName + " does not support Generative Fill");
    require(loadedProfile.value("spatialGuidance", "none") != "none" || !p.contains("spatial"),
        modelName + " does not support region guidance");
    auto prompt = str(p, "prompt");
    const bool nativeFlux = loadedProfile["family"] == "flux3";
    const auto negative = str(p, "negativePrompt");
    Json regions = Json::array();
    Json inputRegions = Json::array();
    std::string annotation;
    if (p.contains("spatial")) {
        try {
            regions = vulkana::diffusion::spatialRegions(p["spatial"]);
            const auto count = p.contains("referenceImages") && p["referenceImages"].is_array()
                ? p["referenceImages"].size() : 0;
            inputRegions = vulkana::diffusion::inputRegionGroups(p["spatial"], count);
        } catch (const std::invalid_argument& e) {
            throw Failure("invalid_arguments", e.what());
        }
        if (nativeFlux) {
            require(!p["spatial"].contains("annotationImage"),
                "FLUX.3 uses structured captions, not annotationImage");
        } else if (!regions.empty())
            annotation = inputFile(p["spatial"], "annotationImage", true);
    }
    const bool cropGuidance = p.contains("spatial") && p["spatial"].value("cropGuidance", false);
    const bool extendedSpatial = p.contains("spatial") && p["spatial"].value("version", 1) == 3;
    require(nativeFlux || !extendedSpatial, "Spatial schema 3 descriptions and transfers currently apply only to FLUX.3");
    require(nativeFlux || !cropGuidance, "spatial.cropGuidance applies only to FLUX.3");
    require(!prompt.empty(), "prompt is required");
    const int width = integer(p, "width", 1024, 32, 8192), height = integer(p, "height", 1024, 32, 8192);
    const int alignment = loadedProfile["alignment"];
    const auto sizeError = imageeditor::diffusion::resolutionError(width, height, alignment, modelName);
    require(sizeError.empty(), sizeError);
    const int steps = integer(p, "steps", loadedProfile["steps"], 1, 1000),
              batch = integer(p, "batch", 1, 1, 8);
    require(
        uint64_t(width) * height * batch <= 67108864, "Combined batch output must not exceed 64 megapixels");
    const auto guidance = number(p, "guidance", loadedProfile["guidance"], 0, 100);
    const int referenceMaxSize = integer(p, "referenceMaxSize", 0, 0, 8192);
    require(referenceMaxSize == 0 || referenceMaxSize >= 64, "referenceMaxSize must be 0 or 64–8192 pixels");
    require(negative.empty() || guidance > 1,
        "negativePrompt requires guidance greater than 1; it is not used with CFG 1");
    const double seed = number(p, "seed", -1, -1, 9007199254740991.0);
    require(std::floor(seed) == seed, "seed must be an integer");
    const auto sampler = str(p, "sampler", loadedProfile["sampler"]),
               scheduler = str(p, "scheduler", loadedProfile.value("scheduler", "auto"));
    sample_method_t method = SAMPLE_METHOD_COUNT;
    for (int i = 0; i < SAMPLE_METHOD_COUNT; ++i)
        if (sampler == sample_method_to_str[i])
            method = static_cast<sample_method_t>(i);
    require(method != SAMPLE_METHOD_COUNT, "Unknown sampler; use a value from hello.samplers");
    scheduler_t sched = SCHEDULER_COUNT;
    if (scheduler != "auto")
        for (int i = 0; i < SCHEDULER_COUNT; ++i)
            if (scheduler == scheduler_to_str[i])
                sched = static_cast<scheduler_t>(i);
    require(scheduler == "auto" || sched != SCHEDULER_COUNT,
        "Unknown scheduler; use auto or a value from hello.schedulers");
    const auto schedule = choice(
        p, "schedule", loadedProfile["schedule"].get<std::string>().c_str(), { "auto", "turbo", "custom" });
    std::vector<float> sigmas;
    if (schedule == "turbo") {
        require(loadedProfile["id"] == "qwen-image-2.1-turbo",
            "turbo schedule requires the Qwen Image 2.1 Turbo profile");
        require(
            steps == 8, "turbo schedule requires 8 steps; select schedule=auto or custom to change steps");
        sigmas = { 1, 0.978453f, 0.95418f, 0.926626f, 0.89508f, 0.845148f, 0.704534f, 0.414568f, 0 };
    }
    if (schedule == "custom") {
        require(p.contains("sigmas") && p["sigmas"].is_array() && p["sigmas"].size() == size_t(steps + 1),
            "Custom sigmas must contain steps+1 numbers");
        double last = 1001;
        for (const auto& s : p["sigmas"]) {
            require(s.is_number(), "sigmas must contain numbers");
            const auto v = s.get<double>();
            require(std::isfinite(v) && v >= 0 && v < last && v <= 1000,
                "sigmas must be finite, non-negative and strictly descending");
            const auto converted = static_cast<float>(v);
            require((sigmas.empty() || converted < sigmas.back()) && (v == 0 || converted > 0),
                "sigmas must remain distinct, descending and positive before the final zero at float "
                "precision");
            sigmas.push_back(converted);
            last = v;
        }
        require(last == 0 && sigmas.back() == 0, "sigmas must end with exactly zero");
    } else
        require(!p.contains("sigmas"), "sigmas require schedule=custom");
    std::vector<Image> images;
    std::vector<sd_image_t> refs;
    std::vector<std::vector<unsigned char>> regionPixels;
    Json regionReferences = Json::array();
    std::size_t cropCount = 0;
    for (const auto& group : inputRegions)
        if (!nativeFlux || cropGuidance) cropCount += group["regions"].size();
    const auto originalRefCount = p.contains("referenceImages") && p["referenceImages"].is_array()
        ? p["referenceImages"].size() : 0;
    require(originalRefCount + cropCount + (annotation.empty() ? 0 : 1)
            <= std::size_t(loadedProfile.value("maxReferenceImages", 8)),
        "Input images and region guides exceed this model's reference limit; remove an image or region");
    if (p.contains("referenceImages")) {
        require(p["referenceImages"].is_array()
                && p["referenceImages"].size() <= loadedProfile.value("maxReferenceImages", 8),
            "referenceImages must be an array within the profile maxReferenceImages limit");
        if (!p["referenceImages"].empty() && loadedProfile["visionRequiredForReferences"] == true)
            require(!str(files, "vision").empty(),
                "Reload with files.vision to use reference images with this profile");
        for (const auto& path : p["referenceImages"]) {
            images.emplace_back(inputFile({ { "path", path } }, "path", true));
            refs.push_back(images.back().view);
        }
    }
    if (!nativeFlux || cropGuidance) for (const auto& group : inputRegions) {
        const auto sourceNumber = group["image"].get<int>();
        const auto source = refs.at(std::size_t(sourceNumber - 1));
        for (const auto& region : group["regions"]) {
            checkCancel();
            const auto crop = vulkana::diffusion::inputRegionCrop(region, int(source.width), int(source.height));
            auto& pixels = regionPixels.emplace_back(std::size_t(crop.width) * crop.height * source.channel);
            const auto rowBytes = std::size_t(crop.width) * source.channel;
            for (int y = 0; y < crop.height; ++y)
                std::copy_n(source.data + (std::size_t(crop.y + y) * source.width + crop.x) * source.channel,
                            rowBytes, pixels.data() + std::size_t(y) * rowBytes);
            refs.push_back({uint32_t(crop.width), uint32_t(crop.height), source.channel, pixels.data()});
            regionReferences.push_back({
                {"name", "Image" + std::to_string(sourceNumber) + "/" + region["label"].get<std::string>()},
                {"sourceImage", "Image" + std::to_string(sourceNumber)},
                {"detailImage", "Image" + std::to_string(refs.size())},
                {"sourceBox", {{"x", region["x"]}, {"y", region["y"]},
                               {"width", region["width"]}, {"height", region["height"]}}},
                {"cropPixels", {crop.x, crop.y, crop.width, crop.height}}});
        }
    }
    try {
        if (!nativeFlux) prompt = vulkana::diffusion::inputRegionPrompt(prompt, regionReferences, originalRefCount);
    } catch (const std::invalid_argument& e) {
        throw Failure("invalid_arguments", e.what());
    }
    if (!annotation.empty()) {
        require(refs.size() < loadedProfile.value("maxReferenceImages", 8),
            "Reserve one reference slot for the region annotation image");
        require(!str(files, "vision").empty(), "Qwen region annotations require vision weights");
        images.emplace_back(annotation);
        refs.push_back(images.back().view);
        prompt += " Use Image " + std::to_string(refs.size())
            + " only as a numbered region guide. Apply the following instructions in those rectangles, "
              "without rendering the guide outlines or labels: ";
        int index = 0;
        for (const auto& r : regions)
            prompt += "Region " + std::to_string(++index) + ": " + r["label"].get<std::string>() + ". ";
    }
    require(prompt.size() <= 65536, "Prompt plus region labels exceeds 65536 bytes");
    const auto initialPath = inputFile(p, "initialImage", false), maskPath = inputFile(p, "mask", false);
    require(maskPath.empty() || !initialPath.empty(),
        "mask requires initialImage; reference images alone do not define an inpainting canvas");
    std::unique_ptr<Image> initial, mask;
    if (!initialPath.empty()) {
        initial = std::make_unique<Image>(initialPath);
        require(initial->view.width == unsigned(width) && initial->view.height == unsigned(height),
            "initialImage must match width and height");
    }
    if (!maskPath.empty()) {
        mask = std::make_unique<Image>(maskPath, 1);
        require(mask->view.width == unsigned(width) && mask->view.height == unsigned(height),
            "mask must match width and height");
    }
    sd_img_gen_params_t params;
    sd_img_gen_params_init(&params);
    std::string referenceArgs;
    std::vector<vulkana::diffusion::SizedReference> resizedReferences;
    Json referenceSizes = Json::array();
    auto fluxInputRegions = inputRegions;
    if (referenceMaxSize > 0 && !refs.empty()) {
        referenceArgs = "resize_before_vae=false";
        if (loadedProfile["family"] == "qwen-image-2.1")
            referenceArgs += ",vlm_resize_mode=longest_side,vlm_max_size=" + std::to_string(referenceMaxSize);
        for (std::size_t i = 0; i < refs.size(); ++i) {
            auto& ref = refs[i];
            auto& resized = resizedReferences.emplace_back(vulkana::diffusion::resizeReference(
                ref.data, int(ref.width), int(ref.height), int(ref.channel), referenceMaxSize));
            const auto size = resized.size;
            referenceSizes.push_back({{"image", i + 1}, {"sourceWidth", ref.width}, {"sourceHeight", ref.height},
                                      {"width", size.width}, {"height", size.height}});
            if (nativeFlux && i < originalRefCount)
                for (auto& group : fluxInputRegions) if (group["image"] == i + 1)
                    for (auto& r : group["regions"]) {
                        r["x"] = (r["x"].get<double>() * resized.contentWidth + resized.left) / size.width;
                        r["y"] = (r["y"].get<double>() * resized.contentHeight + resized.top) / size.height;
                        r["width"] = r["width"].get<double>() * resized.contentWidth / size.width;
                        r["height"] = r["height"].get<double>() * resized.contentHeight / size.height;
                    }
            ref = {uint32_t(size.width), uint32_t(size.height), ref.channel, resized.pixels.data()};
        }
        params.ref_image_args = referenceArgs.c_str();
    }
    Json fluxRegionBindings = Json::array(), fluxRegionRows = Json::array();
    if (nativeFlux) {
        try {
            const int outputScene = extendedSpatial ? integer(p["spatial"], "outputScene", 0, 0, int(originalRefCount)) : 0;
            auto compiled = vulkana::diffusion::fluxSpatialCaption(prompt, regions, fluxInputRegions, originalRefCount, regionReferences, outputScene, extendedSpatial);
            prompt = std::move(compiled.prompt);
            fluxRegionBindings = compiled.bindings; fluxRegionRows = compiled.rows;
        } catch (const std::invalid_argument& e) { throw Failure("invalid_arguments", e.what()); }
        require(prompt.size() <= 65536, "Prompt plus region labels exceeds 65536 bytes");
    }
    params.prompt = prompt.c_str();
    params.loras = loras.empty() ? nullptr : loras.data();
    params.lora_count = static_cast<uint32_t>(loras.size());
    params.negative_prompt = negative.c_str();
    params.width = width;
    params.height = height;
    params.seed = static_cast<int64_t>(seed);
    params.batch_count = batch;
    params.sample_params.sample_method = method;
    params.sample_params.scheduler = sched;
    params.sample_params.sample_steps = steps;
    const auto flowShift = number(p, "flowShift", 0, 0, 100);
    require(loadedProfile.value("flowSchedule", true) || flowShift == 0,
        "Flow shift is unavailable for this model.");
    if (flowShift > 0)
        params.sample_params.flow_shift = static_cast<float>(flowShift);
    const auto eta = number(p, "eta", -1, -1, 10);
    if (eta >= 0)
        params.sample_params.eta = static_cast<float>(eta);
    const auto cacheMode = choice(p, "cacheMode", "disabled", { "disabled", "easycache", "ucache" });
    params.cache.mode = cacheMode == "easycache" ? SD_CACHE_EASYCACHE
        : cacheMode == "ucache"                  ? SD_CACHE_UCACHE
                                                 : SD_CACHE_DISABLED;
    params.cache.reuse_threshold = static_cast<float>(number(p, "cacheReuseThreshold", 0.2, 0, 1));
    params.cache.start_percent = static_cast<float>(number(p, "cacheStart", 0.15, 0, 1));
    params.cache.end_percent = static_cast<float>(number(p, "cacheEnd", 0.95, 0, 1));
    require(params.cache.start_percent < params.cache.end_percent, "cacheStart must be less than cacheEnd");
    params.sample_params.guidance.txt_cfg = static_cast<float>(guidance);
    params.sample_params.custom_sigmas = sigmas.empty() ? nullptr : sigmas.data();
    params.sample_params.custom_sigmas_count = int(sigmas.size());
    params.strength = static_cast<float>(number(p, "strength", 0.75, 0, 1));
    params.ref_images = refs.empty() ? nullptr : refs.data();
    params.ref_images_count = int(refs.size());
    if (initial)
        params.init_image = initial->view;
    if (mask)
        params.mask_image = mask->view;
    params.vae_tiling_params.enabled = flag(p, "vaeTiling", false);
    params.vae_tiling_params.tile_size_w = params.vae_tiling_params.tile_size_h
        = integer(p, "vaeTileSize", 256, 64, 2048);
    require(params.vae_tiling_params.tile_size_w % 32 == 0, "vaeTileSize must be a multiple of 32");
    params.vae_tiling_params.target_overlap = static_cast<float>(number(p, "vaeTileOverlap", 0.25, 0, 0.5));
    const bool preview = flag(p, "preview", true);
    const int interval = integer(p, "previewInterval", 1, 1, 1000);
    checkCancel();
    const auto directory = root / ("job-" + jobId);
    require(fs::create_directory(directory), "Job output directory already exists; use a new request id");
    jobDirectory = directory;
    sd_set_preview_callback(
        [](int step, int count, sd_image_t* frames, bool noisy, void* data) {
            auto& w = *static_cast<Worker*>(data);
            if (w.cancelled || noisy || count < 1)
                return;
            try {
                const auto path = w.jobDirectory / "preview.png";
                png(path, frames[0]);
                w.send({ { "event", "preview" }, { "jobId", w.jobId }, { "path", path.string() },
                    { "step", step }, { "width", frames[0].width }, { "height", frames[0].height } });
            } catch (const std::exception& e) {
                std::fprintf(stderr, "Preview unavailable: %s\n", e.what());
            }
        },
        preview ? PREVIEW_PROJ : PREVIEW_NONE, interval, true, false, this);
    checkCancel();
    const auto engineStart = Clock::now();
    {
        std::lock_guard lock(logMutex);
        loraReport.files.clear();
    }
    sd_image_generation_result_t* raw = nullptr;
    const bool success = generate_image(context, &params, &raw);
    std::unique_ptr<sd_image_generation_result_t, decltype(&free_sd_image_generation_result)> result(
        raw, free_sd_image_generation_result);
    const double engineMs = millis(engineStart);
    sd_set_preview_callback(nullptr, PREVIEW_NONE, 0, false, false, nullptr);
    checkCancel();
    if (!success || !result || result->count != batch) {
        release();
        std::lock_guard lock(logMutex);
        throw Failure(oom       ? "out_of_memory"
                : memoryLimited ? "memory_limit"
                                : "generation_failed",
            std::string("Generation failed; reload the model before retrying. ")
                + (memoryLimited ? "The selected memory budget cannot fit this operation. Increase the "
                                   "budget if memory is available, reduce image size, or move text "
                                   "encoder/VAE computation to CPU. "
                        : oom    ? "Reduce image size or use RAM/disk weight offloading. "
                                 : "")
                + (memoryError.empty() ? lastError : memoryError));
    }
    std::string incompatibleLora;
    {
        std::lock_guard lock(logMutex);
        for (const auto& input : loraInputs)
            if (!loraReport.compatible(input.path)) {
                incompatibleLora = input.name.empty() ? input.path : input.name;
                break;
            }
    }
    if (!incompatibleLora.empty()) {
        release();
        throw Failure("lora_incompatible", "LoRA '" + incompatibleLora
            + "' could not be applied compatibly to this model. Check the LoRA's model family and file, or disable it. The model will reload on the next generation.");
    }
    const auto outputStart = Clock::now();
    Json outputs = Json::array();
    for (int i = 0; i < result->count; ++i) {
        checkCancel();
        const auto path = directory / ("image-" + std::to_string(i) + ".png");
        png(path, result->items[i].image);
        Json metadata = Json::object();
        if (result->items[i].generation_info)
            metadata = Json::parse(result->items[i].generation_info, nullptr, false);
        outputs.push_back({ { "path", path.string() }, { "width", result->items[i].image.width },
            { "height", result->items[i].image.height }, { "channels", result->items[i].image.channel },
            { "generationInfo", metadata } });
    }
    Json timings
        = { { "engineMs", engineMs }, { "outputMs", millis(outputStart) }, { "totalMs", millis(jobStart) } };
    if (stepped) {
        timings["throughFirstProgressMs"]
            = std::chrono::duration<double, std::milli>(firstStep - engineStart).count();
        timings["betweenProgressCallbacksMs"]
            = std::chrono::duration<double, std::milli>(lastStep - firstStep).count();
        timings["afterLastProgressMs"] = millis(lastStep);
    }
    {
        std::lock_guard lock(logMutex);
        timings["engineStagesMs"] = engineTimings;
    }
    return { { "images", outputs }, { "profile", loadedProfile["id"] }, { "contextGeneration", generation },
        { "effectiveSettings", effectiveSettings }, { "request", p }, { "timings", timings },
        { "inputRegionReferences", regionReferences }, { "referenceSizes", referenceSizes },
        { "fluxRegionBindings", fluxRegionBindings }, { "fluxRegionRows", fluxRegionRows } };
}

int main(int argc, char** argv)
{
    // Even third-party printf diagnostics must never contaminate protocol stdio.
    // Keep a private duplicate of stdout for framed protocol output, redirect all
    // other stdout writers to stderr before initializing the engine.
#ifdef _WIN32
    const int out = _dup(_fileno(stdout));
    _dup2(_fileno(stderr), _fileno(stdout));
    FILE* stream = _fdopen(out, "wb");
#else
    const int out = dup(fileno(stdout));
    dup2(fileno(stderr), fileno(stdout));
    FILE* stream = fdopen(out, "w");
#endif
    if (!stream)
        return 2;
    try {
        require(argc == 3 && std::string(argv[1]) == "--work-dir",
            "Usage: vulkana-diffusion-worker --work-dir ABSOLUTE_PRIVATE_DIRECTORY");
        fs::path root = fs::path(reinterpret_cast<const char8_t*>(argv[2]));
        require(root.is_absolute() && fs::is_directory(root) && !fs::is_symlink(root),
            "work-dir must be an existing private directory, not a symbolic link");
#ifndef _WIN32
        struct stat st { };
        require(stat(root.c_str(), &st) == 0 && st.st_uid == getuid() && (st.st_mode & 0077) == 0,
            "work-dir must be owned by the current user with mode 0700");
#endif
        Worker worker(stream, fs::canonical(root));
        worker.announce();
        std::string line;
        line.reserve(4096);
        char c;
        bool run = true;
        while (run && std::cin.get(c)) {
            if (c == '\n') {
                auto request = Json::parse(line, nullptr, false);
                line.clear();
                if (request.is_discarded())
                    worker.fail("", "invalid_json", "Expected one JSON object per line");
                else
                    run = worker.handle(request);
            } else {
                line += c;
                if (line.size() > 1024 * 1024) {
                    worker.fail("", "request_too_large", "Request exceeds 1 MiB");
                    break;
                }
            }
        }
        worker.shutdown();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Vulkana Diffusion: %s\n", e.what());
        return 2;
    }
}
