#include "imageeditor/ui/TextClipboard.hpp"
#include "imageeditor/core/RichText.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>
namespace imageeditor::ui {
namespace {
QJsonObject style(const core::TextStyle& s)
{
    return { { "family", QString::fromStdString(s.font.family) },
        { "style", QString::fromStdString(s.font.style) }, { "weight", s.font.weight },
        { "originalFace", QString::fromStdString(s.font.originalFace) },
        { "italic", s.font.italic }, { "size", s.sizePixels },
        { "rgba", QJsonArray { s.color.red, s.color.green, s.color.blue, s.color.alpha } } };
}
core::TextStyle readStyle(const QJsonObject& o)
{
    if (!o["family"].isString() || !o["style"].isString() || !o["weight"].isDouble()
        || o["weight"].toDouble() != o["weight"].toInt() || !o["italic"].isBool()
        || !o["size"].isDouble() || !o["rgba"].isArray())
        throw std::invalid_argument("Invalid clipboard format");
    const auto c = o["rgba"].toArray();
    if (c.size() != 4)
        throw std::invalid_argument("Invalid clipboard color");
    for (const auto& v : c)
        if (!v.isDouble() || v.toInt(-1) < 0 || v.toInt(-1) > 255 || v.toDouble() != v.toInt())
            throw std::invalid_argument("Invalid clipboard color");
    core::TextStyle s;
    s.font = { o["family"].toString().toStdString(), o["style"].toString().toStdString(),
        o["weight"].toInt(), o["italic"].toBool() };
    s.sizePixels = o["size"].toDouble();
    if(o.contains("originalFace") && (!o["originalFace"].isString() || o["originalFace"].toString().size()>1024))
        throw std::invalid_argument("Invalid original font face");
    s.font.originalFace=o["originalFace"].toString().toStdString();
    s.color = { std::uint8_t(c[0].toInt()), std::uint8_t(c[1].toInt()), std::uint8_t(c[2].toInt()),
        std::uint8_t(c[3].toInt()) };
    return s;
}
std::size_t offset(const QJsonValue& value)
{
    const auto v = value.toDouble(-1);
    if (!std::isfinite(v) || v < 0 || v > core::kMaximumTextBytes || std::floor(v) != v)
        throw std::invalid_argument("Invalid clipboard offset");
    return std::size_t(v);
}
}
QByteArray encodeTextClipboard(const core::TextLayer& text)
{
    const auto t = core::normalizedText(text);
    QJsonArray runs, paragraphs;
    for (const auto& r : t.runs)
        runs.append(QJsonObject { { "start", qint64(r.start) }, { "length", qint64(r.length) },
            { "format", style(r.style) } });
    for (const auto& p : t.paragraphs)
        paragraphs.append(
            QJsonObject { { "start", qint64(p.start) }, { "alignment", int(p.alignment) } });
    return QJsonDocument(
        QJsonObject { { "version", 1 },
            { "text", QString::fromUtf8(t.utf8.data(), qsizetype(t.utf8.size())) },
            { "default", style(t.defaultStyle) }, { "runs", runs }, { "paragraphs", paragraphs } })
        .toJson(QJsonDocument::Compact);
}
std::optional<core::TextLayer> decodeTextClipboard(const QByteArray& bytes)
{
    if (bytes.size() > 8 * 1024 * 1024)
        return { };
    try {
        const auto json = QJsonDocument::fromJson(bytes);
        if (!json.isObject())
            return { };
        const auto o = json.object();
        if (o["version"].toInt() != 1)
            return { };
        if (!o["text"].isString() || !o["default"].isObject() || !o["runs"].isArray()
            || !o["paragraphs"].isArray())
            return { };
        core::TextLayer t;
        t.utf8 = o["text"].toString().toUtf8().toStdString();
        t.defaultStyle = readStyle(o["default"].toObject());
        const auto runs = o["runs"].toArray();
        if (runs.size() > qsizetype(core::kMaximumTextRuns))
            return { };
        for (const auto& value : runs) {
            const auto r = value.toObject();
            t.runs.push_back(
                { offset(r["start"]), offset(r["length"]), readStyle(r["format"].toObject()) });
        }
        t.paragraphs.clear();
        const auto paragraphs = o["paragraphs"].toArray();
        if (paragraphs.size() > 4096)
            return { };
        for (const auto& value : paragraphs) {
            const auto p = value.toObject();
            t.paragraphs.push_back(
                { offset(p["start"]), static_cast<core::TextAlignment>(p["alignment"].toInt(-1)) });
        }
        return core::normalizedText(std::move(t));
    } catch (const std::exception&) {
        return { };
    }
}
}
