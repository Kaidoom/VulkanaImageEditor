#include "imageeditor/ui/ScalarClipboard.hpp"
#include <QDataStream>
#include <QIODevice>
#include <cstring>

namespace imageeditor::ui {
namespace {
constexpr quint32 magic = 0x56534348, version = 1;
constexpr qsizetype headerBytes = 4 * 4 + 9 * 8;
constexpr quint64 maximumPixels = 40ULL * 1024 * 1024;
void configure(QDataStream& stream)
{
    stream.setVersion(QDataStream::Qt_6_0);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
}
}
QByteArray encodeScalarClipboard(const core::ScalarFragment& fragment)
{
    if (!core::validScalarFragment(fragment)) return {};
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly); configure(stream);
    stream << magic << version << quint32(fragment.extent.width) << quint32(fragment.extent.height);
    const auto& t = fragment.pixelsToDocument;
    stream << t.m00 << t.m01 << t.m02 << t.m10 << t.m11 << t.m12 << t.m20 << t.m21 << t.m22;
    if (stream.status() != QDataStream::Ok || bytes.size() != headerBytes) return {};
    bytes.append(reinterpret_cast<const char*>(fragment.values.data()), qsizetype(fragment.values.size()));
    bytes.append(reinterpret_cast<const char*>(fragment.coverage.data()), qsizetype(fragment.coverage.size()));
    return bytes;
}
std::optional<core::ScalarFragment> decodeScalarClipboard(const QByteArray& bytes)
{
    if (bytes.size() < headerBytes || quint64(bytes.size() - headerBytes) > 2 * maximumPixels) return {};
    QDataStream stream(bytes); configure(stream);
    quint32 tag{}, schema{}, width{}, height{};
    stream >> tag >> schema >> width >> height;
    const quint64 pixels = quint64(width) * height;
    if (tag != magic || schema != version || !width || !height || width > 16384 || height > 16384
        || pixels > maximumPixels || quint64(bytes.size() - headerBytes) != pixels * 2) return {};
    try {
        core::ScalarFragment fragment; fragment.extent = {width, height};
        auto& t = fragment.pixelsToDocument;
        stream >> t.m00 >> t.m01 >> t.m02 >> t.m10 >> t.m11 >> t.m12 >> t.m20 >> t.m21 >> t.m22;
        if (stream.status() != QDataStream::Ok || !t.inverted()
            || !t.validOver({0, 0, double(width), double(height)})) return {};
        fragment.values.resize(std::size_t(pixels)); fragment.coverage.resize(std::size_t(pixels));
        std::memcpy(fragment.values.data(), bytes.constData() + headerBytes, std::size_t(pixels));
        std::memcpy(fragment.coverage.data(), bytes.constData() + headerBytes + qsizetype(pixels), std::size_t(pixels));
        return core::validScalarFragment(fragment) ? std::optional(std::move(fragment)) : std::nullopt;
    } catch (const std::exception&) { return {}; }
}
}
