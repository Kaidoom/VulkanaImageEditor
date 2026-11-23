#pragma once

#include <ggml-backend.h>
#include <climits>

namespace layerdiff {
// Optional device kernels. An empty table uses the standard ggml graph.
struct ProjectionKernels {
    using Binary = ggml_tensor *(*)(ggml_context *, ggml_tensor *, ggml_tensor *);
    Binary biasedGate{};
    Binary halfProducts{};
    ggml_tensor *(*affineInput)(ggml_context *, ggml_tensor *, ggml_tensor *, ggml_tensor *){};
};

ProjectionKernels registerCudaProjectionKernels(ggml_backend_t backend);
inline bool usesHalfProjection(int64_t rows) { return rows >= 256 && rows <= INT_MAX; }
inline ggml_tensor *biasedGeglu(ggml_context *ctx, ggml_tensor *projection, ggml_tensor *bias,
                                ProjectionKernels::Binary fused) {
    if (fused) {
        if (auto *result = fused(ctx, projection, bias)) return result;
    }
    const bool half = projection->type == GGML_TYPE_F16;
    if (half) projection = ggml_cast(ctx, projection, GGML_TYPE_F32);
    auto *activated = ggml_geglu_erf_swapped(ctx, ggml_add(ctx, projection, bias));
    return half ? ggml_cast(ctx, activated, GGML_TYPE_F16) : activated;
}
} // namespace layerdiff
