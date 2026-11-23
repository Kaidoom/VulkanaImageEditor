#pragma once

#include "projections.hpp"
#include <deque>
#include <filesystem>
#include <functional>
#include <ggml-alloc.h>
#include <ggml-backend.h>
#include <ggml.h>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace layerdiff {

// Contiguous, outermost dimension first. GGML dimensions are innermost first.
struct Tensor {
    std::vector<int64_t> shape;
    std::vector<float> values;
};
size_t elements(const std::vector<int64_t> &shape);

ggml_backend_reg_t gpuRegistry();

class Backend {
  public:
    explicit Backend(ggml_backend_dev_t device, std::function<void()> checkpoint = {});
    ~Backend();
    Backend(const Backend &) = delete;
    void check() const;
    void trim();
    ggml_backend_t handle;

  private:
    friend class Graph;
    ProjectionKernels projectionKernels_;
    std::function<void()> checkpoint_;
    ggml_gallocr_t allocator_ = nullptr;
};

class Weights {
  public:
    Weights(Backend &, const std::filesystem::path &file);
    ~Weights();
    Weights(const Weights &) = delete;
    ggml_tensor *operator()(const std::string &key) const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class Graph {
  public:
    explicit Graph(Backend &, size_t capacity = 8192);
    ~Graph();
    Graph(const Graph &) = delete;
    ggml_tensor *input(Tensor host);
    // Caller keeps the host tensor alive through synchronous evaluate().
    ggml_tensor *inputRef(const Tensor &host);
    ggml_tensor *inputRef(Tensor &&) = delete;
    Tensor evaluate(ggml_tensor *result, std::vector<int64_t> outputShape = {});
    // Preflight a complete denoising step and leave room for GPU kernel workspaces.
    std::optional<Tensor> evaluateIfFits(ggml_tensor *result, std::vector<int64_t> outputShape);
    ProjectionKernels projectionKernels() const { return backend_.projectionKernels_; }
    ggml_context *ctx;

  private:
    Backend &backend_;
    size_t capacity_;
    std::optional<Tensor> execute(ggml_tensor *result, std::vector<int64_t> shape, bool completeStep);
    std::deque<Tensor> owned_;
    std::vector<std::pair<ggml_tensor *, const Tensor *>> inputs_;
};
} // namespace layerdiff
