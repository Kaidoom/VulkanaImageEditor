#include "PsdReader.hpp"
#include "imageeditor/ui/RasterLimits.hpp"
#include <QCryptographicHash>
#include <QtEndian>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <zlib.h>

namespace imageeditor::ui::psd {
void check(const std::shared_ptr<PsdJob> &j) {
  if (j && j->cancelled)
    throw Cancelled();
}
QByteArrayView Reader::take(qsizetype n) {
  if (n < 0 || n > left())
    throw Error("Truncated PSD section");
  auto result = data.sliced(pos, n);
  pos += n;
  return result;
}
quint8 Reader::u8() { return quint8(take(1)[0]); }
quint16 Reader::u16() { return qFromBigEndian<quint16>(take(2).data()); }
quint32 Reader::u32() { return qFromBigEndian<quint32>(take(4).data()); }
qint32 Reader::i32() { return std::bit_cast<qint32>(u32()); }
double Reader::f64() {
  auto v = std::bit_cast<double>(qFromBigEndian<quint64>(take(8).data()));
  if (!std::isfinite(v))
    throw Error("Non-finite PSD value");
  return v;
}
QByteArray Reader::key() {
  auto n = u32();
  if (n > 4096)
    throw Error("PSD key too long");
  return take(n ? n : 4).toByteArray();
}
QString utf16(QByteArrayView b) {
  if (b.size() % 2 || b.size() > 2 * 1024 * 1024)
    throw Error("Invalid UTF-16 length");
  QString s;
  s.reserve(b.size() / 2);
  for (qsizetype i = 0; i < b.size(); i += 2) {
    const auto u = qFromBigEndian<quint16>(b.data() + i);
    if (QChar::isHighSurrogate(u)) {
      if (i + 3 >= b.size())
        throw Error("Truncated UTF-16 surrogate");
      const auto low = qFromBigEndian<quint16>(b.data() + i + 2);
      if (!QChar::isLowSurrogate(low))
        throw Error("Invalid UTF-16 surrogate");
      s += QChar(u);
      s += QChar(low);
      i += 2;
    } else {
      if (QChar::isLowSurrogate(u))
        throw Error("Unpaired UTF-16 surrogate");
      s += QChar(u);
    }
  }
  return s;
}
QString Reader::unicode() {
  auto n = u32();
  if (n > 1024 * 1024)
    throw Error("PSD string limit");
  return utf16(take(qsizetype(n) * 2));
}
double number(const QVariant &v, double f) {
  return !v.isValid() ? f
         : v.metaType().id() == QMetaType::QVariantMap
             ? v.toMap().value("value", f).toDouble()
             : v.toDouble();
}
QString enumeration(const QVariant &v) {
  return v.toMap().value("enum").toString();
}
namespace {
QVariant value(Reader &r, QByteArray type, int depth, int &nodes);
QVariantMap object(Reader &r, int depth, int &nodes) {
  if (depth > 48 || ++nodes > 131072)
    throw Error("PSD descriptor complexity limit");
  const auto name = r.unicode();
  const auto cls = r.key();
  const auto count = r.u32();
  if (count > 32768)
    throw Error("PSD descriptor count limit");
  QVariantMap result{{"_class", QString::fromLatin1(cls)}, {"_name", name}};
  for (quint32 i = 0; i < count; ++i) {
    auto k = QString::fromLatin1(r.key());
    auto type = r.take(4).toByteArray();
    if (result.contains(k))
      throw Error("Duplicate descriptor key");
    result[k] = value(r, type, depth + 1, nodes);
  }
  return result;
}
QVariant value(Reader &r, QByteArray type, int depth, int &nodes) {
  if (depth > 48 || ++nodes > 131072)
    throw Error("PSD descriptor complexity limit");
  if (type == "Objc" || type == "GlbO")
    return object(r, depth, nodes);
  if (type == "TEXT")
    return r.unicode();
  if (type == "long")
    return r.i32();
  if (type == "comp")
    return qlonglong(
        std::bit_cast<qint64>(qFromBigEndian<quint64>(r.take(8).data())));
  if (type == "doub")
    return r.f64();
  if (type == "bool")
    return r.u8() != 0;
  if (type == "enum") {
    auto cls = r.key();
    auto id = r.key();
    return QVariantMap{{"type", QString::fromLatin1(cls)},
                       {"enum", QString::fromLatin1(id)}};
  }
  if (type == "UntF") {
    auto unit = r.take(4).toByteArray();
    auto v = r.f64();
    return QVariantMap{{"unit", QString::fromLatin1(unit)}, {"value", v}};
  }
  if (type == "tdta" || type == "alis" || type == "Pth ") {
    auto n = r.u32();
    if (n > 16 * 1024 * 1024)
      throw Error("PSD descriptor data limit");
    return r.take(n).toByteArray();
  }
  if (type == "type" || type == "GlbC") {
    auto name = r.unicode();
    return QVariantMap{{"name", name}, {"class", QString::fromLatin1(r.key())}};
  }
  if (type == "VlLs") {
    auto n = r.u32();
    if (n > 32768)
      throw Error("PSD list limit");
    QVariantList list;
    for (quint32 i = 0; i < n; ++i) {
      auto t = r.take(4).toByteArray();
      list.push_back(value(r, t, depth + 1, nodes));
    }
    return list;
  }
  throw Unsupported("Unsupported PSD descriptor value type");
}
class Engine {
  Reader r;
  int nodes{};
  void ws() {
    while (r.left() && (quint8(r.data[r.pos]) <= 32))
      ++r.pos;
  }
  QByteArray token() {
    ws();
    auto first = r.pos;
    while (r.left() && quint8(r.data[r.pos]) > 32 &&
           !QByteArray("[]<>()/").contains(r.data[r.pos]))
      ++r.pos;
    if (r.pos == first)
      throw Error("Invalid text engine token");
    return r.data.sliced(first, r.pos - first).toByteArray();
  }
  QVariant item(int depth) {
    ws();
    if (depth > 48 || ++nodes > 131072 || !r.left())
      throw Error("Text engine complexity/truncation limit");
    const char c = r.data[r.pos];
    if (c == '<') {
      r.skip(1);
      if (r.u8() != '<')
        throw Error("Invalid text engine dictionary");
      QVariantMap m;
      while (true) {
        ws();
        if (!r.left())
          throw Error("Truncated text dictionary");
        if (r.data[r.pos] == '>') {
          r.skip(1);
          if (r.u8() != '>')
            throw Error("Invalid dictionary end");
          return m;
        }
        if (r.u8() != '/')
          throw Error("Invalid text dictionary key");
        auto k = QString::fromLatin1(token());
        if (m.contains(k))
          throw Error("Duplicate text engine key");
        m[k] = item(depth + 1);
      }
    }
    if (c == '[') {
      r.skip(1);
      QVariantList a;
      while (true) {
        ws();
        if (!r.left())
          throw Error("Truncated text array");
        if (r.data[r.pos] == ']') {
          r.skip(1);
          return a;
        }
        a.push_back(item(depth + 1));
      }
    }
    if (c == '(') {
      r.skip(1);
      QByteArray bytes;
      bool done = false;
      while (r.left()) {
        char b = char(r.u8());
        if (b == ')') {
          done = true;
          break;
        }
        if (b == '\\') {
          if (!r.left())
            throw Error("Truncated string escape");
          b = char(r.u8());
        }
        bytes += b;
      }
      if (!done)
        throw Error("Truncated text string");
      if (bytes.isEmpty())
        return QString();
      if (!bytes.startsWith("\xfe\xff"))
        throw Unsupported("Unsupported text string encoding");
      return utf16(QByteArrayView(bytes).sliced(2));
    }
    if (c == '/') {
      r.skip(1);
      return QString::fromLatin1(token());
    }
    const auto t = token();
    if (t == "true")
      return true;
    if (t == "false")
      return false;
    if (t == "null")
      return QVariant();
    bool ok = false;
    double n = t.toDouble(&ok);
    if (!ok || !std::isfinite(n))
      throw Error("Invalid text engine number");
    return n;
  }

public:
  explicit Engine(QByteArrayView b) : r(b) {}
  QVariantMap read() {
    auto result = item(0).toMap();
    ws();
    if (r.left() || result.empty())
      throw Error("Invalid text engine root");
    return result;
  }
};
QRect rect(Reader &r) {
  auto top = r.i32(), left = r.i32(), bottom = r.i32(), right = r.i32();
  if (std::abs(qint64(top)) > 1000000 || std::abs(qint64(left)) > 1000000 ||
      bottom < top || right < left || qint64(bottom) - top > 1000000 ||
      qint64(right) - left > 1000000)
    throw Error("Invalid PSD bounds");
  return QRect(left, top, right - left, bottom - top);
}
void readTags(Reader &r, Record &rec) {
  while (r.left() >= 12) {
    auto sig = r.take(4);
    if (sig == "8B64")
      throw Unsupported("Large additional-info blocks require PSB support");
    if (sig != "8BIM")
      throw Error("Invalid additional layer block signature");
    auto key = r.take(4).toByteArray();
    auto n = r.u32();
    if (n > 64 * 1024 * 1024)
      throw Error("Additional layer block limit");
    auto data = r.take(n);
    if (n % 2)
      r.skip(1);
    // Only retain blocks needed for capability analysis, not embedded files.
    static const QList<QByteArray> retained{
        "luni", "TySh", "vmsk", "vsms", "vstk", "vscg", "SoCo",
        "lfx2", "lmfx", "lrFX", "lsct", "lsdk", "iOpa", "knko",
        "infx", "clbl", "tsly", "lmgm", "vmgm", "shmd", "nvrt", "expA"};
    if (retained.contains(key)) {
      if (rec.tags.contains(key))
        throw Error("Duplicate layer block");
      rec.tags[key] = data.toByteArray();
    }
    static const QList<QByteArray> unsupported{
        "levl", "curv", "brit", "blnc", "hue2", "hue ", "selc", "mixr",
        "grdm", "phfl", "vibA", "blwh", "thrs", "post",
        "CgEd", "GdFl", "PtFl", "SoLd", "SoLE", "PlLd"};
    if (unsupported.contains(key))
      rec.issues << QStringLiteral("Unsupported layer feature: %1")
                        .arg(QString::fromLatin1(key));
  }
  if (r.left()) {
    for (char b : r.take(r.left()))
      if (b)
        throw Error("Invalid layer block padding");
  }
}
} // namespace
QVariantMap descriptor(Reader &r, int depth) {
  int nodes = 0;
  return object(r, depth, nodes);
}
QVariantMap versionedDescriptor(Reader &r) {
  if (r.u32() != 16)
    throw Unsupported("Unsupported descriptor version");
  return descriptor(r);
}
QVariantMap engineData(QByteArrayView b) { return Engine(b).read(); }

QByteArray decodeChannel(QByteArrayView bytes, int width, int height,
                         const std::shared_ptr<PsdJob> &job) {
  if (width < 0 || height < 0 ||
      !rasterExtentWithinLimits(std::uint32_t(std::max(1, width)),
                                std::uint32_t(std::max(1, height))))
    throw Error("PSD channel exceeds raster limits");
  Reader r(bytes);
  auto compression = r.u16();
  const qsizetype count = qsizetype(width) * height;
  if (!count)
    return {};
  QByteArray result(count, Qt::Uninitialized);
  check(job);
  if (compression == 0) {
    auto raw = r.take(count);
    std::memcpy(result.data(), raw.data(), size_t(count));
    if (r.left())
      throw Error("Raw channel length mismatch");
  } else if (compression == 1) {
    std::vector<quint16> lengths;
    lengths.reserve(size_t(height));
    for (int y = 0; y < height; ++y)
      lengths.push_back(r.u16());
    for (int y = 0; y < height; ++y) {
      check(job);
      auto row = r.section(lengths[size_t(y)]);
      int x = 0;
      while (row.left()) {
        int n = row.u8();
        if (n == 128)
          continue;
        const int run = n < 128 ? n + 1 : 257 - n;
        if (run > width - x)
          throw Error("RLE output overruns row");
        if (n < 128) {
          auto literal = row.take(run);
          std::memcpy(result.data() + qsizetype(y) * width + x, literal.data(),
                      size_t(run));
        } else {
          auto b = row.u8();
          std::memset(result.data() + qsizetype(y) * width + x, b, size_t(run));
        }
        x += run;
      }
      if (x != width)
        throw Error("RLE row is incomplete");
    }
    if (r.left())
      throw Error("RLE channel length mismatch");
  } else if (compression == 2 || compression == 3) {
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK)
      throw Error("ZIP decoder unavailable");
    struct End {
      z_stream *p;
      ~End() { inflateEnd(p); }
    } end{&stream};
    auto compressed = r.take(r.left());
    stream.next_in =
        reinterpret_cast<Bytef *>(const_cast<char *>(compressed.data()));
    stream.avail_in = uint(compressed.size());
    int status = Z_OK;
    qsizetype written = 0;
    while (status == Z_OK && written < count) {
      check(job);
      auto n = std::min<qsizetype>(65536, count - written);
      stream.next_out = reinterpret_cast<Bytef *>(result.data() + written);
      stream.avail_out = uint(n);
      status = inflate(&stream, Z_NO_FLUSH);
      written += n - stream.avail_out;
    }
    if (status != Z_STREAM_END || written != count || stream.avail_in)
      throw Error("Invalid ZIP channel size/stream");
    if (compression == 3)
      for (int y = 0; y < height; ++y) {
        check(job);
        auto *row =
            reinterpret_cast<quint8 *>(result.data() + qsizetype(y) * width);
        for (int x = 1; x < width; ++x)
          row[x] = quint8(row[x] + row[x - 1]);
      }
  } else
    throw Unsupported("Unsupported channel compression");
  return result;
}

std::shared_ptr<PsdSource>
parse(QByteArray bytes, const std::shared_ptr<PsdJob> &job, PsdLimits limits) {
  check(job);
  auto s = std::make_shared<PsdSource>();
  s->fingerprint = QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
  s->bytes = std::move(bytes);
  Reader r(s->bytes);
  if (r.take(4) != "8BPS")
    throw Error("Not a PSD file");
  if (r.u16() != 1)
    throw Unsupported("PSB is not supported in PSD Import V1");
  for (char b : r.take(6))
    if (b)
      throw Error("Invalid PSD header reserved bytes");
  s->channels = r.u16();
  auto h = r.u32(), w = r.u32();
  s->depth = r.u16();
  s->mode = r.u16();
  if (s->channels < 3 || s->channels > 56)
    throw Unsupported("Unsupported PSD channel count");
  if (s->depth != 8 || s->mode != 3)
    throw Unsupported("PSD Import V1 supports 8-bit RGB PSD files; no tested "
                      "fallback is available for this depth/color mode");
  if (!rasterExtentWithinLimits(w, h) || w > limits.dimension ||
      h > limits.dimension)
    throw Error("PSD canvas exceeds raster limits");
  s->size = QSize(int(w), int(h));
  r.skip(r.u32());
  auto resources = r.section(r.u32());
  while (resources.left()) {
    check(job);
    if (resources.take(4) != "8BIM")
      throw Error("Invalid resource signature");
    auto id = resources.u16();
    auto n = resources.u8();
    resources.skip(n);
    if ((n + 1) % 2)
      resources.skip(1);
    auto size = resources.u32();
    auto data = resources.section(size);
    if (size % 2)
      resources.skip(1);
    if (id == 1005) {
      double x = double(data.u32()) / 65536;
      data.skip(4);
      double y = double(data.u32()) / 65536;
      if (!std::isfinite(x) || x < 1 || x > 1200 || std::abs(x - y) > 0.01)
        throw Unsupported(
            "Unsupported non-square or out-of-range PSD resolution");
      s->ppi = x;
    }
    if(id==1037)s->globalLightAngle=data.i32();
    if(id==1049)s->globalLightAltitude=data.i32();
    if (id == 1039) {
      s->colorSpace = QColorSpace::fromIccProfile(data.data.toByteArray());
      if (!s->colorSpace.isValid() ||
          s->colorSpace.colorModel() != QColorSpace::ColorModel::Rgb)
        throw Unsupported("Embedded ICC profile cannot be converted to sRGB");
      s->colorDescription = s->colorSpace.description();
    }
    if (id == 1057) {
      if (data.u32() != 1)
        throw Unsupported("Unsupported compatibility resource version");
      s->realComposite = data.u8() != 0;
    }
  }
  if (!s->colorSpace.isValid()) {
    s->colorSpace = QColorSpace(QColorSpace::SRgb);
    s->colorDescription = "Untagged RGB; assumed sRGB";
  }
  const auto sectionSize = r.u32();
  auto allLayers = r.section(sectionSize);
  if (sectionSize) {
    auto info = allLayers.section(allLayers.u32());
    if (info.left()) {
      const auto signedCount = std::bit_cast<qint16>(info.u16());
      const int count = std::abs(int(signedCount));
      s->compositeAlpha = signedCount < 0;
      if (count > 4096)
        throw Error("PSD layer count limit");
      s->records.reserve(size_t(count));
      for (int i = 0; i < count; ++i) {
        check(job);
        Record l;
        l.bounds = rect(info);
        const auto channels = info.u16();
        if (channels > 56)
          throw Error("Layer channel count limit");
        for (int c = 0; c < channels; ++c) {
          int id = std::bit_cast<qint16>(info.u16());
          auto len = info.u32();
          if (len < 2)
            throw Error("Invalid channel length");
          if (std::any_of(l.channels.begin(), l.channels.end(),
                          [id](const auto &x) { return x.id == id; }))
            throw Error("Duplicate layer channel");
          l.channels.push_back({id, 0, len});
        }
        if (info.take(4) != "8BIM")
          throw Error("Invalid layer signature");
        l.blend = info.take(4).toByteArray();
        l.opacity = info.u8();
        l.clipping = info.u8() != 0;
        auto flags = info.u8();
        l.visible = !(flags & 2);
        info.skip(1);
        auto extra = info.section(info.u32());
        auto mask = extra.section(extra.u32());
        if (mask.left()) {
          l.mask.present = true;
          l.mask.bounds = rect(mask);
          l.mask.outside = mask.u8();
          l.mask.flags = mask.u8();
          if (l.mask.flags & 16) {
            auto params = mask.u8();
            if (params & 1)
              l.mask.density = mask.u8();
            if (params & 2)
              l.mask.feather = mask.f64();
            if (params & 4) {
              mask.skip(1);
              l.issues << "Vector mask density is unsupported";
            }
            if (params & 8) {
              mask.skip(8);
              l.issues << "Vector mask feather is unsupported";
            }
          }
          if (mask.left() > 2) {
            l.mask.real = true;
            l.issues << "Combined rendered/user masks require an explicit "
                        "base-pixel or composite choice";
          }
        }
        auto ranges = extra.section(extra.u32());
        if (ranges.left() % 8)
          throw Error("Invalid blending ranges");
        while (ranges.left()) {
          auto src = ranges.u32(), dst = ranges.u32();
          if (src != 0x0000ffff || dst != 0x0000ffff)
            l.blendIf = true;
        }
        auto n = extra.u8();
        l.name = QString::fromLatin1(extra.take(n));
        extra.skip((4 - (n + 1) % 4) % 4);
        readTags(extra, l);
        if (l.tags.contains("luni")) {
          Reader name(l.tags["luni"]);
          l.name = name.unicode();
          while (l.name.endsWith(QChar(0)))
            l.name.chop(1);
        }
        if (l.tags.contains("iOpa")) {
          Reader t(l.tags["iOpa"]);
          l.fill = t.u8();
        }
        if (l.tags.contains("knko")) {
          Reader t(l.tags["knko"]);
          l.knockout = t.u8() != 0;
        }
        const auto sectionKey = l.tags.contains("lsct") ? "lsct" : "lsdk";
        if (l.tags.contains(sectionKey)) {
          Reader t(l.tags[sectionKey]);
          auto sectionType = t.u32();
          if (sectionType > 3)
            throw Unsupported("Unknown layer section kind");
          l.section = int(sectionType);
          if (t.left() >= 8) {
            if (t.take(4) != "8BIM")
              throw Error("Invalid group blend signature");
            l.blend = t.take(4).toByteArray();
          }
        }
        s->records.push_back(std::move(l));
      }
      for (auto &l : s->records)
        for (auto &c : l.channels) {
          check(job);
          auto data = info.take(c.length);
          c.offset = data.data() - s->bytes.constData();
        }
      if (info.left() > 3)
        throw Error("Unexpected bytes after layer channels");
      for (char byte : info.take(info.left()))
        if (byte)
          throw Error("Invalid layer channel padding");
    }
  }
  s->compositeOffset = r.pos;
  if (r.left() < 2)
    s->realComposite = false;
  std::vector<int> parents;
  for (int i = int(s->records.size()) - 1; i >= 0; --i) {
    auto &l = s->records[size_t(i)];
    if (l.section == 3) {
      if (parents.empty())
        throw Error("Unbalanced layer group");
      parents.pop_back();
      continue;
    }
    l.parent = parents.empty() ? -1 : parents.back();
    if (l.section == 1 || l.section == 2) {
      if (parents.size() >= 64)
        throw Error("Group nesting limit");
      parents.push_back(i);
    }
  }
  if (!parents.empty())
    throw Error("Unclosed layer group");
  return s;
}
QImage pixels(const PsdSource &s, const Record &l,
              const std::shared_ptr<PsdJob> &job) {
  if (!rasterExtentWithinLimits(uint(l.bounds.width()),
                                uint(l.bounds.height())))
    throw Error("Saved layer raster has empty/oversized bounds");
  QImage image(l.bounds.size(), QImage::Format_RGBA8888);
  if (image.isNull())
    throw std::bad_alloc();
  image.fill(Qt::transparent);
  bool rgb[3]{};
  bool alpha = false;
  for (const auto &channel : l.channels) {
    if (channel.id < -1 || channel.id > 2)
      continue;
    auto data = decodeChannel(
        QByteArrayView(s.bytes).sliced(channel.offset, channel.length),
        image.width(), image.height(), job);
    const int component = channel.id == -1 ? 3 : channel.id;
    alpha |= component == 3;
    if (component < 3)
      rgb[component] = true;
    for (int y = 0; y < image.height(); ++y) {
      check(job);
      auto *row = image.scanLine(y);
      auto *src = data.constData() + qsizetype(y) * image.width();
      for (int x = 0; x < image.width(); ++x)
        row[x * 4 + component] = quint8(src[x]);
    }
  }
  if (!rgb[0] || !rgb[1] || !rgb[2])
    throw Error("Saved layer RGB channels are unavailable");
  if (!alpha)
    for (int y = 0; y < image.height(); ++y) {
      auto *row = image.scanLine(y);
      for (int x = 0; x < image.width(); ++x)
        row[x * 4 + 3] = 255;
    }
  image.setColorSpace(s.colorSpace);
  return image;
}
QImage composite(const PsdSource &s, const std::shared_ptr<PsdJob> &job) {
  if (!s.realComposite)
    throw Unsupported("No usable saved compatibility composite");
  Reader r(QByteArrayView(s.bytes).sliced(s.compositeOffset));
  auto comp = r.u16();
  const auto w = s.size.width(), h = s.size.height();
  QImage image(s.size, QImage::Format_RGBA8888);
  if (image.isNull())
    throw std::bad_alloc();
  image.fill(Qt::black);
  std::vector<quint16> lengths;
  if (comp == 1)
    for (int i = 0; i < h * s.channels; ++i)
      lengths.push_back(r.u16());
  if (comp > 1)
    throw Unsupported("ZIP compatibility composites are not yet supported; "
                      "layer ZIP channels are supported");
  for (int c = 0; c < s.channels; ++c) {
    check(job);
    QByteArray encoded;
    encoded.append(char(0));
    encoded.append(char(comp));
    if (comp == 0)
      encoded += r.take(qsizetype(w) * h).toByteArray();
    else {
      qsizetype n = 0;
      for (int y = 0; y < h; ++y) {
        auto len = lengths[size_t(c * h + y)];
        encoded += char(len >> 8);
        encoded += char(len & 255);
        n += len;
      }
      encoded += r.take(n).toByteArray();
    }
    if (c > 2 && !(c == 3 && s.compositeAlpha))
      continue;
    auto decoded = decodeChannel(encoded, w, h, job);
    for (int y = 0; y < h; ++y) {
      auto *row = image.scanLine(y);
      for (int x = 0; x < w; ++x)
        row[x * 4 + c] = quint8(decoded[qsizetype(y) * w + x]);
    }
  }
  // A merged transparency channel (negative layer count) uses white-matted
  // encoded RGB. Ordinary layer channels are straight and must NOT enter this
  // path. Recover straight RGB before the existing profile conversion.
  if (s.compositeAlpha && s.channels >= 4) {
    for (int y=0;y<h;++y) {
      check(job);
      auto *row=image.scanLine(y);
      for (int x=0;x<w;++x) {
        const int a=row[x*4+3];
        if(a==0||a==255)continue;
        for(int c=0;c<3;++c)
          row[x*4+c]=std::uint8_t(std::clamp(
              int(std::lround((int(row[x*4+c])+a-255)*255./a)),0,255));
      }
    }
  }
  image.setColorSpace(s.colorSpace);
  return image;
}
} // namespace imageeditor::ui::psd
