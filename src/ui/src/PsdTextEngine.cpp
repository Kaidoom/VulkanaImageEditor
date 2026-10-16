#include "PsdTextEngine.hpp"
#include "PsdTextDefaults.hpp"
#include "PsdWriter.hpp"

namespace imageeditor::ui::psdwrite {
namespace {
using Map = QVariantMap;
using List = QVariantList;
using Field = std::pair<const char *, const char *>;
Map fields(const Map &source, std::initializer_list<Field> mapping) {
  Map result;
  for (auto [key, name] : mapping)
    if (source.contains(name))
      result[key] = source[name];
  return result;
}
Map style(const Map &source) {
  auto result = textEngineDefaults()["1"].toMap()["2"].toMap();
  const auto values = fields(source, {{"0", "Font"},
                                      {"1", "FontSize"},
                                      {"2", "FauxBold"},
                                      {"3", "FauxItalic"},
                                      {"4", "AutoLeading"},
                                      {"5", "Leading"},
                                      {"6", "HorizontalScale"},
                                      {"7", "VerticalScale"},
                                      {"8", "Tracking"},
                                      {"9", "BaselineShift"},
                                      {"12", "FontCaps"},
                                      {"13", "FontBaseline"},
                                      {"18", "Ligatures"},
                                      {"19", "DLigatures"},
                                      {"35", "BaselineDirection"},
                                      {"36", "Tsume"},
                                      {"37", "StyleRunAlignment"},
                                      {"38", "Language"},
                                      {"52", "NoBreak"},
                                      {"56", "FillFlag"},
                                      {"57", "StrokeFlag"},
                                      {"58", "FillFirst"},
                                      {"63", "OutlineWidth"}});
  for (auto it = values.begin(); it != values.end(); ++it)
    result[it.key()] = it.value();
  result["11"] = source.value("AutoKerning", true).toBool() ? 1 : 0;
  result["15"] = 0;
  result["16"] = 0;
  for (auto [key, name] :
       {Field{"53", "FillColor"}, Field{"54", "StrokeColor"}}) {
    const auto color = source[name].toMap();
    result[key] = Map{{"99", QVariant::fromValue(EngineName{"SimplePaint"})},
                      {"0", Map{{"0", color["Type"]}, {"1", color["Values"]}}}};
  }
  return result;
}
Map paragraph(const Map &source) {
  auto result = textEngineDefaults()["1"].toMap()["3"].toMap();
  const auto values = fields(source, {{"0", "Justification"},
                                      {"1", "FirstLineIndent"},
                                      {"2", "StartIndent"},
                                      {"3", "EndIndent"},
                                      {"4", "SpaceBefore"},
                                      {"5", "SpaceAfter"},
                                      {"7", "AutoLeading"},
                                      {"8", "LeadingType"},
                                      {"9", "AutoHyphenate"},
                                      {"17", "WordSpacing"},
                                      {"18", "LetterSpacing"},
                                      {"19", "GlyphSpacing"},
                                      {"21", "Burasagari"},
                                      {"24", "KinsokuOrder"}});
  for (auto it = values.begin(); it != values.end(); ++it)
    result[it.key()] = it.value();
  return result;
}
} // namespace
int TextEngine::append(const Map &body, const Map &resources) {
  const int index = int(objects_.size());
  List remap;
  for (const auto &font : resources["FontSet"].toList()) {
    const auto name = font.toMap()["Name"].toString();
    auto found = fontIndices_.constFind(name);
    if (found == fontIndices_.cend()) {
      const int next = int(fonts_.size());
      fontIndices_.insert(name, next);
      fonts_ << Map{
          {"0", Map{{"99", QVariant::fromValue(EngineName{"CoolTypeFont"})},
                    {"0", Map{{"0", name}, {"2", 1}}}}}};
      remap << next;
    } else
      remap << *found;
  }
  const auto mappedStyle = [&](const Map &properties) {
    auto value = style(properties);
    const int localFont = value["0"].toInt();
    require(localFont >= 0 && localFont < remap.size(),
            "Invalid exported text font index");
    value["0"] = remap[localFont];
    return value;
  };
  List styles, paragraphs;
  const auto runs = body["StyleRun"].toMap();
  const auto styleRuns = runs["RunArray"].toList(),
             lengths = runs["RunLengthArray"].toList();
  require(styleRuns.size() == lengths.size(),
          "Invalid exported text style runs");
  for (qsizetype i = 0; i < styleRuns.size(); ++i) {
    const auto properties =
        styleRuns[i].toMap()["StyleSheet"].toMap()["StyleSheetData"].toMap();
    // Unit flag 0 means document pixels, independent of the document's PPI.
    styles << Map{
        {"0",
         Map{{"0", Map{{"0", ""}, {"5", 0}, {"6", mappedStyle(properties)}}}}},
        {"1", lengths[i]}};
  }
  const auto pars = body["ParagraphRun"].toMap();
  const auto parRuns = pars["RunArray"].toList(),
             parLengths = pars["RunLengthArray"].toList();
  require(parRuns.size() == parLengths.size(),
          "Invalid exported text paragraph runs");
  for (qsizetype i = 0; i < parRuns.size(); ++i) {
    const auto properties =
        parRuns[i].toMap()["ParagraphSheet"].toMap()["Properties"].toMap();
    paragraphs << Map{
        {"0",
         Map{{"0", Map{{"0", ""}, {"5", paragraph(properties)}, {"6", 0}}}}},
        {"1", parLengths[i]}};
  }
  // An unbounded point-text frame. No stale composed glyph/advance cache: the
  // receiving text engine lays out these current runs using its installed font.
  frames_ << Map{{"0", Map{{"2", Map{{"6", List{-1., -1.}},
                                     {"11", Map{{"4", -1}, {"7", false}}}}}}}};
  objects_ << Map{{"0", Map{{"0", body["Editor"].toMap()["Text"]},
                            {"5", Map{{"0", paragraphs}}},
                            {"6", Map{{"0", styles}}},
                            {"7", 0},
                            {"10", Map{{"0", 4}, {"2", true}}}}},
                  {"1", Map{{"0", List{Map{{"0", index}}}}}}};
  return index;
}
QByteArray TextEngine::bytes() const {
  if (objects_.isEmpty())
    return {};
  auto resources = textEngineDefaults()["0"].toMap();
  resources["1"] = Map{{"0", fonts_}};
  resources["8"] = Map{{"0", frames_}};
  auto document = textEngineDefaults()["1"].toMap();
  document["1"] = objects_;
  // Txt2 is a bare dictionary body with the format-version entry first.
  return " /98 " + engine(Map{{"0", 7}}, true) + " /0 " +
         engine(resources, true) + " /1 " + engine(document, true);
}
} // namespace imageeditor::ui::psdwrite
