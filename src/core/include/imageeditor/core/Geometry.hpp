#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <numbers>
#include <limits>

namespace imageeditor::core {

struct Extent2u {
    std::uint32_t width {0};
    std::uint32_t height {0};

    [[nodiscard]] constexpr bool empty() const noexcept
    {
        return width == 0 || height == 0;
    }

    friend constexpr bool operator==(const Extent2u&, const Extent2u&) = default;
};

struct Extent2d {
    double width {0.0};
    double height {0.0};
};

struct Vec2d {
    double x {0.0};
    double y {0.0};

    friend constexpr bool operator==(const Vec2d&, const Vec2d&) = default;
};

constexpr Vec2d operator+(Vec2d lhs, Vec2d rhs) noexcept
{
    return {lhs.x + rhs.x, lhs.y + rhs.y};
}

constexpr Vec2d operator-(Vec2d lhs, Vec2d rhs) noexcept
{
    return {lhs.x - rhs.x, lhs.y - rhs.y};
}

constexpr Vec2d operator*(Vec2d value, double scale) noexcept
{
    return {value.x * scale, value.y * scale};
}

// Document-axis 45° line constraint, shared by line creation and measuring.
// Preserve the pointer's radial distance; only its direction is snapped.
inline Vec2d constrainLineEndpoint(Vec2d anchor, Vec2d pointer) noexcept
{
    const auto delta = pointer - anchor;
    const auto length = std::hypot(delta.x, delta.y);
    if (!std::isfinite(length)) return pointer;
    constexpr double increment = std::numbers::pi / 4;
    const auto angle = std::round(std::atan2(delta.y, delta.x) / increment) * increment;
    auto snapped = Vec2d {std::cos(angle) * length, std::sin(angle) * length};
    if (std::abs(snapped.x) < 1e-9) snapped.x = 0;
    if (std::abs(snapped.y) < 1e-9) snapped.y = 0;
    return anchor + snapped;
}

struct RectI {
    std::int32_t x {0};
    std::int32_t y {0};
    std::int32_t width {0};
    std::int32_t height {0};

    [[nodiscard]] constexpr bool empty() const noexcept
    {
        return width <= 0 || height <= 0;
    }

    [[nodiscard]] constexpr std::int32_t right() const noexcept { return x + width; }
    [[nodiscard]] constexpr std::int32_t bottom() const noexcept { return y + height; }

    [[nodiscard]] constexpr RectI clippedTo(RectI bounds) const noexcept
    {
        const auto left = std::max(x, bounds.x);
        const auto top = std::max(y, bounds.y);
        const auto clippedRight = std::min(right(), bounds.right());
        const auto clippedBottom = std::min(bottom(), bounds.bottom());
        return {left, top, std::max(0, clippedRight - left), std::max(0, clippedBottom - top)};
    }

    [[nodiscard]] constexpr RectI united(RectI other) const noexcept
    {
        if (empty()) {
            return other;
        }
        if (other.empty()) {
            return *this;
        }
        const auto left = std::min(x, other.x);
        const auto top = std::min(y, other.y);
        const auto unitedRight = std::max(right(), other.right());
        const auto unitedBottom = std::max(bottom(), other.bottom());
        return {left, top, unitedRight - left, unitedBottom - top};
    }

    friend constexpr bool operator==(const RectI&, const RectI&) = default;
};

struct RectD {
    double x {0}, y {0}, width {0}, height {0};
    [[nodiscard]] double right() const noexcept { return x + width; }
    [[nodiscard]] double bottom() const noexcept { return y + height; }
    [[nodiscard]] bool empty() const noexcept { return width <= 0 || height <= 0; }
    [[nodiscard]] bool contains(Vec2d p) const noexcept
    { return p.x >= x && p.y >= y && p.x < right() && p.y < bottom(); }
    [[nodiscard]] RectD clippedTo(RectD b) const noexcept
    {
        const auto left=std::max(x,b.x), top=std::max(y,b.y);
        return {left,top,std::max(0.0,std::min(right(),b.right())-left),
            std::max(0.0,std::min(bottom(),b.bottom())-top)};
    }
    friend constexpr bool operator==(const RectD&, const RectD&) = default;
};

// Column-vector homography. The default bottom row keeps the exact, division-
// free affine path. AffineTransform remains a source-compatible alias below.
struct ProjectiveTransform {
    double m00 {1.0};
    double m01 {0.0};
    double m02 {0.0};
    double m10 {0.0};
    double m11 {1.0};
    double m12{0.0};
    double m20{0.0};
    double m21{0.0};
    double m22{1.0};

    [[nodiscard]] constexpr bool isAffine() const noexcept { return m20 == 0 && m21 == 0 && m22 == 1; }
    [[nodiscard]] constexpr double denominator(Vec2d p) const noexcept { return m20 * p.x + m21 * p.y + m22; }

    [[nodiscard]] double maximumScale(Vec2d point = {}) const noexcept
    {
        const auto d = derivatives(point);
        const double norm = d[0].x * d[0].x + d[0].y * d[0].y + d[1].x * d[1].x + d[1].y * d[1].y;
        const double det = d[0].x * d[1].y - d[1].x * d[0].y;
        return std::sqrt(std::max(0.0, .5 * (norm + std::sqrt(std::max(0.0, norm * norm - 4 * det * det)))));
    }
    // Conservative projective density bound: derivative numerators and the
    // homogeneous denominator are linear over the support rectangle.
    [[nodiscard]] double maximumScaleOver(RectD bounds) const noexcept
    {
        if (isAffine())
            return maximumScale();
        if (!validOver(bounds))
            return std::numeric_limits<double>::infinity();
        std::array<double, 4> numerator{};
        double minW = std::numeric_limits<double>::max();
        for (auto p : std::array{Vec2d{bounds.x, bounds.y}, Vec2d{bounds.right(), bounds.y},
                                 Vec2d{bounds.right(), bounds.bottom()}, Vec2d{bounds.x, bounds.bottom()}}) {
            const double w = denominator(p);
            minW = std::min(minW, std::abs(w));
            const double x = m00 * p.x + m01 * p.y + m02, y = m10 * p.x + m11 * p.y + m12;
            const std::array n{m00 * w - x * m20, m10 * w - y * m20, m01 * w - x * m21, m11 * w - y * m21};
            for (std::size_t i = 0; i < 4; ++i)
                numerator[i] = std::max(numerator[i], std::abs(n[i]));
        }
        double norm = 0;
        for (double n : numerator)
            norm += n * n;
        return std::sqrt(norm) / (minW * minW);
    }
    [[nodiscard]] constexpr Vec2d map(Vec2d point) const noexcept
    {
        const Vec2d numerator{
            m00 * point.x + m01 * point.y + m02,
            m10 * point.x + m11 * point.y + m12,
        };
        if (isAffine())
            return numerator;
        const auto w = denominator(point);
        return {numerator.x / w, numerator.y / w};
    }

    [[nodiscard]] std::array<Vec2d, 2> derivatives(Vec2d p) const noexcept
    {
        if (isAffine())
            return {Vec2d{m00, m10}, Vec2d{m01, m11}};
        const auto q = map(p);
        const auto w = denominator(p);
        return {Vec2d{(m00 - q.x * m20) / w, (m10 - q.y * m20) / w},
                Vec2d{(m01 - q.x * m21) / w, (m11 - q.y * m21) / w}};
    }
    // A linear denominator reaches its extrema at the support's corners.
    // Reject a horizon anywhere in the evaluated support, not only at handles.
    [[nodiscard]] bool validOver(RectD support, double limit = 1.0e9) const noexcept
    {
        double smallest = std::numeric_limits<double>::max(), largest = 0;
        double sign = 0;
        for (auto p :
             std::array{Vec2d{support.x, support.y}, Vec2d{support.right(), support.y},
                        Vec2d{support.right(), support.bottom()}, Vec2d{support.x, support.bottom()}}) {
            const auto w = denominator(p);
            const auto q = map(p);
            if (!std::isfinite(w) || w == 0 || !std::isfinite(q.x) || !std::isfinite(q.y) ||
                std::abs(q.x) > limit || std::abs(q.y) > limit ||
                (sign != 0 && std::signbit(w) != std::signbit(sign)))
                return false;
            sign = w;
            smallest = std::min(smallest, std::abs(w));
            largest = std::max(largest, std::abs(w));
        }
        return smallest >= largest * 1.0e-6 && inverted().has_value();
    }

    [[nodiscard]] std::optional<ProjectiveTransform> inverted() const noexcept
    {
        if (!isAffine()) {
            const long double a = m00, b = m01, c = m02, d = m10, e = m11, f = m12, g = m20, h = m21, i = m22;
            const std::array<long double, 9> co{e * i - f * h, c * h - b * i, b * f - c * e,
                                                f * g - d * i, a * i - c * g, c * d - a * f,
                                                d * h - e * g, b * g - a * h, a * e - b * d};
            const auto det = a * co[0] + b * co[3] + c * co[6];
            const auto magnitude = std::abs(a * co[0]) + std::abs(b * co[3]) + std::abs(c * co[6]);
            if (!std::isfinite(det) || det == 0 || std::abs(det) < magnitude * 1.0e-18L)
                return {};
            const auto scale = co[8] != 0 ? co[8] : det;
            std::array<double, 9> v{};
            for (std::size_t n = 0; n < 9; ++n) {
                v[n] = double(co[n] / scale);
                if (!std::isfinite(v[n]))
                    return {};
            }
            return ProjectiveTransform{v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]};
        }
        // Normalize before testing the determinant: a uniformly tiny but
        // well-conditioned layer is still invertible during signed scaling.
        const auto norm = std::max({std::abs(m00), std::abs(m01), std::abs(m10), std::abs(m11)});
        if (!std::isfinite(norm) || norm == 0.0 || !std::isfinite(m02) || !std::isfinite(m12)) {
            return std::nullopt;
        }
        const auto determinant = (m00 / norm) * (m11 / norm) - (m01 / norm) * (m10 / norm);
        if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-12) {
            return std::nullopt;
        }
        const auto inverse = 1.0 / (determinant * norm);
        ProjectiveTransform result{
            .m00 = (m11 / norm) * inverse,
            .m01 = -(m01 / norm) * inverse,
            .m02 = 0.0,
            .m10 = -(m10 / norm) * inverse,
            .m11 = (m00 / norm) * inverse,
            .m12 = 0.0,
        };
        result.m02 = -(result.m00 * m02 + result.m01 * m12);
        result.m12 = -(result.m10 * m02 + result.m11 * m12);
        if (!std::isfinite(result.m00) || !std::isfinite(result.m01) || !std::isfinite(result.m02) ||
            !std::isfinite(result.m10) || !std::isfinite(result.m11) || !std::isfinite(result.m12))
            return std::nullopt;
        return result;
    }
    friend constexpr bool operator==(const ProjectiveTransform &, const ProjectiveTransform &) = default;
};
using AffineTransform = ProjectiveTransform;

[[nodiscard]] inline ProjectiveTransform composeTransform(const ProjectiveTransform &a,
                                                          const ProjectiveTransform &b)
{
    if (a.isAffine() && b.isAffine()) {
        const auto o = a.map({b.m02, b.m12});
        return {a.m00 * b.m00 + a.m01 * b.m10, a.m00 * b.m01 + a.m01 * b.m11, o.x,
                a.m10 * b.m00 + a.m11 * b.m10, a.m10 * b.m01 + a.m11 * b.m11, o.y};
    }
    return {a.m00 * b.m00 + a.m01 * b.m10 + a.m02 * b.m20, a.m00 * b.m01 + a.m01 * b.m11 + a.m02 * b.m21,
            a.m00 * b.m02 + a.m01 * b.m12 + a.m02 * b.m22, a.m10 * b.m00 + a.m11 * b.m10 + a.m12 * b.m20,
            a.m10 * b.m01 + a.m11 * b.m11 + a.m12 * b.m21, a.m10 * b.m02 + a.m11 * b.m12 + a.m12 * b.m22,
            a.m20 * b.m00 + a.m21 * b.m10 + a.m22 * b.m20, a.m20 * b.m01 + a.m21 * b.m11 + a.m22 * b.m21,
            a.m20 * b.m02 + a.m21 * b.m12 + a.m22 * b.m22};
}

// Ordered TL, TR, BR, BL. Both windings are supported; crossed, concave and
// degenerate input is not. No affine fallback on invalid projective geometry.
[[nodiscard]] inline std::optional<ProjectiveTransform> rectangleToQuad(RectD source,
                                                                        const std::array<Vec2d, 4> &q)
{
    if (source.empty())
        return {};
    double edgeScale = 0, orientation = 0;
    for (std::size_t j = 0; j < 4; ++j) {
        const auto e = q[(j + 1) % 4] - q[j];
        edgeScale = std::max(edgeScale, e.x * e.x + e.y * e.y);
    }
    if (!std::isfinite(edgeScale) || edgeScale == 0)
        return {};
    for (std::size_t j = 0; j < 4; ++j) {
        const auto a = q[(j + 1) % 4] - q[j], b = q[(j + 2) % 4] - q[(j + 1) % 4];
        const auto cross = a.x * b.y - a.y * b.x;
        if (!std::isfinite(cross) || std::abs(cross) < edgeScale * 1.0e-8 ||
            (orientation != 0 && std::signbit(cross) != std::signbit(orientation)))
            return {};
        orientation = cross;
    }
    const auto d1 = q[1] - q[2], d2 = q[3] - q[2], d3 = q[0] - q[1] + q[2] - q[3];
    double g = 0, h = 0;
    if (d3.x != 0 || d3.y != 0) {
        const auto det = d1.x * d2.y - d2.x * d1.y;
        if (!std::isfinite(det) || std::abs(det) < edgeScale * 1.0e-12)
            return {};
        g = (d3.x * d2.y - d2.x * d3.y) / det;
        h = (d1.x * d3.y - d3.x * d1.y) / det;
    }
    const ProjectiveTransform unit{q[1].x - q[0].x + g * q[1].x,
                                   q[3].x - q[0].x + h * q[3].x,
                                   q[0].x,
                                   q[1].y - q[0].y + g * q[1].y,
                                   q[3].y - q[0].y + h * q[3].y,
                                   q[0].y,
                                   g,
                                   h,
                                   1};
    const auto result = composeTransform(unit, {1 / source.width, 0, -source.x / source.width, 0,
                                                1 / source.height, -source.y / source.height});
    return result.validOver(source) ? std::optional(result) : std::nullopt;
}

struct Rgba8 {
    std::uint8_t red{0};
    std::uint8_t green{0};
    std::uint8_t blue {0};
    std::uint8_t alpha {0};

    friend constexpr bool operator==(const Rgba8&, const Rgba8&) = default;
};

} // namespace imageeditor::core
