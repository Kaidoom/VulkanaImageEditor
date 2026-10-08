#include "imageeditor/core/SmartSelection.hpp"
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <chrono>
#include <iostream>
namespace c = imageeditor::core;
int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    if (argc != 4) {
        std::cerr << "Usage: review image prompts.json output-directory\n";
        return 2;
    }
    const auto source = QImage(argv[1]).convertToFormat(QImage::Format_RGBA8888);
    QFile prompts(argv[2]);
    if (source.isNull() || !prompts.open(QIODevice::ReadOnly))
        return 2;
    const auto cases = QJsonDocument::fromJson(prompts.readAll()).array();
    QDir output(QString::fromLocal8Bit(argv[3]));
    if (!output.mkpath("."))
        return 2;
    c::SmartReferenceImage image { { unsigned(source.width()), unsigned(source.height()) }, { }, { } };
    for (int y = 0; y < source.height(); ++y)
        for (int x = 0; x < source.width(); ++x) {
            const auto* p = source.constScanLine(y) + 4 * x;
            image.pixels.push_back({ p[0], p[1], p[2], p[3] });
            image.valid.push_back(1);
        }
    source.save(output.filePath("reference.png"));
    QJsonArray report;
    c::SmartSelectionResult previous;
    for (const auto value : cases) {
        const auto item = value.toObject();
        const auto name = item["name"].toString();
        c::QuickSelectionPath path;
        const auto points = item["points"].toArray();
        c::NormalizedPointerSample sample;
        sample.pressure = 1;
        sample.buttons = c::PointerButtonPrimary;
        sample.pointerType = c::PointerType::Mouse;
        for (int i = 0; i < points.size(); ++i) {
            const auto p = points[i].toArray();
            sample.documentPosition = { p[0].toDouble(), p[1].toDouble() };
            sample.timestampMicroseconds = std::uint64_t(i) * 10000;
            if (!i)
                path.begin(item["diameter"].toDouble(24), sample);
            else
                path.append(sample);
        }
        path.end(sample);
        c::QuickSelectionSettings settings;
        settings.edgeSensitivity = item["edges"].toDouble(.4);
        settings.diagnostic
            = [&](std::string_view stage, c::RectI region, std::span<const std::uint8_t> pixels) {
                  QImage view(source.size(), QImage::Format_Grayscale8);
                  view.fill(0);
                  for (int y = 0; y < region.height; ++y)
                      std::copy_n(pixels.data() + std::size_t(y) * std::size_t(region.width), region.width,
                          view.scanLine(y + region.y) + region.x);
                  view.save(output.filePath(
                      name + "-" + QString::fromUtf8(stage.data(), qsizetype(stage.size())) + ".png"));
              };
        const auto operation = item["operation"].toString();
        const auto op = operation == "add" ? c::SelectionOperation::Add
            : operation == "subtract"      ? c::SelectionOperation::Subtract
                                           : c::SelectionOperation::Replace;
        const auto start = std::chrono::steady_clock::now();
        const std::atomic_bool cancel { false };
        auto result = c::buildQuickSelection(
            image, path.dabs(), previous.hints, previous.combined, op, cancel, settings);
        const double ms
            = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        QImage mask(source.size(), QImage::Format_Grayscale8);
        mask.fill(0);
        auto overlay = source.copy();
        std::size_t area = 0;
        for (int y = 0; y < source.height(); ++y)
            for (int x = 0; x < source.width(); ++x) {
                const auto a = result.combined ? result.combined->coverageAtDocumentPixel(x, y) : 0;
                mask.scanLine(y)[x] = std::uint8_t(a);
                area += a >= 128;
                if (a < 128) {
                    auto* p = overlay.scanLine(y) + 4 * x;
                    for (int k = 0; k < 3; ++k)
                        p[k] = std::uint8_t(p[k] / 4);
                }
            }
        mask.save(output.filePath(name + "-published.png"));
        overlay.save(output.filePath(name + "-overlay.png"));
        const auto& s = result.stats;
        QJsonObject row { { "name", name }, { "area", qint64(area) },
            { "evidence", qint64(s.evidencePixels) }, { "cut", s.boundaryCut },
            { "elapsedIncludingDiagnosticsMs", ms }, { "workspaceBytes", qint64(s.workspaceBytes) },
            { "workRegion",
                QJsonArray { s.workRegion.x, s.workRegion.y, s.workRegion.width, s.workRegion.height } },
            { "dabs", int(path.dabs().size()) } };
        report.append(row);
        std::cout << QJsonDocument(row).toJson(QJsonDocument::Compact).constData() << '\n';
        previous = std::move(result);
    }
    QFile reportFile(output.filePath("report.json"));
    if (!reportFile.open(QIODevice::WriteOnly))
        return 2;
    reportFile.write(QJsonDocument(report).toJson());
}
