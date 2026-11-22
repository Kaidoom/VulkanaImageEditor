#include "backend.hpp"
#include "cuda_projections.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <ggml-cuda.h>
#include <ggml-impl.h> // graph views retain use counts across cancellation checkpoints
#include <json.hpp>
#include <limits>
#include <map>
#include <stdexcept>

namespace layerdiff {
namespace {
using Json = nlohmann::json;
using Context = std::unique_ptr<ggml_context, decltype(&ggml_free)>;
using Buffer = std::unique_ptr<ggml_backend_buffer, decltype(&ggml_backend_buffer_free)>;

struct Record {
    std::vector<int64_t> shape;
    ggml_type type;
    size_t offset;
};

class Safetensors {
  public:
    explicit Safetensors(const std::filesystem::path &path) : stream(path, std::ios::binary) {
        uint64_t length = 0;
        if (!stream.read(reinterpret_cast<char *>(&length), sizeof(length)) || length > 64 * 1024 * 1024)
            throw std::runtime_error("Invalid Safetensors header in " + path.filename().string());
        const auto bytes = std::filesystem::file_size(path);
        start = 8 + length;
        if (start > bytes)
            throw std::runtime_error("Truncated Safetensors header");
        std::string header(length, '\0');
        if (!stream.read(header.data(), header.size()))
            throw std::runtime_error("Cannot read tensor index");
        const auto json = Json::parse(header);
        if (!json.is_object() || json.size() > 20000)
            throw std::runtime_error("Invalid tensor index");
        std::vector<std::pair<uint64_t, uint64_t>> extents;
        for (const auto &[name, obj] : json.items()) {
            if (name == "__metadata__")
                continue;
            const auto dtype = obj.at("dtype").get<std::string>();
            auto type = dtype == "BF16"  ? GGML_TYPE_BF16
                        : dtype == "F16" ? GGML_TYPE_F16
                        : dtype == "F32" ? GGML_TYPE_F32
                                         : GGML_TYPE_COUNT;
            if (type == GGML_TYPE_COUNT)
                throw std::runtime_error("Unsupported LayerDiff tensor precision: " + dtype);
            auto shape = obj.at("shape").get<std::vector<int64_t>>();
            const auto offsets = obj.at("data_offsets").get<std::vector<uint64_t>>();
            const size_t n = elements(shape);
            if (offsets.size() != 2 || offsets[1] < offsets[0] || offsets[1] > bytes - start ||
                n > (bytes - start) / ggml_type_size(type) || n * ggml_type_size(type) != offsets[1] - offsets[0])
                throw std::runtime_error("Invalid Safetensors extent: " + name);
            extents.emplace_back(offsets[0], offsets[1]);
            records.emplace(name, Record{std::move(shape), type, size_t(offsets[0])});
        }
        std::sort(extents.begin(), extents.end());
        uint64_t consumed = 0;
        for (auto [begin, end] : extents) {
            if (begin != consumed)
                throw std::runtime_error("Missing or overlapping model tensor data");
            consumed = end;
        }
        if (consumed != bytes - start)
            throw std::runtime_error("Unindexed bytes after model tensors");
        if (records.empty())
            throw std::runtime_error("Empty tensor file");
    }
    void seek(const Record &r) { stream.seekg(start + r.offset); }
    void read(const Record &r, std::vector<float> &out, size_t count) {
        out.resize(count);
        if (r.type == GGML_TYPE_F32) {
            stream.read(reinterpret_cast<char *>(out.data()), count * sizeof(float));
        } else if (r.type == GGML_TYPE_F16) {
            half.resize(count);
            stream.read(reinterpret_cast<char *>(half.data()), count * sizeof(ggml_fp16_t));
            ggml_fp16_to_fp32_row(half.data(), out.data(), count);
        } else {
            brain.resize(count);
            stream.read(reinterpret_cast<char *>(brain.data()), count * sizeof(ggml_bf16_t));
            ggml_bf16_to_fp32_row(brain.data(), out.data(), count);
        }
        if (!stream)
            throw std::runtime_error("Truncated model tensor");
    }
    std::map<std::string, Record> records;

  private:
    uint64_t start;
    std::ifstream stream;
    std::vector<ggml_fp16_t> half;
    std::vector<ggml_bf16_t> brain;
};

ggml_tensor *makeTensor(ggml_context *context, ggml_type type, const std::vector<int64_t> &shape) {
    if (shape.empty() || shape.size() > GGML_MAX_DIMS)
        throw std::runtime_error("Unsupported ggml tensor rank");
    std::array<int64_t, GGML_MAX_DIMS> reverse{1, 1, 1, 1};
    std::reverse_copy(shape.begin(), shape.end(), reverse.begin());
    return ggml_new_tensor(context, type, int(shape.size()), reverse.data());
}
} // namespace
size_t elements(const std::vector<int64_t> &dims) {
    size_t n = 1;
    for (const auto dim : dims) {
        if (dim <= 0 || uint64_t(dim) > std::numeric_limits<size_t>::max() / sizeof(float) / n)
            throw std::runtime_error("Invalid tensor dimensions");
        n *= size_t(dim);
    }
    return n;
}
Backend::Backend(int index, std::function<void()> checkpoint)
    : handle(nullptr), checkpoint_(std::move(checkpoint)) {
    if (index < 0 || index >= ggml_backend_cuda_get_device_count())
        throw std::runtime_error("Selected CUDA device is unavailable");
    std::unique_ptr<ggml_backend, decltype(&ggml_backend_free)> initialized(ggml_backend_cuda_init(index), ggml_backend_free);
    if (!initialized)
        throw std::runtime_error("Could not initialize LayerDiff CUDA backend");
    projectionKernels_ = registerProjectionKernels(initialized.get());
    handle = initialized.release();
}
Backend::~Backend() {
    trim();
    ggml_backend_free(handle);
}
void Backend::trim() {
    if (allocator_)
        ggml_gallocr_free(allocator_);
    allocator_ = nullptr;
}
void Backend::check() const {
    if (checkpoint_)
        checkpoint_();
}

class Weights::Impl {
  public:
    Context context{nullptr, ggml_free};
    Buffer buffer{nullptr, ggml_backend_buffer_free};
    std::map<std::string, ggml_tensor *> tensors;
};
Weights::Weights(Backend &backend, const std::filesystem::path &path) : impl_(std::make_unique<Impl>()) {
    Safetensors disk(path);
    backend.check();
    impl_->context.reset(ggml_init({ggml_tensor_overhead() * (disk.records.size() + 1), nullptr, true}));
    if (!impl_->context)
        throw std::bad_alloc();
    for (const auto &[name, r] : disk.records) {
        auto *t = makeTensor(impl_->context.get(), r.shape.size() >= 2 ? GGML_TYPE_F16 : GGML_TYPE_F32, r.shape);
        ggml_set_name(t, name.c_str());
        impl_->tensors.emplace(name, t);
    }
    impl_->buffer.reset(ggml_backend_alloc_ctx_tensors(impl_->context.get(), backend.handle));
    if (!impl_->buffer)
        throw std::bad_alloc();
    ggml_backend_buffer_set_usage(impl_->buffer.get(), GGML_BACKEND_BUFFER_USAGE_WEIGHTS);
    std::vector<float> floats;
    std::vector<ggml_fp16_t> half;
    for (const auto &[name, r] : disk.records) {
        backend.check();
        auto *t = impl_->tensors.at(name);
        const auto count = elements(r.shape);
        disk.seek(r);
        for (size_t i = 0; i < count;) {
            const auto n = std::min<size_t>(count - i, 1 << 20);
            disk.read(r, floats, n);
            if (t->type == GGML_TYPE_F16) {
                half.resize(n);
                ggml_fp32_to_fp16_row(floats.data(), half.data(), n);
                ggml_backend_tensor_set(t, half.data(), i * sizeof(ggml_fp16_t), n * sizeof(ggml_fp16_t));
            } else
                ggml_backend_tensor_set(t, floats.data(), i * sizeof(float), n * sizeof(float));
            i += n;
        }
    }
}
Weights::~Weights() = default;
ggml_tensor *Weights::operator()(const std::string &key) const {
    auto i = impl_->tensors.find(key);
    if (i == impl_->tensors.end())
        throw std::runtime_error("Missing LayerDiff tensor: " + key);
    return i->second;
}
Graph::Graph(Backend &b, size_t capacity) : ctx(nullptr), backend_(b), capacity_(capacity) {
    b.check();
    ctx = ggml_init(
        {ggml_tensor_overhead() * (capacity_ + 32) + ggml_graph_overhead_custom(capacity_, false), nullptr,
         true});
    if (!ctx)
        throw std::bad_alloc();
}
Graph::~Graph() { ggml_free(ctx); }
ggml_tensor *Graph::input(Tensor host) {
    owned_.push_back(std::move(host));
    return inputRef(owned_.back());
}
ggml_tensor *Graph::inputRef(const Tensor &host) {
    if (elements(host.shape) != host.values.size())
        throw std::runtime_error("LayerDiff graph input size mismatch");
    auto *t = makeTensor(ctx, GGML_TYPE_F32, host.shape);
    ggml_set_input(t);
    inputs_.emplace_back(t, &host);
    return t;
}
Tensor Graph::evaluate(ggml_tensor *t, std::vector<int64_t> shape) {
    return execute(t, std::move(shape), false).value();
}
std::optional<Tensor> Graph::evaluateIfFits(ggml_tensor *t, std::vector<int64_t> shape) {
    return execute(t, std::move(shape), true);
}
std::optional<Tensor> Graph::execute(ggml_tensor *t, std::vector<int64_t> shape, bool completeStep) {
    if (t->type != GGML_TYPE_F32)
        t = ggml_cast(ctx, t, GGML_TYPE_F32);
    if (!ggml_is_contiguous(t))
        t = ggml_cont(ctx, t);
    if (shape.empty())
        for (int i = ggml_n_dims(t) - 1; i >= 0; --i)
            shape.push_back(t->ne[i]);
    if (elements(shape) != size_t(ggml_nelements(t)))
        throw std::runtime_error("LayerDiff graph output size mismatch");
    auto *graph = ggml_new_graph_custom(ctx, capacity_, false);
    ggml_set_output(t);
    ggml_build_forward_expand(graph, t);
    for (int n = 0; n < ggml_graph_n_nodes(graph); ++n) {
        auto *node = ggml_graph_node(graph, n);
        if (!ggml_backend_supports_op(backend_.handle, node))
            throw std::runtime_error(std::string("Unsupported CUDA operator in LayerDiff: ") + ggml_op_desc(node));
    }
    const auto bufferType = ggml_backend_get_default_buffer_type(backend_.handle);
    if (!backend_.allocator_)
        backend_.allocator_ = ggml_gallocr_new(bufferType);
    if (!backend_.allocator_)
        throw std::bad_alloc();
    if (completeStep) {
        // Measuring mutates the allocator's plan. Use a temporary allocator so the live
        // buffers remain usable when the smaller, host-staged path is needed.
        std::unique_ptr<ggml_gallocr, decltype(&ggml_gallocr_free)> measure(ggml_gallocr_new(bufferType), ggml_gallocr_free);
        if (!measure) throw std::bad_alloc();
        size_t required = 0, available = 0, total = 0;
        ggml_gallocr_reserve_n_size(measure.get(), graph, nullptr, nullptr, &required);
        ggml_backend_dev_memory(ggml_backend_get_device(backend_.handle), &available, &total);
        const auto scratch = ggml_gallocr_get_buffer_size(backend_.allocator_, 0);
        // cuBLAS and the convolution/attention kernels also allocate outside the graph.
        const size_t headroom = std::max<size_t>(768ull << 20, total / 16);
        if (required + headroom > available + scratch) {
            backend_.trim();
            return std::nullopt;
        }
    }
    if (!ggml_gallocr_alloc_graph(backend_.allocator_, graph)) {
        backend_.trim();
        if (completeStep) return std::nullopt;
        throw std::bad_alloc();
    }
    for (const auto &[input, host] : inputs_)
        if (input->data)
            ggml_backend_tensor_set(input, host->values.data(), 0, host->values.size() * sizeof(float));
    const int nodes = ggml_graph_n_nodes(graph);
    // Keep cancellation responsive without reading activations back to the host.
    const int batch = completeStep ? 256 : nodes;
    for (int first = 0; first < nodes; first += batch) {
        backend_.check();
        auto view = ggml_graph_view(graph, first, std::min(first + batch, nodes));
        auto status = ggml_backend_graph_compute(backend_.handle, completeStep ? &view : graph);
        if (status == GGML_STATUS_ALLOC_FAILED) {
            backend_.trim();
            if (completeStep) return std::nullopt;
            throw std::bad_alloc();
        }
        if (status != GGML_STATUS_SUCCESS)
            throw std::runtime_error(std::string("LayerDiff CUDA graph failed: ") + ggml_status_to_string(status));
    }
    Tensor result{std::move(shape), std::vector<float>(size_t(ggml_nelements(t)))};
    ggml_backend_tensor_get(t, result.values.data(), 0, result.values.size() * sizeof(float));
    backend_.check();
    return result;
}
} // namespace layerdiff
