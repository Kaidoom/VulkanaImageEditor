#include "projections.hpp"

#include <cublas_v2.h>
#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <ggml-ext.h>
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <map>
#include <mutex>

namespace layerdiff {
namespace {
bool gateShape(ggml_tensor *const *inputs, int count, const void *, ggml_ext_tensor_desc *out) {
    if (count != 2) return false;
    const auto *x = inputs[0], *bias = inputs[1];
    if ((x->type != GGML_TYPE_F32 && x->type != GGML_TYPE_F16) || bias->type != GGML_TYPE_F32 || !ggml_is_matrix(x) ||
        !ggml_is_vector(bias) || !ggml_is_contiguous(x) || !ggml_is_contiguous(bias) ||
        x->ne[0] != bias->ne[0] || x->ne[0] % 2 || x->ne[1] > 65535 || ggml_nelements(x) > INT_MAX)
        return false;
    out->type = x->type;
    std::copy_n(x->ne, GGML_MAX_DIMS, out->ne);
    out->ne[0] /= 2;
    return true;
}
ggml_ext_op_t gateOp() {
    static auto op = [] {
        const ggml_ext_op_desc desc{GGML_EXT_ABI_VERSION, "vulkana.layerdiff.biased_geglu", 0, gateShape};
        return ggml_ext_register_op(&desc);
    }();
    return op;
}

template<class T>
__global__ void applyBiasedGeglu(const T *x, const float *bias, T *output, int width) {
    const int col = blockIdx.x * blockDim.x + threadIdx.x;
    if (col >= width) return;
    const int row = blockIdx.y;
    const int i = row * (2 * width) + col;
    const float content = float(x[i]) + bias[col], gate = float(x[i + width]) + bias[col + width];
    // Match ggml_geglu_erf_swapped's FP32 operation order.
    const float activated = 0.5f * gate * (1.0f + erff(gate * 0.7071067811865475244f));
    output[row * width + col] = activated * content;
}

ggml_ext_cuda_context_fn streamQuery(ggml_backend_dev_t device) {
    return reinterpret_cast<ggml_ext_cuda_context_fn>(
        ggml_backend_reg_get_proc_address(ggml_backend_dev_backend_reg(device), "ggml_ext_cuda_context_v1"));
}
ggml_status runGate(const ggml_ext_execution_context *execution, ggml_tensor *const *nodes, int, void *, void *) {
    ggml_ext_cuda_context cuda{};
    const auto query = streamQuery(ggml_backend_get_device(execution->backend));
    if (!query || !query(execution->backend, &cuda)) return GGML_STATUS_FAILED;
    auto *out = nodes[0];
    const int width = int(out->ne[0]);
    constexpr int threads = 256;
    const dim3 grid((width + threads - 1) / threads, unsigned(out->ne[1]));
    const auto stream = static_cast<cudaStream_t>(cuda.stream);
    const auto *bias = static_cast<const float *>(out->src[1]->data);
    if (out->type == GGML_TYPE_F16)
        applyBiasedGeglu<<<grid, threads, 0, stream>>>(static_cast<const __half *>(out->src[0]->data), bias,
                                                       static_cast<__half *>(out->data), width);
    else
        applyBiasedGeglu<<<grid, threads, 0, stream>>>(static_cast<const float *>(out->src[0]->data), bias,
                                                       static_cast<float *>(out->data), width);
    return cudaGetLastError() == cudaSuccess ? GGML_STATUS_SUCCESS : GGML_STATUS_FAILED;
}

bool productShape(ggml_tensor *const *inputs, int count, const void *, ggml_ext_tensor_desc *out) {
    if (count != 2) return false;
    const auto *weight = inputs[0], *input = inputs[1];
    if (weight->type != GGML_TYPE_F16 || input->type != GGML_TYPE_F16 ||
        !ggml_is_matrix(weight) || !ggml_is_matrix(input) ||
        !ggml_is_contiguous(weight) || !ggml_is_contiguous(input) ||
        weight->ne[0] != input->ne[0] || weight->ne[0] > INT_MAX ||
        weight->ne[1] > INT_MAX || !usesHalfProjection(input->ne[1])) return false;
    *out = {GGML_TYPE_F16, {weight->ne[1], input->ne[1], 1, 1}};
    return true;
}
ggml_ext_op_t productOp() {
    static auto op = [] {
        const ggml_ext_op_desc desc{GGML_EXT_ABI_VERSION, "vulkana.layerdiff.half_projection", 0, productShape};
        return ggml_ext_register_op(&desc);
    }();
    return op;
}
ggml_ext_cuda_blas_fn blasQuery(ggml_backend_dev_t device) {
    return reinterpret_cast<ggml_ext_cuda_blas_fn>(
        ggml_backend_reg_get_proc_address(ggml_backend_dev_backend_reg(device), "ggml_ext_cuda_blas_v1"));
}
ggml_status runProduct(const ggml_ext_execution_context *execution, ggml_tensor *const *nodes, int, void *, void *) {
    auto *out = nodes[0];
    const auto *weight = out->src[0], *input = out->src[1];
    const auto borrow = blasQuery(ggml_backend_get_device(execution->backend));
    auto handle = borrow ? static_cast<cublasHandle_t>(borrow(execution->backend)) : nullptr;
    if (!handle) return GGML_STATUS_FAILED;
    const int k = int(weight->ne[0]), m = int(weight->ne[1]), n = int(input->ne[1]);
    const __half one = __float2half(1.f), zero = __float2half(0.f);
    // Same handle, layout, algorithm, compute type and output rounding as
    // ggml_cuda_mul_mat_cublas_impl<F16> for contiguous, unbatched matrices.
    const auto status = cublasGemmEx(handle, CUBLAS_OP_T, CUBLAS_OP_N, m, n, k,
        &one, weight->data, CUDA_R_16F, k, input->data, CUDA_R_16F, k,
        &zero, out->data, CUDA_R_16F, m, CUBLAS_COMPUTE_16F, CUBLAS_GEMM_DEFAULT_TENSOR_OP);
    if (status == CUBLAS_STATUS_ALLOC_FAILED) return GGML_STATUS_ALLOC_FAILED;
    return status == CUBLAS_STATUS_SUCCESS ? GGML_STATUS_SUCCESS : GGML_STATUS_FAILED;
}

bool affineShape(ggml_tensor *const *inputs, int count, const void *, ggml_ext_tensor_desc *out) {
    if (count != 3) return false;
    const auto *x = inputs[0];
    if (x->type != GGML_TYPE_F32 || !ggml_is_contiguous(x) || ggml_nelements(x) > INT_MAX)
        return false;
    for (int i = 1; i < 3; ++i) {
        const auto *v = inputs[i];
        if (v->type != GGML_TYPE_F32 || !ggml_is_vector(v) || !ggml_is_contiguous(v) || v->ne[0] != x->ne[0])
            return false;
    }
    out->type = GGML_TYPE_F16;
    std::copy_n(x->ne, GGML_MAX_DIMS, out->ne);
    return true;
}
ggml_ext_op_t affineOp() {
    static auto op = [] {
        const ggml_ext_op_desc desc{GGML_EXT_ABI_VERSION, "vulkana.layerdiff.affine_projection_input", 0, affineShape};
        return ggml_ext_register_op(&desc);
    }();
    return op;
}

__global__ void applyAffine(const float *x, const float *scale, const float *bias, __half *out, int width, int rows) {
    const int col = blockIdx.x * blockDim.x + threadIdx.x;
    if (col >= width) return;
    for (int64_t row = blockIdx.y; row < rows; row += gridDim.y) {
        const int64_t i = row * width + col;
        // The generic graph multiplies, adds and then rounds for the GEMM input.
        out[i] = __float2half_rn(__fadd_rn(__fmul_rn(x[i], scale[col]), bias[col]));
    }
}
ggml_status runAffine(const ggml_ext_execution_context *execution, ggml_tensor *const *nodes, int, void *, void *) {
    ggml_ext_cuda_context cuda{};
    const auto query = streamQuery(ggml_backend_get_device(execution->backend));
    if (!query || !query(execution->backend, &cuda)) return GGML_STATUS_FAILED;
    auto *out = nodes[0];
    const int width = int(out->ne[0]), rows = int(ggml_nrows(out));
    constexpr int threads = 256;
    const dim3 grid(1 + (width - 1) / threads, std::min(rows, 65535));
    applyAffine<<<grid, threads, 0, static_cast<cudaStream_t>(cuda.stream)>>>(
        static_cast<const float *>(out->src[0]->data), static_cast<const float *>(out->src[1]->data),
        static_cast<const float *>(out->src[2]->data), static_cast<__half *>(out->data), width, rows);
    return cudaGetLastError() == cudaSuccess ? GGML_STATUS_SUCCESS : GGML_STATUS_FAILED;
}

bool install(ggml_backend_dev_t device, const char *name, ggml_ext_op_t op, decltype(ggml_ext_kernel_desc::execute) run) {
    if (!op) return false;
    ggml_ext_kernel_desc kernel{};
    kernel.abi_version = GGML_EXT_ABI_VERSION;
    kernel.name = name;
    kernel.op = GGML_OP_CUSTOM;
    kernel.custom_op = op;
    // The extension registry matches this exact custom op and its shape inference has validated all inputs.
    kernel.supports = [](ggml_backend_dev_t, const ggml_tensor *, void *) { return true; };
    kernel.execute = run;
    return ggml_ext_register_kernel_ex(device, &kernel, GGML_EXT_KERNEL_CAPTURE_SAFE | GGML_EXT_KERNEL_REPLAY_REUSABLE);
}
ggml_tensor *halfProjection(ggml_context *ctx, ggml_tensor *weight, ggml_tensor *input) {
    ggml_tensor *inputs[] = {weight, input->type == GGML_TYPE_F16 ? input : ggml_cast(ctx, input, GGML_TYPE_F16)};
    return ggml_ext_op_create(ctx, productOp(), inputs, 2, nullptr);
}

ggml_tensor *gateProjection(ggml_context *ctx, ggml_tensor *projection, ggml_tensor *bias) {
    ggml_tensor *inputs[] = {projection, bias};
    return ggml_ext_op_create(ctx, gateOp(), inputs, 2, nullptr);
}
ggml_tensor *affineProjectionInput(ggml_context *ctx, ggml_tensor *normalized, ggml_tensor *scale, ggml_tensor *bias) {
    ggml_tensor *inputs[] = {normalized, scale, bias};
    return ggml_ext_op_create(ctx, affineOp(), inputs, 3, nullptr);
}
} // namespace

ProjectionKernels registerCudaProjectionKernels(ggml_backend_t backend) {
    const auto device = ggml_backend_get_device(backend);
    static std::mutex mutex;
    static std::map<ggml_backend_dev_t, ProjectionKernels> devices;
    const std::lock_guard lock(mutex);
    auto [it, added] = devices.try_emplace(device);
    auto &kernels = it->second;
    if (added && streamQuery(device)) {
        if (install(device, "vulkana.layerdiff.biased_geglu.cuda", gateOp(), runGate)) kernels.biasedGate = gateProjection;
        const auto ordinal = reinterpret_cast<ggml_ext_cuda_device_fn>(
            ggml_backend_reg_get_proc_address(ggml_backend_dev_backend_reg(device), "ggml_ext_cuda_device_v1"));
        int major = 0;
        if (kernels.biasedGate && ordinal && blasQuery(device) &&
            cudaDeviceGetAttribute(&major, cudaDevAttrComputeCapabilityMajor, ordinal(device)) == cudaSuccess && major >= 8) {
            if (install(device, "vulkana.layerdiff.half_projection.cuda", productOp(), runProduct)) kernels.halfProducts = halfProjection;
            if (kernels.halfProducts && install(device, "vulkana.layerdiff.affine_projection_input.cuda", affineOp(), runAffine))
                kernels.affineInput = affineProjectionInput;
        }
    }
    // Explicit compute-type overrides belong to ggml. Do not narrow their inputs.
    if (std::getenv("GGML_CUDA_CUBLAS_COMPUTE_TYPE")) return {kernels.biasedGate, nullptr, nullptr};
    return kernels;
}

} // namespace layerdiff
