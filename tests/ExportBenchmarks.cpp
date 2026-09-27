#include "imageeditor/core/RichText.hpp"
#include "imageeditor/ui/ImageExport.hpp"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <iomanip>
#include <iostream>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    const int width = app.arguments().contains("--5k") ? 5120 : 3840;
    const int height = width * 9 / 16;
    c::Document doc({ { std::uint32_t(width), std::uint32_t(height) }, 300 });
    for (int i = 0; i < 3; ++i) {
        auto surface = std::make_shared<c::ContiguousRasterSurface>(
            c::Extent2u { std::uint32_t(width), std::uint32_t(height) });
        std::vector<std::byte> row(std::size_t(width) * 4);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                row[std::size_t(x) * 4] = std::byte((x * 255 / width + i * 47) % 256);
                row[std::size_t(x) * 4 + 1] = std::byte((y * 255 / height + i * 23) % 256);
                row[std::size_t(x) * 4 + 2] = std::byte((x / 7 + y / 11 + i * 61) % 256);
                row[std::size_t(x) * 4 + 3] = std::byte((x / 130 + y / 150) % 5 ? 190 : 0);
            }
            surface->replaceRgba8({ 0, y, width, 1 }, row, std::size_t(width) * 4);
        }
        auto layer = c::Layer::raster("Gradient raster", surface);
        layer.opacity = .8F;
        layer.blendMode = i == 0 ? c::BlendMode::Normal
            : i == 1             ? c::BlendMode::Multiply
                                 : c::BlendMode::Screen;
        if (i == 2)
            layer.localToDocument = { .98, .04, 11, -.04, .98, 17 };
        doc.insertLayer(doc.layers().size(), std::move(layer));
    }
    c::TextLayer text;
    text.utf8 = "Vulkana Export\nColored text · sRGB";
    text.defaultStyle.sizePixels = 110;
    text.defaultStyle.color = { 240, 40, 180, 200 };
    auto title = c::Layer::text("Editable title", c::normalizedText(text));
    title.localToDocument.m02 = 160;
    title.localToDocument.m12 = 170;
    doc.insertLayer(doc.layers().size(), std::move(title));
    c::ShapeLayer shape;
    shape.kind = c::ShapeKind::Ellipse;
    shape.size = { 500, 200 };
    shape.fillColor = { 30, 180, 250, 100 };
    shape.strokeEnabled = true;
    shape.strokeWidth = 4;
    shape.strokeColor = { 255, 220, 50, 180 };
    auto ellipse = c::Layer::shape("Editable ellipse", shape);
    ellipse.localToDocument.m02 = 60;
    ellipse.localToDocument.m12 = 600;
    doc.insertLayer(doc.layers().size(), std::move(ellipse));
    const auto args = app.arguments();
    const auto artifactIndex = args.indexOf("--artifacts");
    const auto artifacts = artifactIndex < 0 ? QString { } : args.value(artifactIndex + 1);
    if (!artifacts.isEmpty())
        QDir().mkpath(artifacts);
    std::cout << std::fixed << std::setprecision(1) << width << 'x' << height
              << " · 3 raster + text + shape\n";
    for (int mode = 0; mode < 4; ++mode) {
        u::ExportSettings s;
        s.size = { width, height };
        s.format = mode == 0 ? u::ExportFormat::Png
            : mode == 1      ? u::ExportFormat::Jpeg
                             : u::ExportFormat::WebP;
        s.webpLossless = mode == 3;
        QElapsedTimer timer;
        timer.start();
        qint64 previous = 0, largestGap = 0;
        std::size_t yields = 0;
        auto rendered = u::renderExport(doc, s, [&](auto, auto) {
            const auto now = timer.nsecsElapsed();
            largestGap = std::max(largestGap, now - previous);
            previous = now;
            ++yields;
            return true;
        });
        const auto renderMs = double(timer.nsecsElapsed()) / 1e6;
        if (!rendered) {
            std::cerr << rendered.error.toStdString() << '\n';
            return 1;
        }
        timer.restart();
        std::atomic_bool cancel { false };
        const auto encoded = u::encodeExport(rendered.image, s, cancel);
        const auto encodeMs = double(timer.nsecsElapsed()) / 1e6;
        if (!encoded) {
            std::cerr << encoded.error.toStdString() << '\n';
            return 1;
        }
        QFile status("/proc/self/status");
        if (!status.open(QIODevice::ReadOnly))
            return 1;
        QString peak;
        for (const auto& line : status.readAll().split('\n'))
            if (line.startsWith("VmHWM:"))
                peak = QString::fromLatin1(line).simplified();
        std::cout << u::exportFormatName(s.format).toStdString() << (s.webpLossless ? " lossless" : "")
                  << " render=" << renderMs << " ms encode+decode=" << encodeMs
                  << " ms bytes=" << encoded.bytes.size() << " maxCallbackGap=" << double(largestGap) / 1e6
                  << " ms callbacks=" << yields << ' ' << peak.toStdString() << std::endl;
        if (!artifacts.isEmpty()) {
            auto path = artifacts
                + QStringLiteral("/export-%1.%2")
                      .arg(mode)
                      .arg(QString::fromLatin1(u::exportFormatName(s.format)));
            if (!u::writeExportAtomically(path, encoded.bytes, cancel))
                return 1;
        }
    }
}
