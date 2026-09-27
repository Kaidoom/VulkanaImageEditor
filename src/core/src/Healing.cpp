#include "imageeditor/core/Healing.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>

namespace imageeditor::core {
namespace {

constexpr int supportRadius = 6;
constexpr std::size_t hardPixelLimit = 4U * 1024U * 1024U;
constexpr std::size_t fitSampleLimit = 8192;
constexpr float screening = 1.0F / 256.0F;
constexpr float boundaryStrength = 4.0F;
constexpr float alphaFloor = 1.0e-6F;
using Triple = std::array<double, 3>;
using Plane = std::array<Triple, 3>; // channel, then constant/x/y coefficient

bool usable(PremultipliedColor value)
{
    return value[3] > alphaFloor && value[3] <= 1.0F
        && std::all_of(value.begin(), value.end(), [](float v) { return std::isfinite(v); });
}

PremultipliedColor sanitized(PremultipliedColor value)
{
    // Tiny positive alpha still belongs to the sampled image. Preserve it even
    // when it is too small to be numerically reliable illumination context.
    if (!(value[3] > 0.0F && value[3] <= 1.0F)
        || !std::all_of(value.begin(), value.end(), [](float v) { return std::isfinite(v); })) return {};
    for (std::size_t c = 0; c < 3; ++c) value[c] = std::clamp(value[c], 0.0F, value[3]);
    return value;
}

Triple difference(PremultipliedColor destination, PremultipliedColor source)
{
    Triple result {};
    for (std::size_t c = 0; c < 3; ++c) {
        result[c] = std::clamp(double(destination[c]) / destination[3], 0.0, 1.0)
            - std::clamp(double(source[c]) / source[3], 0.0, 1.0);
    }
    return result;
}

double norm(Triple value)
{
    return std::sqrt((value[0] * value[0] + value[1] * value[1] + value[2] * value[2]) / 3.0);
}

double median(std::vector<double>& values)
{
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

Triple evaluate(const Plane& plane, const Triple& basis)
{
    Triple result {};
    for (std::size_t c = 0; c < 3; ++c)
        for (std::size_t k = 0; k < 3; ++k) result[c] += plane[c][k] * basis[k];
    return result;
}

Triple subtract(Triple left, Triple right)
{
    for (std::size_t c = 0; c < 3; ++c) left[c] -= right[c];
    return left;
}

struct Sample {
    Triple basis;
    Triple delta;
    double weight;
};

// Three-variable pivoted elimination. Coordinate normalization and a tiny
// slope-only ridge handle single rows/columns without inventing a slope.
bool solvePlane(std::array<Triple, 3> matrix, Triple rhs, Triple& output)
{
    for (std::size_t column = 0; column < 3; ++column) {
        std::size_t pivot = column;
        for (std::size_t row = column + 1; row < 3; ++row)
            if (std::abs(matrix[row][column]) > std::abs(matrix[pivot][column])) pivot = row;
        if (std::abs(matrix[pivot][column]) < 1.0e-15) return false;
        std::swap(matrix[pivot], matrix[column]);
        std::swap(rhs[pivot], rhs[column]);
        for (std::size_t row = column + 1; row < 3; ++row) {
            const double factor = matrix[row][column] / matrix[column][column];
            for (std::size_t k = column; k < 3; ++k) matrix[row][k] -= factor * matrix[column][k];
            rhs[row] -= factor * rhs[column];
        }
    }
    for (std::size_t row = 3; row-- > 0;) {
        double value = rhs[row];
        for (std::size_t k = row + 1; k < 3; ++k) value -= matrix[row][k] * output[k];
        output[row] = value / matrix[row][row];
        if (!std::isfinite(output[row])) return false;
    }
    return true;
}

double robustWeight(double residual, double cutoff)
{
    if (residual >= cutoff) return 0.0;
    const double ratio = residual / cutoff;
    const double remaining = 1.0 - ratio * ratio;
    return remaining * remaining;
}

// A bounded Tukey IRLS fit rejects unrelated boundary objects. The model is
// spatial, not a single mean offset; the subsequent PDE also fits its residual.
bool fitPlane(const std::vector<Sample>& samples, Plane& plane, double& cutoff,
    const HealingOptions& options, bool& wasCancelled)
{
    if (samples.size() < 3) return false;
    std::vector<double> work(samples.size());
    for (std::size_t c = 0; c < 3; ++c) {
        for (std::size_t i = 0; i < samples.size(); ++i) work[i] = samples[i].delta[c];
        plane[c][0] = median(work);
    }
    for (int iteration = 0; iteration < 8; ++iteration) {
        if (options.cancelled && options.cancelled()) { wasCancelled = true; return false; }
        for (std::size_t i = 0; i < samples.size(); ++i)
            work[i] = norm(subtract(samples[i].delta, evaluate(plane, samples[i].basis)));
        // Floor permits a few linear-light levels of texture disagreement.
        cutoff = std::max(0.025, 4.685 * 1.4826 * median(work));
        std::array<Triple, 3> matrix {};
        Plane rhs {};
        std::size_t accepted = 0;
        for (const auto& sample : samples) {
            const double residual = norm(subtract(sample.delta, evaluate(plane, sample.basis)));
            const double weight = sample.weight * robustWeight(residual, cutoff);
            if (weight <= 0.0) continue;
            ++accepted;
            for (std::size_t row = 0; row < 3; ++row) {
                for (std::size_t column = 0; column < 3; ++column)
                    matrix[row][column] += weight * sample.basis[row] * sample.basis[column];
                for (std::size_t c = 0; c < 3; ++c)
                    rhs[c][row] += weight * sample.basis[row] * sample.delta[c];
            }
        }
        if (accepted < 3 || matrix[0][0] <= 0.0) return false;
        matrix[1][1] += matrix[0][0] * 1.0e-8;
        matrix[2][2] += matrix[0][0] * 1.0e-8;
        for (std::size_t c = 0; c < 3; ++c)
            if (!solvePlane(matrix, rhs[c], plane[c])) return false;
    }
    // Recompute the rejection scale for the final model, not the prior iterate.
    for (std::size_t i = 0; i < samples.size(); ++i)
        work[i] = norm(subtract(samples[i].delta, evaluate(plane, samples[i].basis)));
    cutoff = std::max(0.025, 4.685 * 1.4826 * median(work));
    return true;
}

template<class F>
void neighbors(int pixel, int width, int height, F&& function)
{
    const int x = pixel % width;
    if (x > 0) function(pixel - 1);
    if (x + 1 < width) function(pixel + 1);
    if (pixel >= width) function(pixel - width);
    if (pixel < width * (height - 1)) function(pixel + width);
}

struct SolverBuffers {
    std::vector<float> diagonal, boundary, x, residual, direction, product, preconditioned;
    explicit SolverBuffers(std::size_t count)
        : diagonal(count), boundary(count), x(count), residual(count), direction(count),
          product(count), preconditioned(count) {}
};

enum class SolveStatus { Converged, Failed, Cancelled };

SolveStatus solveChannel(const HealingPatch& patch, const HealingOptions& options,
    const std::vector<int>& component, const std::vector<int>& indices,
    const Plane& plane, double centerX, double centerY, double scale,
    std::size_t channel, SolverBuffers& buffers, HealingDiagnostics& diagnostics)
{
    double rhsNormSquared = 0.0;
    double rz = 0.0;
    for (std::size_t i = 0; i < component.size(); ++i) {
        if ((i & 16383U) == 0 && options.cancelled && options.cancelled()) return SolveStatus::Cancelled;
        const int pixel = component[i];
        const Triple basis {1.0, (pixel % patch.width - centerX) / scale,
            (pixel / patch.width - centerY) / scale};
        double rhs = 0.0;
        if (buffers.boundary[i] > 0.0F)
            rhs = buffers.boundary[i] * (difference(patch.destination[static_cast<std::size_t>(pixel)], patch.source[static_cast<std::size_t>(pixel)])[channel]
                - evaluate(plane, basis)[channel]);
        buffers.x[i] = 0.0F;
        buffers.residual[i] = static_cast<float>(rhs);
        buffers.preconditioned[i] = buffers.residual[i] / buffers.diagonal[i];
        buffers.direction[i] = buffers.preconditioned[i];
        rhsNormSquared += rhs * rhs;
        rz += double(buffers.residual[i]) * buffers.preconditioned[i];
    }
    const double tolerance = std::clamp(double(options.relativeTolerance), 1.0e-7, 1.0e-2);
    // Absolute RMS guards near-zero RHS without amplifying floating point noise.
    const double thresholdSquared = std::max(rhsNormSquared * tolerance * tolerance,
        double(component.size()) * 1.0e-16);
    double residualSquared = rhsNormSquared;
    const unsigned limit = std::min(options.maxIterations, 1000U);
    unsigned iteration = 0;
    while (residualSquared > thresholdSquared && iteration < limit) {
        if (options.cancelled && options.cancelled()) return SolveStatus::Cancelled;
        double denominator = 0.0;
        for (std::size_t i = 0; i < component.size(); ++i) {
            if ((i & 16383U) == 0 && options.cancelled && options.cancelled()) return SolveStatus::Cancelled;
            double product = buffers.diagonal[i] * buffers.direction[i];
            neighbors(component[i], patch.width, patch.height, [&](int adjacent) {
                const int index = indices[static_cast<std::size_t>(adjacent)];
                if (index >= 0) product -= buffers.direction[static_cast<std::size_t>(index)];
            });
            buffers.product[i] = static_cast<float>(product);
            denominator += double(buffers.direction[i]) * product;
        }
        if (!(denominator > 0.0) || !std::isfinite(denominator) || !std::isfinite(rz))
            return SolveStatus::Failed;
        const double step = rz / denominator;
        double newRz = 0.0;
        residualSquared = 0.0;
        for (std::size_t i = 0; i < component.size(); ++i) {
            if ((i & 16383U) == 0 && options.cancelled && options.cancelled()) return SolveStatus::Cancelled;
            buffers.x[i] += static_cast<float>(step * buffers.direction[i]);
            buffers.residual[i] -= static_cast<float>(step * buffers.product[i]);
            buffers.preconditioned[i] = buffers.residual[i] / buffers.diagonal[i];
            residualSquared += double(buffers.residual[i]) * buffers.residual[i];
            newRz += double(buffers.residual[i]) * buffers.preconditioned[i];
        }
        ++iteration;
        if (!std::isfinite(residualSquared)) return SolveStatus::Failed;
        if (residualSquared > thresholdSquared) {
            if (!(rz > 0.0)) return SolveStatus::Failed;
            const double beta = newRz / rz;
            for (std::size_t i = 0; i < component.size(); ++i) {
                if ((i & 16383U) == 0 && options.cancelled && options.cancelled()) return SolveStatus::Cancelled;
                buffers.direction[i] = buffers.preconditioned[i] + static_cast<float>(beta * buffers.direction[i]);
            }
        }
        rz = newRz;
    }
    // Judge the final iterate against the actual matrix, not only the recursive
    // residual (which can drift in single precision). This also detects NaNs.
    residualSquared = 0.0;
    for (std::size_t i = 0; i < component.size(); ++i) {
        if ((i & 16383U) == 0 && options.cancelled && options.cancelled()) return SolveStatus::Cancelled;
        const int pixel = component[i];
        const Triple basis {1.0, (pixel % patch.width - centerX) / scale,
            (pixel / patch.width - centerY) / scale};
        double rhs = 0.0;
        if (buffers.boundary[i] > 0.0F)
            rhs = buffers.boundary[i] * (difference(patch.destination[static_cast<std::size_t>(pixel)], patch.source[static_cast<std::size_t>(pixel)])[channel]
                - evaluate(plane, basis)[channel]);
        double applied = double(buffers.diagonal[i]) * buffers.x[i];
        neighbors(pixel, patch.width, patch.height, [&](int adjacent) {
            const int index = indices[static_cast<std::size_t>(adjacent)];
            if (index >= 0) applied -= buffers.x[static_cast<std::size_t>(index)];
        });
        const double residual = rhs - applied;
        residualSquared += residual * residual;
    }
    diagnostics.iterations = std::max(diagnostics.iterations, iteration);
    diagnostics.relativeResidual = std::max(diagnostics.relativeResidual,
        static_cast<float>(std::sqrt(residualSquared / std::max(rhsNormSquared, 1.0e-30))));
    diagnostics.absoluteResidualRms = std::max(diagnostics.absoluteResidualRms,
        static_cast<float>(std::sqrt(residualSquared / double(component.size()))));
    return residualSquared <= thresholdSquared ? SolveStatus::Converged : SolveStatus::Failed;
}

HealingResult run(const HealingPatch& patch, const HealingOptions& options)
{
    HealingResult result;
    auto& diagnostics = result.diagnostics;
    auto cancel = [&] {
        if (!options.cancelled || !options.cancelled()) return false;
        result.status = HealingStatus::Cancelled;
        result.pixels.clear();
        diagnostics.message = "Heal cancelled; no result was published.";
        return true;
    };
    if (cancel()) return result;
    if (patch.width <= 0 || patch.height <= 0 || !std::isfinite(options.adaptation)
        || !std::isfinite(options.relativeTolerance)) {
        diagnostics.message = "Heal requires valid patch dimensions and finite settings.";
        return result;
    }
    const auto count = static_cast<std::size_t>(patch.width) * static_cast<std::size_t>(patch.height);
    if (count > std::min(options.maxPixels, hardPixelLimit)) {
        result.status = HealingStatus::LimitExceeded;
        diagnostics.message = "Heal region exceeds the bounded solver budget; use smaller strokes or Stamp.";
        return result;
    }
    if (patch.source.size() != count || patch.destination.size() != count || patch.coverage.size() != count) {
        diagnostics.message = "Heal source, context, and coverage arrays must match the patch dimensions.";
        return result;
    }
    diagnostics.estimatedScratchBytes = count * 64U + fitSampleLimit * (sizeof(Sample) + sizeof(double));
    result.pixels.resize(count);
    std::vector<std::uint8_t> distance(count, 255);
    std::vector<int> indices(count, -2);
    std::vector<int> queue;
    queue.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if ((i & 16383U) == 0 && cancel()) return result;
        result.pixels[i] = sanitized(patch.source[i]);
        if (std::isfinite(patch.coverage[i]) && patch.coverage[i] > 0.0F && usable(patch.source[i])) {
            distance[i] = 0;
            indices[i] = -1;
            queue.push_back(static_cast<int>(i));
            ++diagnostics.affectedPixels;
        }
    }
    const float adaptation = std::clamp(options.adaptation, 0.0F, 1.0F);
    if (queue.empty() || adaptation == 0.0F) {
        result.status = HealingStatus::Converged;
        diagnostics.message = queue.empty() ? "Heal has no valid source coverage." : "Heal adaptation is zero (Stamp).";
        return result;
    }
    // Multi-source dilation. Exterior support can read valid destination context,
    // but never appears in the final write mask or crosses invalid source pixels.
    for (std::size_t head = 0; head < queue.size(); ++head) {
        if ((head & 16383U) == 0 && cancel()) return result;
        const int pixel = queue[head];
        if (distance[static_cast<std::size_t>(pixel)] >= supportRadius) continue;
        neighbors(pixel, patch.width, patch.height, [&](int adjacent) {
            if (distance[static_cast<std::size_t>(adjacent)] != 255 || !usable(patch.source[static_cast<std::size_t>(adjacent)])) return;
            distance[static_cast<std::size_t>(adjacent)] = static_cast<std::uint8_t>(distance[static_cast<std::size_t>(pixel)] + 1);
            indices[static_cast<std::size_t>(adjacent)] = -1;
            queue.push_back(adjacent);
        });
    }
    queue.clear();
    for (std::size_t seed = 0; seed < count; ++seed) {
        if ((seed & 16383U) == 0 && cancel()) return result;
        if (indices[seed] != -1) continue;
        queue.clear();
        queue.push_back(static_cast<int>(seed));
        indices[seed] = 0;
        int minX = patch.width, minY = patch.height, maxX = 0, maxY = 0;
        std::size_t componentWrites = 0;
        std::size_t contextCount = 0;
        for (std::size_t head = 0; head < queue.size(); ++head) {
            if ((head & 16383U) == 0 && cancel()) return result;
            const int pixel = queue[head];
            const int x = pixel % patch.width, y = pixel / patch.width;
            minX = std::min(minX, x); maxX = std::max(maxX, x);
            minY = std::min(minY, y); maxY = std::max(maxY, y);
            if (distance[static_cast<std::size_t>(pixel)] == 0) ++componentWrites;
            else if (patch.coverage[static_cast<std::size_t>(pixel)] <= 0.0F && usable(patch.destination[static_cast<std::size_t>(pixel)])) ++contextCount;
            neighbors(pixel, patch.width, patch.height, [&](int adjacent) {
                if (indices[static_cast<std::size_t>(adjacent)] != -1) return;
                indices[static_cast<std::size_t>(adjacent)] = static_cast<int>(queue.size());
                queue.push_back(adjacent);
            });
        }
        ++diagnostics.components;
        diagnostics.contextSamples += contextCount;
        auto fallback = [&] {
            ++diagnostics.fallbackComponents;
            diagnostics.fallbackPixels += componentWrites;
            for (const int pixel : queue)
                if (distance[static_cast<std::size_t>(pixel)] == 0) result.pixels[static_cast<std::size_t>(pixel)] = sanitized(patch.source[static_cast<std::size_t>(pixel)]);
        };
        if (contextCount < 3) { fallback(); continue; }
        const double centerX = (double(minX) + maxX) * 0.5;
        const double centerY = (double(minY) + maxY) * 0.5;
        const double scale = std::max(1.0, double(std::max(maxX - minX, maxY - minY)) * 0.5);
        const std::size_t sampleStride = std::max(std::size_t(1), (contextCount + fitSampleLimit - 1) / fitSampleLimit);
        std::vector<Sample> samples;
        samples.reserve(std::min(contextCount, fitSampleLimit));
        std::size_t ordinal = 0;
        std::size_t visited = 0;
        for (const int pixel : queue) {
            if ((visited++ & 16383U) == 0 && cancel()) return result;
            if (distance[static_cast<std::size_t>(pixel)] == 0 || !(patch.coverage[static_cast<std::size_t>(pixel)] <= 0.0F) || !usable(patch.destination[static_cast<std::size_t>(pixel)])) continue;
            if (ordinal++ % sampleStride != 0) continue;
            samples.push_back({{1.0, (pixel % patch.width - centerX) / scale,
                (pixel / patch.width - centerY) / scale}, difference(patch.destination[static_cast<std::size_t>(pixel)], patch.source[static_cast<std::size_t>(pixel)]),
                std::min(patch.destination[static_cast<std::size_t>(pixel)][3], patch.source[static_cast<std::size_t>(pixel)][3])});
        }
        Plane plane {};
        double cutoff = 0.025;
        bool fitCancelled = false;
        if (!fitPlane(samples, plane, cutoff, options, fitCancelled)) {
            if (fitCancelled) {
                result.status = HealingStatus::Cancelled;
                result.pixels.clear();
                diagnostics.message = "Heal cancelled; no result was published.";
                return result;
            }
            if (cancel()) return result;
            fallback(); continue;
        }
        if (cancel()) return result;
        SolverBuffers buffers(queue.size());
        std::size_t accepted = 0;
        for (std::size_t i = 0; i < queue.size(); ++i) {
            if ((i & 16383U) == 0 && cancel()) return result;
            const int pixel = queue[i];
            int degree = 0;
            neighbors(pixel, patch.width, patch.height, [&](int adjacent) { if (indices[static_cast<std::size_t>(adjacent)] >= 0) ++degree; });
            if (distance[static_cast<std::size_t>(pixel)] > 0 && patch.coverage[static_cast<std::size_t>(pixel)] <= 0.0F && usable(patch.destination[static_cast<std::size_t>(pixel)])) {
                const Triple basis {1.0, (pixel % patch.width - centerX) / scale,
                    (pixel / patch.width - centerY) / scale};
                const double residual = norm(subtract(difference(patch.destination[static_cast<std::size_t>(pixel)], patch.source[static_cast<std::size_t>(pixel)]),
                    evaluate(plane, basis)));
                const double weight = robustWeight(residual, cutoff);
                if (weight == 0.0) ++diagnostics.rejectedContextSamples;
                else ++accepted;
                buffers.boundary[i] = static_cast<float>(boundaryStrength * weight
                    * std::min(patch.destination[static_cast<std::size_t>(pixel)][3], patch.source[static_cast<std::size_t>(pixel)][3]));
            }
            buffers.diagonal[i] = float(degree) + screening + buffers.boundary[i];
        }
        if (accepted < 3) { fallback(); continue; }
        bool converged = true;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const auto status = solveChannel(patch, options, queue, indices, plane,
                centerX, centerY, scale, channel, buffers, diagnostics);
            if (status == SolveStatus::Cancelled || cancel()) {
                result.status = HealingStatus::Cancelled;
                result.pixels.clear();
                diagnostics.message = "Heal cancelled; no result was published.";
                return result;
            }
            if (status != SolveStatus::Converged) { converged = false; break; }
            for (std::size_t i = 0; i < queue.size(); ++i) {
                if ((i & 16383U) == 0 && cancel()) return result;
                const int pixel = queue[i];
                if (distance[static_cast<std::size_t>(pixel)] != 0) continue;
                const Triple basis {1.0, (pixel % patch.width - centerX) / scale,
                    (pixel / patch.width - centerY) / scale};
                const auto source = sanitized(patch.source[static_cast<std::size_t>(pixel)]);
                const double correction = evaluate(plane, basis)[channel] + buffers.x[i];
                result.pixels[static_cast<std::size_t>(pixel)][channel] = static_cast<float>(std::clamp(double(source[channel]) / source[3]
                    + adaptation * correction, 0.0, 1.0) * source[3]);
            }
        }
        if (!converged) {
            ++diagnostics.unconvergedComponents;
            fallback();
        } else {
            ++diagnostics.healedComponents;
            diagnostics.healedPixels += componentWrites;
        }
    }
    if (cancel()) return result;
    if (diagnostics.fallbackComponents == 0) {
        result.status = HealingStatus::Converged;
        diagnostics.message = "Heal converged using surrounding image context.";
    } else {
        result.status = diagnostics.healedComponents ? HealingStatus::PartialFallback : HealingStatus::StampFallback;
        diagnostics.message = "Heal used Stamp fallback for " + std::to_string(diagnostics.fallbackPixels)
            + " pixels: insufficient surrounding context or solver convergence.";
    }
    return result;
}

} // namespace

HealingResult healPatch(const HealingPatch& patch, const HealingOptions& options)
{
    try {
        return run(patch, options);
    } catch (const std::bad_alloc&) {
        HealingResult result;
        result.status = HealingStatus::LimitExceeded;
        result.diagnostics.message = "Heal could not allocate its bounded scratch buffers; use smaller strokes or Stamp.";
        return result;
    }
}

} // namespace imageeditor::core
