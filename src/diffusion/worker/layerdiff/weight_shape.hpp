#pragma once

#include "backend.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>

namespace layerdiff {

// Check every dimension before giving a user-supplied weight to ggml. Many
// operations assert internally on incompatible dimensions instead of returning an error.
inline ggml_tensor* checkedWeight(const Weights& weights, const std::string& name, int64_t ne0,
                                  int64_t ne1 = 1, int64_t ne2 = 1, int64_t ne3 = 1) {
    auto* tensor = weights(name);
    const std::array<int64_t, 4> expected{ne0, ne1, ne2, ne3};
    const auto storage = ne1 == 1 && ne2 == 1 && ne3 == 1 ? GGML_TYPE_F32 : GGML_TYPE_F16;
    if (!std::equal(expected.begin(), expected.end(), tensor->ne) || tensor->type != storage)
        throw std::runtime_error("Unexpected See-through tensor shape: " + name);
    return tensor;
}

} // namespace layerdiff
