#include "PsdWriter.hpp"
#include <QColorSpace>
#include <QtEndian>
#include <algorithm>
#include <bit>
#include <limits>

namespace imageeditor::ui::psdwrite {
void require(bool ok, const char *s) {
  if (!ok)
    throw Error(s);
}
void check(const std::atomic_bool &c) {
  if (c)
    throw Cancelled{};
}
void u16(QByteArray &b, int n) {
  b += char((n >> 8) & 255);
  b += char(n & 255);
}
void u32(QByteArray &b, quint32 n) {
  u16(b, int(n >> 16));
  u16(b, int(n & 65535));
}
void f64(QByteArray &b, double n) {
  require(std::isfinite(n), "Non-finite PSD descriptor value");
  auto bits = std::bit_cast<quint64>(n);
  for (int i = 7; i >= 0; --i)
    b += char((bits >> (i * 8)) & 255);
}
void unicode(QByteArray &b, const QString &s) {
  require(s.size() <= 1024 * 1024, "PSD string exceeds the writer limit");
  u32(b, quint32(s.size()));
  for (auto c : s)
    u16(b, c.unicode());
}
QVariantMap enumeration(const char *t, const char *v) {
  return {{"$enum", v}, {"$type", t}};
}
QVariantMap unit(double n, const char *u) {
  return {{"$unit", u}, {"$value", n}};
}
QVariantMap rgb(core::Rgba8 c) {
  return {{"$class", "RGBC"},
          {"Rd  ", double(c.red)},
          {"Grn ", double(c.green)},
          {"Bl  ", double(c.blue)}};
}
namespace {
void key(QByteArray &b, QByteArray k) {
  u32(b, k.size() == 4 ? 0 : quint32(k.size()));
  b += k;
}
void value(QByteArray &, const QVariant &, int);
void object(QByteArray &b, const QVariantMap &m, int depth) {
  require(depth < 48 && m.size() < 32768, "PSD descriptor complexity limit");
  unicode(b, m.value("$name").toString());
  key(b, m.value("$class", "null").toByteArray());
  int count = 0;
  for (auto i = m.begin(); i != m.end(); ++i)
    if (!i.key().startsWith('$'))
      ++count;
  u32(b, quint32(count));
  for (auto i = m.begin(); i != m.end(); ++i)
    if (!i.key().startsWith('$')) {
      key(b, i.key().toLatin1());
      value(b, i.value(), depth + 1);
    }
}
void value(QByteArray &b, const QVariant &v, int depth) {
  require(depth < 48, "PSD descriptor nesting limit");
  switch (v.metaType().id()) {
  case QMetaType::QVariantMap: {
    auto m = v.toMap();
    if (m.contains("$enum")) {
      b += "enum";
      key(b, m["$type"].toByteArray());
      key(b, m["$enum"].toByteArray());
    } else if (m.contains("$unit")) {
      b += "UntF";
      b += m["$unit"].toByteArray();
      f64(b, m["$value"].toDouble());
    } else {
      b += "Objc";
      object(b, m, depth + 1);
    }
    break;
  }
  case QMetaType::QVariantList: {
    auto list = v.toList();
    require(list.size() < 32768, "PSD descriptor list limit");
    b += "VlLs";
    u32(b, quint32(list.size()));
    for (auto &x : list)
      value(b, x, depth + 1);
    break;
  }
  case QMetaType::QString:
    b += "TEXT";
    unicode(b, v.toString());
    break;
  case QMetaType::QByteArray: {
    auto data = v.toByteArray();
    b += "tdta";
    u32(b, quint32(data.size()));
    b += data;
    break;
  }
  case QMetaType::Bool:
    b += "bool";
    b += char(v.toBool());
    break;
  case QMetaType::Int:
    b += "long";
    u32(b, quint32(v.toInt()));
    break;
  default:
    b += "doub";
    f64(b, v.toDouble());
    break;
  }
}
class Output {
  QIODevice &d;
  std::uint64_t limit;

public:
  Output(QIODevice &device, std::uint64_t maximum)
      : d(device), limit(maximum) {}
  qint64 pos() const { return d.pos(); }
  void seek(qint64 n) {
    require(n >= 0 && d.seek(n), "Could not seek PSD output");
  }
  void bytes(QByteArrayView b) {
    require(pos() >= 0 &&
                std::uint64_t(pos()) + std::uint64_t(b.size()) <= limit,
            "Standard PSD file-size limit exceeded; choose fewer pixels or "
            "another format");
    require(d.write(b.data(), b.size()) == b.size(),
            "Could not write PSD data (check disk space)");
  }
  void n16(int n) {
    QByteArray b;
    u16(b, n);
    bytes(b);
  }
  void n32(quint32 n) {
    QByteArray b;
    u32(b, n);
    bytes(b);
  }
  void patch(qint64 at, qint64 n) {
    require(n >= 0 && quint64(n) <= UINT32_MAX, "PSD section length overflow");
    auto p = pos();
    seek(at);
    n32(quint32(n));
    seek(p);
  }
  void pad(int n) {
    while (pos() % n)
      bytes(QByteArrayView("\0", 1));
  }
};
QByteArray pack(QByteArrayView row) {
  QByteArray b;
  b.reserve(row.size() + row.size() / 128 + 1);
  qsizetype x = 0;
  while (x < row.size()) {
    qsizetype run = 1;
    while (run < 128 && x + run < row.size() && row[x + run] == row[x])
      ++run;
    if (run >= 3) {
      b += char(1 - int(run));
      b += row[x];
      x += run;
      continue;
    }
    const auto start = x;
    x += run;
    while (x < row.size() && x - start < 128) {
      run = 1;
      while (run < 3 && x + run < row.size() && row[x + run] == row[x])
        ++run;
      if (run >= 3)
        break;
      x += std::min(run, 128 - (x - start));
    }
    b += char(x - start - 1);
    b += row.sliced(start, x - start);
  }
  return b;
}
void rectangle(QByteArray &b, QRect r) {
  u32(b, quint32(r.y()));
  u32(b, quint32(r.x()));
  u32(b, quint32(qint64(r.y()) + r.height()));
  u32(b, quint32(qint64(r.x()) + r.width()));
}
void tag(QByteArray &b, QByteArray keyName, QByteArray data) {
  // Per-layer additional-info lengths INCLUDE their payload padding. Readers
  // do not all infer an extra pad from the unrounded descriptor length.
  while (data.size() % 4)
    data += '\0';
  b += "8BIM";
  b += keyName;
  u32(b, quint32(data.size()));
  b += data;
}
QByteArray layerRecord(const Record &r, int id) {
  QByteArray b;
  rectangle(b, r.bounds);
  u16(b, int(r.channels.size()));
  for (auto c : r.channels) {
    u16(b, c.id);
    u32(b, c.length);
  }
  b += "8BIM";
  b += r.blend;
  b += char(r.opacity);
  b += char(r.clipping);
  b += char(8 | (r.visible ? 0 : 2) |
            ((r.section || r.tags.contains("vmsk")) ? 16 : 0));
  b += '\0';
  QByteArray extra;
  u32(extra, r.mask ? 20 : 0);
  if (r.mask) {
    rectangle(extra, r.maskBounds);
    extra += char(r.maskOutside);
    extra += char(r.maskEnabled ? 0 : 2);
    u16(extra, 0);
  }
  u32(extra, 0); // Default blending ranges.
  auto name = r.name.toLatin1().left(255);
  extra += char(name.size());
  extra += name;
  while ((name.size() + 1) % 4) {
    extra += '\0';
    name += '\0';
  }
  QByteArray s;
  unicode(s, r.name);
  tag(extra, "luni", s);
  s.clear();
  u32(s, quint32(id));
  tag(extra, "lyid", s);
  s.clear();
  u16(s, r.label);
  s += QByteArray(6, '\0');
  tag(extra, "lclr", s);
  if (r.section) {
    s.clear();
    u32(s, quint32(r.section));
    s += "8BIM";
    s += r.blend;
    tag(extra, "lsct", s);
  }
  for (auto i = r.tags.begin(); i != r.tags.end(); ++i)
    tag(extra, i.key(), i.value());
  u32(b, quint32(extra.size()));
  return b + extra;
}
void resource(QByteArray &b, int id, const QByteArray &data) {
  b += "8BIM";
  u16(b, id);
  u16(b, 0);
  u32(b, quint32(data.size()));
  b += data;
  if (data.size() % 2)
    b += '\0';
}
void copy(Output &out, QIODevice &from, qint64 start, qint64 count,
          const std::atomic_bool &cancel) {
  require(from.seek(start), "Could not read PSD spool");
  while (count) {
    check(cancel);
    auto b = from.read(std::min<qint64>(count, 1024 * 1024));
    require(!b.isEmpty(), "Truncated PSD spool");
    out.bytes(b);
    count -= b.size();
  }
}
} // namespace
QByteArray descriptor(const QVariantMap &m, const char *cls) {
  auto data = m;
  data["$class"] = cls;
  QByteArray b;
  u32(b, 16);
  object(b, data, 0);
  return b;
}
QByteArray engine(const QVariant &v) {
  if (v.metaType().id() == QMetaType::QVariantMap) {
    QByteArray b = "<<\n";
    auto m = v.toMap();
    for (auto i = m.begin(); i != m.end(); ++i)
      b += "/" + i.key().toLatin1() + " " + engine(i.value()) + "\n";
    return b + ">>";
  }
  if (v.metaType().id() == QMetaType::QVariantList) {
    QByteArray b = "[ ";
    for (auto &x : v.toList())
      b += engine(x) + " ";
    return b + "]";
  }
  if (v.metaType().id() == QMetaType::QString) {
    QByteArray raw = QByteArray::fromHex("feff"), b = "(";
    for (auto c : v.toString())
      u16(raw, c.unicode());
    for (char c : raw) {
      if (c == '(' || c == ')' || c == '\\')
        b += '\\';
      b += c;
    }
    return b + ")";
  }
  if (v.metaType().id() == QMetaType::Bool)
    return v.toBool() ? "true" : "false";
  // EngineData accepts decimal tokens, not C-style scientific notation.
  auto s = QByteArray::number(v.toDouble(), 'f', 8);
  while (s.endsWith('0'))
    s.chop(1);
  if (s.endsWith('.'))
    s.chop(1);
  return s;
}
QByteArray blend(core::BlendMode b, bool desc) {
  using B = core::BlendMode;
  struct Mapping {
    B b;
    const char *key, *descriptor;
  };
  static constexpr Mapping mappings[] = {
      {B::Normal, "norm", "Nrml"},
      {B::Multiply, "mul ", "Mltp"},
      {B::Screen, "scrn", "Scrn"},
      {B::Overlay, "over", "Ovrl"},
      {B::SoftLight, "sLit", "SftL"},
      {B::HardLight, "hLit", "HrdL"},
      {B::Darken, "dark", "Drkn"},
      {B::Lighten, "lite", "Lghn"},
      {B::Difference, "diff", "Dfrn"},
      {B::Exclusion, "smud", "Xclu"},
      {B::Hue, "hue ", "H   "},
      {B::Saturation, "sat ", "Strt"},
      {B::Color, "colr", "Clr "},
      {B::Luminosity, "lum ", "Lmns"},
      {B::ColorDodge, "div ", "CDdg"},
      {B::LinearDodge, "lddg", "linearDodge"},
      {B::ColorBurn, "idiv", "CBrn"},
      {B::LinearBurn, "lbrn", "linearBurn"},
      {B::Subtract, "fsub", "blendSubtraction"},
      {B::Divide, "fdiv", "blendDivide"}};
  for (auto m : mappings)
    if (m.b == b)
      return desc ? m.descriptor : m.key;
  throw Error("Unsupported PSD blend mode");
}
Channel spoolChannel(QIODevice &device, int id, const QImage &image,
                     int component, const std::atomic_bool &cancel,
                     std::uint64_t limit) {
  Output out(device, limit);
  Channel c{id, out.pos(), 0};
  require(image.width() <= 30000 && image.height() <= 30000,
          "PSD layer/channel exceeds 30000 pixels");
  out.n16(image.isNull() ? 0 : 1);
  if (!image.isNull()) {
    const auto table = out.pos();
    out.bytes(QByteArray(image.height() * 2, '\0'));
    QByteArray lengths, row(image.width(), Qt::Uninitialized);
    lengths.reserve(image.height() * 2);
    for (int y = 0; y < image.height(); ++y) {
      check(cancel);
      auto p = image.constScanLine(y);
      for (int x = 0; x < image.width(); ++x)
        row[x] = char(p[x * (component < 0 ? 1 : 4) + std::max(0, component)]);
      auto encoded = pack(row);
      require(encoded.size() <= 65535, "PSD RLE row length overflow");
      u16(lengths, int(encoded.size()));
      out.bytes(encoded);
    }
    auto end = out.pos();
    out.seek(table);
    out.bytes(lengths);
    out.seek(end);
  }
  require(out.pos() - c.offset <= UINT32_MAX, "PSD channel length overflow");
  c.length = quint32(out.pos() - c.offset);
  return c;
}
void write(QIODevice &device, QIODevice &spool,
           const std::vector<Record> &records, const QImage &composite,
           core::CanvasSpec canvas, const std::atomic_bool &cancel,
           std::uint64_t limit) {
  require(!records.empty() && records.size() <= 32767,
          "PSD layer record count outside standard limits");
  Output out(device, limit);
  out.bytes("8BPS");
  out.n16(1);
  out.bytes(QByteArray(6, '\0'));
  out.n16(4);
  out.n32(canvas.extent.height);
  out.n32(canvas.extent.width);
  out.n16(8);
  out.n16(3);
  out.n32(0);
  QByteArray resources, r;
  auto fixed = std::llround(canvas.dotsPerInch * 65536);
  require(fixed > 0 && fixed <= UINT32_MAX,
          "PSD resolution exceeds 16.16 limits");
  u32(r, quint32(fixed));
  u16(r, 1);
  u16(r, 1);
  u32(r, quint32(fixed));
  u16(r, 1);
  u16(r, 1);
  resource(resources, 1005, r);
  resource(resources, 1039, QColorSpace(QColorSpace::SRgb).iccProfile());
  r.clear();
  u32(r, 1);
  r += char(1);
  unicode(r, "Vulkana");
  unicode(r, "Vulkana PSD writer V1");
  u32(r, 1);
  resource(resources, 1057, r);
  out.n32(quint32(resources.size()));
  out.bytes(resources);
  auto outer = out.pos();
  out.n32(0);
  auto info = out.pos();
  out.n32(0);
  out.n16(-int(records.size()));
  int id = 1;
  for (auto &record : records) {
    check(cancel);
    out.bytes(layerRecord(record, id++));
  }
  for (auto &record : records)
    for (auto c : record.channels)
      copy(out, spool, c.offset, c.length, cancel);
  out.pad(2);
  out.patch(info, out.pos() - info - 4);
  out.n32(0);
  out.patch(outer, out.pos() - outer - 4);
  // The merged transparency channel is white-matted in encoded RGB. Layer
  // channels above remain straight, including hidden RGB at alpha zero.
  out.n16(1);
  auto table = out.pos();
  out.bytes(QByteArray(composite.height() * 8, '\0'));
  QByteArray lengths, row(composite.width(), Qt::Uninitialized);
  for (int c = 0; c < 4; ++c)
    for (int y = 0; y < composite.height(); ++y) {
      check(cancel);
      auto p = composite.constScanLine(y);
      for (int x = 0; x < composite.width(); ++x) {
        int a = p[x * 4 + 3], v = p[x * 4 + c];
        if (c < 3)
          v = (v * a + 255 * (255 - a) + 127) / 255;
        row[x] = char(v);
      }
      auto encoded = pack(row);
      require(encoded.size() <= 65535, "PSD composite row length overflow");
      u16(lengths, int(encoded.size()));
      out.bytes(encoded);
    }
  auto end = out.pos();
  out.seek(table);
  out.bytes(lengths);
  out.seek(end);
}
void verify(QIODevice &d, core::CanvasSpec canvas, int records) {
  require(d.seek(0), "Cannot verify completed PSD");
  const auto take = [&](qint64 n) {
    auto b = d.read(n);
    require(b.size() == n, "Truncated completed PSD");
    return b;
  };
  const auto n16 = [&] {
    auto b = take(2);
    return qFromBigEndian<quint16>(b.constData());
  };
  const auto n32 = [&] {
    auto b = take(4);
    return qFromBigEndian<quint32>(b.constData());
  };
  const auto skip = [&](quint64 n) {
    require(n <= quint64(d.size() - d.pos()) && d.seek(d.pos() + qint64(n)),
            "Invalid completed PSD section length");
  };
  require(take(4) == "8BPS" && n16() == 1, "Invalid completed PSD header");
  skip(6);
  require(n16() == 4 && n32() == canvas.extent.height &&
              n32() == canvas.extent.width && n16() == 8 && n16() == 3,
          "Invalid completed PSD geometry");
  skip(n32());
  skip(n32());
  const auto length = n32();
  const auto end = d.pos() + length;
  const auto layerLength = n32();
  const auto layerEnd = d.pos() + layerLength;
  require(end <= d.size() && layerEnd <= end && qint16(n16()) == -records,
          "Invalid completed PSD layer section");
  quint64 channels = 0;
  for (int i = 0; i < records; ++i) {
    skip(16);
    auto count = n16();
    require(count <= 56, "Invalid channel count");
    for (int c = 0; c < count; ++c) {
      skip(2);
      channels += n32();
    }
    require(take(4) == "8BIM", "Invalid completed PSD layer signature");
    skip(8);
    skip(n32());
  }
  require(channels <= quint64(layerEnd - d.pos()) &&
              layerEnd - d.pos() - qint64(channels) < 2,
          "Invalid completed PSD channel lengths");
  require(d.seek(end) && n16() == 1, "Missing completed PSD composite");
  quint64 bytes = 0;
  for (quint64 i = 0; i < quint64(canvas.extent.height) * 4; ++i)
    bytes += n16();
  require(bytes == quint64(d.size() - d.pos()),
          "Invalid completed PSD composite lengths");
}
} // namespace imageeditor::ui::psdwrite
