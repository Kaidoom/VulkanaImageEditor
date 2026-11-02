#include "SpatialGuidance.h"
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
    const auto prompt = vulkana::diffusion::flux3RegionPrompt("A balloon.", regions);
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
    bad["version"] = 2;
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
        vulkana::diffusion::flux3RegionPrompt("test", vulkana::diffusion::spatialRegions(bad));
        return 8;
    } catch (const std::invalid_argument&) {
    }
    auto quoted = valid;
    quoted["regions"][0]["label"] = "a \"red\" [balloon] 日本語";
    const auto text
        = vulkana::diffusion::flux3RegionPrompt("test", vulkana::diffusion::spatialRegions(quoted));
    if (J::parse(text.substr(text.find('[')))[0]["desc"] != quoted["regions"][0]["label"])
        return 9;
    std::cout << "Spatial schema, axis order, bounds and escaped labels passed\n";
}
