// Compare the native ConvRot path with an independent dense Kronecker-product
// reference. No model files are needed; CUDA is exercised when compiled in.
#include "core/ggml_extensions.h"
#include "ggml-alloc.h"
#include "ggml-backend.h"
#include "ggml-cpu.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static void check(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}

static int rotationSign(int out, int in, int group)
{
    // H4 = [+++-, ++-+, +-++, -+++]; larger rotations are H4 tensor powers.
    int sign = 1;
    for (int size = group; size > 1; size /= 4, out /= 4, in /= 4)
        if ((out % 4) + (in % 4) == 3) sign = -sign;
    return sign;
}

static void run(ggml_backend_t backend, int group)
{
    const int k = std::max(256, 2 * group), rows = 3, n = 8;
    ggml_init_params params{4 * 1024 * 1024, nullptr, true};
    auto* ctx = ggml_init(params);
    check(ctx, "context allocation failed");
    auto* x = ggml_new_tensor_2d(ctx, GGML_TYPE_F32, k, rows);
    auto* w = ggml_new_tensor_2d(ctx, GGML_TYPE_I8, k, n);
    auto* ws = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n);
    auto* bias = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n);
    auto* quant = ggml_quantize_i8_convrot(ctx, x, group);
    auto* out = ggml_mul_mat_i8_tensorwise(ctx, w, quant, ws, bias, group);
    check(quant && out, "valid group rejected");
    check(ggml_backend_supports_op(backend, quant), "backend rejects quantization");
    check(ggml_backend_supports_op(backend, out), "backend rejects packed matmul");
    auto* graph = ggml_new_graph(ctx);
    ggml_build_forward_expand(graph, out);
    auto buffer = ggml_backend_alloc_ctx_tensors(ctx, backend);
    check(buffer, "tensor allocation failed");
    std::vector<float> input(k * rows), scales(n), biases(n);
    std::vector<int8_t> weights(k * n), expected(k * rows);
    // Row zero checks zero scales. Other rows have different scales and signs.
    for (int i = k; i < k * rows; ++i)
        input[i] = float((i * 37 + i / 11) % 101 - 50) * float(i / k) / 32.f;
    for (int i = 0; i < k * n; ++i) weights[i] = int8_t((i * 13 + i / 7) % 23 - 11);
    for (int i = 0; i < n; ++i) { scales[i] = (i + 1) / 32.f; biases[i] = (i - 4) / 8.f; }
    std::vector<float> rowScales(rows), rotated(k);
    for (int row = 0; row < rows; ++row) {
        for (int base = 0; base < k; base += group)
            for (int o = 0; o < group; ++o) {
                double sum = 0;
                for (int i = 0; i < group; ++i)
                    sum += rotationSign(o, i, group) * double(input[row * k + base + i]);
                rotated[base + o] = float(sum / std::sqrt(double(group)));
            }
        float amax = 0;
        for (float value : rotated) amax = std::max(amax, std::abs(value));
        rowScales[row] = amax / 127.f;
        const float inverse = amax == 0 ? 0 : 1.f / rowScales[row];
        for (int i = 0; i < k; ++i)
            expected[row * k + i] = int8_t(std::clamp(std::lrintf(rotated[i] * inverse), -127L, 127L));
    }
    ggml_backend_tensor_set(x, input.data(), 0, input.size() * sizeof(float));
    ggml_backend_tensor_set(w, weights.data(), 0, weights.size());
    ggml_backend_tensor_set(ws, scales.data(), 0, scales.size() * sizeof(float));
    ggml_backend_tensor_set(bias, biases.data(), 0, biases.size() * sizeof(float));
    // Reuse the graph too: verifies replay does not retain temporary output state.
    for (int repeat = 0; repeat < 2; ++repeat) {
        check(ggml_backend_graph_compute(backend, graph) == GGML_STATUS_SUCCESS, "graph execution failed");
        std::vector<int8_t> packed(ggml_nbytes(quant));
        std::vector<float> actual(n * rows);
        ggml_backend_tensor_get(quant, packed.data(), 0, packed.size());
        ggml_backend_tensor_get(out, actual.data(), 0, actual.size() * sizeof(float));
        const int padded = (rows + 3) / 4 * 4;
        for (int i = 0; i < k * rows; ++i)
            check(packed[i] == expected[i], "quantized values differ from dense reference");
        for (int i = k * rows; i < k * padded; ++i)
            check(packed[i] == 0, "row padding is not zero");
        for (int row = 0; row < rows; ++row) {
            float scale;
            std::memcpy(&scale, packed.data() + k * padded + row * sizeof(float), sizeof(float));
            check(std::abs(scale - rowScales[row]) < 1e-7f, "row scale differs");
            for (int col = 0; col < n; ++col) {
                int sum = 0;
                for (int i = 0; i < k; ++i) sum += int(expected[row * k + i]) * int(weights[col * k + i]);
                const float value = float(sum) * rowScales[row] * scales[col] + biases[col];
                check(std::abs(actual[row * n + col] - value) <= 1e-4f * std::max(1.f, std::abs(value)), "matrix product differs");
            }
        }
    }
    ggml_backend_buffer_free(buffer);
    ggml_free(ctx);
    std::cout << ggml_backend_name(backend) << " group " << group << " passed\n";
}

int main(int argc, char** argv)
{
    try {
        const bool cuda = argc > 1 && std::string(argv[1]) == "cuda";
        ggml_backend_load_all();
        auto* device = cuda ? ggml_backend_dev_by_name("CUDA0") : ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_CPU);
        if (!device && cuda) return 77;
        check(device, "no test device");
        auto backend = ggml_backend_dev_init(device, nullptr);
        check(backend, "backend initialization failed");
        sd_ggml_register_extensions(backend);
        if (!cuda) ggml_backend_cpu_set_n_threads(backend, 3);
        for (int group : {1, 4, 16, 64, 256, 1024, 4096}) {
            if (cuda && (group == 4 || group == 16)) continue;
            run(backend, group);
        }
        ggml_backend_free(backend);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
