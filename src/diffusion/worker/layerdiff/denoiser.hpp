#pragma once

#include "backend.hpp"

namespace layerdiff {

// One coupled body or face group. Input is [parts, 8, height, width], the four
// noisy channels followed by the four source-image channels. Text and pooled
// conditioning have a separate entry per trained body-part tag.
Tensor denoise(Backend& backend, Weights& weights, const Tensor& input, double timestep, const Tensor& context,
               const Tensor& pooled, const Tensor& timeIds, int group, bool& tryResident);
} // namespace layerdiff
