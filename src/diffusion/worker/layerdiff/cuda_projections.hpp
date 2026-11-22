#pragma once

#include <ggml-backend.h>

namespace layerdiff {
struct ProjectionKernels {
    bool biasedGate = false;
    bool halfProducts = false;
    bool affineInput = false;
};

ProjectionKernels registerProjectionKernels(ggml_backend_t backend);
bool usesHalfProjection(int64_t rows);
ggml_tensor *halfProjection(ggml_context *ctx, ggml_tensor *weight, ggml_tensor *input);
ggml_tensor *biasedGeglu(ggml_context *ctx, ggml_tensor *projection, ggml_tensor *bias, bool fused);
ggml_tensor *affineProjectionInput(ggml_context *ctx, ggml_tensor *normalized, ggml_tensor *scale, ggml_tensor *bias);
} // namespace layerdiff
