#include "imageeditor/core/Healing.hpp"

#include <QGuiApplication>
#include <QImage>
#include <QPainter>

#include <cmath>
#include <iostream>
#include <vector>

namespace {
using namespace imageeditor::core;
constexpr int width = 160, height = 140;
struct Fixture {
    std::vector<PremultipliedColor> source, clean, damaged;
    std::vector<float> coverage;
};

Fixture fixture(int variant)
{
    Fixture result;
    result.source.resize(width * height); result.clean.resize(width * height);
    result.damaged.resize(width * height); result.coverage.resize(width * height);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const auto i = std::size_t(y * width + x);
        const double fine = .014 * std::sin(x * 1.73 + y * .83) + .012 * std::cos(x * .57 - y * 1.91);
        const double waves = .018 * std::sin(x * .28) * std::cos(y * .22);
        const double sourceLight = .18 + .035 * x / width + fine + waves;
        const double edge = variant && x > 116 ? .24 : 0;
        const double lighting = variant
            ? .08 + .15 * x / width + .065 * y / height + .055 * std::sin(x * .036) * std::cos(y * .025)
            : .16;
        const std::array cast = variant ? std::array {.02, -.012, .055} : std::array {.02, .045, -.02};
        result.source[i] = {float(sourceLight + edge), float(sourceLight * .91 + edge), float(sourceLight * .75 + edge), 1};
        for (std::size_t c = 0; c < 3; ++c)
            result.clean[i][c] = float(result.source[i][c] + lighting + cast[c]);
        result.clean[i][3] = 1;
        result.damaged[i] = result.clean[i];
        const auto radius = std::hypot(x - 80.0, y - 70.0);
        const double mask = std::clamp((34 - radius) / 7, 0.0, 1.0);
        result.coverage[i] = float(mask * mask * (3 - 2 * mask));
        const bool defect = variant ? (std::abs(x - 80) < 3 && std::abs(y - 70) < 20) : radius < 12;
        if (defect) for (std::size_t c = 0; c < 3; ++c) result.damaged[i][c] *= .26F;
    }
    return result;
}

std::vector<PremultipliedColor> painted(const Fixture& fixture,
    const std::vector<PremultipliedColor>& repair)
{
    auto output = fixture.damaged;
    for (std::size_t i = 0; i < output.size(); ++i)
        output[i] = compositeLayer(output[i], repair[i], fixture.coverage[i], BlendMode::Normal);
    return output;
}

double rmse(const Fixture& fixture, const std::vector<PremultipliedColor>& output)
{
    double error = 0, weight = 0;
    for (std::size_t i = 0; i < output.size(); ++i) if (fixture.coverage[i] > 0) {
        for (std::size_t c = 0; c < 3; ++c) {
            const auto delta = output[i][c] - fixture.clean[i][c];
            error += fixture.coverage[i] * delta * delta;
        }
        weight += fixture.coverage[i] * 3;
    }
    return std::sqrt(error / weight);
}

QImage image(const std::vector<PremultipliedColor>& colors)
{
    QImage result(width, height, QImage::Format_RGBA8888);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const auto color = encodeColor(colors[std::size_t(y * width + x)]);
        auto* p = result.scanLine(y) + x * 4;
        p[0] = color.red; p[1] = color.green; p[2] = color.blue; p[3] = color.alpha;
    }
    return result;
}
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    if (argc != 2) {
        std::cerr << "Usage: imageeditor_cloning_comparison output.png\n";
        return 1;
    }
    QImage sheet(1380, 850, QImage::Format_RGB32);
    sheet.fill(QColor(24, 27, 32));
    QPainter painter(&sheet);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QColor(238, 241, 245));
    painter.setFont(QFont("Sans Serif", 21, QFont::DemiBold));
    painter.drawText(25, 40, "Cloning: Stamp and adaptive Heal");
    painter.setFont(QFont("Sans Serif", 11));
    painter.setPen(QColor(172, 183, 198));
    painter.drawText(25, 66, "Controlled fixtures with identical source texture and different destination lighting. Same soft write mask; full adaptation.");
    const std::array titles {QString("Source reference"), QString("Damaged destination"),
        QString("Stamp / feather-only"), QString("Adaptive Heal")};
    bool improved = true;
    for (int row = 0; row < 2; ++row) {
        const auto data = fixture(row);
        HealingOptions options; options.adaptation = 1;
        const auto healed = healPatch({width, height, data.source, data.damaged, data.coverage}, options);
        if (healed.pixels.size() != data.source.size()) {
            std::cerr << "Comparison solve failed: " << healed.diagnostics.message << '\n';
            return 1;
        }
        const auto stamp = painted(data, data.source), heal = painted(data, healed.pixels);
        const auto stampError = rmse(data, stamp), healError = rmse(data, heal);
        improved = improved && healError < stampError;
        std::cout << "Fixture " << row << ": Stamp linear-RGB RMSE=" << stampError << " Heal=" << healError
                  << " iterations=" << healed.diagnostics.iterations << " residual=" << healed.diagnostics.relativeResidual
                  << " fallback-components=" << healed.diagnostics.fallbackComponents << '\n';
        const int top = 114 + row * 364;
        painter.setFont(QFont("Sans Serif", 11, QFont::DemiBold));
        painter.setPen(QColor(226, 231, 238));
        painter.drawText(25, top - 13, row ? "Curved spatial lighting + scratch near a strong edge" : "Brightness and color cast + spot");
        const std::array images {image(data.source), image(data.damaged), image(stamp), image(heal)};
        for (int column = 0; column < 4; ++column) {
            const int left = 25 + column * 337;
            painter.drawImage(QRect(left, top + 22, width * 2, height * 2), images[std::size_t(column)]);
            painter.setFont(QFont("Sans Serif", 11, QFont::DemiBold));
            painter.setPen(QColor(227, 232, 240));
            painter.drawText(left, top + 13, titles[std::size_t(column)]);
            painter.setFont(QFont("Sans Serif", 10));
            painter.setPen(QColor(173, 185, 200));
            const auto caption = column == 0 ? QString("Unadjusted source color and detail")
                : column == 1 ? QString("Known clean reference retained for measurement")
                : QString("Mask-weighted linear RGB RMSE: %1").arg(column == 2 ? stampError : healError, 0, 'f', 5);
            painter.drawText(left, top + 320, caption);
        }
    }
    painter.setFont(QFont("Sans Serif", 10));
    painter.setPen(QColor(160, 172, 190));
    painter.drawText(25, 832, "Synthetic quality check, not a universal guarantee. Feather-only baseline uses exactly the Stamp source-over result and mask.");
    painter.end();
    if (!sheet.save(QString::fromLocal8Bit(argv[1]))) {
        std::cerr << "Could not save comparison image\n";
        return 1;
    }
    return improved ? 0 : 2;
}
