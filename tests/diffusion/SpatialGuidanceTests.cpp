#include "SpatialGuidance.h"
#include "FluxSpatial.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "ReferenceSizing.h"
#include <functional>
#include <iostream>
using J = nlohmann::json;
int main()
{
    const auto valid = J { { "version", 1 },
        { "regions",
            J::array({ { { "x", .2 }, { "y", .1 }, { "width", .3 }, { "height", .4 },
                { "label", "red balloon" } } }) } };
    auto regions = vulkana::diffusion::spatialRegions(valid);
    const auto prompt = vulkana::diffusion::fluxSpatialCaption("A balloon.", regions, J::array(), 0).prompt;
    const auto table = J::parse(prompt.substr(prompt.find('[')));
    if (table[0]["bbox"] != J::array({ 100, 200, 500, 500 }) || table[0]["desc"] != "red balloon")
        return 1;
    const auto rejects = [](const J& value) {
        try {
            vulkana::diffusion::spatialRegions(value);
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    };
    auto bad = valid;
    bad["version"] = 4;
    if (!rejects(bad))
        return 2;
    bad = valid;
    bad["regions"][0]["x"] = -.1;
    if (!rejects(bad))
        return 3;
    bad = valid;
    bad["regions"][0]["width"] = .9;
    if (!rejects(bad))
        return 4;
    bad = valid;
    bad["regions"][0]["label"] = "";
    if (!rejects(bad))
        return 5;
    bad = valid;
    bad["regions"][0]["x"] = ".2";
    if (!rejects(bad))
        return 6;
    bad = valid;
    bad["regions"][0]["extra"] = true;
    if (!rejects(bad))
        return 7;
    bad = valid;
    bad["regions"][0]["width"] = 1e-8;
    try {
        vulkana::diffusion::fluxSpatialCaption("test", vulkana::diffusion::spatialRegions(bad), J::array(), 0);
        return 8;
    } catch (const std::invalid_argument&) {
    }
    auto quoted = valid;
    quoted["regions"][0]["label"] = "a \"red\" [balloon] 日本語";
    const auto text
        = vulkana::diffusion::fluxSpatialCaption("test", vulkana::diffusion::spatialRegions(quoted), J::array(), 0).prompt;
    if (J::parse(text.substr(text.find('[')))[0]["desc"] != quoted["regions"][0]["label"])
        return 9;
    auto perImage = J{{"version", 2}, {"regions", J::array()},
        {"inputRegions", J::array({{{"image", 2}, {"regions", valid["regions"]}},
                                  {{"image", 1}, {"regions", valid["regions"]}}})}};
    if (!vulkana::diffusion::spatialRegions(perImage).empty()) return 10;
    const auto groups = vulkana::diffusion::inputRegionGroups(perImage, 2);
    if (groups.size() != 2 || groups[0]["image"] != 2) return 11;
    const auto crop = vulkana::diffusion::inputRegionCrop(groups[0]["regions"][0], 100, 200);
    if (crop.x != 20 || crop.y != 20 || crop.width != 30 || crop.height != 80) return 12;
    const auto rejectsInput = [](const J& value) {
        try {
            vulkana::diffusion::spatialRegions(value);
            vulkana::diffusion::inputRegionGroups(value, 2);
            return false;
        } catch (const std::invalid_argument&) { return true; }
    };
    bad = perImage; bad["inputRegions"][0]["image"] = 3;
    if (!rejectsInput(bad)) return 13;
    bad = perImage; bad["inputRegions"][0]["image"] = 1;
    if (!rejectsInput(bad)) return 14;
    bad = perImage; bad["inputRegions"][0]["regions"].push_back(valid["regions"][0]);
    if (!rejectsInput(bad)) return 15;
    bad = perImage; bad["inputRegions"][0]["regions"][0]["width"] = 2;
    if (!rejectsInput(bad)) return 16;
    bad = perImage; bad["version"] = 1;
    if (!rejectsInput(bad)) return 17;
    bad = perImage; bad["inputRegions"][0]["path"] = "not-an-image-index";
    if (!rejectsInput(bad)) return 18;
    bad = perImage; bad["inputRegions"][0]["image"] = 1.5;
    if (!rejectsInput(bad)) return 19;
    const auto portrait = vulkana::diffusion::boundedReferenceSize(1161, 1546, 1024);
    if (portrait.width != 800 || portrait.height != 1024) return 20;
    const auto landscape = vulkana::diffusion::boundedReferenceSize(1000, 667, 1024);
    if (landscape.width != 1024 || landscape.height != 672) return 21;
    const auto detail = vulkana::diffusion::boundedReferenceSize(225, 261, 1024);
    if (detail.width != 256 || detail.height != 288) return 22;
    const auto tiny = vulkana::diffusion::boundedReferenceSize(1, 40, 100);
    if (tiny.width != 32 || tiny.height != 64) return 23;
    const unsigned char rgba[]{10, 20, 30, 255, 50, 60, 70, 128};
    const auto padded = vulkana::diffusion::resizeReference(rgba, 2, 1, 4, 64);
    if (padded.size.width != 32 || padded.size.height != 32) return 24;
    // The tiny source remains at native scale, with exact edge and alpha extension.
    if (!std::equal(rgba, rgba + 4, padded.pixels.begin()) ||
        !std::equal(rgba + 4, rgba + 8, padded.pixels.end() - 4)) return 25;
    std::vector<unsigned char> large(160 * 80 * 4, 127);
    const auto smaller = vulkana::diffusion::resizeReference(large.data(), 160, 80, 4, 64);
    if (smaller.size.width != 64 || smaller.size.height != 32 ||
        std::any_of(smaller.pixels.begin(), smaller.pixels.end(), [](auto value){ return value != 127; })) return 26;
    const auto aliases = J::array({{{"name", "Image2/Region1"}, {"sourceImage", "Image2"}, {"detailImage", "Image3"}},
                                  {{"name", "Image2/Region10"}, {"sourceImage", "Image2"}, {"detailImage", "Image4"}},
                                  {{"name", "Image1/猫の顔"}, {"sourceImage", "Image1"}, {"detailImage", "Image5"}}});
    const auto compiled = vulkana::diffusion::inputRegionPrompt("Use Image1 as the base and move the selected cat in Image2/Region1 to Image1", aliases);
    if (!compiled.starts_with("Use Image1 as the base and move the selected cat in Image3 to Image1") ||
        compiled.find("Image3 is the selected crop of Image2.") == std::string::npos ||
        compiled.find("sourceBox") != std::string::npos || compiled.find("Image4") != std::string::npos) return 27;
    const auto multiple = vulkana::diffusion::inputRegionPrompt("Match Image2/Region10 with Image2/Region1 and Image1/猫の顔.", aliases);
    if (!multiple.starts_with("Match Image4 with Image3 and Image5.")) return 28;
    if (vulkana::diffusion::inputRegionPrompt("An image of a cat", aliases) != "An image of a cat") return 29;
    try {
        vulkana::diffusion::inputRegionPrompt("Use Image2/Region100", aliases);
        return 30;
    } catch (const std::invalid_argument&) {}
    if (vulkana::diffusion::inputRegionPrompt("MyImage2/Region1", aliases) != "MyImage2/Region1") return 31;
    auto single = J::array({{{"image", 1}, {"regions", J::array({{{"x", 32./1024}, {"y", 190./1024},
        {"width", 343./1024}, {"height", 664./1024}, {"label", "Region1"}}})}}});
    const auto collar = vulkana::diffusion::fluxSpatialCaption("In <Image1>, add a collar to <Region1>.", J::array(), single, 1);
    if (!collar.prompt.starts_with("In <ref_image_0>, add a collar to <obj_1>. ") ||
        J(collar.rows) != J::array({{{"id", "obj_1"}, {"bbox", J::array({186,31,834,366})}, {"desc", ""}}})) {
        std::cerr << "Unexpected native caption: " << collar.prompt << '\n'; return 32;
    }
    auto two = single; two[0]["image"] = 2;
    const auto transfer = vulkana::diffusion::fluxSpatialCaption("Use <Image1> as the base and move <Image2/Region1> into it.", J::array(), two, 2);
    if (!transfer.prompt.starts_with("Use <ref_image_0> as the base and move <obj_1> into it.") ||
        transfer.rows[0]["from"] != "ref_image_1" || transfer.rows[0]["src_bbox"] != J::array({186,31,834,366}) ||
        !transfer.rows[0]["tgt_bbox"].is_null() || transfer.rows[0].contains("bbox")) return 33;
    try { vulkana::diffusion::fluxSpatialCaption("Use <Region1>", J::array(), two, 2); return 34; }
    catch (const std::invalid_argument&) {}
    two.push_back(single[0]);
    const auto scoped = vulkana::diffusion::fluxSpatialCaption("Match <Image2/Region1> and <Image1/Region1>", regions, two, 2);
    if (!scoped.prompt.starts_with("Match <obj_2> and <obj_1>") || scoped.rows[2]["id"] != "obj_3") return 35;
    const auto raw = "In <ref_image_0>, change <obj_1>. [{\"id\":\"obj_1\",\"bbox\":[186,31,834,366],\"desc\":\"\"}]";
    if (vulkana::diffusion::fluxSpatialCaption(raw, J::array(), J::array(), 1).prompt != raw) return 36;
    try { vulkana::diffusion::fluxSpatialCaption(raw, J::array(), single, 1); return 37; }
    catch (const std::invalid_argument&) {}
    const auto cropAlias = J::array({{{"name", "Image1/Region1"}, {"sourceImage", "Image1"}, {"detailImage", "Image2"}}});
    const auto enhanced = vulkana::diffusion::fluxSpatialCaption("Change <Region1>", J::array(), single, 1, cropAlias);
    if (enhanced.prompt.find("A detail view of <obj_1> is provided in <ref_image_1>.") == std::string::npos) return 38;
    const auto qwenAlias = vulkana::diffusion::inputRegionPrompt("In <Image1>, change <Region1>", cropAlias, 1);
    if (!qwenAlias.starts_with("In Image1, change Image2")) return 39;
    bad = perImage; bad["cropGuidance"] = "true";
    if (!rejectsInput(bad)) return 40;
    const auto named = vulkana::diffusion::resolveSpatialAliases("<Image1/Region1>, <Image1/Region10>",
        {{"Image1/Region1", {"<obj_1>", true}}, {"Image1/Region10", {"<obj_2>", true}}});
    if (named != "<obj_1>, <obj_2>") return 41;
    auto tied = single; tied[0]["regions"][0]["x"] = .0625;
    const auto tiedCaption = vulkana::diffusion::fluxSpatialCaption("Change <Region1>", J::array(), tied, 1);
    if (tiedCaption.rows[0]["bbox"][1] != 62) return 42;
    auto transferInput = single;
    transferInput[0]["image"] = 2;
    auto& source = transferInput[0]["regions"][0];
    source["id"] = "source-id"; source["description"] = "a kitten"; source["targetId"] = "floor-id";
    const auto locations = J::array({{{"id", "floor-id"}, {"label", "Floor"}, {"targetOnly", true},
       {"description", ""}, {"x", .617}, {"y", .679}, {"width", .287}, {"height", .308}}});
    const auto linked = vulkana::diffusion::fluxSpatialCaption("Use <Scene>. Transfer <Image2/Region1> to <Output/Floor>.", locations, transferInput, 2, J::array(), 1, true);
    if (!linked.prompt.starts_with("Use <ref_image_0>. Transfer <obj_1> to the target position of <obj_1>.") || linked.rows.size() != 1 ||
        linked.rows[0]["src_bbox"] != J::array({186,31,834,366}) || linked.rows[0]["tgt_bbox"] != J::array({679,617,987,904}) ||
        linked.rows[0]["desc"] != "a kitten") return 43;
    auto schema3 = J{{"version", 3}, {"outputScene", 1}, {"regions", locations}, {"inputRegions", transferInput}, {"cropGuidance", false}};
    vulkana::diffusion::spatialRegions(schema3); vulkana::diffusion::inputRegionGroups(schema3, 2);
    const auto rejectsTransfer = [&](const J& dest, const J& sources, int scene) {
        try { vulkana::diffusion::fluxSpatialCaption("test", dest, sources, 2, J::array(), scene, true); return false; }
        catch (const std::invalid_argument&) {return true;}
    };
    if (!rejectsTransfer(J::array(), transferInput, 1) || !rejectsTransfer(locations, transferInput, 0)) return 44;
    auto duplicate = locations; duplicate[0]["id"] = "source-id";
    if (!rejectsTransfer(duplicate, transferInput, 1)) return 45;
    auto unlinked = transferInput; unlinked[0]["regions"][0].erase("targetId");
    const auto noTargetObject = vulkana::diffusion::fluxSpatialCaption("Edit <Image2/Region1>", locations, unlinked, 2, J::array(), 1, true);
    if (noTargetObject.rows.size() != 1 || !noTargetObject.rows[0]["tgt_bbox"].is_null()) return 46;
    const auto noInventedDescription = vulkana::diffusion::fluxSpatialCaption("Edit <Image1>", regions, J::array(), 1, J::array(), 1, true);
    if (noInventedDescription.rows[0]["desc"] != "") return 47;
    const auto singleV3 = vulkana::diffusion::fluxSpatialCaption("In <Image1>, add a collar to <Region1>.", locations, single, 1, J::array(), 1, true);
    if (singleV3.prompt != collar.prompt) return 48; // orphan locations cannot regress single-image formatting
    bad = schema3; bad["inputRegions"][0]["regions"][0]["description"] = 7;
    if (!rejectsInput(bad)) return 49;
    bad = schema3; bad["outputScene"] = 1.5;
    if (!rejectsInput(bad)) return 50;
    std::cout << "Spatial schema, axis order, reference sizing, bounds and escaped labels passed\n";
}
