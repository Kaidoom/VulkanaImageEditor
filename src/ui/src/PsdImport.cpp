#include "PsdReader.hpp"
#include "imageeditor/platform/AvailableMemory.hpp"
#include "imageeditor/core/RichText.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/QtRasterImageLoader.hpp"
#include "imageeditor/ui/QtTextLayoutService.hpp"
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontInfo>
#include <QRawFont>
#include <QRunnable>
#include <QTextBlock>
#include <QTextLayout>
#include <QThreadPool>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <bit>

namespace imageeditor::ui {
namespace {
using namespace psd;
using Map = QVariantMap;
constexpr std::uint64_t MiB = 1024ULL * 1024;
std::uint64_t memoryAdd(std::uint64_t a, std::uint64_t b) {
  if (b > std::numeric_limits<std::uint64_t>::max() - a)
    throw Error("PSD memory estimate overflow");
  return a + b;
}
PsdLimits resolvedLimits(PsdLimits limits) {
  if (limits.workingBytes)
    return limits;
  const auto available = platform::availableMemoryBytes();
  limits.workingBytes = available
      ? psdWorkingMemoryLimit(*available, limits.existingBytes)
      : memoryAdd(limits.existingBytes, 1536 * MiB);
  return limits;
}
void admitMemory(PsdConversion &out, PsdLimits limits, std::uint64_t sourceBytes,
                 std::uint64_t retained, std::uint64_t largestScratch,
                 bool derivedPreview) {
  out.rasterBytes = retained;
  // Decoder/color-normalization scratch is needed for ONE layer at a time.
  // Publication has a different peak: authoritative pixels plus power-of-two
  // upload staging and driver/device backing (which can count against host RAM).
  // Reserve conservatively for that phase, not just the CPU import preview.
  const auto decodePeak = memoryAdd(retained, largestScratch);
  const auto twiceRetained = memoryAdd(retained, retained);
  const auto publicationPeak = memoryAdd(twiceRetained, twiceRetained);
  auto estimate = std::max(decodePeak, publicationPeak);
  estimate = memoryAdd(estimate, memoryAdd(sourceBytes, sourceBytes));
  estimate = memoryAdd(estimate, 64 * MiB);
  if (derivedPreview)
    estimate = memoryAdd(estimate, FlattenedDocumentLimits{}.derivedCacheBytes);
  out.estimatedWorkingBytes = estimate;
  const auto headroom = limits.workingBytes > limits.existingBytes
      ? limits.workingBytes - limits.existingBytes : 0;
  if (retained > limits.rasterBytes || estimate > headroom) {
    auto message = QStringLiteral(
        "Selected PSD content needs about %1 MiB of working memory "
        "(%2 MiB of layer/mask pixels); available import allowance is %3 MiB. "
        "Close other documents/apps or exclude more layers.")
        .arg(double(estimate) / MiB, 0, 'f', 0)
        .arg(double(retained) / MiB, 0, 'f', 0)
        .arg(double(headroom) / MiB, 0, 'f', 0);
    if (retained > limits.rasterBytes)
      message += QStringLiteral(" Caller-specified raster ceiling: %1 MiB.")
                     .arg(double(limits.rasterBytes) / MiB, 0, 'f', 0);
    throw Error(message.toStdString());
  }
}

// PSD channel rectangles often cover the whole canvas despite containing only
// a small painted island. Preserve hidden RGB as well as every nonzero alpha;
// masks/opacity/other layers never influence this storage-only compaction.
QRect storedPixelBounds(const QImage& image, const std::shared_ptr<PsdJob>& job) {
  int left=image.width(),top=image.height(),right=0,bottom=0;
  for(int y=0;y<image.height();++y) {
    if((y&63)==0) check(job);
    const auto* row=image.constScanLine(y);
    for(int x=0;x<image.width();++x) {
      const auto* p=row+x*4;
      if(p[0]||p[1]||p[2]||p[3]) {
        left=std::min(left,x);right=std::max(right,x+1);
        top=std::min(top,y);bottom=std::max(bottom,y+1);
      }
    }
  }
  if(right<=left||bottom<=top)return {0,0,1,1};
  // Retain a transparent sampling guard where it existed, but never invent
  // padding at an original opaque edge (which has clamped sampling semantics).
  left=std::max(0,left-1);top=std::max(0,top-1);
  right=std::min(image.width(),right+1);bottom=std::min(image.height(),bottom+1);
  return {left,top,right-left,bottom-top};
}
int indexValue(const QVariant &value, qsizetype count, const char *message) {
  const double n = number(value, -1);
  if (!std::isfinite(n) || n < 0 || n >= double(count) || std::floor(n) != n)
    throw Error(message);
  return int(n);
}
std::uint8_t alphaByte(double value) {
  if (!std::isfinite(value) || value < 0 || value > 1)
    throw Error("Invalid coverage/opacity");
  return std::uint8_t(std::lround(value * 255));
}
core::AffineTransform translation(core::Vec2d p) {
  return {.m02 = p.x, .m12 = p.y};
}
std::optional<core::BlendMode> blend(QByteArray key) {
  using B = core::BlendMode;
  static const QMap<QByteArray, B> modes{
      {"norm", B::Normal},      {"Nrml", B::Normal},
      {"mul ", B::Multiply},    {"Mltp", B::Multiply},
      {"scrn", B::Screen},      {"Scrn", B::Screen},
      {"over", B::Overlay},     {"Ovrl", B::Overlay},
      {"sLit", B::SoftLight},   {"SftL", B::SoftLight},
      {"hLit", B::HardLight},   {"HrdL", B::HardLight},
      {"dark", B::Darken},      {"Drkn", B::Darken},
      {"lite", B::Lighten},     {"Lghn", B::Lighten},
      {"diff", B::Difference},  {"Dfrn", B::Difference},
      {"smud", B::Exclusion},   {"Xclu", B::Exclusion},
      {"hue ", B::Hue},         {"H   ", B::Hue},
      {"sat ", B::Saturation},  {"Strt", B::Saturation},
      {"colr", B::Color},       {"Clr ", B::Color},
      {"lum ", B::Luminosity},  {"Lmns", B::Luminosity},
      {"div ", B::ColorDodge},  {"CDdg", B::ColorDodge},
      {"lddg", B::LinearDodge}, {"linearDodge", B::LinearDodge},
      {"idiv", B::ColorBurn},   {"CBrn", B::ColorBurn},
      {"lbrn", B::LinearBurn},  {"linearBurn", B::LinearBurn},
      {"fsub", B::Subtract},    {"blendSubtraction", B::Subtract},
      {"fdiv", B::Divide},      {"blendDivide", B::Divide}};
  auto it = modes.find(key);
  return it == modes.end() ? std::nullopt : std::optional(*it);
}
core::Rgba8 color(const PsdSource &s, const Map &c) {
  if (!c.contains("Rd  ") || !c.contains("Grn ") || !c.contains("Bl  "))
    throw Unsupported("Only RGB descriptor colors can be converted");
  auto byte = [](double v) {
    if (!std::isfinite(v) || v < 0 || v > 255.01)
      throw Error("Invalid RGB component");
    return std::uint8_t(std::clamp(std::lround(v), 0L, 255L));
  };
  core::Rgba8 result{byte(number(c["Rd  "])), byte(number(c["Grn "])),
                     byte(number(c["Bl  "])), 255};
  if (s.colorSpace != QColorSpace(QColorSpace::SRgb)) {
    QImage image(1, 1, QImage::Format_RGBA8888);
    auto *p = image.bits();
    p[0] = result.red;
    p[1] = result.green;
    p[2] = result.blue;
    p[3] = 255;
    image.setColorSpace(s.colorSpace);
    image = image.convertedToColorSpace(QColorSpace::SRgb);
    if (image.isNull())
      throw Error("Color conversion failed");
    result = {image.bits()[0], image.bits()[1], image.bits()[2], 255};
  }
  return result;
}
Map desc(const Record &r, const QByteArray &key, int prefix = 0) {
  Reader reader(r.tags.value(key));
  reader.skip(prefix);
  return versionedDescriptor(reader);
}
QString psName(const QRawFont &font) {
  try {
    auto data = font.fontTable("name");
    Reader r(data);
    r.skip(2);
    auto count = r.u16(), offset = r.u16();
    if (count > 4096)
      return {};
    for (int i = 0; i < count; ++i) {
      auto platform = r.u16();
      r.skip(4);
      auto id = r.u16(), length = r.u16(), pos = r.u16();
      if (id != 6)
        continue;
      Reader all(data);
      all.skip(qsizetype(offset) + pos);
      auto bytes = all.take(length);
      return platform == 0 || platform == 3 ? utf16(bytes)
                                            : QString::fromLatin1(bytes);
    }
  } catch (const Error &) {
  }
  return {};
}
struct Face {
  QString family, style, postscript;
  int weight;
  bool italic;
};
const std::vector<Face> &faces() {
  static std::once_flag once;
  static std::vector<Face> list;
  std::call_once(once, [] {
    for (const auto &family : QFontDatabase::families())
      for (const auto &style : QFontDatabase::styles(family)) {
        auto font = QFontDatabase::font(family, style, 24);
        auto raw = QRawFont::fromFont(font);
        if (raw.isValid())
          list.push_back({raw.familyName(), raw.styleName(), psName(raw),
                          int(raw.weight()),
                          raw.style() != QFont::StyleNormal});
      }
  });
  return list;
}
core::FontDescriptor resolve(const QString &requested, const PsdChoice &choice,
                             Map &replacements) {
  const auto &available = faces();
  const Face *found = nullptr;
  auto family = choice.replacements.value(requested);
  if (family.isEmpty()) {
    auto it =
        std::find_if(available.begin(), available.end(), [&](const Face &f) {
          return f.postscript.compare(requested, Qt::CaseInsensitive) == 0;
        });
    if (it == available.end())
      it = std::find_if(available.begin(), available.end(), [&](const Face &f) {
        return f.family.compare(requested, Qt::CaseInsensitive) == 0 &&
               !f.italic && f.weight == 400 &&
               (f.style == "Regular" || f.style == "Normal" ||
                f.style == "Book" || f.style == "Roman");
      });
    if (it != available.end())
      found = &*it;
  }
  if (!found) {
    if (family.isEmpty())
      family = QFontInfo(QFontDatabase::systemFont(QFontDatabase::GeneralFont))
                   .family();
    auto it =
        std::find_if(available.begin(), available.end(), [&](const Face &f) {
          return f.family == family && !f.italic && f.weight == 400 &&
                 (f.style == "Regular" || f.style == "Normal" ||
                  f.style == "Book" || f.style == "Roman");
        });
    if (it == available.end())
      it = std::find_if(available.begin(), available.end(),
                        [&](const Face &f) { return f.family == family; });
    if (it == available.end())
      throw Unsupported("No usable installed replacement font");
    found = &*it;
    replacements[requested] = family;
  }
  return {found->family.toStdString(), found->style.toStdString(),
          found->weight, found->italic, requested.toStdString()};
}
struct Model {
  core::AdjustmentState adjustment;
  std::optional<core::TextLayer> text;
  std::optional<core::ShapeLayer> shape;
  core::AffineTransform transform;
  core::LayerEffectState effects;
  QStringList substitutions;
  Map replacements;
};
bool isAdjustment(const Record& rec) {return rec.tags.contains("nvrt")||rec.tags.contains("expA");}
void adjustmentModel(const PsdSource& source,const Record& rec,Model& model) {
  if(!isAdjustment(rec))return;
  if(rec.tags.contains("nvrt")&&rec.tags.contains("expA"))throw Unsupported("Multiple native adjustment records on one PSD layer");
  if(rec.blend!="norm" || rec.fill!=255 || rec.tags.contains("lfx2"))throw Unsupported("Adjustment blending/fill/styles need the saved composite");
  auto stack=std::make_shared<core::AdjustmentStack>();
  if(rec.tags.contains("nvrt"))stack->items[9].enabled=true;
  else {
    Reader reader(rec.tags.value("expA"));
    if(reader.u16()!=1)throw Unsupported("Unsupported exposure record version");
    const auto exposure=std::bit_cast<float>(reader.u32()),offset=std::bit_cast<float>(reader.u32()),gamma=std::bit_cast<float>(reader.u32());
    if(offset!=0 || gamma!=1 || !std::isfinite(exposure) || exposure < -20 || exposure > 20)
      throw Unsupported("Exposure offset/gamma or range requires the saved composite");
    stack->items[0].enabled=true;stack->items[0].parameters=core::ExposureParameters{exposure};
  }
  model.adjustment=std::move(stack);model.transform={};
  if(rec.opacity!=255 || rec.mask.present)
    model.substitutions<<"Adjustment strength uses linear-light mixing; fractional masks/opacity may differ between editors.";
  if(source.colorSpace.isValid()&&source.colorSpace!=QColorSpace(QColorSpace::SRgb))
    model.substitutions<<"The correction is evaluated in Vulkana's sRGB working space after profile conversion.";
}
void effects(const PsdSource &s, const Record &rec, Model &m) {
  const auto key =
      rec.tags.contains("lmfx") ? QByteArray("lmfx") : QByteArray("lfx2");
  if (!rec.tags.contains(key)) {
    if (rec.tags.contains("lrFX"))
      throw Unsupported("Legacy-only layer effects are unsupported; saved "
                        "layer pixels do not include them");
    return;
  }
  auto d = desc(rec, key, 4);
  const bool masterEnabled=d.value("masterFXSwitch", true).toBool();
  auto stack = std::make_shared<core::LayerEffectStack>();
  bool active = false;
  double scale = number(d.value("Scl "), 100) / 100;
  if (scale <= 0 || scale > 100)
    throw Error("Invalid style scale");
  for (auto it = d.begin(); it != d.end(); ++it) {
    if (it.key().startsWith('_') || it.key() == "Scl " ||
        it.key() == "masterFXSwitch" || it.key() == "numModifyingFX")
      continue;
    QVariantList list = it.value().metaType().id() == QMetaType::QVariantList
                            ? it.value().toList()
                            : QVariantList{it.value()};
    int enabled = 0;
    for (const auto &item : list) {
      auto e = item.toMap();
      if(it.key()=="ebbl" && !e.empty()) {
        auto& target=stack->items[std::size_t(core::LayerEffectType::BevelEmboss)];auto& b=target.bevel;
        if(++enabled>1)throw Unsupported("Multiple bevel representations are unsupported");
        if(enumeration(e["bvlT"])!="SfBL"||e.value("useTexture",false).toBool())
          throw Unsupported("Bevel technique/texture needs the saved composite or Base pixels only");
        const auto style=enumeration(e["bvlS"]),direction=enumeration(e["bvlD"]);
        if(style!="InrB"&&style!="OtrB"&&style!="Embs")throw Unsupported("This bevel style is unsupported");
        if(direction!="In  "&&direction!="Out ")throw Unsupported("Unknown bevel direction");
        b.style=style=="InrB"?core::BevelStyle::Inner:style=="OtrB"?core::BevelStyle::Outer:core::BevelStyle::Emboss;
        b.down=direction=="Out ";target.enabled=masterEnabled&&e.value("enab",true).toBool()&&e.value("present",true).toBool();
        target.size=number(e["blur"])*scale;b.soften=number(e["Sftn"])*scale;b.depth=number(e["srgR"],100)/100;
        const bool global=e.value("uglg",false).toBool();
        target.angle=std::remainder(-(global?s.globalLightAngle:number(e["lagl"],120)),360.);
        b.altitude=global?s.globalLightAltitude:number(e["Lald"],30);
        const auto hm=blend(enumeration(e["hglM"]).toLatin1()),sm=blend(enumeration(e["sdwM"]).toLatin1());
        if(!hm||!sm)throw Unsupported("Unsupported bevel highlight/shadow blend mode");
        target.blendMode=*hm;b.shadowBlend=*sm;target.color=color(s,e["hglC"].toMap());target.secondColor=color(s,e["sdwC"].toMap());
        target.opacity=number(e["hglO"],75)/100;b.shadowOpacity=number(e["sdwO"],75)/100;
        const auto curve=[](const QVariant& v,bool on) {
          core::EffectContour c;c.enabled=on;
          if(!v.isValid())return c;
          const auto points=v.toMap()["Crv "].toList();
          if(points.size()<2||points.size()>16)throw Unsupported("Bevel contour point count is unsupported");
          c.points.clear();bool allCorners=true;
          for(const auto& value:points) {
            const auto p=value.toMap();const bool corner=!p.value("Cnty",true).toBool();allCorners&=corner;
            c.points.push_back({number(p["Hrzn"])/255,number(p["Vrtc"])/255,corner});
          }
          if(allCorners){c.interpolation=core::EffectContourInterpolation::Linear;for(auto& p:c.points)p.corner=false;}
          if(!core::validEffectContour(c))throw Unsupported("Bevel contour has invalid or unsupported points");
          return c;
        };
        b.gloss=curve(e["TrnS"],true);b.surface=curve(e["MpgS"],e.value("useShape",false).toBool());
        if(b.surface.enabled&&number(e["Inpr"],100)!=100)
          throw Unsupported("Bevel surface contour range requires composite or Base pixels only");
        m.substitutions<<"Editable Smooth bevel: surface and lighting response may differ outside Vulkana.";
        if(global)m.substitutions<<"Shared light resolved to this effect's own angle and altitude.";
        active=true;continue;
      }
      if (e.empty() || !e.value("enab").toBool() ||
          !e.value("present", true).toBool() || !masterEnabled)
        continue;
      if (++enabled > 1)
        throw Unsupported(
            "Multiple instances of a layer effect are unsupported");
      core::LayerEffectType type;
      if (it.key() == "FrFX" || it.key() == "frameFXMulti")
        type = core::LayerEffectType::Stroke;
      else if (it.key() == "SoFi" || it.key() == "solidFillMulti")
        type = core::LayerEffectType::ColorOverlay;
      else
        throw Unsupported(
            "An active effect cannot be reproduced by the saved base pixels");
      auto &target = stack->items[size_t(type)];
      if (target.enabled)
        throw Unsupported("Duplicate enabled effect representation");
      target.enabled = true;
      active = true;
      auto mode = blend(enumeration(e["Md  "]).toLatin1());
      if (!mode)
        throw Unsupported("Unsupported effect blend mode");
      target.blendMode = *mode;
      target.opacity = number(e["Opct"], 100) / 100;
      target.color = color(s, e["Clr "].toMap());
      if (type == core::LayerEffectType::Stroke) {
        if (enumeration(e["PntT"]) != "SClr")
          throw Unsupported("Gradient/pattern Stroke is unsupported");
        target.size = number(e["Sz  "]) * scale;
        auto position = enumeration(e["Styl"]);
        if (position != "InsF" && position != "CtrF" && position != "OutF")
          throw Unsupported("Unknown Stroke position");
        target.position = position == "InsF"   ? core::StrokePosition::Inside
                          : position == "CtrF" ? core::StrokePosition::Center
                                               : core::StrokePosition::Outside;
      }
    }
  }
  if (!core::validLayerEffects(*stack))
    throw Unsupported("Effect dimensions exceed supported limits");
  if (active)
    m.effects = stack;
}
void textModel(const PsdSource &s, const Record &rec, const PsdChoice &choice,
               Model &model) {
  Reader reader(rec.tags["TySh"]);
  if (reader.u16() != 1)
    throw Unsupported("Unsupported text descriptor version");
  double a = reader.f64(), b = reader.f64(), c = reader.f64(), d = reader.f64(),
         tx = reader.f64(), ty = reader.f64();
  model.transform = {a, c, tx, b, d, ty};
  for (auto coefficient : {a, b, c, d, tx, ty})
    if (std::abs(coefficient) > 1e12)
      throw Error("Text transform exceeds project limits");
  if (!model.transform.inverted())
    throw Error("Singular text transform");
  if (reader.u16() != 50)
    throw Unsupported("Unsupported text data version");
  auto text = versionedDescriptor(reader);
  if (reader.u16() != 1)
    throw Unsupported("Unsupported text warp version");
  auto warp = versionedDescriptor(reader);
  if (enumeration(warp["warpStyle"]) != "warpNone")
    throw Unsupported("Text warp unsupported; saved raster may preserve it");
  if (enumeration(text["Ornt"]) != "Hrzn")
    throw Unsupported("Vertical text is unsupported");
  auto engine = engineData(text["EngineData"].toByteArray());
  auto resource = engine["ResourceDict"].toMap();
  auto body = engine["EngineDict"].toMap();
  auto children =
      body["Rendered"].toMap()["Shapes"].toMap()["Children"].toList();
  if (children.size() != 1 ||
      number(children.front().toMap()["ShapeType"]) != 0)
    throw Unsupported("Paragraph-box text requires saved raster in V1");
  QString string = body["Editor"].toMap()["Text"].toString();
  if (string.isEmpty())
    throw Error("Missing text engine string");
  const auto sourceLength = string.size();
  if (!string.endsWith('\r'))
    throw Unsupported("Text engine terminal character is missing");
  string.chop(1);
  string.replace('\r', '\n');
  core::TextLayer result;
  result.utf8 = string.toUtf8().toStdString();
  if (result.utf8.size() > core::kMaximumTextBytes)
    throw Error("Text exceeds native byte limit");
  if (result.utf8.empty())
    throw Unsupported("Empty editable text");
  core::Utf8TextIndex index(result.utf8);
  auto fontSet = resource["FontSet"].toList();
  auto sheets = resource["StyleSheetSet"].toList();
  auto normal = indexValue(resource.value("TheNormalStyleSheet", 0),
                           sheets.size(), "Invalid default text style");
  auto defaults = sheets[normal].toMap()["StyleSheetData"].toMap();
  auto runs = body["StyleRun"].toMap();
  auto inherited = runs["DefaultRunData"]
                       .toMap()["StyleSheet"]
                       .toMap()["StyleSheetData"]
                       .toMap();
  for (auto i = inherited.begin(); i != inherited.end(); ++i)
    defaults[i.key()] = i.value();
  const auto styles = runs["RunArray"].toList(),
             lengths = runs["RunLengthArray"].toList();
  if (styles.size() != lengths.size() ||
      styles.size() > qsizetype(core::kMaximumTextRuns))
    throw Error("Invalid text style runs");
  qsizetype position = 0;
  for (qsizetype i = 0; i < styles.size(); ++i) {
    auto run =
        styles[i].toMap()["StyleSheet"].toMap()["StyleSheetData"].toMap();
    auto properties = defaults;
    for (auto it = run.begin(); it != run.end(); ++it)
      properties[it.key()] = it.value();
    const auto length = number(lengths[i]);
    if (length < 1 || std::floor(length) != length ||
        length > double(sourceLength - position))
      throw Error("Invalid UTF-16 run length");
    auto end = position + qsizetype(length);
    const auto first = std::min(position, string.size()),
               last = std::min(end, string.size());
    for (auto boundary : {first, last})
      if (boundary > 0 && boundary < string.size() &&
          string[boundary].isLowSurrogate())
        throw Error("Text run splits a surrogate pair");
    const auto font = indexValue(properties["Font"], fontSet.size(),
                                 "Invalid text font index");
    core::TextStyle style;
    auto request = fontSet[font].toMap()["Name"].toString();
    style.font = resolve(request, choice, model.replacements);
    style.sizePixels = number(properties["FontSize"]);
    if (properties["FauxBold"].toBool())
      style.font.weight = 700;
    if (properties["FauxItalic"].toBool())
      style.font.italic = true;
    if (properties.contains("AutoKerning") &&
        !properties["AutoKerning"].toBool())
      model.substitutions
          << "Character kerning uses Vulkana's native text layout.";
    if (!properties.value("AutoLeading", true).toBool() &&
        number(properties["Leading"]) != 0)
      throw Unsupported("Explicit text leading requires saved raster");
    // Engine sizes are local pixel em sizes; the TySh matrix applies once.
    auto rgba = properties["FillColor"].toMap()["Values"].toList();
    if (rgba.size() != 4 ||
        number(properties["FillColor"].toMap()["Type"]) != 1)
      throw Unsupported("Unsupported text color");
    style.color = color(s, {{"Rd  ", number(rgba[1]) * 255},
                            {"Grn ", number(rgba[2]) * 255},
                            {"Bl  ", number(rgba[3]) * 255}});
    style.color.alpha = alphaByte(number(rgba[0]));
    for (const auto &key :
         {"Tracking", "BaselineShift", "FontCaps", "FontBaseline"})
      if (number(properties[key]) != 0)
        throw Unsupported("Text tracking, baseline shift or capitalization "
                          "requires raster fallback");
    if (number(properties.value("HorizontalScale"), 1) != 1 ||
        number(properties.value("VerticalScale"), 1) != 1 ||
        properties["Underline"].toBool() ||
        properties["Strikethrough"].toBool() ||
        properties.value("StrokeFlag", false).toBool() ||
        !properties.value("FillFlag", true).toBool())
      throw Unsupported("Unsupported character scaling or decoration");
    if (!i)
      result.defaultStyle = style;
    if (last > first)
      result.runs.push_back(
          {index.byteOffset(size_t(first)),
           index.byteOffset(size_t(last)) - index.byteOffset(size_t(first)),
           style});
    position = end;
  }
  if (position != sourceLength)
    throw Error("Text runs do not cover the engine string");
  const auto paragraphs = body["ParagraphRun"].toMap();
  Map paragraphDefaults;
  auto paragraphSheets = resource["ParagraphSheetSet"].toList();
  if (!paragraphSheets.empty()) {
    const auto normalParagraph =
        indexValue(resource.value("TheNormalParagraphSheet", 0),
                   paragraphSheets.size(), "Invalid default paragraph sheet");
    paragraphDefaults =
        paragraphSheets[normalParagraph].toMap()["Properties"].toMap();
  }
  auto inheritedParagraph = paragraphs["DefaultRunData"]
                                .toMap()["ParagraphSheet"]
                                .toMap()["Properties"]
                                .toMap();
  for (auto it = inheritedParagraph.begin(); it != inheritedParagraph.end();
       ++it)
    paragraphDefaults[it.key()] = it.value();
  const auto parRuns = paragraphs["RunArray"].toList(),
             parLengths = paragraphs["RunLengthArray"].toList();
  if (parRuns.size() != parLengths.size())
    throw Error("Invalid paragraph run lengths");
  position = 0;
  result.paragraphs.clear();
  for (qsizetype i = 0; i < parRuns.size(); ++i) {
    auto props = paragraphDefaults;
    const auto paragraphOverride =
        parRuns[i].toMap()["ParagraphSheet"].toMap()["Properties"].toMap();
    for (auto it = paragraphOverride.begin(); it != paragraphOverride.end();
         ++it)
      props[it.key()] = it.value();
    auto alignment =
        indexValue(props.value("Justification", 0), 3,
                   "Justified/invalid paragraphs require raster fallback");
    for (auto key : {"FirstLineIndent", "StartIndent", "EndIndent",
                     "SpaceBefore", "SpaceAfter"})
      if (number(props[key]) != 0)
        throw Unsupported("Paragraph spacing requires raster fallback");
    auto length = number(parLengths[i]);
    if (length < 1 || std::floor(length) != length ||
        length > double(sourceLength - position))
      throw Error("Invalid paragraph coverage");
    auto last = std::min(position + qsizetype(length), string.size());
    for (qsizetype j = position; j <= last && j < string.size(); ++j)
      if (j == 0 || string[j - 1] == '\n')
        result.paragraphs.push_back({index.byteOffset(size_t(j)),
                                     alignment == 1 ? core::TextAlignment::Right
                                     : alignment == 2
                                         ? core::TextAlignment::Center
                                         : core::TextAlignment::Left});
    position += qsizetype(length);
  }
  if (position != sourceLength)
    throw Error("Paragraphs do not cover the text");
  result = core::normalizedText(std::move(result));
  QtTextLayout layout(result);
  auto block = layout.document().begin();
  auto line = block.layout()->lineAt(0);
  if (!line.isValid())
    throw Error("Unable to lay out imported text");
  double baseline = block.layout()->position().y() + line.y() + line.ascent();
  double originX = 0;
  auto alignment = result.paragraphs.front().alignment;
  if (alignment == core::TextAlignment::Center)
    originX = -layout.bounds().width() / 2;
  else if (alignment == core::TextAlignment::Right)
    originX = -layout.bounds().width();
  model.transform = core::composeTransform(model.transform,
                                           translation({originX, -baseline}));
  model.text = std::move(result);
  for (auto it = model.replacements.begin(); it != model.replacements.end();
       ++it)
    model.substitutions
        << QStringLiteral(
               "Font unavailable or replaced: %1 → %2. Layout may change.")
               .arg(it.key(), it.value().toString());
}
void shapeModel(const PsdSource &s, const Record &rec, Model &model) {
  auto key =
      rec.tags.contains("vsms") ? QByteArray("vsms") : QByteArray("vmsk");
  Reader path(rec.tags[key]);
  if (path.u32() != 3)
    throw Unsupported("Unsupported vector path version");
  auto flags = path.u32();
  if (flags & 5)
    throw Unsupported(
        "Inverted/disabled vector geometry requires saved raster");
  std::vector<core::Vec2d> points;
  std::vector<std::array<core::Vec2d, 3>> knots;
  bool curved = false;
  int paths = 0, expected = 0, actual = 0;
  while (path.left() >= 26) {
    auto record = path.section(26);
    auto selector = record.u16();
    if (selector == 0 || selector == 3) {
      if (expected != actual)
        throw Error("Path knot count mismatch");
      if (selector == 3 || ++paths > 1)
        throw Unsupported("Open or compound vector path requires saved raster");
      expected = record.u16();
      actual = 0;
      auto operation = record.u16();
      if (operation != 1)
        throw Unsupported("Unsupported vector path Boolean operation");
    } else if (selector == 1 || selector == 2) {
      if (!paths || ++actual > expected)
        throw Error("Unexpected vector knot");
      core::Vec2d controls[3];
      for (auto &p : controls) {
        double y = double(record.i32()) / 16777216,
               x = double(record.i32()) / 16777216;
        p = {x * s.size.width(), y * s.size.height()};
      }
      curved |= controls[0] != controls[1] || controls[1] != controls[2];
      knots.push_back({controls[0], controls[1], controls[2]});
      if (points.empty() || points.back() != controls[1])
        points.push_back(controls[1]);
    } else if (selector == 8) {
      if (record.u16() != 0)
        throw Unsupported("Initially filled vector masks require saved raster");
    } else if (selector != 6 && selector != 7)
      throw Unsupported("Unsupported vector path record");
  }
  if (path.left() > 3)
    throw Error("Truncated path record");
  for (char byte : path.take(path.left()))
    if (byte)
      throw Error("Invalid vector path padding");
  if (expected != actual)
    throw Error("Incomplete vector path");
  if (points.size() > 1 && points.front() == points.back())
    points.pop_back();
  if (points.size() < 3)
    throw Unsupported("Empty shape geometry");
  double left = points[0].x, right = left, top = points[0].y, bottom = top;
  for (auto p : points) {
    left = std::min(left, p.x);
    right = std::max(right, p.x);
    top = std::min(top, p.y);
    bottom = std::max(bottom, p.y);
  }
  core::ShapeLayer shape;
  shape.kind = core::ShapeKind::Polygon;
  shape.size = {right - left, bottom - top};
  for (auto p : points)
    shape.points.push_back({p.x - left, p.y - top});
  model.transform = translation({left, top});
  if (points.size() == 4 &&
      std::all_of(points.begin(), points.end(), [&](auto p) {
        return (p.x == left || p.x == right) && (p.y == top || p.y == bottom);
      })) {
    shape.kind = core::ShapeKind::Rectangle;
    shape.points.clear();
  }
  if (curved) {
    // Recognize a complete four-cubic ellipse, including affine placement.
    // Other curves stay explicit fallbacks, never their bounding rectangle.
    if (knots.size() != 4)
      throw Unsupported("Curved vector paths require saved raster");
    const auto center = (knots[0][1] + knots[2][1]) * .5;
    const auto u = (knots[1][1] - knots[3][1]) * .5;
    const auto v = (knots[2][1] - knots[0][1]) * .5;
    const double rx = std::hypot(u.x, u.y), ry = std::hypot(v.x, v.y);
    constexpr double k = .5522847498307936;
    const double tolerance = std::max(s.size.width(), s.size.height()) * 4. / 16777216;
    const auto near = [&](core::Vec2d a, core::Vec2d b) {
      return std::hypot(a.x - b.x, a.y - b.y) <= tolerance;
    };
    const std::array<std::array<core::Vec2d, 3>, 4> expectedKnots{{
      {center-v-u*k, center-v, center-v+u*k},
      {center+u-v*k, center+u, center+u+v*k},
      {center+v+u*k, center+v, center+v-u*k},
      {center-u+v*k, center-u, center-u-v*k}}};
    if (rx < 1e-6 || ry < 1e-6)
      throw Unsupported("Degenerate ellipse requires saved raster");
    for (size_t i=0;i<4;++i) for (size_t j=0;j<3;++j)
      if (!near(knots[i][j], expectedKnots[i][j]))
        throw Unsupported("Curved vector paths require saved raster");
    shape.kind = core::ShapeKind::Ellipse;
    shape.points.clear(); shape.size = {2*rx, 2*ry};
    model.transform = {u.x/rx, v.x/ry, center.x-u.x-v.x,
                       u.y/rx, v.y/ry, center.y-u.y-v.y};
  }
  Map fill;
  if (rec.tags.contains("vscg")) {
    Reader v(rec.tags["vscg"]);
    if (v.take(4) != "SoCo")
      throw Unsupported("Only solid vector fill is supported");
    fill = versionedDescriptor(v);
  } else if (rec.tags.contains("SoCo"))
    fill = desc(rec, "SoCo");
  else
    throw Unsupported("Missing solid shape fill descriptor");
  shape.fillColor = color(s, fill["Clr "].toMap());
  if (rec.tags.contains("vstk")) {
    auto style = desc(rec, "vstk");
    shape.fillEnabled = style.value("fillEnabled", true).toBool();
    shape.strokeEnabled = style.value("strokeEnabled", false).toBool();
    if (shape.strokeEnabled) {
      auto mode = blend(enumeration(style["strokeStyleBlendMode"]).toLatin1());
      if (!mode || *mode != core::BlendMode::Normal ||
          enumeration(style["strokeStyleLineAlignment"]) !=
              "strokeStyleAlignCenter" ||
          !style["strokeStyleLineDashSet"].toList().empty())
        throw Unsupported("Non-normal, dashed or non-centered shape stroke "
                          "requires saved raster");
      auto width = style["strokeStyleLineWidth"];
      auto unit = width.toMap()["unit"].toString();
      shape.strokeWidth = number(width) * (unit == "#Pnt" ? s.ppi / 72 : 1);
      if (unit != "#Pnt" && unit != "#Pxl")
        throw Unsupported("Unsupported vector stroke units");
      shape.strokeColor =
          color(s, style["strokeStyleContent"].toMap()["Clr "].toMap());
      shape.strokeColor.alpha =
          alphaByte(number(style["strokeStyleOpacity"], 100) / 100);
      auto join = enumeration(style["strokeStyleLineJoinType"]),
           cap = enumeration(style["strokeStyleLineCapType"]);
      if (join != "strokeStyleMiterJoin" && join != "strokeStyleRoundJoin" &&
          join != "strokeStyleBevelJoin")
        throw Unsupported("Unknown vector stroke join");
      if (cap != "strokeStyleButtCap" && cap != "strokeStyleRoundCap" &&
          cap != "strokeStyleSquareCap")
        throw Unsupported("Unknown vector stroke cap");
      shape.strokeJoin = join == "strokeStyleMiterJoin" ? core::ShapeJoin::Miter
                         : join == "strokeStyleBevelJoin"
                             ? core::ShapeJoin::Bevel
                             : core::ShapeJoin::Round;
      shape.strokeCap = cap == "strokeStyleButtCap"     ? core::ShapeCap::Butt
                        : cap == "strokeStyleSquareCap" ? core::ShapeCap::Square
                                                        : core::ShapeCap::Round;
      shape.strokeMiterLimit = number(style["strokeStyleMiterLimit"], 4) / 2;
    }
  }
  if (!core::validShape(shape))
    throw Error("Invalid converted shape");
  if (curved && shape.strokeEnabled &&
      std::abs(model.transform.m00 * model.transform.m01 +
               model.transform.m10 * model.transform.m11) > 1e-5)
    throw Unsupported("Sheared ellipse stroke requires saved raster");
  model.shape = std::move(shape);
}
bool savedPixels(const Record &r) {
  if (r.bounds.isEmpty())
    return false;
  for (int id : {0, 1, 2})
    if (std::none_of(r.channels.begin(), r.channels.end(),
                     [id](auto c) { return c.id == id; }))
      return false;
  return true;
}
bool isShape(const Record &r) {
  return (r.tags.contains("vsms") || r.tags.contains("vmsk")) &&
         (r.tags.contains("SoCo") || r.tags.contains("vscg"));
}
QStringList restrictions(const Record &r, bool localAdjustmentGroup=false) {
  auto issues = r.issues;
  if (r.blendIf)
    issues << "Blend If is unsupported";
  if (r.knockout)
    issues << "Knockout blending is unsupported";
  if (!blend(r.blend) &&
      !(r.blend == "pass" && (r.section == 1 || r.section == 2)))
    issues << "Unsupported layer blend mode";
  if (r.fill != 255)
    issues << "Fill opacity is distinct from layer opacity and is not "
              "supported in V1";
  if (r.mask.present &&
      (r.mask.density != 255 || r.mask.feather != 0 || (r.mask.flags & 12)))
    issues << "Mask density, feather or rendered/inverted-mask semantics "
              "require an explicit fallback";
  if (r.mask.present && isShape(r))
    issues << "Bitmap plus vector masks require an explicit raster/composite "
              "choice";
  if ((r.section == 1 || r.section == 2) &&
      ((r.blend != "pass" && !(localAdjustmentGroup&&r.blend=="norm")) || r.opacity != 255 || r.mask.present ||
       r.tags.contains("lfx2")))
    issues
        << "Isolated/styled groups cannot be converted to pass-through folders";
  if (!isShape(r) && (r.tags.contains("vsms") || r.tags.contains("vmsk")))
    issues << "Vector masking of pixel/text content is unsupported";
  return issues;
}
Model modelFor(const PsdSource &s, const Record &rec, const PsdChoice &choice,
               bool typed) {
  Model m;
  m.transform = translation({double(rec.bounds.x()), double(rec.bounds.y())});
  effects(s, rec, m);
  if (typed) {
    if (isAdjustment(rec))adjustmentModel(s,rec,m);
    else if (rec.tags.contains("TySh"))
      textModel(s, rec, choice, m);
    else if (isShape(rec))
      shapeModel(s, rec, m);
  }
  if(m.effects&&m.effects->items[7]!=core::defaultLayerEffect(core::LayerEffectType::BevelEmboss)) {
    // PSD relief dimensions/light are in document space; native styles travel
    // with local typed geometry. A similarity has an exact inverse for both.
    const auto& t=m.transform;const double x=std::hypot(t.m00,t.m10),y=std::hypot(t.m01,t.m11);
    if(!t.isAffine()||x<1e-9||std::abs(x-y)>1e-6*std::max(x,y)||
       std::abs(t.m00*t.m01+t.m10*t.m11)>1e-6*x*y)
      throw Unsupported("Bevel on nonuniform/sheared typed geometry requires a raster or composite choice");
    auto stack=std::make_shared<core::LayerEffectStack>(*m.effects);auto& e=stack->items[7];
    e.size/=x;e.bevel.soften/=x;
    const auto radians=e.angle*std::numbers::pi/180;const auto inverse=t.inverted();
    const auto direction=inverse->map({std::cos(radians),std::sin(radians)})-inverse->map({0,0});
    e.angle=std::atan2(direction.y,direction.x)*180/std::numbers::pi;
    if(!core::validLayerEffects(*stack))throw Unsupported("Local bevel dimensions exceed supported limits");
    m.effects=std::move(stack);
  }
  return m;
}
core::LayerMaskState maskFor(const PsdSource &s, const Record &rec,
                             const core::AffineTransform &matrix,
                             const std::shared_ptr<PsdJob> &job) {
  if (!rec.mask.present)
    return {};
  const auto channel = std::find_if(rec.channels.begin(), rec.channels.end(),
                                    [](auto c) { return c.id == -2; });
  if (channel == rec.channels.end())
    throw Error("Mask descriptor has no user-mask channel");
  const auto &source = rec.mask;
  auto rect = source.bounds;
  if (source.flags & 1)
    rect.translate(rec.bounds.topLeft());
  if (rect.isEmpty())
    throw Unsupported("Empty bitmap mask rectangle");
  auto bytes = decodeChannel(
      QByteArrayView(s.bytes).sliced(channel->offset, channel->length),
      rect.width(), rect.height(), job);
  auto mask = std::make_shared<core::LayerMask>();
  mask->coverage = core::SelectionMask::fromR8(
      {uint(rect.width()), uint(rect.height())},
      std::span(reinterpret_cast<const std::uint8_t *>(bytes.constData()),
                size_t(bytes.size())),
      size_t(rect.width()));
  mask->outside = std::uint8_t(source.outside);
  mask->enabled = !(source.flags & 2);
  mask->localToMask = core::composeTransform(
      translation({-double(rect.x()), -double(rect.y())}), matrix);
  if (!core::validLayerMask(mask))
    throw Error("Invalid imported mask mapping");
  return mask;
}
struct Worker {
  QThreadPool pool;
  std::atomic<int> queued{};
  Worker() {
    pool.setMaxThreadCount(1);
    pool.setExpiryTimeout(-1);
  }
  ~Worker() { pool.waitForDone(); }
};
Worker &psdWorker() {
  static Worker worker;
  return worker;
}
template <class F> auto submit(F f) {
  using R = decltype(f());
  auto &worker = psdWorker();
  if (worker.queued.fetch_add(1) >= 3) {
    --worker.queued;
    std::promise<R> p;
    R r;
    r.error = "PSD import queue is busy";
    p.set_value(std::move(r));
    return p.get_future();
  }
  auto task = std::make_shared<std::packaged_task<R()>>(
      [f = std::move(f), &worker]() mutable {
        struct Done {
          Worker &worker;
          ~Done() { --worker.queued; }
        } done{worker};
        return f();
      });
  auto future = task->get_future();
  worker.pool.start(QRunnable::create([task] { (*task)(); }));
  return future;
}
FlattenedDocumentResult renderImportPreview(const core::Document &document,
                                           const std::shared_ptr<PsdJob> &job) {
  if (job) {
    job->previewPercent = 0;
    job->renderingPreview = true;
  }
  const auto size = document.canvas().extent;
  const double scale = std::min({1.0, 480.0 / size.width, 420.0 / size.height});
  return flattenDocumentAtSize(document,
      {uint(std::max(1L, std::lround(size.width * scale))),
       uint(std::max(1L, std::lround(size.height * scale)))},
      [job](auto done, auto total) {
        if (!job) return true;
        job->previewPercent = total ? int(done * 100 / total) : 0;
        return !job->cancelled;
      });
}
} // namespace
std::uint64_t psdWorkingMemoryLimit(std::uint64_t available,
                                  std::uint64_t existing) {
  const auto allowance = available - available / 4;
  return existing > std::numeric_limits<std::uint64_t>::max() - allowance
      ? std::numeric_limits<std::uint64_t>::max() : existing + allowance;
}
bool isPsdFile(const QString &path) {
  const auto suffix = QFileInfo(path).suffix();
  if (suffix.compare("psd", Qt::CaseInsensitive) == 0 ||
      suffix.compare("psb", Qt::CaseInsensitive) == 0)
    return true;
  QFile f(path);
  return f.open(QIODevice::ReadOnly) && f.read(4) == "8BPS";
}
PsdInspection inspectPsd(const QString &path,
                         const std::shared_ptr<PsdJob> &job, PsdLimits limits) {
  PsdInspection result;
  try {
    limits = resolvedLimits(limits);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
      throw Error("Could not open PSD");
    if (file.size() < 26 || std::uint64_t(file.size()) > limits.fileBytes)
      throw Error("PSD file exceeds input budget or is incomplete");
    if (memoryAdd(limits.existingBytes, std::uint64_t(file.size()) * 2) >
        limits.workingBytes)
      throw Error("Not enough import working memory");
    const auto originalSize = file.size();
    const auto originalTime = QFileInfo(file).lastModified();
    QByteArray bytes;
    bytes.reserve(file.size());
    while (!file.atEnd()) {
      check(job);
      auto block = file.read(1024 * 1024);
      if (block.isEmpty() && file.error() != QFile::NoError)
        throw Error("PSD read failed");
      bytes += block;
      if (std::uint64_t(bytes.size()) > limits.fileBytes)
        throw Error("PSD changed beyond input limit");
    }
    if (file.size() != originalSize || bytes.size() != originalSize ||
        QFileInfo(file).lastModified() != originalTime)
      throw Error("PSD changed while reading; please open it again");
    auto s = parse(std::move(bytes), job, limits);
    result.source = s;
    result.size = s->size;
    result.ppi = s->ppi;
    result.summary = QStringLiteral("%1 × %2 px · %3 PPI · 8-bit RGB · %4")
                         .arg(s->size.width())
                         .arg(s->size.height())
                         .arg(s->ppi)
                         .arg(s->colorDescription);
    result.report =
        QStringLiteral(
            "Source SHA-256: %1\n%2\nSaved raster pixels are base pixels, "
            "separate from opacity, bitmap masks and modern effects.\n")
            .arg(s->fingerprint, result.summary);
    for (size_t i = 0; i < s->records.size(); ++i) {
      check(job);
      const auto &rec = s->records[i];
      if (rec.section == 3)
        continue;
      PsdLayerInfo info;
      info.sourceIndex = int(i);
      info.parent = rec.parent;
      info.name = rec.name;
      info.visible = rec.visible;
      info.container = rec.section == 1 || rec.section == 2;
      info.type = info.container              ? "Group"
                  : isAdjustment(rec)         ? "Adjustment"
                  : rec.tags.contains("TySh") ? "Text"
                  : isShape(rec)              ? "Shape"
                                              : "Raster";
      info.basePixels = !info.container && !isAdjustment(rec) && savedPixels(rec);
      info.savedRaster.available = info.basePixels;
      info.savedRaster.vectorShapeBaked = isShape(rec);
      const bool localGroup=info.container&&std::ranges::any_of(s->records,[&](const auto& child){return child.parent==int(i)&&isAdjustment(child);});
      info.issues = restrictions(rec,localGroup);
      Model model;
      bool effectsOk = true, typedOk = true;
      try {
        model = modelFor(*s, rec, {}, false);
      } catch (const std::bad_alloc &) {
        throw;
      } catch (const std::exception &e) {
        effectsOk = false;
        info.issues << QString::fromUtf8(e.what());
      }
      if (!info.container)
        try {
          if (isAdjustment(rec))adjustmentModel(*s,rec,model);
          else if (rec.tags.contains("TySh"))
            textModel(*s, rec, {}, model);
          else if (isShape(rec))
            shapeModel(*s, rec, model);
        } catch (const std::bad_alloc &) {
          throw;
        } catch (const std::exception &e) {
          typedOk = false;
          info.issues << QString::fromUtf8(e.what());
        }
      const auto external = restrictions(rec,localGroup);
      info.raster = info.basePixels && external.empty() && effectsOk;
      info.editable =
          typedOk && effectsOk && external.empty() &&
          (info.container || model.text || model.shape || model.adjustment || info.basePixels);
      for (auto it = model.replacements.begin(); it != model.replacements.end();
           ++it) {
        info.fonts << it.key();
        info.proposedFonts[it.key()] = it.value().toString();
      }
      info.issues += model.substitutions;
      info.issues.removeDuplicates();
      info.suggested = info.editable ? PsdRoute::Editable
                       : info.raster ? PsdRoute::Raster
                                     : PsdRoute::Skip;
      info.status = info.editable
                        ? (info.issues.empty() ? "Editable"
                                               : "Editable with substitution")
                    : info.raster ? "Saved raster available"
                                  : "Review required";
      info.details =
          QStringLiteral("Record %1; bounds (%2, %3), %4 × %5; opacity %6/255; "
                         "Fill %7/255; blend %8; channels %9.\n%10")
              .arg(i)
              .arg(rec.bounds.x())
              .arg(rec.bounds.y())
              .arg(rec.bounds.width())
              .arg(rec.bounds.height())
              .arg(rec.opacity)
              .arg(rec.fill)
              .arg(QString::fromLatin1(rec.blend))
              .arg(rec.channels.size())
              .arg(info.issues.join('\n'));
      result.report += info.name + ": " + info.type + " → " + info.status +
                       "\n" + info.details + "\n";
      result.layers.push_back(std::move(info));
    }
    // Clipping chains are sibling-scoped. A hidden base is still the base.
    QMap<int,int> bases;
    for(auto& layer:result.layers) {
      const auto& rec=s->records[size_t(layer.sourceIndex)];
      if(!rec.clipping) { bases[layer.parent]=layer.sourceIndex; continue; }
      const int base=bases.value(layer.parent,-1);
      const bool operatorBase=base>=0&&isAdjustment(s->records[size_t(base)]);
      const bool grouped=base>=0 && !operatorBase && (!s->records[size_t(base)].tags.contains("clbl")
          || (!s->records[size_t(base)].tags.value("clbl").isEmpty() && s->records[size_t(base)].tags.value("clbl").at(0)!=0));
      if(grouped) {
        layer.clippingBase=base;
        layer.status += " · Clipped";
        layer.details += QStringLiteral("\nNative clipping group; base record %1. Base opacity/blend governs the stack; upper styles stay clipped.").arg(base);
        if(s->records[size_t(base)].tags.contains("lfx2") || rec.tags.contains("lfx2")) {
          layer.issues << "Native clipping style order: base styles follow stack colors; upper styles are clipped to base content coverage";
          layer.details += '\n'+layer.issues.back();
        }
      } else {
        layer.editable=layer.raster=false;layer.suggested=PsdRoute::Skip;layer.status="Review required";
        layer.issues << (base<0 ? "Clipping chain has no base in this group" : operatorBase ? "An adjustment as clipping base has no native drawable coverage; use the saved composite or review the omission." : "Blend Clipped Layers As Group is disabled; use saved composite or explicitly import base pixels only");
      }
    }
    for(const auto& layer:result.layers)if(layer.clippingBase>=0)result.report+=layer.name+": "+layer.details+'\n';
    // Unsupported parent compositing cannot silently become organizational.
    for (auto &layer : result.layers)
      for (int parent = layer.parent; parent >= 0;
           parent = s->records[size_t(parent)].parent) {
        auto p = std::find_if(
            result.layers.begin(), result.layers.end(),
            [parent](const auto &l) { return l.sourceIndex == parent; });
        if (p != result.layers.end() && !p->editable) {
          layer.editable = layer.raster = false;
          layer.suggested = PsdRoute::Skip;
          layer.issues << "Ancestor group composition is unsupported; saved "
                          "composite preserves the complete scene";
          layer.status = "Review required";
        }
      }
    // Validate actual full-resolution compatibility bytes, never the thumbnail.
    try {
      if (std::uint64_t(s->size.width()) * uint(s->size.height()) * 8 +
              std::uint64_t(s->bytes.size()) + limits.existingBytes <=
          limits.workingBytes) {
        auto image = composite(*s, job);
        result.savedComposite = !image.isNull();
      }
    } catch (const Cancelled &) {
      throw;
    } catch (const Error &e) {
      result.report +=
          "Saved composite unavailable: " + QString::fromUtf8(e.what()) + '\n';
    }
  } catch (const Cancelled &) {
    result.error = "Cancelled";
  } catch (const std::bad_alloc &) {
    result.error = "Not enough memory to inspect PSD";
  } catch (const std::exception &e) {
    result.error = QString::fromUtf8(e.what());
  }
  return result;
}
PsdOptions defaultPsdOptions(const PsdInspection &in) {
  PsdOptions out;
  for (const auto &entry : in.layers)
    out.layers.push_back({entry.suggested, entry.proposedFonts});
  return out;
}
PsdConversion convertPsd(const PsdInspection &in, const PsdOptions &options,
                         const std::shared_ptr<PsdJob> &job, PsdLimits limits,
                         bool preview) {
  PsdConversion out;
  QString activeLayer;
  QElapsedTimer timer;
  timer.start();
  try {
    check(job);
    if (job) {
      job->renderingPreview = false;
      job->previewPercent = 0;
    }
    limits = resolvedLimits(limits);
    if (!in.source || !in.error.isEmpty())
      throw Error("No valid PSD inspection");
    const auto &s = *in.source;
    auto document = std::make_unique<core::Document>(core::CanvasSpec{
        {uint(in.size.width()), uint(in.size.height())}, in.ppi});
    if (options.composite) {
      if (!in.savedComposite)
        throw Unsupported("Saved compatibility composite unavailable");
      const auto bytes =
          std::uint64_t(in.size.width()) * uint(in.size.height()) * 4;
      admitMemory(out, limits, std::uint64_t(s.bytes.size()), bytes, bytes * 3,
                  false);
      auto loaded = rasterLayerFromImage(composite(s, job), "Saved composite");
      if (!loaded)
        throw Error("Could not normalize saved composite");
      out.rasterBytes =
          std::uint64_t(in.size.width()) * uint(in.size.height()) * 4;
      if (!document->insertLayer(0, std::move(*loaded.layer)))
        throw Error("Could not insert composite");
      out.report =
          "Saved full-document composite → one RasterLayer. All original "
          "visible content is consolidated; per-layer choices do not apply.";
    } else {
      if (options.layers.size() != in.layers.size())
        throw Error("Import plan no longer matches source");
      std::uint64_t required = 0;
      std::uint64_t largestScratch = 0;
      bool derivedPreview = false;
      std::vector<Model> models(in.layers.size());
      std::vector<bool> included(in.layers.size());
      for (size_t i = 0; i < in.layers.size(); ++i) {
        const auto &info = in.layers[i];
        const auto &rec = s.records[size_t(info.sourceIndex)];
        auto route = options.layers[i].route;
        included[i] = route != PsdRoute::Skip;
        for (int parent = info.parent; parent >= 0;
             parent = s.records[size_t(parent)].parent) {
          auto p = std::find_if(
              in.layers.begin(), in.layers.end(),
              [parent](const auto &x) { return x.sourceIndex == parent; });
          if (p != in.layers.end() &&
              options.layers[size_t(p - in.layers.begin())].route ==
                  PsdRoute::Skip)
            included[i] = false;
        }
        if (!included[i])
          continue;
        if(info.clippingBase>=0 && route!=PsdRoute::BasePixels) {
          const auto base=std::ranges::find_if(in.layers,[&](const auto& l){return l.sourceIndex==info.clippingBase;});
          if(base==in.layers.end() || options.layers[size_t(base-in.layers.begin())].route==PsdRoute::Skip)
            throw Error(QStringLiteral("%1 requires clipping base %2. Include the base, skip this member, or choose Base pixels only to release clipping.")
                .arg(info.name,base==in.layers.end()?QStringLiteral("(missing)"):base->name).toStdString());
        }
        if ((route == PsdRoute::Editable && !info.editable) ||
            (route == PsdRoute::Raster && !info.raster) ||
            (route == PsdRoute::BasePixels && !info.basePixels))
          throw Error("Selected import route is unavailable");
        if (!info.container) {
          if (rec.bounds.width() > int(limits.dimension) ||
              rec.bounds.height() > int(limits.dimension))
            throw Error("Layer exceeds GPU dimensions");
          activeLayer = info.name;
          const bool base = route == PsdRoute::BasePixels;
          auto &model = models[i];
          if (!base)
            model = modelFor(s, rec, options.layers[i], route == PsdRoute::Editable);
          else
            model.transform = translation({double(rec.bounds.x()), double(rec.bounds.y())});
          if (!model.text && !model.shape && !model.adjustment) {
            const auto bytes = std::uint64_t(rec.bounds.width()) *
                               uint(rec.bounds.height()) * 4;
            // Decode one layer at a time, then admit its actual compact size.
            // Do not reject a mostly-empty PSD based on full stored rectangles.
            largestScratch = std::max(largestScratch, bytes * 3);
          }
          if (!base && rec.mask.present) {
            const auto bytes = std::uint64_t(rec.mask.bounds.width()) *
                               uint(rec.mask.bounds.height());
            required = memoryAdd(required, bytes);
            largestScratch = std::max(largestScratch, bytes * 2);
          }
          derivedPreview |= bool(model.text) || bool(model.shape) ||
                            core::hasActiveLayerEffects(model.effects);
        }
      }
      for(size_t i=0;i<in.layers.size();++i)if(included[i]&&in.layers[i].container&&s.records[size_t(in.layers[i].sourceIndex)].blend=="norm") {
        bool ownsDomain=false;
        for(size_t j=0;j<in.layers.size();++j)if(included[j]&&in.layers[j].parent==in.layers[i].sourceIndex&&models[j].adjustment)ownsDomain=true;
        if(!ownsDomain)throw Error(QStringLiteral("%1 needs an included scoped adjustment to retain its isolated composition. Include that adjustment, skip the folder, or import the saved composite.").arg(in.layers[i].name).toStdString());
      }
      activeLayer.clear();
      admitMemory(out, limits, std::uint64_t(s.bytes.size()), required,
                  largestScratch, preview && derivedPreview);
      core::LayerTree tree;
      std::vector<core::Layer> layers;
      QMap<int, core::LayerId> ids;
      if (job) {
        job->completed = 0;
        job->total = int(in.layers.size());
      }
      for (size_t i = 0; i < in.layers.size(); ++i)
        if (included[i])
          ids[in.layers[i].sourceIndex] = core::makeLayerId();
      for (size_t i = 0; i < in.layers.size(); ++i) {
        check(job);
        if (job)
          job->completed = int(i);
        const auto &info = in.layers[i];
        const auto &rec = s.records[size_t(info.sourceIndex)];
        if (!included[i]) {
          out.report +=
              info.name + ": not imported (choice/ancestor exclusion).\n";
          continue;
        }
        auto id = ids[info.sourceIndex];
        const auto route = options.layers[i].route;
        activeLayer = info.name;
        if (info.container)
          tree.containers.push_back({id,
                                     info.name.toStdString(),
                                     core::ContainerKind::Folder,
                                     core::ColorLabel::None,
                                     {},
                                     rec.visible});
        else {
          auto &m = models[i];
          bool base = route == PsdRoute::BasePixels;
          if (!base && route == PsdRoute::Raster &&
              (info.savedRaster.bitmapMaskBaked ||
               info.savedRaster.layerEffectsBaked ||
               info.savedRaster.layerOpacityBaked))
            throw Error("Unsupported saved raster baked-state contract");
          core::Layer layer;
          if (m.adjustment) {
            const bool local=info.parent>=0&&s.records[size_t(info.parent)].blend=="norm";
            layer=core::Layer::adjustment(info.name.toStdString(),local?core::AdjustmentScope::ThisGroup:core::AdjustmentScope::AllBelow);
            layer.adjustments=m.adjustment;
          } else if (m.text)
            layer =
                core::Layer::text(info.name.toStdString(), std::move(*m.text));
          else if (m.shape)
            layer = core::Layer::shape(info.name.toStdString(),
                                       std::move(*m.shape));
          else {
            auto image = pixels(s, rec, job);
            if(image.colorSpace().isValid() && image.colorSpace()!=QColorSpace(QColorSpace::SRgb))
              image.convertToColorSpace(QColorSpace::SRgb);
            const auto storage = storedPixelBounds(image, job);
            if(storage != image.rect()) image = image.copy(storage);
            required = memoryAdd(required,std::uint64_t(storage.width())*uint(storage.height())*4);
            admitMemory(out,limits,std::uint64_t(s.bytes.size()),required,largestScratch,preview&&derivedPreview);
            auto loaded = rasterLayerFromImage(image, info.name);
            if (!loaded)
              throw Error("Layer raster normalization failed");
            layer = std::move(*loaded.layer);
            if(storage != QRect(0,0,rec.bounds.width(),rec.bounds.height())) {
              layer.rasterOrigin={double(storage.x()),double(storage.y())};
              layer.rasterEffectFrame=core::RectD{0,0,double(rec.bounds.width()),double(rec.bounds.height())};
            }
          }
          layer.id = id;
          layer.visible = rec.visible;
          layer.localToDocument = m.transform;
          layer.opacity = float(rec.opacity) / 255;
          layer.blendMode = blend(rec.blend).value_or(core::BlendMode::Normal);
          layer.effects = m.effects;
          if (!base)
            layer.mask = maskFor(s, rec, layer.localToDocument, job);
          layers.push_back(std::move(layer));
          out.report +=
              info.name + ": " + info.type + " → " +
              (m.adjustment ? "AdjustmentLayer"
               : m.text    ? "TextLayer"
               : m.shape ? "ShapeLayer"
                         : "RasterLayer") +
              (base ? " (base pixels; unsupported composition/masks/effects "
                      "omitted)"
                    : "; opacity/blend/mask/effects remain separate") +
              '\n' + m.substitutions.join('\n') + '\n';
        }
      }
      for (size_t i = 0; i < in.layers.size(); ++i)
        if (included[i]) {
          const auto &info = in.layers[i];
          auto parent = info.parent;
          auto *siblings = tree.children(parent < 0 ? 0 : ids.value(parent));
          if (!siblings)
            throw Error("Invalid import hierarchy");
          siblings->push_back(ids[info.sourceIndex]);
        }
      if (layers.empty())
        throw Error("Choose at least one renderable layer");
      QMap<int,std::vector<core::LayerId>> clippingChains;
      for(size_t i=0;i<in.layers.size();++i) {
        const auto& info=in.layers[i];
        if(!included[i] || info.clippingBase<0 || options.layers[i].route==PsdRoute::BasePixels)continue;
        auto& chain=clippingChains[info.clippingBase];
        if(chain.empty())chain.push_back(ids.value(info.clippingBase));
        chain.push_back(ids.value(info.sourceIndex));
      }
      for(auto it=clippingChains.begin();it!=clippingChains.end();++it) {
        const auto& children=it.value();
        const auto position=tree.placement(children.back());
        if(!position)throw Error("Clipping base was omitted by an ancestor choice");
        const auto id=core::makeLayerId();
        tree.containers.push_back({id,"Clipping Mask Group",core::ContainerKind::ClippingMaskGroup,core::ColorLabel::None,{}});
        auto& siblings=*tree.children(position->parent);
        siblings.insert(siblings.begin()+std::ptrdiff_t(position->index+1),id);
        if(!tree.reparent(children,{id,0}))throw Error("Invalid clipping hierarchy");
        out.report+=QStringLiteral("Clipping Mask Group: base record %1 + %2 clipped members; editable native children.\n").arg(it.key()).arg(children.size()-1);
      }
      if (!document->replaceStructure(document->tree(), std::move(tree), {},
                                      layers))
        throw Error("Invalid converted layer tree");
    }
    document->markUnsaved();
    activeLayer.clear();
    check(job);
    out.preparationMilliseconds = timer.elapsed();
    if (preview) {
      auto rendered = renderImportPreview(*document, job);
      if (!rendered) {
        if (rendered.cancelled)
          throw Cancelled();
        throw Error(rendered.error.toStdString());
      }
      out.preview = std::move(rendered.image);
      out.previewMilliseconds = timer.elapsed() - out.preparationMilliseconds;
    }
    check(job);
    out.document = std::move(document);
    if (job)
      job->completed = job->total.load();
    out.milliseconds = timer.elapsed();
  } catch (const Cancelled &) {
    out.cancelled = true;
  } catch (const std::bad_alloc &) {
    out.error = "Not enough memory to convert this PSD";
  } catch (const std::exception &e) {
    out.error =
        activeLayer.isEmpty()
            ? QString::fromUtf8(e.what())
            : QStringLiteral(
                  "Layer ‘%1’: %2. You can exclude this layer and retry.")
                  .arg(activeLayer, QString::fromUtf8(e.what()));
  }
  return out;
}
std::future<PsdInspection>
inspectPsdAsync(QString path, std::shared_ptr<PsdJob> job, PsdLimits limits) {
  return submit([path = std::move(path), job = std::move(job), limits] {
    return inspectPsd(path, job, limits);
  });
}
std::future<PsdConversion> convertPsdAsync(PsdInspection inspection,
                                           PsdOptions options,
                                           std::shared_ptr<PsdJob> job,
                                           PsdLimits limits, bool preview) {
  return submit([inspection = std::move(inspection),
                 options = std::move(options), job = std::move(job), limits,
                 preview] {
    return convertPsd(inspection, options, job, limits, preview);
  });
}
std::future<PsdConversion> previewPsdImportAsync(PsdConversion prepared,
    std::shared_ptr<PsdJob> job, PsdLimits limits) {
  return submit([out = std::move(prepared), job = std::move(job), limits]() mutable {
    QElapsedTimer timer;
    timer.start();
    out.cancelled = false;
    out.error.clear();
    try {
      check(job);
      if (!out.document) throw Error("No prepared PSD import to preview");
      if (!out.preview.isNull()) return std::move(out);
      const auto budget = resolvedLimits(limits);
      const auto estimate = memoryAdd(out.estimatedWorkingBytes,
          FlattenedDocumentLimits{}.derivedCacheBytes + 2 * MiB);
      if (budget.workingBytes <= budget.existingBytes ||
          estimate > budget.workingBytes - budget.existingBytes)
        throw Error("Not enough working memory for a preview. Disable preview to import without it.");
      auto rendered = renderImportPreview(*out.document, job);
      if (!rendered) {
        if (rendered.cancelled) throw Cancelled();
        throw Error(rendered.error.toStdString());
      }
      check(job);
      out.preview = std::move(rendered.image);
      out.previewMilliseconds = timer.elapsed();
      out.milliseconds = out.preparationMilliseconds + out.previewMilliseconds;
      out.estimatedWorkingBytes = estimate;
    } catch (const Cancelled &) {
      out.cancelled = true;
    } catch (const std::exception &e) {
      out.error = QString::fromUtf8(e.what());
    }
    return std::move(out);
  });
}
} // namespace imageeditor::ui
