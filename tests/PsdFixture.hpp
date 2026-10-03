#pragma once
// Independent test writer for the documented PSD subset. No production parser
// helpers, private artwork or external runtime are used to construct inputs.
#include <QByteArray>
#include <QColorSpace>
#include <QMap>
#include <QRect>
#include <QVariant>
#include <bit>
#include <vector>
namespace psdfixture {
inline void u16(QByteArray &b, int n) {
  b += char((n >> 8) & 255);
  b += char(n & 255);
}
inline void u32(QByteArray &b, int n) {
  u16(b, (n >> 16) & 65535);
  u16(b, n & 65535);
}
inline void f64(QByteArray &b, double n) {
  auto v = std::bit_cast<quint64>(n);
  for (int i = 7; i >= 0; --i)
    b += char((v >> (i * 8)) & 255);
}
inline QByteArray unicode(QString s) {
  QByteArray b;
  for (auto c : s)
    u16(b, c.unicode());
  return b;
}
inline void text(QByteArray &b, QString s) {
  u32(b, int(s.size()));
  b += unicode(s);
}
inline void key(QByteArray &b, QByteArray s) {
  u32(b, int(s.size()));
  b += s;
}
inline QVariantMap enumeration(QString s) { return {{"enum", s}}; }
inline QVariantMap unit(double n, QString u = "#Pxl") {
  return {{"unit", u}, {"value", n}};
}
inline QByteArray object(const QVariantMap &);
inline QByteArray value(QVariant v) {
  QByteArray b;
  switch (v.metaType().id()) {
  case QMetaType::QVariantMap: {
    auto m = v.toMap();
    if (m.contains("enum")) {
      b = "enum";
      key(b, "type");
      key(b, m["enum"].toByteArray());
    } else if (m.contains("unit")) {
      b = "UntF";
      b += m["unit"].toByteArray();
      f64(b, m["value"].toDouble());
    } else {
      b = "Objc";
      b += object(m);
    }
    break;
  }
  case QMetaType::QByteArray:
    b = "tdta";
    u32(b, int(v.toByteArray().size()));
    b += v.toByteArray();
    break;
  case QMetaType::QString:
    b = "TEXT";
    text(b, v.toString());
    break;
  case QMetaType::Bool:
    b = "bool";
    b += char(v.toBool());
    break;
  case QMetaType::QVariantList:
    b = "VlLs";
    u32(b, int(v.toList().size()));
    for (auto x : v.toList())
      b += value(x);
    break;
  default:
    b = "doub";
    f64(b, v.toDouble());
    break;
  }
  return b;
}
inline QByteArray object(const QVariantMap &m) {
  QByteArray b;
  text(b, {});
  key(b, "null");
  u32(b, int(m.size()));
  for (auto it = m.begin(); it != m.end(); ++it) {
    key(b, it.key().toLatin1());
    b += value(it.value());
  }
  return b;
}
inline QByteArray descriptor(const QVariantMap &m) {
  QByteArray b;
  u32(b, 16);
  return b + object(m);
}
inline QByteArray engine(const QVariant &v) {
  if (v.metaType().id() == QMetaType::QVariantMap) {
    QByteArray b = "<< ";
    auto m = v.toMap();
    for (auto it = m.begin(); it != m.end(); ++it)
      b += "/" + it.key().toLatin1() + " " + engine(it.value()) + " ";
    return b + ">>";
  }
  if (v.metaType().id() == QMetaType::QVariantList) {
    QByteArray b = "[ ";
    for (auto x : v.toList())
      b += engine(x) + " ";
    return b + "]";
  }
  if (v.metaType().id() == QMetaType::QString) {
    QByteArray b = "(";
    auto data = QByteArray::fromHex("feff") + unicode(v.toString());
    for (char c : data) {
      if (c == '(' || c == ')' || c == '\\')
        b += '\\';
      b += c;
    }
    return b + ")";
  }
  if (v.metaType().id() == QMetaType::Bool)
    return v.toBool() ? "true" : "false";
  return QByteArray::number(v.toDouble(), 'g', 15);
}
inline QByteArray textTag(QString words,
                          QString face = "Unavailable-fixture-face",
                          QVariantList lengths = {}, bool warp = false,
                          double fontIndex = 0) {
  if (lengths.empty())
    lengths = {double(words.size())};
  QVariantList runs;
  for (qsizetype i = 0; i < lengths.size(); ++i)
    runs << QVariantMap{
        {"StyleSheet",
         QVariantMap{
             {"StyleSheetData",
              QVariantMap{
                  {"FontSize", i ? 24. : 16.},
                  {"FillColor",
                   QVariantMap{{"Type", 1.},
                               {"Values", QVariantList{1., i ? 0. : 1., 0.,
                                                       i ? 1. : 0.}}}}}}}}};
  QVariantMap defaults{
      {"Font", fontIndex},
      {"FontSize", 16.},
      {"FillColor",
       QVariantMap{{"Type", 1.}, {"Values", QVariantList{1., 1., 0., 0.}}}}};
  QVariantMap body{
      {"Editor", QVariantMap{{"Text", words}}},
      {"Rendered",
       QVariantMap{
           {"Shapes", QVariantMap{{"Children", QVariantList{QVariantMap{
                                                   {"ShapeType", 0.}}}}}}}},
      {"StyleRun",
       QVariantMap{{"RunArray", runs}, {"RunLengthArray", lengths}}},
      {"ParagraphRun",
       QVariantMap{{"RunArray",
                    QVariantList{QVariantMap{
                        {"ParagraphSheet",
                         QVariantMap{{"Properties",
                                      QVariantMap{{"Justification", 0.}}}}}}}},
                   {"RunLengthArray", QVariantList{double(words.size())}}}}};
  QVariantMap resources{{"TheNormalStyleSheet", 0.},
                        {"StyleSheetSet", QVariantList{QVariantMap{
                                              {"StyleSheetData", defaults}}}},
                        {"FontSet", QVariantList{QVariantMap{{"Name", face}}}}};
  QByteArray b;
  u16(b, 1);
  for (double n : {1., 0., 0., 1., 12., 40.})
    f64(b, n);
  u16(b, 50);
  b += descriptor(
      {{"Ornt", enumeration("Hrzn")},
       {"EngineData", engine(QVariantMap{{"EngineDict", body},
                                         {"ResourceDict", resources}})}});
  u16(b, 1);
  b += descriptor({{"warpStyle", enumeration(warp ? "warpArc" : "warpNone")}});
  b += QByteArray(16, 0);
  return b;
}
struct Layer {
  QString name = "fixture";
  QRect rect{0, 0, 3, 2};
  QMap<QByteArray, QByteArray> tags;
  QByteArray blend = "norm";
  int opacity = 255;
  bool hidden = false, clipping = false;
  bool pixels = true;
  QMap<int,QByteArray> channelData;
  QByteArray mask;
  QRect maskRect;
  int maskFlags = 0, maskOutside = 255;
};
inline QByteArray group(int type, QByteArray blend = "pass") {
  QByteArray b;
  u32(b, type);
  b += "8BIM";
  return b + blend;
}
inline QByteArray file(std::vector<Layer> layers, QSize canvas = {96, 96},
                       int ppi = 72, QByteArray profile = {},
                       bool composite = true) {
  QByteArray b = "8BPS";
  u16(b, 1);
  b += QByteArray(6, 0);
  u16(b, 3);
  u32(b, canvas.height());
  u32(b, canvas.width());
  u16(b, 8);
  u16(b, 3);
  u32(b, 0);
  QByteArray resources;
  auto resource = [&](int id, QByteArray data) {
    resources += "8BIM";
    u16(resources, id);
    u16(resources, 0);
    u32(resources, int(data.size()));
    resources += data;
    if (data.size() % 2)
      resources += char(0);
  };
  QByteArray res;
  u32(res, ppi * 65536);
  u16(res, 1);
  u16(res, 1);
  u32(res, ppi * 65536);
  u16(res, 1);
  u16(res, 1);
  resource(1005, res);
  if (!profile.isEmpty())
    resource(1039, profile);
  res.clear();
  u32(res, 1);
  res += char(composite);
  resource(1057, res);
  u32(b, int(resources.size()));
  b += resources;
  QByteArray records, channels;
  u16(records, int(layers.size()));
  for (auto &l : layers) {
    u32(records, l.rect.y());
    u32(records, l.rect.x());
    u32(records, l.rect.y() + l.rect.height());
    u32(records, l.rect.x() + l.rect.width());
    u16(records, (l.pixels ? 4 : 0) + (!l.mask.isEmpty() ? 1 : 0));
    if (l.pixels)
      for (int id : {-1, 0, 1, 2}) {
        QByteArray raw;
        u16(raw, 0);
        raw += l.channelData.contains(id) ? l.channelData[id] :
            QByteArray(l.rect.width() * l.rect.height(), char(id == -1  ? 255
                                                              : id == 0 ? 180
                                                              : id == 1 ? 85
                                                                        : 25));
        u16(records, id);
        u32(records, int(raw.size()));
        channels += raw;
      }
    if (!l.mask.isEmpty()) {
      u16(records, -2);
      u32(records, int(l.mask.size() + 2));
      u16(channels, 0);
      channels += l.mask;
    }
    records += "8BIM" + l.blend;
    records += char(l.opacity);
    records += char(l.clipping);
    records += char(l.hidden ? 2 : 0);
    records += char(0);
    QByteArray extra;
    if (!l.mask.isEmpty()) {
      u32(extra, 20);
      u32(extra, l.maskRect.y());
      u32(extra, l.maskRect.x());
      u32(extra, l.maskRect.y() + l.maskRect.height());
      u32(extra, l.maskRect.x() + l.maskRect.width());
      extra += char(l.maskOutside);
      extra += char(l.maskFlags);
      u16(extra, 0);
    } else
      u32(extra, 0);
    u32(extra, 0);
    extra += QByteArray(4, 0);
    QByteArray name;
    text(name, l.name);
    l.tags["luni"] = name;
    for (auto it = l.tags.begin(); it != l.tags.end(); ++it) {
      extra += "8BIM" + it.key();
      u32(extra, int(it.value().size()));
      extra += it.value();
      if (it.value().size() % 2)
        extra += char(0);
    }
    u32(records, int(extra.size()));
    records += extra;
  }
  records += channels;
  if (records.size() % 2)
    records += char(0);
  QByteArray section;
  u32(section, int(records.size()));
  section += records;
  u32(section, 0);
  u32(b, int(section.size()));
  b += section;
  if (composite) {
    u16(b, 0);
    b += QByteArray(canvas.width() * canvas.height() * 3, char(63));
  }
  return b;
}
inline QByteArray polygon(std::vector<QPointF> points, QSize canvas = {96, 96},
                          bool curve = false) {
  QByteArray b;
  u32(b, 3);
  u32(b, 0);
  u16(b, 0);
  u16(b, int(points.size()));
  u16(b, 1);
  u16(b, 2);
  b += QByteArray(18, 0);
  for (auto p : points) {
    u16(b, 1);
    for (int i = 0; i < 3; ++i) {
      u32(b, int(p.y() / canvas.height() * 16777216));
      u32(b,
          int((p.x() + (curve && i == 0 ? 1 : 0)) / canvas.width() * 16777216));
    }
  }
  return b;
}
} // namespace psdfixture
