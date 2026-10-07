#include "imageeditor/core/SelectionRefinement.hpp"
#include "imageeditor/core/SmartSelection.hpp"
#include <QDir>
#include <QFont>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <chrono>
#include <cstring>
#include <iostream>

using namespace imageeditor::core;
namespace {
SelectionState mask(const QImage &alpha) {
  std::vector<std::uint8_t> values(std::size_t(alpha.width()) *
                                   std::size_t(alpha.height()));
  for (int y = 0; y < alpha.height(); ++y)
    for (int x = 0; x < alpha.width(); ++x)
      values[std::size_t(y) * std::size_t(alpha.width()) + std::size_t(x)] =
          std::uint8_t(qAlpha(alpha.pixel(x, y)));
  return SelectionMask::fromR8(
      {std::uint32_t(alpha.width()), std::uint32_t(alpha.height())}, values,
      std::size_t(alpha.width()));
}
QImage cutout(const SmartReferenceImage &image, const SelectionState &alpha,
              QColor bg) {
  QImage out(int(image.extent.width), int(image.extent.height),
             QImage::Format_RGB32);
  for (int y = 0; y < out.height(); ++y)
    for (int x = 0; x < out.width(); ++x) {
      const auto c =
          image.pixels[std::size_t(y) * image.extent.width + std::size_t(x)];
      const float a = float(alpha->coverageAtDocumentPixel(x, y)) / 255;
      out.setPixelColor(
          x, y,
          QColor(linearToSrgb(
                     float(srgbToLinear(c.red) * a +
                           srgbToLinear(std::uint8_t(bg.red())) * (1 - a))),
                 linearToSrgb(
                     float(srgbToLinear(c.green) * a +
                           srgbToLinear(std::uint8_t(bg.green())) * (1 - a))),
                 linearToSrgb(
                     float(srgbToLinear(c.blue) * a +
                           srgbToLinear(std::uint8_t(bg.blue())) * (1 - a)))));
    }
  return out;
}
QImage gray(const SelectionState &alpha) {
  auto e = alpha->extent();
  QImage image(int(e.width), int(e.height), QImage::Format_RGB32);
  for (int y = 0; y < image.height(); ++y)
    for (int x = 0; x < image.width(); ++x) {
      const auto a = alpha->coverageAtDocumentPixel(x, y);
      image.setPixel(x, y, qRgb(a, a, a));
    }
  return image;
}
double mae(const SelectionState &a, const SelectionState &truth) {
  double sum = 0;
  const auto e = a->extent();
  for (std::uint32_t y = 0; y < e.height; ++y)
    for (std::uint32_t x = 0; x < e.width; ++x)
      sum += std::abs(int(a->coverageAtDocumentPixel(int(x), int(y))) -
                      int(truth->coverageAtDocumentPixel(int(x), int(y))));
  return sum / (255. * e.width * e.height);
}
SmartReferenceImage composite(const SelectionState &truth, int kind) {
  SmartReferenceImage image;
  image.extent = truth->extent();
  const auto n = std::size_t(image.extent.width) * image.extent.height;
  image.pixels.resize(n);
  image.valid.resize(n, 1);
  for (std::uint32_t y = 0; y < image.extent.height; ++y)
    for (std::uint32_t x = 0; x < image.extent.width; ++x) {
      const double a = truth->coverageAtDocumentPixel(int(x), int(y)) / 255.;
      const double texture =
          kind == 4 ? .06 * std::sin(x * .5) * std::cos(y * .4) : 0;
      const std::array fg =
          kind == 5 ? std::array{.3, .3, .3}
                    : std::array{.8 + texture, .5 + texture, .13 + texture};
      const std::array bg =
          kind == 5 ? std::array{.301, .301, .301}
                    : std::array{.04 + texture * .2, .1 + texture * .2,
                                 .5 + texture * .2};
      image.pixels[std::size_t(y) * image.extent.width + x] = {
          linearToSrgb(float(a * fg[0] + (1 - a) * bg[0])),
          linearToSrgb(float(a * fg[1] + (1 - a) * bg[1])),
          linearToSrgb(float(a * fg[2] + (1 - a) * bg[2])), 255};
    }
  return image;
}
} // namespace
int main(int argc, char **argv) {
  QGuiApplication app(argc, argv);
  const auto args = app.arguments();
  const QString out = args.size() > 1 ? args[1] : QString{};
  if (!out.isEmpty())
    QDir().mkpath(out);
  const QStringList names{"Geometry / holes",    "Text counters",
                          "Fine strands",        "Soft fabric",
                          "Textured background", "Ambiguous colors",
                          "Border / islands",    "Strands + Refine Edge"};
  QImage sheet(256 * 7, 216 * int(names.size()), QImage::Format_RGB32);
  sheet.fill(QColor(30, 30, 30));
  QPainter display(&sheet);
  int failed = 0;
  for (int kind = 0; kind < names.size(); ++kind) {
    QImage alpha(256, 192, QImage::Format_ARGB32);
    alpha.fill(Qt::transparent);
    QPainter p(&alpha);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    if (kind == 1) {
      QFont font("DejaVu Sans", 74, QFont::Bold);
      p.setFont(font);
      p.setPen(Qt::white);
      p.drawText(QRect(0, 0, 256, 192), Qt::AlignCenter, "OB");
    } else if (kind == 2 || kind == 7) {
      p.drawEllipse(QRectF(40, 28, 138, 138));
      p.setBrush(Qt::NoBrush);
      p.setPen(QPen(Qt::white, 1.5));
      for (int i = 0; i < 20; ++i) {
        QPainterPath path;
        path.moveTo(100, 45 + i * 5);
        path.cubicTo(200, 20 + i * 4, 155, 165 - i * 3, 231, 42 + i * 7);
        p.drawPath(path);
      }
    } else if (kind == 6) {
      p.drawEllipse(QRectF(-20, -30, 150, 170));
      p.drawEllipse(QRectF(162, 70, 64, 70));
      p.drawEllipse(QRectF(160, 16, 8, 8));
    } else {
      QPainterPath shape;
      shape.addRoundedRect(QRectF(38, 25, 178, 144), 22, 22);
      shape.addEllipse(QRectF(90, 64, 64, 64));
      shape.setFillRule(Qt::OddEvenFill);
      p.drawPath(shape);
    }
    p.end();
    auto truth = mask(alpha);
    if (kind == 3) {
      RefinementState soft;
      soft.settings.feather = 7;
      truth = refineSelection(truth, nullptr, soft).coverage;
    }
    // Deliberately imperfect starting coverage; the solver receives only
    // this mask and the composite, never the independent alpha reference.
    auto input = truth->adjusted(3, 3);
    auto image = composite(truth, kind);
    RefinementState state;
    state.settings.radius = 8;
    if (kind == 7) {
      auto region = std::make_shared<RefinementStroke>();
      region->kind = RefinementBrush::Edge;
      region->hardness = 1;
      BrushDab dab;
      dab.documentCenter = {206, 100};
      dab.diameterPixels = 155;
      region->dabs.push_back(dab);
      state.strokes.push_back(region);
    }
    const auto start = std::chrono::steady_clock::now();
    auto refined = refineSelection(input, &image, state);
    RefinementState feather;
    feather.settings.feather = 6;
    auto blurred = refineSelection(input, nullptr, feather);
    const double before = mae(input, truth),
                 after = mae(refined.coverage, truth),
                 maskOnly = mae(blurred.coverage, truth);
    std::cout << names[kind].toStdString() << ": initial=" << before
              << " refined=" << after << " feather=" << maskOnly
              << " limited=" << refined.unresolvedPixels << " ms="
              << std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - start)
                     .count()
              << '\n';
    if (kind == 5) {
      if (!refined.coverage->equivalent(*input))
        ++failed;
    } else if (after >= before || (kind != 4 && after >= maskOnly))
      ++failed;
    // Retain the pre-fix quality limits: recovering swallowed islands must
    // not sacrifice existing thin strands, counters or textured edges.
    const std::array maximumError{.00003, .001, .0225, .0005, .0013,
                                   1., .0022, .0001};
    if (after > maximumError[std::size_t(kind)])
      ++failed;
    const std::array<QImage, 7> images{
        gray(input),
        gray(refined.coverage),
        gray(truth),
        cutout(image, refined.coverage, Qt::black),
        cutout(image, refined.coverage, Qt::white),
        cutout(image, refined.coverage, QColor(35, 130, 75)),
        gray(blurred.coverage)};
    const QStringList labels{"Input",    "Refined",  "Known alpha", "On black",
                             "On white", "On color", "Feather only"};
    for (int col = 0; col < 7; ++col) {
      display.drawImage(col * 256, kind * 216 + 24, images[std::size_t(col)]);
      display.setPen(Qt::white);
      display.drawText(col * 256 + 4, kind * 216 + 17,
                       names[kind] + " · " + labels[col]);
    }
    if (!out.isEmpty()) {
      gray(refined.coverage)
          .save(out + QString("/case-%1-refined.png").arg(kind));
      gray(truth).save(out + QString("/case-%1-truth.png").arg(kind));
    }
  }
  display.end();
  if (!out.isEmpty())
    sheet.save(out + "/quality-sheet.png");
  if (args.size() > 2 && args[2] == "--flower") {
    if (args.size() < 4)
      return 1;
    QImage source(args[3]);
    source = source.convertToFormat(QImage::Format_RGBA8888);
    if (source.isNull())
      return 1;
    SmartReferenceImage image;
    image.extent = {std::uint32_t(source.width()),
                    std::uint32_t(source.height())};
    image.pixels.resize(std::size_t(source.width()) *
                        std::size_t(source.height()));
    image.valid.resize(image.pixels.size(), 1);
    for (int y = 0; y < source.height(); ++y)
      std::memcpy(image.pixels.data() + std::size_t(y) * image.extent.width,
                  source.constScanLine(y), std::size_t(source.width()) * 4);
    const std::atomic_bool cancelled{false};
    auto input = buildMagicWand(image, {502.5, 636.5}, 12, {},
                                SelectionOperation::Replace, cancelled)
                     .combined;
    QImage review(360 * 6, 360 * 3 + 28, QImage::Format_RGB32);
    review.fill(QColor(30, 30, 30));
    QPainter painter(&review);
    const QRect crop(370, 575, 180, 180);
    for (int col = 0; col < 6; ++col) {
      RefinementState state;
      const std::array radii{0., 5., 26., 53., 0., 0.};
      state.settings.radius = radii[std::size_t(col)];
      if (col >= 4) {
        auto stroke = std::make_shared<RefinementStroke>();
        stroke->kind = RefinementBrush::Edge;
        stroke->hardness = 1;
        BrushDab dab;
        dab.documentCenter = {479, 655};
        dab.diameterPixels = col == 4 ? 75 : 130;
        stroke->dabs.push_back(dab);
        state.strokes.push_back(stroke);
      }
      auto result = refineSelection(input, &image, state);
      double area = 0;
      for (int y = 0; y < source.height(); ++y)
        for (int x = 0; x < source.width(); ++x)
          area += result.coverage->coverageAtDocumentPixel(x, y) / 255.;
      std::cout << "Flower " << col << ": area=" << area
                << " analyzed=" << result.analyzedPixels
                << " limited=" << result.unresolvedPixels
                << " analysis_ms=" << result.analysisMs << '\n';
      painter.setPen(Qt::white);
      painter.drawText(
          col * 360 + 4, 19,
          col < 4 ? QString("Radius %1").arg(state.settings.radius)
                  : QString("Refine Edge %1 px").arg(col == 4 ? 75 : 130));
      painter.drawImage(QRect(col * 360, 28, 360, 360),
                        cutout(image, result.coverage, Qt::black), crop);
      painter.drawImage(QRect(col * 360, 388, 360, 360),
                        cutout(image, result.coverage, Qt::white), crop);
      painter.drawImage(QRect(col * 360, 748, 360, 360), gray(result.coverage),
                        crop);
      if (!out.isEmpty())
        gray(result.coverage).save(out + QString("/flower-%1.png").arg(col));
    }
    painter.end();
    if (!out.isEmpty())
      review.save(out + "/flower-review.png");
  } else if (args.size() > 2) {
    QImage source(args[2]);
    source = source.convertToFormat(QImage::Format_RGBA8888);
    if (source.isNull())
      return 1;
    SmartReferenceImage image;
    image.extent = {std::uint32_t(source.width()),
                    std::uint32_t(source.height())};
    image.pixels.resize(std::size_t(source.width()) *
                        std::size_t(source.height()));
    image.valid.resize(image.pixels.size(), 1);
    for (int y = 0; y < source.height(); ++y)
      std::memcpy(image.pixels.data() + std::size_t(y) * image.extent.width,
                  source.constScanLine(y), std::size_t(source.width()) * 4);
    QImage alpha(source.size(), QImage::Format_ARGB32);
    alpha.fill(Qt::transparent);
    QPainter p(&alpha);
    p.scale(source.width() / 1080., source.height() / 810.);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    p.drawPolygon(QPolygonF{{247, 810},
                            {244, 610},
                            {232, 530},
                            {290, 357},
                            {337, 218},
                            {426, 69},
                            {520, 1},
                            {706, 0},
                            {813, 114},
                            {854, 243},
                            {871, 387},
                            {910, 522},
                            {926, 631},
                            {878, 732},
                            {868, 810}});
    p.end();
    auto input = mask(alpha);
    RefinementState state;
    state.settings.radius = 28;
    auto refined = refineSelection(input, &image, state);
    if (!out.isEmpty()) {
      gray(input).save(out + "/owned-input.png");
      gray(refined.coverage).save(out + "/owned-refined.png");
      cutout(image, refined.coverage, Qt::white).save(out + "/owned-white.png");
      cutout(image, refined.coverage, Qt::black).save(out + "/owned-black.png");
    }
    std::cout << "Owned illustration (no ground truth): analyzed="
              << refined.analyzedPixels
              << " limited=" << refined.unresolvedPixels
              << " analysis_ms=" << refined.analysisMs << '\n';
  }
  return failed ? 1 : 0;
}
