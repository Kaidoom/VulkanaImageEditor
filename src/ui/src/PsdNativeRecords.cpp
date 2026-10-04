#include "PsdNativeRecords.hpp"
#include "PsdReader.hpp"
#include "imageeditor/ui/QtTextLayoutService.hpp"
#include <QFontDatabase>
#include <QRawFont>
#include <QTextBlock>
#include <QTextLayout>
#include <bit>

namespace imageeditor::ui::psdwrite {
namespace {
using Map = QVariantMap;
using List = QVariantList;
QString faceName(const QRawFont &font) {
  // The actual resolved SFNT name, not QFont's possibly substituted request.
  try {
    auto data = font.fontTable("name");
    psd::Reader r(data);
    r.skip(2);
    auto count = r.u16(), offset = r.u16();
    require(count <= 4096, "Invalid font name table");
    for (int i = 0; i < count; ++i) {
      auto platform = r.u16();
      r.skip(4);
      auto id = r.u16(), length = r.u16(), pos = r.u16();
      if (id != 6)
        continue;
      psd::Reader name(data);
      name.skip(qsizetype(offset) + pos);
      auto bytes = name.take(length);
      return platform == 0 || platform == 3 ? psd::utf16(bytes)
                                            : QString::fromLatin1(bytes);
    }
  } catch (const std::exception &) {
  }
  return {};
}
Map fill(core::Rgba8 c) {
  return {{"Type", 1},
          {"Values",
           List{c.alpha / 255., c.red / 255., c.green / 255., c.blue / 255.}}};
}
Map paragraph(int align, double leading = 1.2) {
  return {{"Justification", align},
          {"FirstLineIndent", 0.},
          {"StartIndent", 0.},
          {"EndIndent", 0.},
          {"SpaceBefore", 0.},
          {"SpaceAfter", 0.},
          {"AutoHyphenate", false},
          {"WordSpacing", List{.8, 1., 1.33}},
          {"LetterSpacing", List{0., 0., 0.}},
          {"GlyphSpacing", List{1., 1., 1.}},
          {"AutoLeading", leading},
          {"LeadingType", 0},
          {"Hanging", false},
          {"Burasagari", false},
          {"KinsokuOrder", 0},
          {"EveryLineComposer", false}};
}
Map style(const core::TextStyle &s, int font) {
  return {{"Font", font},
          {"FontSize", s.sizePixels},
          {"FauxBold", false},
          {"FauxItalic", false},
          {"AutoLeading", true},
          {"Leading", 0.},
          {"HorizontalScale", 1.},
          {"VerticalScale", 1.},
          {"Tracking", 0},
          {"AutoKerning", true},
          {"Kerning", 0.},
          {"BaselineShift", 0.},
          {"FontCaps", 0},
          {"FontBaseline", 0},
          {"Underline", false},
          {"Strikethrough", false},
          {"Ligatures", true},
          {"DLigatures", false},
          {"BaselineDirection", 2},
          {"Tsume", 0.},
          {"StyleRunAlignment", 2},
          {"Language", 0},
          {"NoBreak", false},
          {"FillColor", fill(s.color)},
          {"StrokeColor", fill({0, 0, 0, 255})},
          {"FillFlag", true},
          {"StrokeFlag", false},
          {"FillFirst", true},
          {"YUnderline", 1},
          {"OutlineWidth", 1.},
          {"CharacterDirection", 0},
          {"HindiNumbers", false},
          {"Kashida", 1},
          {"DiacriticPos", 2}};
}
void f32(QByteArray &b, double n) {
  require(std::isfinite(n) && std::abs(n) < 1e9, "Invalid text bounds");
  u32(b, std::bit_cast<quint32>(float(n)));
}
struct Knot {
  core::Vec2d in, anchor, out;
};
} // namespace
bool similarity(const core::AffineTransform &t, double *scale) {
  const auto x = std::hypot(t.m00, t.m10), y = std::hypot(t.m01, t.m11);
  if (!t.isAffine() || x < 1e-9 || std::abs(x - y) > 1e-7 * std::max(x, y) ||
      std::abs(t.m00 * t.m01 + t.m10 * t.m11) > 1e-7 * x * y)
    return false;
  if (scale)
    *scale = x;
  return true;
}
QByteArray textRecord(const core::Layer &layer, QStringList *fonts,
                      QStringList *differences) {
  require(layer.localToDocument.isAffine(),
          "Projective text must be baked into pixels");
  auto text = core::normalizedText(std::get<core::TextLayer>(layer.payload));
  QtTextLayout layout(text);
  core::Utf8TextIndex index(text.utf8);
  auto string = QString::fromStdString(text.utf8);
  string.replace('\n', '\r');
  string += '\r';
  List fontSet, runs, lengths, paragraphs, paragraphLengths;
  QStringList faces;
  auto styles = text.runs;
  if (styles.empty())
    styles.push_back({0, 0, text.defaultStyle});
  Map defaults;
  for (size_t i = 0; i < styles.size(); ++i) {
    const auto &s = styles[i];
    auto font = QtTextLayout::format(s.style).font();
    auto raw = QRawFont::fromFont(font);
    auto name = faceName(raw);
    require(
        raw.isValid() && !name.isEmpty(),
        "Resolved font has no usable PostScript identity; rasterize this text");
    require(
        int(raw.weight()) == s.style.font.weight &&
            (raw.style() != QFont::StyleNormal) == s.style.font.italic,
        "Synthetic bold/italic has no tested PSD mapping; rasterize this text");
    const int first = int(index.utf16Offset(s.start)),
              last = int(index.utf16Offset(s.start + s.length));
    for (auto block = layout.document().begin(); block.isValid();
         block = block.next()) {
      const auto a = std::max(first, block.position()),
                 b = std::min(last, block.position() + block.length() - 1);
      if (b <= a)
        continue;
      for (auto &glyphRun :
           block.layout()->glyphRuns(a - block.position(), b - a))
        require(faceName(glyphRun.rawFont()) == name,
                "This run uses fallback fonts; rasterize it to preserve the "
                "actual glyphs");
    }
    if (differences &&
        (!s.style.font.originalFace.empty() ||
         raw.familyName().compare(QString::fromStdString(s.style.font.family),
                                  Qt::CaseInsensitive) != 0))
      *differences << QStringLiteral("Current resolved face %1 is exported, "
                                     "not the original font request.")
                          .arg(name);
    int f = int(faces.indexOf(name));
    if (f < 0) {
      f = int(faces.size());
      faces << name;
      fontSet << Map{
          {"Name", name}, {"Script", 0}, {"FontType", 0}, {"Synthetic", 0}};
    }
    auto properties = style(s.style, f);
    if (i == 0)
      defaults = properties;
    runs << Map{{"StyleSheet", Map{{"StyleSheetData", properties}}}};
    lengths << last - first + (i + 1 == styles.size() ? 1 : 0);
  }
  for (auto block = layout.document().begin(); block.isValid();
       block = block.next()) {
    auto align = block.blockFormat().alignment();
    int a = align.testFlag(Qt::AlignRight)     ? 1
            : align.testFlag(Qt::AlignHCenter) ? 2
                                               : 0;
    const auto line = block.layout()->lineAt(0);
    double em = 0;
    for (auto &s : styles) {
      int first = int(index.utf16Offset(s.start)),
          last = int(index.utf16Offset(s.start + s.length));
      if (last >= block.position() && first < block.position() + block.length())
        em = std::max(em, s.style.sizePixels);
    }
    double leading = line.isValid() ? line.height() / std::max(1., em) : 1.2;
    paragraphs << Map{
        {"ParagraphSheet",
         Map{{"DefaultStyleSheet", 0}, {"Properties", paragraph(a, leading)}}},
        {"Adjustments", Map{{"Axis", List{1, 0, 1}}, {"XY", List{0, 0}}}}};
    paragraphLengths << block.length();
  }
  auto block = layout.document().begin();
  auto line = block.layout()->lineAt(0);
  require(line.isValid(), "Cannot lay out PSD text");
  const auto baseline =
      block.layout()->position().y() + line.y() + line.ascent();
  const auto alignment = text.paragraphs.front().alignment;
  double originX =
      alignment == core::TextAlignment::Center  ? layout.bounds().width() / 2
      : alignment == core::TextAlignment::Right ? layout.bounds().width()
                                                : 0;
  auto t = core::composeTransform(
      layer.localToDocument,
      core::AffineTransform{.m02 = originX, .m12 = baseline});
  Map resources{{"TheNormalStyleSheet", 0},
                {"TheNormalParagraphSheet", 0},
                {"FontSet", fontSet},
                {"StyleSheetSet", List{Map{{"Name", "Normal RGB"},
                                           {"StyleSheetData", defaults}}}},
                {"ParagraphSheetSet", List{Map{{"Name", "Normal RGB"},
                                               {"DefaultStyleSheet", 0},
                                               {"Properties", paragraph(0)}}}},
                {"SuperscriptSize", .583},
                {"SuperscriptPosition", .333},
                {"SubscriptSize", .583},
                {"SubscriptPosition", .333},
                {"SmallCapSize", .7}};
  Map shapeBase{{"ShapeType", 0},
                {"TransformPoint0", List{1., 0.}},
                {"TransformPoint1", List{0., 1.}},
                {"TransformPoint2", List{0., 0.}}};
  Map cookie{
      {"ShapeType", 0}, {"PointBase", List{0., 0.}}, {"Base", shapeBase}};
  Map shape{{"ShapeType", 0},
            {"Procession", 0},
            {"Lines", Map{{"WritingDirection", 0}, {"Children", List{}}}},
            {"Cookie", Map{{"Photoshop", cookie}}}};
  Map rendered{
      {"Version", 1},
      {"Shapes", Map{{"WritingDirection", 0}, {"Children", List{shape}}}}};
  Map engineDict{
      {"Editor", Map{{"Text", string}}},
      {"StyleRun", Map{{"DefaultRunData",
                        Map{{"StyleSheet", Map{{"StyleSheetData", Map{}}}}}},
                       {"RunArray", runs},
                       {"RunLengthArray", lengths},
                       {"IsJoinable", 2}}},
      {"ParagraphRun", Map{{"DefaultRunData",
                            Map{{"ParagraphSheet", Map{{"DefaultStyleSheet", 0},
                                                       {"Properties", Map{}}}},
                                {"Adjustments", Map{{"Axis", List{1, 0, 1}},
                                                    {"XY", List{0, 0}}}}}},
                           {"RunArray", paragraphs},
                           {"RunLengthArray", paragraphLengths},
                           {"IsJoinable", 1}}},
      {"AntiAlias", 3},
      {"UseFractionalGlyphWidths", true},
      {"GridInfo", Map{{"GridIsOn", false},
                       {"ShowGrid", false},
                       {"GridSize", 18.},
                       {"GridLeading", 22.},
                       {"GridColor", fill({0, 0, 255, 255})},
                       {"GridLeadingFillColor", fill({0, 0, 255, 255})},
                       {"AlignLineHeightToGridFlags", false}}},
      {"Rendered", rendered}};
  QByteArray b;
  u16(b, 1);
  for (double n : {t.m00, t.m10, t.m01, t.m11, t.m02, t.m12})
    f64(b, n);
  u16(b, 50);
  auto plain = string;
  plain.chop(1);
  b += descriptor(
      {{"Txt ", plain},
       {"textGridding", enumeration("textGridding", "None")},
       {"Ornt", enumeration("Ornt", "Hrzn")},
       {"AntA", enumeration("Annt", "AnSm")},
       {"TextIndex", 0},
       {"EngineData", engine(Map{{"EngineDict", engineDict},
                                 {"ResourceDict", resources},
                                 {"DocumentResources", resources}})}},
      "TxLr");
  u16(b, 1);
  b += descriptor({{"warpStyle", enumeration("warpStyle", "warpNone")},
                   {"warpValue", 0.},
                   {"warpPerspective", 0.},
                   {"warpPerspectiveOther", 0.},
                   {"warpRotate", enumeration("Ornt", "Hrzn")}},
                  "warp");
  for (double n : {0., 0., layout.bounds().width(), layout.bounds().height()})
    f32(b, n);
  if (fonts)
    *fonts = faces;
  return b;
}
QMap<QByteArray, QByteArray> shapeRecords(const core::Layer &layer,
                                          core::CanvasSpec canvas) {
  auto &s = std::get<core::ShapeLayer>(layer.payload);
  double scale = 1;
  require(layer.localToDocument.isAffine(),
          "Projective shapes must be baked into pixels");
  require(!s.strokeEnabled || similarity(layer.localToDocument, &scale),
          "Nonuniform/sheared vector strokes must be baked");
  require(s.fillColor.alpha == 255 || !s.fillEnabled,
          "Translucent vector fills must be baked");
  require(s.kind != core::ShapeKind::Line &&
              s.kind != core::ShapeKind::RoundedRectangle,
          "This geometry requires raster output in PSD V1");
  std::vector<Knot> knots;
  const auto w = s.size.width, h = s.size.height;
  auto point = [&](core::Vec2d p) { knots.push_back({p, p, p}); };
  if (s.kind == core::ShapeKind::Ellipse) {
    constexpr double k = .5522847498307936;
    const auto x = w / 2, y = h / 2;
    knots = {{{x - k * x, 0}, {x, 0}, {x + k * x, 0}},
             {{w, y - k * y}, {w, y}, {w, y + k * y}},
             {{x + k * x, h}, {x, h}, {x - k * x, h}},
             {{0, y + k * y}, {0, y}, {0, y - k * y}}};
  } else if (s.kind == core::ShapeKind::Rectangle) {
    point({0, 0});
    point({w, 0});
    point({w, h});
    point({0, h});
  } else if (s.kind == core::ShapeKind::Triangle) {
    point({w / 2, 0});
    point({w, h});
    point({0, h});
  } else
    for (auto p : s.points)
      point(p);
  require(knots.size() >= 3 && knots.size() <= 65535,
          "PSD vector knot count outside supported range");
  QByteArray path;
  u32(path, 3);
  u32(path, 0);
  u16(path, 6);
  path += QByteArray(24, '\0');
  u16(path, 8);
  path += QByteArray(24, '\0');
  u16(path, 0);
  u16(path, int(knots.size()));
  u16(path, 1);
  u16(path, 1);
  path += QByteArray(18, '\0');
  for (auto knot : knots) {
    u16(path, 1);
    for (auto p : {knot.in, knot.anchor, knot.out}) {
      p = layer.localToDocument.map(p);
      for (double n : {p.y / canvas.extent.height, p.x / canvas.extent.width}) {
        auto fixed = std::llround(n * 16777216.);
        require(fixed >= INT32_MIN && fixed <= INT32_MAX,
                "Vector coordinates exceed PSD 8.24 range");
        u32(path, quint32(fixed));
      }
    }
  }
  const auto join =
      s.strokeJoin == core::ShapeJoin::Miter   ? "strokeStyleMiterJoin"
      : s.strokeJoin == core::ShapeJoin::Bevel ? "strokeStyleBevelJoin"
                                               : "strokeStyleRoundJoin";
  const auto cap = s.strokeCap == core::ShapeCap::Butt ? "strokeStyleButtCap"
                   : s.strokeCap == core::ShapeCap::Square
                       ? "strokeStyleSquareCap"
                       : "strokeStyleRoundCap";
  Map stroke{
      {"strokeStyleVersion", 2},
      {"strokeEnabled", s.strokeEnabled},
      {"fillEnabled", s.fillEnabled},
      {"strokeStyleLineWidth", unit(s.strokeWidth * scale)},
      {"strokeStyleLineDashOffset", unit(0)},
      {"strokeStyleMiterLimit", s.strokeMiterLimit * 2},
      {"strokeStyleLineCapType", enumeration("strokeStyleLineCapType", cap)},
      {"strokeStyleLineJoinType", enumeration("strokeStyleLineJoinType", join)},
      {"strokeStyleLineAlignment",
       enumeration("strokeStyleLineAlignment", "strokeStyleAlignCenter")},
      {"strokeStyleScaleLock", false},
      {"strokeStyleStrokeAdjust", false},
      {"strokeStyleLineDashSet", List{}},
      {"strokeStyleBlendMode", enumeration("BlnM", "Nrml")},
      {"strokeStyleOpacity", unit(s.strokeColor.alpha * 100. / 255, "#Prc")},
      {"strokeStyleContent",
       Map{{"$class", "solidColorLayer"}, {"Clr ", rgb(s.strokeColor)}}},
      {"strokeStyleResolution", canvas.dotsPerInch}};
  return {{"vmsk", path},
          {"vscg", QByteArray("SoCo") +
                       descriptor({{"Clr ", rgb(s.fillColor)}}, "null")},
          {"vstk", descriptor(stroke, "strokeStyle")}};
}
QByteArray effectRecord(const core::Layer &layer) {
  if (!layer.effects || *layer.effects == core::LayerEffectStack{})
    return {};
  double scale = 1;
  require(similarity(layer.localToDocument, &scale),
          "Transformed styles need a reviewed raster fallback");
  Map effects{{"masterFXSwitch", true}, {"Scl ", unit(100, "#Prc")}};
  for (size_t i = 0; i < core::layerEffectCount; ++i) {
    auto &e = layer.effects->items[i];
    if (e == core::defaultLayerEffect(core::LayerEffectType(i)))
      continue;
    auto mode = blend(e.blendMode, true);
    constexpr const char *classes[]{"FrFX", "DrSh", "IrSh", "OrGl",
                                    "IrGl", "SoFi", "GrFl", "ebbl"};
    const auto cls = classes[i];
    Map effect{{"$class", cls},
               {"enab", e.enabled},
               {"present", true},
               {"showInDialog", true},
               {"Md  ", enumeration("BlnM", mode.constData())},
               {"Opct", unit(e.opacity * e.color.alpha * 100. / 255, "#Prc")},
               {"Clr ", rgb(e.color)}};
    if(i==7) {
      const auto& b=e.bevel;
      effect.remove("Md  ");effect.remove("Clr ");effect.remove("Opct");
      effect["hglM"]=enumeration("BlnM",mode.constData());effect["hglC"]=rgb(e.color);
      effect["hglO"]=unit(e.opacity*e.color.alpha*100/255.,"#Prc");
      const auto shadowMode=blend(b.shadowBlend,true);
      effect["sdwM"]=enumeration("BlnM",shadowMode.constData());effect["sdwC"]=rgb(e.secondColor);
      effect["sdwO"]=unit(b.shadowOpacity*e.secondColor.alpha*100/255.,"#Prc");
      effect["bvlT"]=enumeration("bvlT","SfBL");
      effect["bvlS"]=enumeration("BESl",b.style==core::BevelStyle::Inner?"InrB":b.style==core::BevelStyle::Outer?"OtrB":"Embs");
      effect["bvlD"]=enumeration("BESs",b.down?"Out ":"In  ");
      const auto angle=e.angle*std::numbers::pi/180;
      const auto direction=layer.localToDocument.map({std::cos(angle),std::sin(angle)})-layer.localToDocument.map({0,0});
      effect["uglg"]=false;effect["lagl"]=unit(-std::atan2(direction.y,direction.x)*180/std::numbers::pi,"#Ang");
      effect["Lald"]=unit(b.altitude,"#Ang");effect["srgR"]=unit(b.depth*100,"#Prc");
      effect["blur"]=unit(e.size*scale);effect["Sftn"]=unit(b.soften*scale);
      const auto curve=[](const core::EffectContour& c,bool honorEnabled) {
        List points;
        const auto effective=honorEnabled&&!c.enabled?core::EffectContour{}:c;
        for(const auto& p:effective.points)points<<Map{{"$class","CrPt"},{"Hrzn",p.input*255},{"Vrtc",p.output*255},
          {"Cnty",!p.corner&&effective.interpolation==core::EffectContourInterpolation::Smooth}};
        return Map{{"$class","ShpC"},{"Nm  ",QString("Vulkana contour")},{"Crv ",points}};
      };
      effect["TrnS"]=curve(b.gloss,true);effect["antialiasGloss"]=true;
      effect["useShape"]=b.surface.enabled;effect["MpgS"]=curve(b.surface,false);
      effect["AntA"]=true;effect["Inpr"]=unit(100,"#Prc");effect["useTexture"]=false;
    } else if (i == 0) {
      effect["Styl"] = enumeration(
          "FStl", e.position == core::StrokePosition::Inside   ? "InsF"
                  : e.position == core::StrokePosition::Center ? "CtrF"
                                                               : "OutF");
      effect["PntT"] = enumeration("FrFl", "SClr");
      effect["Sz  "] = unit(e.size * scale);
      effect["overprint"] = false;
    } else if (i >= 1 && i <= 4) {
      // These are explicit editable approximations: PSD contour/spread and
      // softness are not the native distance/Gaussian profile. No cached
      // shadow pixels are also written into the source representation.
      effect["blur"] = unit(e.size * scale);
      effect["Ckmt"] = unit(e.spread * e.size * scale);
      effect["Nose"] = unit(0, "#Prc");
      effect["AntA"] = true;
      effect["TrnS"] =
          Map{{"$class", "ShpC"},
              {"Nm  ", QString("Linear")},
              {"Crv ",
               List{Map{{"$class", "CrPt"}, {"Hrzn", 0.}, {"Vrtc", 0.}},
                    Map{{"$class", "CrPt"}, {"Hrzn", 255.}, {"Vrtc", 255.}}}}};
      if (i == 1 || i == 2) {
        const auto radians = e.angle * std::numbers::pi / 180;
        const auto origin = layer.localToDocument.map({0, 0});
        const auto direction =
            layer.localToDocument.map({std::cos(radians), std::sin(radians)}) -
            origin;
        const double angle = std::remainder(
            180 - std::atan2(direction.y, direction.x) * 180 / std::numbers::pi,
            360.);
        effect["uglg"] = false;
        effect["lagl"] = unit(angle, "#Ang");
        effect["Dstn"] = unit(e.distance * scale);
        if (i == 1)
          effect["layerConceals"] = true;
      } else {
        effect["GlwT"] = enumeration("BETE", "SfBL");
        effect["Inpr"] = unit(50, "#Prc");
        effect["ShdN"] = unit(0, "#Prc");
        if (i == 4)
          effect["glwS"] = enumeration("IGSr", "SrcE");
      }
    } else if (i == 6) {
      effect.remove("Clr ");
      effect["Opct"] = unit(e.opacity * 100, "#Prc");
      const auto radians = e.angle * std::numbers::pi / 180;
      const auto origin = layer.localToDocument.map({0, 0});
      const auto direction =
          layer.localToDocument.map({std::cos(radians), std::sin(radians)}) -
          origin;
      effect["Angl"] =
          unit(-std::atan2(direction.y, direction.x) * 180 / std::numbers::pi,
               "#Ang");
      effect["Type"] = enumeration(
          "GrdT", e.gradient == core::GradientType::Linear ? "Lnr " : "Rdl ");
      effect["Rvrs"] = e.reverse;
      effect["Algn"] = true;
      effect["Dthr"] = false;
      effect["Scl "] = unit(e.scale * 100, "#Prc");
      effect["Ofst"] = Map{{"$class", "Pnt "},
                           {"Hrzn", unit(0, "#Prc")},
                           {"Vrtc", unit(0, "#Prc")}};
      List colors, alpha;
      for (int endpoint = 0; endpoint < 2; ++endpoint) {
        auto color = endpoint ? e.secondColor : e.color;
        colors << Map{{"$class", "Clrt"},
                      {"Clr ", rgb(color)},
                      {"Type", enumeration("Clry", "UsrS")},
                      {"Lctn", endpoint * 4096},
                      {"Mdpn", 50}};
        alpha << Map{{"$class", "TrnS"},
                     {"Opct", unit(color.alpha * 100. / 255, "#Prc")},
                     {"Lctn", endpoint * 4096},
                     {"Mdpn", 50}};
      }
      effect["Grad"] = Map{{"$class", "Grdn"},
                           {"Nm  ", QString("Vulkana two-color gradient")},
                           {"GrdF", enumeration("GrdF", "CstS")},
                           {"Intr", 4096},
                           {"Clrs", colors},
                           {"Trns", alpha}};
    }
    effects[cls] = effect;
  }
  QByteArray b;
  u32(b, 0);
  return b + descriptor(effects, "null");
}
QMap<QByteArray,QByteArray> adjustmentRecord(const core::Layer& layer) {
  require(std::holds_alternative<core::AdjustmentLayer>(layer.payload),"Not an adjustment layer");
  const core::Adjustment* chosen=nullptr;
  if(layer.adjustments)for(const auto& a:layer.adjustments->items)if(a.enabled&&!core::adjustmentIsNeutral(a)) {
    require(!chosen,"Several combined corrections require a reviewed group consolidation or Flattened PSD; combined strength cannot be repeated per correction.");
    require(!a.mask,"Per-correction captured masks require group consolidation or Flattened PSD.");
    chosen=&a;
  }
  if(!chosen) {QByteArray data;u16(data,1);u32(data,std::bit_cast<quint32>(0.0F));u32(data,0);u32(data,std::bit_cast<quint32>(1.0F));return {{"expA",data}};}
  if(chosen->type==core::AdjustmentType::Invert)return {{"nvrt",QByteArray{}}};
  require(chosen->type==core::AdjustmentType::Exposure,"This correction has no verified native PSD mapping. Consolidate its group or use Flattened PSD.");
  QByteArray data;u16(data,1);u32(data,std::bit_cast<quint32>(float(std::get<core::ExposureParameters>(chosen->parameters).stops)));
  u32(data,0);u32(data,std::bit_cast<quint32>(1.0F));return {{"expA",data}};
}
} // namespace imageeditor::ui::psdwrite
