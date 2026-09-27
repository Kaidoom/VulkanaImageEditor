#include "imageeditor/core/BlendCompositing.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numeric>
#include <span>
#include <string>
#include <string_view>

namespace {
using namespace imageeditor::core;
using Color = std::array<double, 3>;
using Pixel = std::array<double, 4>;

int failures = 0;
void near(double actual, double expected, double tolerance, std::string_view what)
{
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        if (failures < 30)
            std::cerr << what << ": actual " << actual << ", expected " << expected << '\n';
        ++failures;
    }
}
void check(bool condition, std::string_view what)
{
    if (!condition) {
        if (failures < 30) std::cerr << what << '\n';
        ++failures;
    }
}

constexpr std::array modes {
    BlendMode::Normal, BlendMode::Multiply, BlendMode::Screen, BlendMode::Overlay,
    BlendMode::SoftLight, BlendMode::HardLight, BlendMode::Darken, BlendMode::Lighten,
    BlendMode::Difference, BlendMode::Exclusion, BlendMode::Hue, BlendMode::Saturation,
    BlendMode::Color, BlendMode::Luminosity,
    BlendMode::ColorDodge, BlendMode::LinearDodge, BlendMode::ColorBurn,
    BlendMode::LinearBurn, BlendMode::Subtract, BlendMode::Divide,
};
constexpr std::array addedModes {BlendMode::ColorDodge, BlendMode::LinearDodge,
    BlendMode::ColorBurn, BlendMode::LinearBurn, BlendMode::Subtract, BlendMode::Divide};

// Independent double-precision oracle, deliberately not using production color
// conversions or blend helpers. B follows W3C Compositing Level 1, sections 10,
// 10.1.10 and 10.2: https://www.w3.org/TR/compositing-1/#blending
// Vulkana's named B functions operate on straight sRGB; the three coverage terms
// are accumulated in linear premultiplied space. This is not a claim that CSS's
// encoded-space alpha compositing matches Vulkana at fractional alpha.
double linear(double v)
{
    return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4);
}
double encoded(double v)
{
    if (v == 1) return 1;
    return v <= .0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - .055;
}
double luminosity(Color c) { return .30 * c[0] + .59 * c[1] + .11 * c[2]; }
double saturation(Color c) { return *std::ranges::max_element(c) - *std::ranges::min_element(c); }
Color setLuminosity(Color c, double l)
{
    const double delta = l - luminosity(c);
    for (auto& channel : c) channel += delta;
    const double minimum = *std::ranges::min_element(c);
    const double maximum = *std::ranges::max_element(c);
    if (minimum < 0)
        for (auto& channel : c) channel = l + (channel - l) * l / (l - minimum);
    if (maximum > 1)
        for (auto& channel : c) channel = l + (channel - l) * (1 - l) / (maximum - l);
    return c;
}
Color setSaturation(Color c, double amount)
{
    // Ordered indices exercise ties independently of min/max-vector approaches.
    std::array<std::size_t, 3> order {0, 1, 2};
    std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) { return c[a] < c[b]; });
    const auto low = c[order[0]], high = c[order[2]];
    Color result {};
    if (high > low) {
        result[order[1]] = (c[order[1]] - low) * amount / (high - low);
        result[order[2]] = amount;
    }
    return result;
}
Color blendReference(Color b, Color s, BlendMode mode)
{
    switch (mode) {
    case BlendMode::Hue: return setLuminosity(setSaturation(s, saturation(b)), luminosity(b));
    case BlendMode::Saturation: return setLuminosity(setSaturation(b, saturation(s)), luminosity(b));
    case BlendMode::Color: return setLuminosity(s, luminosity(b));
    case BlendMode::Luminosity: return setLuminosity(b, luminosity(s));
    default: break;
    }
    Color result {};
    for (std::size_t i = 0; i != 3; ++i) {
        const auto x = b[i], y = s[i];
        switch (mode) {
        case BlendMode::Normal: result[i] = y; break;
        case BlendMode::Multiply: result[i] = x * y; break;
        case BlendMode::Screen: result[i] = 1 - (1 - x) * (1 - y); break;
        case BlendMode::Overlay:
            result[i] = x <= .5 ? 2 * x * y : 1 - 2 * (1 - x) * (1 - y); break;
        case BlendMode::SoftLight: {
            const auto d = x <= .25 ? ((16 * x - 12) * x + 4) * x : std::sqrt(x);
            result[i] = y <= .5 ? x - (1 - 2 * y) * x * (1 - x) : x + (2 * y - 1) * (d - x);
            break;
        }
        case BlendMode::HardLight:
            result[i] = y <= .5 ? 2 * x * y : 1 - 2 * (1 - x) * (1 - y); break;
        case BlendMode::Darken: result[i] = std::min(x, y); break;
        case BlendMode::Lighten: result[i] = std::max(x, y); break;
        case BlendMode::Difference: result[i] = std::abs(x - y); break;
        case BlendMode::Exclusion: result[i] = x + y - 2 * x * y; break;
        // Reference equations deliberately retain the divisions. In double
        // precision even float's smallest positive denominator is safe here;
        // production float/GLSL may avoid division after proving saturation.
        case BlendMode::ColorDodge:
            result[i] = x == 0 ? 0 : y == 1 ? 1 : std::min(1.,x/(1-y)); break;
        case BlendMode::LinearDodge: result[i] = std::min(1.,x+y); break;
        case BlendMode::ColorBurn:
            result[i] = x == 1 ? 1 : y == 0 ? 0 : 1-std::min(1.,(1-x)/y); break;
        case BlendMode::LinearBurn: result[i] = std::max(0.,x+y-1); break;
        case BlendMode::Subtract: result[i] = std::max(0.,x-y); break;
        case BlendMode::Divide:
            result[i] = x == 0 ? 0 : y == 0 ? 1 : std::min(1.,x/y); break;
        default: std::abort();
        }
    }
    return result;
}
Pixel decodeReference(Rgba8 rgba)
{
    const double alpha = rgba.alpha / 255.0;
    return {linear(rgba.red / 255.0) * alpha, linear(rgba.green / 255.0) * alpha,
        linear(rgba.blue / 255.0) * alpha, alpha};
}
Pixel compositeReference(Pixel b, Pixel s, double opacity, BlendMode mode)
{
    const double alpha = s[3] * opacity;
    Pixel result {};
    result[3] = alpha + b[3] * (1 - alpha);
    if (mode == BlendMode::Normal || b[3] == 0 || alpha == 0) {
        for (std::size_t i = 0; i != 3; ++i) result[i] = s[i] * opacity + b[i] * (1 - alpha);
        return result;
    }
    Color backdrop {}, source {};
    for (std::size_t i = 0; i != 3; ++i) {
        backdrop[i] = std::clamp(encoded(b[i] / b[3]), 0.0, 1.0);
        source[i] = std::clamp(encoded(s[i] / s[3]), 0.0, 1.0);
    }
    const auto blended = blendReference(backdrop, source, mode);
    for (std::size_t i = 0; i != 3; ++i)
        result[i] = (1 - alpha) * b[i] + (1 - b[3]) * s[i] * opacity
            + b[3] * alpha * linear(std::clamp(blended[i], 0.0, 1.0));
    return result;
}
PremultipliedColor asFloat(Pixel p)
{
    return {float(p[0]), float(p[1]), float(p[2]), float(p[3])};
}
void compare(PremultipliedColor result, Pixel reference, double tolerance = 3e-6)
{
    for (std::size_t channel = 0; channel != 4; ++channel)
        near(result[channel], reference[channel], tolerance, "premultiplied linear oracle");
    for (std::size_t channel = 0; channel != 3; ++channel)
        check(result[channel] >= -1e-6 && result[channel] <= result[3] + 1e-6,
            "premultiplied color must remain inside its coverage envelope");
}

void fixedReferences()
{
    // Fixed offline double-precision outputs, in the public enum order. The
    // opaque case catches sRGB-vs-linear blend-space substitutions immediately.
    constexpr std::array<Rgba8, 14> opaque {{
        {204,77,26,255}, {41,46,23,255}, {214,184,233,255}, {82,113,210,255},
        {89,129,212,255}, {173,92,47,255}, {51,77,26,255}, {204,153,230,255},
        {153,76,204,255}, {173,138,209,255}, {226,98,47,255}, {51,153,229,255},
        {225,98,47,255}, {30,132,209,255},
    }};
    constexpr std::array<Pixel, 14> partial {{
        {.096969071727,.175690768191,.413453290281,.652815686275},
        {.050873146995,.171993113672,.413333735156,.652815686275},
        {.102517269932,.207692763869,.476907456807,.652815686275},
        {.055749686412,.182795009709,.463746523032,.652815686275},
        {.057035748009,.187132989172,.464820961448,.652815686275},
        {.082408258387,.178367028719,.414878263102,.652815686275},
        {.051754415235,.175690768191,.413453290281,.652815686275},
        {.096969071727,.195047707420,.475324340892,.652815686275},
        {.074368138833,.175536938462,.460472258182,.652815686275},
        {.082408258387,.189819220981,.463200907835,.652815686275},
        {.109329542913,.179528558885,.414879057992,.652815686275},
        {.051797815187,.195002898912,.474983436218,.652815686275},
        {.109010329406,.179566533978,.414920738861,.652815686275},
        {.050139121851,.187977388188,.462941288419,.652815686275},
    }};
    for (std::size_t i = 0; i != opaque.size(); ++i) {
        const auto actual = encodeColor(compositeLayer(decodeColor({51,153,230,255}),
            decodeColor({204,77,26,255}), 1, modes[i]));
        near(actual.red, opaque[i].red, 1, "fixed opaque red");
        near(actual.green, opaque[i].green, 1, "fixed opaque green");
        near(actual.blue, opaque[i].blue, 1, "fixed opaque blue");
        near(actual.alpha, 255, 0, "opaque output coverage");
        compare(compositeLayer(decodeColor({51,153,230,153}),
            decodeColor({204,77,26,91}), .37f, modes[i]), partial[i]);
    }
}

void stableModesAndAddedReferences()
{
    // Public numeric IDs are a GPU ABI. Appending modes must never move any
    // existing value or detach the persisted textual ID from its meaning.
    constexpr std::array<std::string_view,20> ids {
        "normal","multiply","screen","overlay","soft-light","hard-light",
        "darken","lighten","difference","exclusion","hue","saturation","color","luminosity",
        "color-dodge","linear-dodge","color-burn","linear-burn","subtract","divide",
    };
    check(modes == allBlendModes,"all mode list retains existing order and appends the six new modes");
    for (std::size_t i = 0; i < modes.size(); ++i) {
        check(static_cast<std::uint32_t>(modes[i]) == i,"stable blend-mode numeric ID");
        check(blendModeId(modes[i]) == ids[i],"stable blend-mode serialized ID");
        check(blendModeFromId(ids[i]) == modes[i],"blend-mode serialized ID resolves correctly");
    }
    check(!blendModeFromId("future-unsupported-mode"),"unknown serialized modes remain explicitly unavailable");
    constexpr std::array<Rgba8,6> opaque {{
        {255,219,255,255},{255,230,255,255},{0,0,10,255},
        {0,0,1,255},{0,76,204,255},{64,255,255,255},
    }};
    constexpr std::array<Pixel,6> partial {{
        {.128355268199,.226038553492,.491858454654,.652815686275},
        {.128355268199,.232500723024,.491858454654,.652815686275},
        {.049131738787,.169811307374,.412870765709,.652815686275},
        {.049131738787,.169811307374,.412658971721,.652815686275},
        {.049131738787,.175536938462,.460472258182,.652815686275},
        {.053162322055,.249034836786,.491858454654,.652815686275},
    }};
    for (std::size_t i = 0; i < addedModes.size(); ++i) {
        const auto actual = encodeColor(compositeLayer(decodeColor({51,153,230,255}),
            decodeColor({204,77,26,255}),1,addedModes[i]));
        near(actual.red,opaque[i].red,1,"added fixed opaque red");
        near(actual.green,opaque[i].green,1,"added fixed opaque green");
        near(actual.blue,opaque[i].blue,1,"added fixed opaque blue");
        near(actual.alpha,255,0,"added opaque output alpha");
        compare(compositeLayer(decodeColor({51,153,230,153}),
            decodeColor({204,77,26,91}),.37f,addedModes[i]),partial[i]);
    }
}

float directBlend(float backdrop, float source, BlendMode mode)
{
    return blend_detail::bBlend({backdrop,backdrop,backdrop},
        {source,source,source},int(mode)).x;
}
void directReference(float backdrop, float source, BlendMode mode)
{
    // Both paths receive the SAME representable input floats. A double color
    // converted independently to float can move a pole enough to invalidate
    // a test of the equation rather than expose a real implementation defect.
    const auto actual = directBlend(backdrop,source,mode);
    const double b = backdrop, s = source;
    const auto expected = blendReference({b,b,b},{s,s,s},mode)[0];
    const auto label = std::string("direct ")+std::string(blendModeName(mode));
    near(actual,expected,2e-7,label);
    check(std::isfinite(actual) && actual >= 0 && actual <= 1,"direct blend output is finite normalized coverage");
}

void divisionEndpointsAndGradients()
{
    const float smallest = std::numeric_limits<float>::denorm_min();
    const float normal = std::numeric_limits<float>::min();
    const float belowOne = std::nextafter(1.f,0.f);
    const std::array values {0.f,smallest,smallest*2,normal,std::nextafter(normal,1.f),
        1e-30f,1e-12f,1.f/255.f,.25f,.5f,.75f,belowOne,1.f};
    for (auto mode : addedModes)
        for (auto b : values) for (auto s : values) directReference(b,s,mode);

    near(directBlend(0,1,BlendMode::ColorDodge),0,0,"Color Dodge gives black priority over white source");
    near(directBlend(1,0,BlendMode::ColorBurn),1,0,"Color Burn gives white priority over black source");
    near(directBlend(0,0,BlendMode::Divide),0,0,"Divide 0/0 is explicitly black");
    near(directBlend(.5f,0,BlendMode::Divide),1,0,"Divide positive/0 is explicitly white");
    near(directBlend(smallest,smallest,BlendMode::Divide),1,0,"Divide retains tiny nonzero denominators");
    near(directBlend(smallest,smallest*2,BlendMode::Divide),.5,0,"Divide does not replace tiny denominators with epsilon");
    near(directBlend(normal,normal*4,BlendMode::Divide),.25,0,"Divide operates correctly near minimum normal float");
    const float gap = 1-belowOne;
    near(directBlend(gap*.25f,belowOne,BlendMode::ColorDodge),.25,0,"Color Dodge nearest-white finite denominator");
    near(directBlend(belowOne,gap*2,BlendMode::ColorBurn),.5,0,"Color Burn nearest-white finite numerator");

    // Clipping boundaries and gradients use exact binary fractions. Include
    // values just below, exactly at, and above each threshold.
    for (float pivot : {.125f,.25f,.5f,.75f,.875f}) {
        for (float b : {std::nextafter(pivot,0.f),pivot,std::nextafter(pivot,1.f)}) {
            for (auto mode : addedModes) {
                directReference(b,pivot,mode);
                directReference(b,1-pivot,mode);
            }
        }
    }
    for (auto mode : addedModes) for (float source : {0.f,.125f,.5f,.875f,1.f}) {
        float previous = -1;
        for (int index = 0; index <= 4096; ++index) {
            const float b = float(index)/4096;
            directReference(b,source,mode);
            const auto current = directBlend(b,source,mode);
            check(current >= previous,"new-mode clipping gradients are monotonic in backdrop");
            previous = current;
        }
    }
    for (auto mode : addedModes) {
        const bool decreasing = mode == BlendMode::Subtract || mode == BlendMode::Divide;
        for (float b : {.125f,.5f,.875f}) {
            float previous = directBlend(b,0,mode);
            for (int index = 1; index <= 4096; ++index) {
                const float s = float(index)/4096;
                const float current = directBlend(b,s,mode);
                check(decreasing ? current <= previous : current >= previous,
                    "new-mode clipping gradients have the documented source direction");
                previous = current;
            }
        }
    }
}

void fullCompositeEndpointAndNeutralBehavior()
{
    near(blend_detail::bEncode(0),0,0,"linear zero encodes exactly to zero");
    near(blend_detail::bEncode(1),1,0,"linear one encodes exactly to one for singular endpoints");
    near(blend_detail::bDecode(0),0,0,"encoded zero decodes exactly to zero");
    near(blend_detail::bDecode(1),1,0,"encoded one decodes exactly to one");
    const auto gray = [](std::uint8_t value) { return Rgba8 {value,value,value,255}; };
    struct Endpoint { BlendMode mode; std::uint8_t backdrop,source,result; };
    for (const auto test : {Endpoint{BlendMode::ColorDodge,0,255,0},
        Endpoint{BlendMode::ColorDodge,1,255,255},Endpoint{BlendMode::ColorBurn,255,0,255},
        Endpoint{BlendMode::ColorBurn,254,0,0},Endpoint{BlendMode::Divide,0,0,0},
        Endpoint{BlendMode::Divide,1,0,255},Endpoint{BlendMode::LinearDodge,128,128,255},
        Endpoint{BlendMode::LinearBurn,0,255,0},Endpoint{BlendMode::Subtract,31,63,0}}) {
        check(encodeColor(compositeLayer(decodeColor(gray(test.backdrop)),decodeColor(gray(test.source)),
            1,test.mode)) == gray(test.result),"full pipeline preserves singular endpoint semantics");
    }
    // Neutral identities require an opaque backdrop: a transparent backdrop
    // also receives the unoccluded source, even when its blend function is neutral.
    for (auto mode : addedModes) {
        const auto source = (mode == BlendMode::ColorBurn || mode == BlendMode::LinearBurn
            || mode == BlendMode::Divide) ? gray(255) : gray(0);
        for (int value = 0; value < 256; ++value) {
            const Rgba8 b {std::uint8_t(value),std::uint8_t(255-value),77,255};
            for (float opacity : {.125f,.7f,1.f})
                check(encodeColor(compositeLayer(decodeColor(b),decodeColor(source),opacity,mode)) == b,
                    "opaque neutral identity survives source alpha/opacity handling");
        }
    }
    // The transfer functions and division are tested together on identical
    // representable LINEAR floats. These inputs remain away from a pole in
    // encoded space, while still exercising tiny nonzero source components.
    const float normal = std::numeric_limits<float>::min();
    for (float alpha : {.25f,.5f,1.f}) {
        const PremultipliedColor b {normal,normal*2,normal*4,alpha};
        const PremultipliedColor s {normal*4,normal*4,normal*8,alpha};
        const Pixel db {b[0],b[1],b[2],b[3]}, ds {s[0],s[1],s[2],s[3]};
        for (auto mode : addedModes)
            compare(compositeLayer(b,s,.75f,mode),compositeReference(db,ds,.75,mode));
    }
}

void stackedWhiteEndpointCoverage()
{
    // B(white,white)=white for these modes. Separately summing the three RGB
    // coverage terms can round below the algebraically identical alpha sum;
    // subsequent singular modes then mistake truly white content for nonwhite.
    // Verify the complete stack, not only direct blend-function endpoints.
    constexpr std::array whiteModes {
        BlendMode::Normal,BlendMode::Multiply,BlendMode::Screen,BlendMode::Overlay,
        BlendMode::SoftLight,BlendMode::HardLight,BlendMode::Darken,BlendMode::Lighten,
        BlendMode::Hue,BlendMode::Saturation,BlendMode::Color,BlendMode::Luminosity,
        BlendMode::ColorDodge,BlendMode::LinearDodge,BlendMode::ColorBurn,
        BlendMode::LinearBurn,BlendMode::Divide,
    };
    constexpr std::array alphaBytes {1,2,4,9,27,64,128,254};
    for (auto mode : whiteModes) for (int backdropAlpha : alphaBytes)
        for (int sourceAlpha : alphaBytes) for (float opacity : {.37f,.7f,1.f}) {
            const auto backdrop = decodeColor({255,255,255,std::uint8_t(backdropAlpha)});
            const auto source = decodeColor({255,255,255,std::uint8_t(sourceAlpha)});
            const auto white = compositeLayer(backdrop,source,opacity,mode);
            const auto burnt = compositeLayer(white,decodeColor({0,0,0,255}),1,BlendMode::ColorBurn);
            for (std::size_t channel = 0; channel < 3; ++channel) {
                near(white[channel],white[3],0,"stacked white RGB must retain its exact alpha endpoint");
                // The overlapping covered region stays white; the uncovered
                // region receives opaque black. Result alpha is therefore one.
                near(burnt[channel],white[3],0,"black Color Burn must preserve stacked white coverage");
            }
            near(burnt[3],1,0,"opaque black Color Burn produces opaque output");
        }
    // An endpoint identity must not become epsilon snapping. These deliberately
    // nonwhite linear inputs are exact, representable floats; even the nearest
    // representable input below white must not acquire white's priority branch.
    for (float value : {std::nextafter(1.f,0.f),1.f-1e-6f,1.f-1e-4f}) {
        const PremultipliedColor nearWhite {value,value,value,1};
        const auto burnt = compositeLayer(nearWhite,decodeColor({0,0,0,255}),1,BlendMode::ColorBurn);
        for (std::size_t channel = 0; channel < 3; ++channel)
            near(burnt[channel],0,0,"deliberately near-white content must not snap to white");
        near(burnt[3],1,0,"near-white endpoint probe remains opaque");
    }
}

void alphaAndOpacity()
{
    constexpr Rgba8 cleanTransparent {0,0,0,0}, hiddenColor {255,0,199,0};
    compare(decodeColor(hiddenColor), {});
    const auto backdrop = decodeColor({19,213,68,137});
    const auto source = decodeColor({241,22,79,63});
    for (auto mode : modes) {
        for (std::size_t i = 0; i != 4; ++i) {
            near(compositeLayer(backdrop, decodeColor(hiddenColor), .71f, mode)[i],
                backdrop[i], 1e-7, "hidden RGB must not contaminate backdrop");
            near(compositeLayer(backdrop, source, 0, mode)[i], backdrop[i], 1e-7,
                "zero layer opacity must leave backdrop unchanged");
            near(compositeLayer(decodeColor(cleanTransparent), source, .5f, mode)[i],
                source[i] * .5, 1e-7, "transparent backdrop must preserve source chroma");
        }
        for (int coverage : {1,2,16,64,127,128,254,255}) {
            const Rgba8 edge {236,12,155,static_cast<std::uint8_t>(coverage)};
            for (float opacity : {.001f,.25f,.5f,1.f})
                compare(compositeLayer(backdrop, decodeColor(edge), opacity, mode),
                    compositeReference(decodeReference({19,213,68,137}), decodeReference(edge), opacity, mode));
        }
    }
    // Normal retains the already approved linear-light source-over pipeline.
    const auto normal = encodeColor(compositeLayer(decodeColor({0,0,0,255}),
        decodeColor({255,255,255,128}), 1, BlendMode::Normal));
    near(normal.red, 188, 0, "linear Normal must not regress to encoded-space gray 128");
    // Decode/encode straight RGBA8 preserves every nonzero-alpha channel.
    for (int channel = 0; channel != 256; ++channel)
        for (int alpha : {1,2,17,128,254,255}) {
            const Rgba8 pixel {static_cast<std::uint8_t>(channel),
                static_cast<std::uint8_t>(255-channel),77,static_cast<std::uint8_t>(alpha)};
            check(encodeColor(decodeColor(pixel)) == pixel, "straight pixel round-trip");
        }
}

void breakpointsTiesAndClipping()
{
    // Exact thresholds cannot all be represented in RGBA8, so these synthetic
    // linear inputs test both sides of the actual SoftLight/HardLight branches.
    constexpr std::array values {0.,.000001,.04,.249999,.25,.250001,.499999,.5,.500001,.999999,1.};
    for (double b : values) for (double s : values) {
        const Pixel backdrop {linear(b),linear(1-b),linear(b),1};
        const Pixel source {linear(s),linear(s),linear(1-s),1};
        for (auto mode : std::span(modes).first(14))
            compare(compositeLayer(asFloat(backdrop), asFloat(source), 1, mode),
                compositeReference(backdrop, source, 1, mode));
    }
    constexpr std::array<Color, 10> colors {{
        {0,0,0},{1,1,1},{.5,.5,.5},{1,0,0},{0,1,0},{0,0,1},
        {1,1,0},{1,0,1},{0,1,1},{.001,.999,.002},
    }};
    for (auto b : colors) for (auto s : colors) {
        const Pixel backdrop {linear(b[0]),linear(b[1]),linear(b[2]),1};
        const Pixel source {linear(s[0]),linear(s[1]),linear(s[2]),1};
        for (auto mode : {BlendMode::Hue,BlendMode::Saturation,BlendMode::Color,BlendMode::Luminosity}) {
            const auto result = compositeLayer(asFloat(backdrop), asFloat(source), 1, mode);
            compare(result, compositeReference(backdrop,source,1,mode));
            const Color straight {encoded(result[0]),encoded(result[1]),encoded(result[2])};
            near(luminosity(straight), luminosity(mode == BlendMode::Luminosity ? s : b),
                3e-6, "nonseparable mode must preserve the specified weighted luminosity");
        }
    }
}

void deterministicSweepAndStacks()
{
    // Fixed portable LCG, not an implementation-specific distribution.
    std::uint32_t state = 0x51A7B1E9u;
    const auto byte = [&]() mutable {
        state = state * 1664525u + 1013904223u;
        return static_cast<std::uint8_t>(state >> 24);
    };
    auto next = byte;
    const auto randomPixel = [&]() { return Rgba8 {next(),next(),next(),next()}; };
    for (int iteration = 0; iteration != 2048; ++iteration) {
        const auto b = randomPixel(), s = randomPixel();
        const float opacity = next() / 255.f;
        for (auto mode : modes)
            compare(compositeLayer(decodeColor(b), decodeColor(s), opacity, mode),
                compositeReference(decodeReference(b),decodeReference(s),opacity,mode));
    }
    for (int stack = 0; stack != 100; ++stack) {
        PremultipliedColor actual {};
        Pixel expected {};
        for (int layer = 0; layer != 32; ++layer) {
            const auto source = randomPixel();
            const auto mode = modes[static_cast<std::size_t>(next()) % modes.size()];
            const float opacity = next() / 255.f;
            actual = compositeLayer(actual,decodeColor(source),opacity,mode);
            expected = compositeReference(expected,decodeReference(source),opacity,mode);
            compare(actual,expected,5e-6);
        }
    }
}
} // namespace

int main()
{
    fixedReferences();
    stableModesAndAddedReferences();
    divisionEndpointsAndGradients();
    fullCompositeEndpointAndNeutralBehavior();
    stackedWhiteEndpointCoverage();
    alphaAndOpacity();
    breakpointsTiesAndClipping();
    deterministicSweepAndStacks();
    if (failures) std::cerr << failures << " blend-math checks failed\n";
    else std::cout << "Blend math: 20 modes, fixed references, singular endpoints, clipped gradients, alpha/opacity, nonseparable ties, and deterministic stacks passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
