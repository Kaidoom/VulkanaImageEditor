#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/core/SelectionCommands.hpp"
#include "imageeditor/core/SelectionRefinement.hpp"
#include <chrono>
#include <cmath>
#include <iostream>
#include <sys/resource.h>

using namespace imageeditor::core;
int failures = 0;
#define CHECK(...)                                                             \
  do {                                                                         \
    if (!(__VA_ARGS__)) {                                                      \
      ++failures;                                                              \
      std::cerr << __LINE__ << ": " #__VA_ARGS__ "\n";                         \
    }                                                                          \
  } while (false)
std::uint8_t at(const SelectionState &s, int x, int y) {
  return s->coverageAtDocumentPixel(x, y);
}
void globals() {
  const auto input = SelectionMask::rectangle({64, 64}, {16, 16, 32, 32}, 91);
  CHECK(refineSelection(input, nullptr, {}).coverage == input);
  RefinementState state;
  state.settings.feather = 6;
  const auto feather = refineSelection(input, nullptr, state).coverage;
  CHECK(at(feather, 15, 32) > 0 && at(feather, 16, 32) < 91);
  state.settings.feather = 0;
  CHECK(refineSelection(input, nullptr, state).coverage == input);
  state.settings.shift = 3;
  const auto grown = refineSelection(input, nullptr, state).coverage;
  CHECK(at(grown, 13, 32) == 91);
  CHECK(at(grown, 12, 32) == 0);
  CHECK(at(grown, 13, 13) == 0); // circle, not square kernel
  state.settings.shift = .5;
  CHECK(std::abs(
            int(at(refineSelection(input, nullptr, state).coverage, 15, 32)) -
            46) <= 1);
  state.settings.shift = -2;
  const auto shrunk = refineSelection(input, nullptr, state).coverage;
  CHECK(at(shrunk, 16, 32) == 0 && at(shrunk, 18, 32) == 91);
  for (auto v : {0, 255})
    for (auto shift : {-4., 4.}) {
      const auto uniform = SelectionMask::filled({31, 27}, std::uint8_t(v));
      state.settings = {0, 3, 6, .4, shift};
      CHECK(refineSelection(uniform, nullptr, state).coverage == uniform);
    }
  std::vector<std::uint8_t> ramp(256);
  for (int i = 0; i < 256; ++i)
    ramp[std::size_t(i)] = std::uint8_t(i);
  auto soft = SelectionMask::fromR8({256, 1}, ramp, 256);
  state.settings = {0, 0, 0, 1, 0};
  auto contrast = refineSelection(soft, nullptr, state).coverage;
  CHECK(at(contrast, 0, 0) == 0 && at(contrast, 255, 0) == 255);
  CHECK(at(contrast, 120, 0) > 0 && at(contrast, 136, 0) < 255);
  std::vector<std::uint8_t> noisy(32 * 32);
  for (int y = 4; y < 28; ++y)
    for (int x = 4; x < 28; ++x)
      noisy[std::size_t(y * 32 + x)] = 255;
  noisy[15 * 32 + 3] = 255;
  noisy[15 * 32 + 4] = 0;
  auto jagged = SelectionMask::fromR8({32, 32}, noisy, 32);
  state.settings = {0, 2, 0, 0, 0};
  auto smooth = refineSelection(jagged, nullptr, state).coverage;
  CHECK(at(smooth, 10, 10) == 255);
  CHECK(at(smooth, 3, 15) < 255);
  CHECK(at(smooth, 3, 10) == 0);
}
struct Fixture {
  SmartReferenceImage image;
  SelectionState input;
  std::vector<float> truth;
};
Fixture strands(int w, int h) {
  Fixture f;
  f.image.extent = {std::uint32_t(w), std::uint32_t(h)};
  const auto n = std::size_t(w) * std::size_t(h);
  f.image.pixels.resize(n);
  f.image.valid.resize(n, 1);
  f.truth.resize(n);
  std::vector<std::uint8_t> coverage(n);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const double boundary =
          w * .45 + 5 * std::sin(y * .07) + 2 * std::sin(y * .41);
      double a = std::clamp((boundary - x) / 2 + .5, 0., 1.);
      if (x > boundary && x < boundary + 14) {
        const double strand =
            std::abs(std::sin(y * .22 + (x - boundary) * .04));
        a = std::max(a, std::clamp((.11 - strand) * 10, 0., .85));
      }
      const auto i = std::size_t(y) * std::size_t(w) + std::size_t(x);
      f.truth[i] = float(a);
      const std::array foreground{.72, .32, .08}, background{.04, .1, .55};
      std::array<std::uint8_t, 3> rgb;
      for (int k = 0; k < 3; ++k)
        rgb[std::size_t(k)] =
            linearToSrgb(float(a * foreground[std::size_t(k)] +
                               (1 - a) * background[std::size_t(k)]));
      f.image.pixels[i] = {rgb[0], rgb[1], rgb[2], 255};
      coverage[i] = x < boundary + 3 ? 255 : 0;
    }
  f.input = SelectionMask::fromR8(f.image.extent, coverage, std::size_t(w));
  return f;
}
double error(const SelectionState &s, const Fixture &f) {
  double sum = 0;
  for (std::uint32_t y = 0; y < f.image.extent.height; ++y)
    for (std::uint32_t x = 0; x < f.image.extent.width; ++x) {
      const auto i = std::size_t(y) * f.image.extent.width + x;
      sum += std::abs(at(s, int(x), int(y)) / 255. - f.truth[i]);
    }
  return sum / double(f.truth.size());
}
void matting() {
  auto f = strands(192, 128);
  RefinementState state;
  state.settings.radius = 18;
  auto result = refineSelection(f.input, &f.image, state);
  RefinementState feather;
  feather.settings.feather = 6;
  auto blurred = refineSelection(f.input, &f.image, feather);
  std::cout << "alpha MAE input=" << error(f.input, f)
            << " guided=" << error(result.coverage, f)
            << " feather=" << error(blurred.coverage, f)
            << " analyzed=" << result.analyzedPixels
            << " unresolved=" << result.unresolvedPixels << '\n';
  CHECK(error(result.coverage, f) < error(f.input, f) * .55);
  CHECK(error(result.coverage, f) < error(blurred.coverage, f) * .6);
  CHECK(at(result.coverage, 0, 0) == 255);
  CHECK(at(result.coverage, 191, 127) == 0);
  for (std::uint32_t y = 0; y < f.image.extent.height; ++y)
    for (std::uint32_t x = 0; x < f.image.extent.width; ++x)
      if (!at(result.region, int(x), int(y)))
        CHECK(at(result.coverage, int(x), int(y)) ==
              at(f.input, int(x), int(y)));
  auto stroke = std::make_shared<RefinementStroke>();
  stroke->kind = RefinementBrush::Subtract;
  stroke->hardness = 1;
  BrushDab dab;
  dab.documentCenter = {60.5, 60.5};
  dab.diameterPixels = 12;
  stroke->dabs = {dab};
  state.strokes = {stroke};
  result = refineSelection(f.input, &f.image, state);
  CHECK(at(result.coverage, 60, 60) == 0);
  state.settings.feather = 7;
  state.settings.shift = 2;
  CHECK(at(refineSelection(f.input, &f.image, state).coverage, 60, 60) == 0);
  stroke = std::make_shared<RefinementStroke>();
  stroke->kind = RefinementBrush::Edge;
  stroke->hardness = 1;
  dab.documentCenter = {90, 60};
  dab.diameterPixels = 32;
  stroke->dabs = {dab};
  state = {{}, {stroke}};
  const auto local = refineSelection(f.input, &f.image, state);
  CHECK(local.analyzedPixels > 0);
  CHECK(at(local.coverage, 0, 0) == 255);
  state.strokes.push_back(stroke);
  CHECK(refineSelection(f.input, &f.image, state)
            .coverage->equivalent(*local.coverage));
  auto ambiguous = f.image;
  std::fill(ambiguous.pixels.begin(), ambiguous.pixels.end(),
            Rgba8{100, 100, 100, 255});
  state.settings.radius = 4;
  state.strokes.clear();
  auto unchanged = refineSelection(f.input, &ambiguous, state);
  CHECK(unchanged.coverage == f.input);
  CHECK(unchanged.unresolvedPixels > 0);
  std::fill(ambiguous.pixels.begin(), ambiguous.pixels.end(),
            Rgba8{255, 0, 255, 0});
  CHECK(refineSelection(f.input, &ambiguous, state).coverage == f.input);
  RefinementOptions cancel;
  cancel.cancelled = [] { return true; };
  CHECK(refineSelection(f.input, &f.image, state, cancel).cancelled);
  std::atomic_uint calls{0};
  cancel.cancelled = [&] { return ++calls > 780; };
  CHECK(refineSelection(f.input, &f.image, state, cancel).cancelled);
  RefinementOptions cached;
  cached.referenceOwner = std::make_shared<const SmartReferenceImage>(f.image);
  state.settings = {8, 0, 0, 0, 0};
  const auto first =
      refineSelection(f.input, cached.referenceOwner.get(), state, cached);
  cached.analysis = first.analysis;
  state.settings.feather = 2;
  auto second =
      refineSelection(f.input, cached.referenceOwner.get(), state, cached);
  CHECK(second.analysis == first.analysis);
  CHECK(second.coverage->equivalent(
      *refineSelection(f.input, &f.image, state).coverage));
  state.settings.radius = 9;
  CHECK(refineSelection(f.input, cached.referenceOwner.get(), state, cached)
            .analysis != first.analysis);
}
void strokes() {
  BrushSettings brush;
  brush.sizePixels = 16;
  brush.hardness = .7;
  brush.opacity = .6;
  auto draw = [&](int steps) {
    RefinementPath path;
    NormalizedPointerSample sample;
    sample.documentPosition = {10, 20};
    sample.buttons = PointerButtonPrimary;
    path.begin(RefinementBrush::Add, brush, sample);
    for (int i = 1; i <= steps; ++i) {
      sample.documentPosition = {10 + 80. * i / steps, 20};
      path.append(sample);
    }
    path.end(sample);
    return path.stroke();
  };
  const auto input = SelectionMask::filled({100, 50}, 0);
  const auto a = refineSelection(input, nullptr, {{}, {draw(2)}}).coverage;
  const auto b = refineSelection(input, nullptr, {{}, {draw(80)}}).coverage;
  CHECK(a->equivalent(*b)); // event cadence is not brush spacing
}
void history() {
  EditorSession session;
  session.replaceDocument(std::make_unique<Document>(CanvasSpec{{32, 32}, 96}));
  auto input = SelectionMask::rectangle({32, 32}, {4, 4, 16, 16});
  session.execute(std::make_unique<SetSelectionCommand>(input, "Input"));
  session.document()->markSaved();
  RefinementState state;
  state.settings.feather = 3;
  auto result = refineSelection(input, nullptr, state);
  session.execute(
      std::make_unique<SetSelectionCommand>(result.coverage, "Refine"));
  CHECK(!session.document()->isModified());
  CHECK(session.undo());
  CHECK(session.document()->selection() == input);
  CHECK(session.history().canRedo());
  CHECK(!session.execute(std::make_unique<SetSelectionCommand>(
      refineSelection(input, nullptr, {}).coverage, "Refine")));
  CHECK(session.history().canRedo());
  CHECK(session.redo());
  CHECK(session.document()->selection() == result.coverage);
}
void smallForegroundEvidence() {
  // A tiny definite island is wholly swallowed by a large analysis band.
  // Truth contains a larger disconnected/holed shape; no truth enters the
  // service. RGB varies spatially, avoiding a two-constant-color-only oracle.
  constexpr int w = 192, h = 160;
  SmartReferenceImage image;
  image.extent = {w, h};
  image.pixels.resize(w * h);
  image.valid.resize(w * h, 1);
  std::vector<std::uint8_t> truth(w * h), start(w * h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const auto i = std::size_t(y * w + x);
      const double d = std::hypot(x - 96., y - 80.);
      double a = std::clamp(30.5 - d, 0., 1.);
      a *= std::clamp(std::hypot(x - 84., y - 83.) - 4.5, 0., 1.);
      a = std::max(a, std::clamp(4.5 - std::hypot(x - 137., y - 80.), 0., 1.));
      truth[i] = std::uint8_t(std::lround(a * 255));
      start[i] = std::uint8_t(std::lround(std::clamp(4.5 - d, 0., 1.) * 255));
      const double t = .008 * std::sin(x * .37) * std::cos(y * .13);
      const std::array fg{.8 + t, .4 + t, .04 + t}, bg{.05, .12 + t, .6 + t};
      image.pixels[i] = {linearToSrgb(float(a * fg[0] + (1 - a) * bg[0])),
                         linearToSrgb(float(a * fg[1] + (1 - a) * bg[1])),
                         linearToSrgb(float(a * fg[2] + (1 - a) * bg[2])), 255};
    }
  const auto input = SelectionMask::fromR8(image.extent, start, w);
  double lastArea = 0;
  for (int pass = 0; pass < 4; ++pass) {
    RefinementState state;
    state.settings.radius = std::array{8., 26., 53., 0.}[std::size_t(pass)];
    if (pass == 3) {
      auto stroke = std::make_shared<RefinementStroke>();
      stroke->kind = RefinementBrush::Edge;
      stroke->hardness = 1;
      BrushDab dab;
      dab.documentCenter = {96, 80};
      dab.diameterPixels = 120;
      stroke->dabs.push_back(dab);
      state.strokes.push_back(stroke);
    }
    const auto r = refineSelection(input, &image, state);
    double area = 0, error = 0;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const auto i = std::size_t(y * w + x);
        const int value = at(r.coverage, x, y);
        area += value / 255.;
        error += std::abs(value - int(truth[i])) / 255.;
        if (at(r.region, x, y) == 0)
          CHECK(value == start[i]);
        if (truth[i] == 0)
          CHECK(value < 4); // Does not turn the uncertain disk into foreground.
      }
    if (pass < 3) {
      CHECK(area > lastArea + 25);
      lastArea = area;
    }
    if (pass >= 2) {
      CHECK(error / (w * h) < .002);
      CHECK(at(r.coverage, 137, 80) >
            250);                         // Disconnected foreground recovered.
      CHECK(at(r.coverage, 84, 83) == 0); // Hole stays hollow.
    }
    std::cout << "small island " << pass << " area=" << area
              << " MAE=" << error / (w * h) << '\n';
    CHECK(r.unresolvedPixels < r.analyzedPixels / 10);
    auto again = refineSelection(input, &image, state);
    CHECK(again.coverage->equivalent(*r.coverage));
  }
  CHECK(refineSelection(input, &image, {}).coverage == input);
}
int main(int argc, char **argv) {
  globals();
  matting();
  strokes();
  history();
  smallForegroundEvidence();
  if (argc > 1 && std::string_view(argv[1]) == "--profile") {
    for (auto e : {Extent2u{3840, 2160}, Extent2u{5120, 2880}}) {
      auto f = strands(int(e.width), int(e.height));
      RefinementState s;
      s.settings.radius = 8;
      auto start = std::chrono::steady_clock::now();
      auto r = refineSelection(f.input, &f.image, s);
      rusage usage{};
      getrusage(RUSAGE_SELF, &usage);
      std::cout << e.width << 'x' << e.height << " total_ms="
                << std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - start)
                       .count()
                << " analysis_ms=" << r.analysisMs
                << " rss_KiB=" << usage.ru_maxrss << '\n';
      for (auto name : {"smooth", "shift", "feather"}) {
        RefinementState global;
        if (std::string_view(name) == "smooth")
          global.settings.smooth = 2;
        if (std::string_view(name) == "shift")
          global.settings.shift = 3;
        if (std::string_view(name) == "feather")
          global.settings.feather = 6;
        const auto measured = refineSelection(f.input, nullptr, global);
        std::cout << e.width << ' ' << name
                  << " global_ms=" << measured.globalMs << '\n';
      }
    }
  }
  return failures ? 1 : 0;
}
