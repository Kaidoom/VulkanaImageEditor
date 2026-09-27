#pragma once
#include "imageeditor/core/Geometry.hpp"
#include <QJsonArray>
#include <stdexcept>
namespace imageeditor::ui::detail
{
inline QJsonArray encodeTransform(const core::ProjectiveTransform &t)
{
    QJsonArray result{t.m00, t.m01, t.m02, t.m10, t.m11, t.m12};
    if (!t.isAffine()) {
        result.append(t.m20);
        result.append(t.m21);
        result.append(t.m22);
    }
    return result;
}
inline core::ProjectiveTransform decodeTransform(const QJsonValue &value)
{
    const auto a = value.toArray();
    if (!value.isArray() || (a.size() != 6 && a.size() != 9))
        throw std::runtime_error("Invalid transform dimensions");
    std::array<double, 9> c{0, 0, 0, 0, 0, 0, 0, 0, 1};
    for (qsizetype i = 0; i < a.size(); ++i) {
        if (!a[i].isDouble() || !std::isfinite(a[i].toDouble()) || std::abs(a[i].toDouble()) > 1e12)
            throw std::runtime_error("Invalid transform coefficient");
        c[std::size_t(i)] = a[i].toDouble();
    }
    core::ProjectiveTransform t{c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8]};
    if (!t.inverted())
        throw std::runtime_error("Singular transform");
    return t;
}
} // namespace imageeditor::ui::detail
