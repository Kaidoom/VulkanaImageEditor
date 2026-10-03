#include "imageeditor/core/SmartSelection.hpp"

#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPolygonF>
#include <QStringList>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <vector>

namespace c = imageeditor::core;
namespace {
int failures = 0;
QString currentCase;
#define CHECK(...)                                                                                           \
    do {                                                                                                     \
        if (!(__VA_ARGS__)) {                                                                                \
            ++failures;                                                                                      \
            std::cerr << "FAIL " << currentCase.toStdString() << ':' << __LINE__ << ": " << #__VA_ARGS__     \
                      << '\n';                                                                               \
        }                                                                                                    \
    } while (false)

struct Probe {
    QPoint point;
    bool selected;
    const char* label;
    bool allowBoundaryFringe { false };
};
struct ArtCase {
    QString name;
    c::Vec2d seed;
    QRect allowedBounds;
    QPolygonF interior;
    std::size_t minimumArea, maximumArea;
    QSize minimumSize;
    QRect reviewBounds;
    std::vector<Probe> probes;
    double radius { 3 };
};

// Approximate, hand-authored behavioral oracles, not pixel-perfect masks copied
// from an external editor and not thresholds derived from the implementation.
// They identify unambiguous interior/exterior regions in the supplied artwork.
// The eye oracle deliberately excludes the bright sclera and the white glint:
// the requested example connects the colored iris, dark rim and eyelashes.
std::vector<ArtCase> cases()
{
    const ArtCase nose { QStringLiteral("nose-highlight"), { 621.5, 460.5 }, { 604, 444, 36, 37 },
        { { 611, 460 }, { 617, 452 }, { 623, 451 }, { 630, 461 }, { 628, 470 }, { 620, 473 }, { 612, 469 } },
        550, 850, { 27, 28 }, { 590, 431, 64, 66 },
        { { { 621, 460 }, true, "seed" }, { { 610, 460 }, true, "white-left" },
            { { 628, 470 }, true, "white-bottom" }, { { 621, 480 }, false, "skin-below" },
            { { 642, 460 }, false, "skin-right" }, { { 606, 446 }, false, "skin-top-left" },
            { { 700, 413 }, false, "distant-eye" }, { { 525, 379 }, false, "distant-glint" } } };
    ArtCase eye { QStringLiteral("right-eye-iris"), { 700.5, 413.5 }, { 667, 374, 109, 74 },
        { { 678, 413 }, { 686, 403 }, { 697, 394 }, { 711, 397 }, { 715, 410 }, { 708, 425 }, { 699, 435 },
            { 688, 432 }, { 680, 425 } },
        2500, 4300, { 85, 52 }, { 656, 365, 131, 97 },
        { { { 700, 413 }, true, "seed" }, { { 700, 425 }, true, "lower-iris" },
            { { 683, 428 }, true, "iris-left" }, { { 748, 402 }, true, "upper-lash" },
            { { 755, 416 }, true, "outer-lash" }, { { 716, 382 }, true, "top-lash" },
            { { 715, 427 }, true, "dark-rim" }, { { 689, 402 }, true, "upper-iris" },
            { { 730, 419 }, false, "sclera" }, { { 699, 401 }, false, "white-glint" },
            { { 700, 448 }, false, "skin-below" }, { { 672, 433 }, false, "skin-left" },
            { { 790, 410 }, false, "hair" }, { { 520, 390 }, false, "other-eye" },
            { { 621, 460 }, false, "nose-highlight" } } };
    auto pupil = eye;
    pupil.name = QStringLiteral("right-eye-pupil");
    // 700,404 is adjacent to the white glint, not an unambiguous pupil sample.
    pupil.seed = { 699.5, 411.5 };
    auto besideGlint = eye;
    besideGlint.name = QStringLiteral("right-eye-pupil-near-glint");
    besideGlint.seed = { 700.5, 404.5 };
    // This pixel's center is sqrt(10) from the radius-3 dab: the pixel overlaps
    // deliberate foreground evidence's edge. Demand binary exclusion, not zero
    // geometric AA coverage. Unambiguous exterior probes remain exactly zero.
    for (auto& probe : besideGlint.probes)
        if (probe.point == QPoint(699, 401))
            probe.allowBoundaryFringe = true;
    auto largeNose = nose;
    largeNose.name = QStringLiteral("nose-highlight-radius12");
    largeNose.radius = 12;
    auto largeEye = eye;
    largeEye.name = QStringLiteral("right-eye-iris-radius12");
    largeEye.radius = 12;
    for (auto& probe : largeEye.probes)
        if (probe.point == QPoint(699, 401)) {
            // The larger brush deliberately touches the glint. Its neighboring
            // white pixel is now hard foreground, not an exclusion oracle.
            probe.point = QPoint(699, 402);
            probe.selected = true;
            probe.label = "brush-covered-glint";
        }
        
    //Left/right always mean the viewer's left/right.
    // They deliberately include smooth lighting/texture within the region,
    // rather than accepting only pixels close to the seed's precise color.
    const ArtCase leftShoulder { QStringLiteral("viewer-left-shoulder"), { 330.5, 735.5 },
        { 251, 668, 133, 142 },
        { { 279, 714 }, { 295, 695 }, { 319, 691 }, { 350, 704 }, { 365, 720 }, { 346, 801 }, { 268, 801 } },
        10000, 19000, { 95, 120 }, { 243, 657, 164, 153 },
        { { { 330, 735 }, true, "seed-skin" }, { { 283, 744 }, true, "outer-shoulder" },
            { { 311, 695 }, true, "upper-shoulder" }, { { 312, 795 }, true, "lower-shoulder" },
            { { 350, 745 }, true, "inner-shoulder" }, { { 260, 693 }, false, "background" },
            { { 320, 654 }, false, "pink-hair" }, { { 385, 720 }, false, "brown-strap" },
            { { 375, 780 }, false, "brown-strap-bottom" }, { { 425, 780 }, false, "shirt" },
            { { 621, 460 }, false, "distant-highlight" } } };
    const ArtCase rightShoulder { QStringLiteral("viewer-right-shoulder"), { 865.5, 760.5 },
        { 827, 694, 78, 116 },
        { { 869, 711 }, { 881, 730 }, { 887, 751 }, { 893, 780 }, { 894, 804 }, { 837, 804 }, { 850, 786 },
            { 862, 768 }, { 866, 741 } },
        2500, 8500, { 55, 95 }, { 811, 680, 110, 130 },
        { { { 865, 760 }, true, "seed-skin" }, { { 880, 755 }, true, "outer-shoulder" },
            { { 860, 790 }, true, "lower-shoulder" }, { { 838, 795 }, true, "inner-bottom-skin" },
            { { 873, 719 }, true, "upper-shoulder" }, { { 892, 798 }, true, "outer-bottom-skin" },
            { { 850, 760 }, false, "braid-not-shoulder" }, { { 906, 760 }, false, "lit-window" },
            { { 866, 695 }, false, "hair-above" }, { { 814, 784 }, false, "braid-left" },
            { { 784, 750 }, false, "shirt-sliver" }, { { 736, 732 }, false, "black-strap" } } };
    const ArtCase strap { QStringLiteral("viewer-right-black-strap"), { 736.5, 732.5 },
        // Include the dark bottom clasp/seam at x=795..796, independently
        // visible inside the supplied reference contour; not the pale shirt.
        { 692, 654, 105, 156 },
        { { 720, 675 }, { 736, 675 }, { 747, 696 }, { 761, 729 }, { 770, 767 }, { 775, 795 }, { 762, 795 },
            { 750, 757 }, { 737, 720 } },
        5500, 10500, { 70, 140 }, { 679, 643, 131, 167 },
        { { { 736, 732 }, true, "seed-strap" }, { { 710, 665 }, true, "strap-top-seam" },
            { { 736, 675 }, true, "upper-strap" }, { { 732, 700 }, true, "strap-middle" },
            { { 750, 740 }, true, "strap-lower-middle" }, { { 770, 790 }, true, "strap-bottom" },
            { { 766, 715 }, true, "strap-right-edge-interior" }, { { 715, 710 }, false, "shirt-left" },
            { { 783, 751 }, false, "shirt-sliver-right" }, { { 748, 655 }, false, "hair-above" },
            { { 816, 744 }, false, "braid" }, { { 865, 760 }, false, "shoulder" } } };
    const ArtCase leftBrow { QStringLiteral("viewer-left-brow-skin"), { 525.5, 340.5 }, { 473, 314, 112, 50 },
        { { 488, 341 }, { 507, 327 }, { 538, 326 }, { 561, 330 }, { 570, 344 }, { 568, 347 }, { 537, 346 },
            { 505, 347 }, { 490, 350 } },
        2200, 5000, { 90, 35 }, { 461, 301, 135, 83 },
        { { { 525, 340 }, true, "seed-brow-skin" }, { { 490, 344 }, true, "outer-brow-skin" },
            { { 510, 326 }, true, "upper-brow-skin" }, { { 553, 333 }, true, "inner-brow-skin" },
            { { 570, 348 }, true, "inner-lower-skin" }, { { 480, 325 }, false, "diagonal-hair" },
            { { 532, 309 }, false, "bright-skin-above-brow" }, { { 539, 362 }, false, "eyelash" },
            { { 590, 341 }, false, "central-hair" }, { { 621, 460 }, false, "nose-highlight" } } };
    const ArtCase rightBrow { QStringLiteral("viewer-right-brow-skin"), { 735.5, 355.5 },
        { 704, 341, 62, 41 },
        { { 718, 365 }, { 728, 351 }, { 737, 352 }, { 750, 358 }, { 754, 369 }, { 741, 367 }, { 727, 367 } },
        750, 1900, { 45, 25 }, { 692, 326, 90, 77 },
        { { { 735, 355 }, true, "seed-brow-skin" }, { { 722, 364 }, true, "inner-brow-skin" },
            { { 750, 361 }, true, "outer-brow-skin" }, { { 732, 369 }, true, "lower-brow-skin" },
            { { 728, 336 }, false, "brow-line-above" }, { { 742, 384 }, false, "eyelash" },
            { { 772, 362 }, false, "outer-hair" }, { { 705, 350 }, false, "inner-hair" },
            { { 525, 340 }, false, "other-brow" } } };
    return { nose, eye, pupil, besideGlint, largeNose, largeEye, leftShoulder, rightShoulder, strap, leftBrow,
        rightBrow };
}

std::uint8_t at(const c::SelectionState& mask, int x, int y)
{
    return mask ? mask->coverageAtDocumentPixel(x, y) : 0;
}
c::SmartReferenceImage referenceFrom(const QImage& original)
{
    const auto image = original.convertToFormat(QImage::Format_RGBA8888);
    c::SmartReferenceImage reference;
    reference.extent = { std::uint32_t(image.width()), std::uint32_t(image.height()) };
    reference.pixels.reserve(std::size_t(image.width()) * std::size_t(image.height()));
    reference.valid.assign(std::size_t(image.width()) * std::size_t(image.height()), 1);
    for (int y = 0; y < image.height(); ++y) {
        const auto* row = image.constScanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            const auto* p = row + std::size_t(x) * 4;
            reference.pixels.push_back(p[3] ? c::Rgba8 { p[0], p[1], p[2], p[3] } : c::Rgba8 { 0, 0, 0, 0 });
        }
    }
    return reference;
}

std::size_t connectedComponents(const c::SelectionState& mask)
{
    if (!mask)
        return 0;
    const auto bounds = mask->bounds();
    std::vector<std::uint8_t> visited(std::size_t(bounds.width) * std::size_t(bounds.height));
    std::vector<QPoint> pending;
    std::size_t components = 0;
    const auto offset = [&](int x, int y) {
        return std::size_t(y - bounds.y) * std::size_t(bounds.width) + std::size_t(x - bounds.x);
    };
    for (int y = bounds.y; y < bounds.bottom(); ++y)
        for (int x = bounds.x; x < bounds.right(); ++x) {
            if (visited[offset(x, y)] || at(mask, x, y) < 128)
                continue;
            ++components;
            pending = { QPoint(x, y) };
            visited[offset(x, y)] = 1;
            for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
                const auto p = pending[cursor];
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (!dx && !dy)
                            continue;
                        const int nx = p.x() + dx, ny = p.y() + dy;
                        if (nx < bounds.x || nx >= bounds.right() || ny < bounds.y || ny >= bounds.bottom())
                            continue;
                        if (visited[offset(nx, ny)] || at(mask, nx, ny) < 128)
                            continue;
                        visited[offset(nx, ny)] = 1;
                        pending.emplace_back(nx, ny);
                    }
            }
        }
    return components;
}

struct Measurement {
    std::size_t area { 0 }, nonzero { 0 }, partial { 0 }, outside { 0 }, interior { 0 }, recovered { 0 };
    std::size_t components { 0 };
    double elapsedMs { 0 };
};
Measurement inspect(const ArtCase& test, const c::SmartSelectionResult& result, double elapsed)
{
    Measurement m;
    m.elapsedMs = elapsed;
    CHECK(result.combined);
    if (!result.combined)
        return m;
    const auto mask = result.combined;
    const auto bounds = mask->bounds();
    for (int y = bounds.y; y < bounds.bottom(); ++y)
        for (int x = bounds.x; x < bounds.right(); ++x) {
            const auto a = at(mask, x, y);
            m.nonzero += a > 0;
            m.partial += a > 0 && a < 255;
            m.area += a >= 128;
            m.outside += a >= 128 && !test.allowedBounds.contains(x, y);
        }
    for (int y = test.allowedBounds.top(); y <= test.allowedBounds.bottom(); ++y)
        for (int x = test.allowedBounds.left(); x <= test.allowedBounds.right(); ++x) {
            if (!test.interior.containsPoint(QPointF(x + .5, y + .5), Qt::OddEvenFill))
                continue;
            ++m.interior;
            m.recovered += at(mask, x, y) >= 128;
        }
    m.components = connectedComponents(mask);
    std::cout << test.name.toStdString() << " seed=" << test.seed.x << ',' << test.seed.y
              << " radius=" << test.radius << " area=" << m.area << " nonzero=" << m.nonzero
              << " partial=" << m.partial << " bounds=" << bounds.x << ',' << bounds.y << '+' << bounds.width
              << 'x' << bounds.height << " outside=" << m.outside << " interior=" << m.recovered << '/'
              << m.interior << " components=" << m.components << " elapsed_ms=" << elapsed
              << " evaluated=" << result.stats.evaluatedPixels << " queue_pops=" << result.stats.queuePops
              << " workspace_bytes=" << result.stats.workspaceBytes << '\n';
    CHECK(m.area >= test.minimumArea && m.area <= test.maximumArea);
    CHECK(bounds.width >= test.minimumSize.width() && bounds.height >= test.minimumSize.height());
    CHECK(m.outside == 0);
    CHECK(m.interior > 0 && m.recovered * 100 >= m.interior * 92);
    CHECK(m.components == 1);
    CHECK(m.partial > 0);
    CHECK(at(mask, int(std::floor(test.seed.x)), int(std::floor(test.seed.y))) == 255);
    for (const auto& probe : test.probes) {
        const auto value = at(mask, probe.point.x(), probe.point.y());
        std::cout << "  " << probe.label << '(' << probe.point.x() << ',' << probe.point.y()
                  << ")=" << int(value) << (probe.selected ? " expected selected" : " expected excluded")
                  << '\n';
        if (probe.selected)
            CHECK(value >= 128);
        else if (probe.allowBoundaryFringe)
            CHECK(value < 128);
        else
            CHECK(value == 0);
    }
    return m;
}

QImage maskImage(const c::SelectionState& mask, QSize size)
{
    QImage image(size, QImage::Format_Grayscale8);
    image.fill(0);
    if (mask) {
        const auto bounds = mask->bounds();
        for (int y = bounds.y; y < bounds.bottom(); ++y) {
            auto* row = image.scanLine(y);
            for (int x = bounds.x; x < bounds.right(); ++x)
                row[x] = at(mask, x, y);
        }
    }
    return image;
}
QImage reviewRow(const QImage& source, const ArtCase& test, const c::SelectionState& mask,
    const Measurement& measured, const QString& output)
{
    constexpr int scale = 4, cellWidth = 540, cellHeight = 440, titleHeight = 44;
    const auto gray = maskImage(mask, source.size());
    CHECK(gray.save(output + '/' + test.name + QStringLiteral("-mask.png")));
    QImage overlay = source.copy(test.reviewBounds).convertToFormat(QImage::Format_ARGB32);
    {
        QPainter p(&overlay);
        p.translate(-test.reviewBounds.topLeft());
        if (mask) {
            p.setPen(QPen(QColor(255, 240, 15), .6));
            for (const auto& edge : mask->boundaryEdges())
                p.drawLine(QPointF(edge.from.x, edge.from.y), QPointF(edge.to.x, edge.to.y));
        }
        p.setPen(QPen(QColor(30, 240, 255), .7));
        p.drawEllipse(QPointF(test.seed.x, test.seed.y), test.radius, test.radius);
    }
    CHECK(overlay.scaled(overlay.size() * scale)
            .save(output + '/' + test.name + QStringLiteral("-contour.png")));
    QImage oracle = source.copy(test.reviewBounds).convertToFormat(QImage::Format_ARGB32);
    {
        QPainter p(&oracle);
        p.translate(-test.reviewBounds.topLeft());
        p.setPen(QPen(QColor(0, 255, 100), .7));
        p.setBrush(QColor(0, 255, 100, 40));
        p.drawPolygon(test.interior);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(255, 220, 60), .6));
        p.drawRect(test.allowedBounds);
        for (const auto& probe : test.probes) {
            if (!test.reviewBounds.contains(probe.point))
                continue;
            p.setPen(QPen(probe.selected ? QColor(0, 255, 100) : QColor(255, 50, 80), 1));
            p.drawEllipse(QPointF(probe.point) + QPointF(.5, .5), .9, .9);
        }
    }
    QImage row(cellWidth * 3, cellHeight + titleHeight, QImage::Format_ARGB32);
    row.fill(QColor(25, 25, 28));
    QPainter p(&row);
    p.setPen(Qt::white);
    p.drawText(QRect(10, 2, row.width() - 20, 22), Qt::AlignLeft | Qt::AlignVCenter,
        QStringLiteral("%1 · area %2 px · %3 ms · connected components %4")
            .arg(test.name)
            .arg(measured.area)
            .arg(measured.elapsedMs, 0, 'f', 1)
            .arg(measured.components));
    const std::array labels { QStringLiteral("Actual contour; cyan = evidence dab"),
        QStringLiteral("R8 mask (not confidence)"),
        QStringLiteral("Approximate oracle: interior / extent / probes") };
    const std::array cells { overlay, gray.copy(test.reviewBounds), oracle };
    for (int i = 0; i < 3; ++i) {
        p.drawText(QRect(i * cellWidth + 10, 22, cellWidth - 20, 22), labels[std::size_t(i)]);
        const auto image = cells[std::size_t(i)].scaled(
            (cells[std::size_t(i)].size() * scale).boundedTo(QSize(cellWidth - 16, cellHeight - 16)),
            Qt::KeepAspectRatio);
        p.drawImage(QPoint(i * cellWidth + (cellWidth - image.width()) / 2, titleHeight), image);
    }
    return row;
}

void correctionSequence(const c::SmartReferenceImage& reference, const QString& output)
{
    currentCase = QStringLiteral("art-correction-sequence");
    const std::atomic_bool cancelled { false };
    auto apply
        = [&](c::Vec2d seed, c::SelectionOperation operation, const c::SmartSelectionResult& previous) {
              const std::array dabs { c::QuickHintDab { seed, 3 } };
              return c::buildQuickSelection(
                  reference, dabs, previous.hints, previous.combined, operation, cancelled);
          };
    const auto start = std::chrono::steady_clock::now();
    const auto initial = apply({ 700.5, 413.5 }, c::SelectionOperation::Add, { });
    const auto subtract = apply({ 700.5, 425.5 }, c::SelectionOperation::Subtract, initial);
    CHECK(at(initial.combined, 700, 425) == 255);
    CHECK(at(subtract.combined, 700, 425) == 0);
    CHECK(at(subtract.hints.background, 700, 425) == 255);
    CHECK(at(subtract.hints.foreground, 700, 425) == 0);
    CHECK(at(subtract.combined, 700, 413) == 255);

    const auto nose = apply({ 621.5, 460.5 }, c::SelectionOperation::Add, subtract);
    CHECK(at(nose.combined, 621, 460) == 255);
    CHECK(at(nose.combined, 700, 425) == 0);
    CHECK(at(nose.hints.background, 700, 425) == 255);
    CHECK(at(nose.combined, 700, 413) == 255);
    // New evidence on the same eye, not just a distant independent addition,
    // must keep the earlier deliberate rejection effective.
    const auto nearby = apply({ 748.5, 402.5 }, c::SelectionOperation::Add, nose);
    CHECK(at(nearby.combined, 748, 402) == 255);
    CHECK(at(nearby.combined, 700, 425) == 0);
    CHECK(at(nearby.hints.background, 700, 425) == 255);
    CHECK(at(nearby.combined, 621, 460) == 255);
    std::size_t corrected = 0, reinfectedDistant = 0, reinfectedNearby = 0;
    for (int y = 418; y < 434; ++y)
        for (int x = 690; x < 712; ++x) {
            if (at(initial.combined, x, y) < 128 || at(subtract.combined, x, y) >= 128)
                continue;
            ++corrected;
            reinfectedDistant += at(nose.combined, x, y) >= 128;
            reinfectedNearby += at(nearby.combined, x, y) >= 128;
            if (at(nearby.combined, x, y) >= 128)
                std::cout << "  reinfected(" << x << ',' << y
                          << ") subtract=" << int(at(subtract.combined, x, y))
                          << " nearby=" << int(at(nearby.combined, x, y))
                          << " negative_hint=" << int(at(nearby.hints.background, x, y)) << '\n';
        }
    CHECK(corrected > 28); // Subtract inferred beyond the 3-pixel-radius brush.
    CHECK(reinfectedDistant == 0);
    CHECK(reinfectedNearby == 0);

    const auto override = apply({ 700.5, 425.5 }, c::SelectionOperation::Add, nearby);
    CHECK(at(override.combined, 700, 425) == 255);
    CHECK(at(override.hints.foreground, 700, 425) == 255);
    CHECK(at(override.hints.background, 700, 425) == 0);
    CHECK(at(override.combined, 621, 460) == 255);
    CHECK(at(subtract.hints.background, 700, 425) == 255); // Prior snapshots immutable.
    CHECK(at(nearby.combined, 700, 425) == 0);
    const auto elapsed
        = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::cout << "art-corrections corrected=" << corrected
              << " reinfected_after_distant_add=" << reinfectedDistant
              << " reinfected_after_nearby_add=" << reinfectedNearby << " five_gestures_ms=" << elapsed
              << '\n';
    if (!output.isEmpty()) {
        const std::array masks { initial.combined, subtract.combined, nose.combined, nearby.combined,
            override.combined };
        for (std::size_t i = 0; i < masks.size(); ++i)
            CHECK(maskImage(masks[i], QSize(int(reference.extent.width), int(reference.extent.height)))
                    .save(output + QStringLiteral("/correction-%1-mask.png").arg(i)));
    }
}
}

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    const auto arguments = application.arguments();
    const auto asset = QString::fromUtf8(IMAGEEDITOR_SOURCE_DIR) + QStringLiteral("/private/reference/SmartSelect.png");
    const QImage source(asset);
    CHECK(!source.isNull() && source.size() == QSize(1080, 810));
    if (source.isNull())
        return EXIT_FAILURE;
    const auto reference = referenceFrom(source);
    QString output;
    if (const auto index = arguments.indexOf(QStringLiteral("--review")); index >= 0) {
        output = index + 1 < arguments.size() ? arguments[index + 1]
                                              : QString::fromUtf8(IMAGEEDITOR_SOURCE_DIR)
                + QStringLiteral("/build/test-artifacts/smart-select-art");
        CHECK(QDir().mkpath(output));
    }
    std::vector<QImage> rows;
    for (const auto& test : cases()) {
        currentCase = test.name;
        try {
            const std::array dabs { c::QuickHintDab { test.seed, test.radius } };
            const std::atomic_bool cancelled { false };
            const auto start = std::chrono::steady_clock::now();
            const auto result = c::buildQuickSelection(
                reference, dabs, { }, { }, c::SelectionOperation::Replace, cancelled);
            const auto elapsed
                = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            const auto measured = inspect(test, result, elapsed);
            if (!output.isEmpty())
                rows.push_back(reviewRow(source, test, result.combined, measured, output));
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << test.name.toStdString() << ": " << error.what() << '\n';
            ++failures;
        }
    }
    correctionSequence(reference, output);
    if (!rows.empty()) {
        QImage sheet(rows.front().width(), rows.front().height() * int(rows.size()), QImage::Format_ARGB32);
        QPainter painter(&sheet);
        int y = 0;
        for (const auto& row : rows) {
            painter.drawImage(0, y, row);
            y += row.height();
        }
        painter.end();
        CHECK(sheet.save(output + QStringLiteral("/contact-sheet.png")));
        std::cout << "Review artifacts: " << output.toStdString() << '\n';
    }
    std::cout << "Smart Select supplied-art checks: " << failures << " failures\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
