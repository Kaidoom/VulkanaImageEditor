#include "imageeditor/core/BasicPixelBrushEngine.hpp"
#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/History.hpp"
#include "imageeditor/core/Layer.hpp"
#include "imageeditor/core/RasterSurface.hpp"
#include "imageeditor/ui/BrushAssetLibrary.hpp"

#include <QDir>
#include <QCryptographicHash>
#include <QImageReader>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QString>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace imageeditor;

constexpr int kCellWidth = 220;
constexpr int kCellHeight = 96;
constexpr core::Extent2u kCellExtent {kCellWidth, kCellHeight};

int failures = 0;

void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

core::NormalizedPointerSample sample(double x, double y, double pressure,
    std::uint64_t timestamp)
{
    return {
        .documentPosition = {x, y},
        .timestampMicroseconds = timestamp,
        .pressure = pressure,
        .tiltX = 0.25,
        .tiltY = -0.15,
        .rotationDegrees = 18.0,
        .barrelRotationDegrees = 18.0,
        .pointerType = core::PointerType::Pen,
        .buttons = core::PointerButtonPrimary,
        .modifiers = core::PointerModifierNone,
    };
}

QImage renderCombination(const std::shared_ptr<ui::BrushAssetLibrary>& library,
    const core::BrushAssetRecord& tip, const core::BrushAssetRecord* grain)
{
    const core::Rgba8 paper {244, 239, 224, 255};
    core::Document document(core::CanvasSpec {.extent = kCellExtent});
    auto surface = std::make_shared<core::ContiguousRasterSurface>(
        kCellExtent, paper);
    auto layer = core::Layer::raster("Component review", surface);
    const auto layerId = layer.id;
    CHECK(document.insertLayer(0, std::move(layer)));

    auto settings = core::proceduralBrushPreset(
        core::ProceduralBrushPreset::DryInk);
    settings.tip.assetId = tip.id;
    settings.tip.aspectRatio = 0.72;
    // This atlas fixes rendering inputs, not mutable product preset defaults.
    // Its 57c9f6b reference uses 0 degrees; f70ec5a intentionally changed three
    // default rotations to 45. Keep the exact golden and test defaults separately.
    settings.tip.angleDegrees = 0.0;
    settings.tip.rotationMode = core::BrushTipRotationMode::Fixed;
    settings.grain.assetId = grain ? grain->id
                                   : std::string(core::BrushAssetIds::NoGrain);
    settings.grain.scalePixels = grain
        ? grain->defaultScalePixels.value_or(128.0) : 96.0;
    settings.grain.angleDegrees = grain
        ? grain->defaultRotationDegrees.value_or(0.0) : 0.0;
    settings.grain.strength = grain ? 0.62 : 0.0;
    settings.grain.invert = false;
    settings.sizePixels = 40.0;
    settings.hardness = 0.72;
    settings.opacity = 0.94;
    settings.flow = 0.58;
    settings.spacingPercent = 11.0;
    settings.foreground = {34, 38, 47, 255};
    settings.pressureToSize = true;
    settings.pressureToFlow = true;
    settings.smoothing = core::BrushSmoothingMode::None;
    settings.deterministicSeed = 0xB6A55E7ULL;

    const std::vector<core::NormalizedPointerSample> samples {
        sample(9.5, 76.0, 0.18, 0),
        sample(61.0, 28.5, 0.42, 21000),
        sample(116.5, 70.0, 0.74, 43000),
        sample(165.0, 23.0, 1.0, 65000),
        sample(212.0, 61.5, 0.58, 87000),
    };
    core::History history;
    core::BasicPixelBrushStroke stroke(document, layerId, settings,
        std::make_unique<core::BasicPixelBrushEngine>(),
        std::unique_ptr<core::IBrushTip> {},
        std::unique_ptr<core::IBrushGrain> {}, library.get());
    CHECK(stroke.valid());
    CHECK(stroke.begin(samples.front()));
    for (std::size_t index = 1; index + 1 < samples.size(); ++index) {
        CHECK(stroke.append(samples[index]));
    }
    CHECK(stroke.end(samples.back(), history)
        == core::RasterEditCommitResult::Committed);
    CHECK(history.undoDepth() == 1);
    CHECK(!stroke.lastDirtySet().empty());

    std::vector<std::byte> pixels(static_cast<std::size_t>(kCellWidth)
        * static_cast<std::size_t>(kCellHeight) * 4U);
    surface->copyRgba8({0, 0, kCellWidth, kCellHeight}, pixels,
        static_cast<std::size_t>(kCellWidth) * 4U);
    return QImage(reinterpret_cast<const uchar*>(pixels.data()),
        kCellWidth, kCellHeight, kCellWidth * 4,
        QImage::Format_RGBA8888).copy();
}

struct Matrix {
    std::vector<const core::BrushAssetRecord*> tips;
    std::vector<const core::BrushAssetRecord*> grains;
    std::vector<QImage> cells;
    QImage atlas;
};

Matrix renderMatrix(const std::shared_ptr<ui::BrushAssetLibrary>& library)
{
    Matrix result;
    for (const auto& asset : library->registry().assets()) {
        if (asset.type == core::BrushAssetType::Tip) {
            result.tips.push_back(&asset);
        } else {
            result.grains.push_back(&asset);
        }
    }
    const auto byId = [](const core::BrushAssetRecord* left,
                          const core::BrushAssetRecord* right) {
        return left->id < right->id;
    };
    std::sort(result.tips.begin(), result.tips.end(), byId);
    std::sort(result.grains.begin(), result.grains.end(), byId);
    CHECK(result.tips.size() == 5);
    CHECK(result.grains.size() == 9);

    QString error;
    for (const auto* tip : result.tips) {
        CHECK(library->prepareAsset(
            tip->id, core::BrushAssetType::Tip, &error));
    }
    for (const auto* grain : result.grains) {
        CHECK(library->prepareAsset(
            grain->id, core::BrushAssetType::Grain, &error));
    }
    const auto hotPathBaseline = library->runtimeStats();
    CHECK(hotPathBaseline.residentMaskCount == 14);
    CHECK(hotPathBaseline.residentBytes < 48U * 1024U * 1024U);

    const auto columns = static_cast<int>(result.grains.size() + 1U);
    const auto rows = static_cast<int>(result.tips.size());
    result.atlas = QImage(columns * kCellWidth, rows * kCellHeight,
        QImage::Format_RGBA8888);
    result.atlas.fill(Qt::transparent);
    QPainter painter(&result.atlas);
    result.cells.reserve(result.tips.size() * (result.grains.size() + 1U));
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const auto* grain = column == 0
                ? nullptr : result.grains[static_cast<std::size_t>(column - 1)];
            auto cell = renderCombination(library,
                *result.tips[static_cast<std::size_t>(row)], grain);
            painter.drawImage(column * kCellWidth, row * kCellHeight, cell);
            result.cells.push_back(std::move(cell));
        }
    }
    painter.end();

    const auto afterPainting = library->runtimeStats();
    CHECK(afterPainting.resourceReads == hotPathBaseline.resourceReads);
    CHECK(afterPainting.imageDecodes == hotPathBaseline.imageDecodes);
    CHECK(afterPainting.mipBuilds == hotPathBaseline.mipBuilds);
    CHECK(afterPainting.residentMaskCount == hotPathBaseline.residentMaskCount);
    CHECK(afterPainting.residentBytes == hotPathBaseline.residentBytes);
    CHECK(afterPainting.resolverMisses == hotPathBaseline.resolverMisses);
    CHECK(afterPainting.resolverHits == hotPathBaseline.resolverHits
        + result.cells.size() * 2U);
    return result;
}

QImage makeReviewSheet(const Matrix& matrix,
    const std::shared_ptr<ui::BrushAssetLibrary>& library)
{
    constexpr int headerHeight = 72;
    constexpr int rowHeaderWidth = 190;
    constexpr int reviewCellHeight = 124;
    const auto columns = static_cast<int>(matrix.grains.size() + 1U);
    const auto rows = static_cast<int>(matrix.tips.size());
    QImage sheet(rowHeaderWidth + columns * kCellWidth,
        headerHeight + rows * reviewCellHeight, QImage::Format_RGBA8888);
    sheet.fill(QColor(QStringLiteral("#151820")));
    QPainter painter(&sheet);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setPen(QColor(QStringLiteral("#D9DCE5")));
    QFont headerFont;
    headerFont.setPointSize(8);
    headerFont.setBold(true);
    painter.setFont(headerFont);
    painter.drawText(QRect(12, 8, rowHeaderWidth - 20, headerHeight - 16),
        Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap,
        QStringLiteral("M2B.6\nTip × Grain"));
    for (int column = 0; column < columns; ++column) {
        const auto name = column == 0 ? QStringLiteral("None")
            : library->component(matrix.grains[
                  static_cast<std::size_t>(column - 1)]->id)->displayName;
        painter.drawText(QRect(rowHeaderWidth + column * kCellWidth + 6, 5,
                             kCellWidth - 12, headerHeight - 10),
            Qt::AlignHCenter | Qt::AlignVCenter | Qt::TextWordWrap, name);
    }
    for (int row = 0; row < rows; ++row) {
        const auto name = library->component(
            matrix.tips[static_cast<std::size_t>(row)]->id)->displayName;
        painter.drawText(QRect(10, headerHeight + row * reviewCellHeight,
                             rowHeaderWidth - 20, reviewCellHeight),
            Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, name);
        for (int column = 0; column < columns; ++column) {
            const auto& cell = matrix.cells[static_cast<std::size_t>(row * columns + column)];
            const QRect target(rowHeaderWidth + column * kCellWidth + 5,
                headerHeight + row * reviewCellHeight + 14,
                kCellWidth - 10, kCellHeight - 4);
            painter.drawImage(target, cell);
            painter.setPen(QColor(QStringLiteral("#343A49")));
            painter.drawRect(target.adjusted(0, 0, -1, -1));
            painter.setPen(QColor(QStringLiteral("#D9DCE5")));
        }
    }
    return sheet;
}

void compareOrUpdateGolden(const QImage& atlas, bool update)
{
    const auto path = QStringLiteral(IMAGEEDITOR_SOURCE_DIR)
        + QStringLiteral("/tests/assets/brush-component-goldens/component-atlas.png");
    if (update) {
        CHECK(QDir().mkpath(QFileInfo(path).absolutePath()));
        CHECK(atlas.save(path));
        return;
    }
    const QImage expected(path);
    CHECK(!expected.isNull());
    const auto rgba = expected.convertToFormat(QImage::Format_RGBA8888);
    if (rgba.size() == atlas.size() && rgba != atlas) {
        std::size_t changed=0; int maximum=0; std::uint64_t sum=0;
        for(int y=0;y<atlas.height();++y)for(int x=0;x<atlas.width()*4;++x) {
            const int delta=std::abs(int(rgba.constScanLine(y)[x])-int(atlas.constScanLine(y)[x]));
            changed+=delta!=0;maximum=std::max(maximum,delta);sum+=std::uint64_t(delta);
        }
        std::cerr<<"Golden diagnostic (reference unchanged): differing channels="<<changed
                 <<", maximum="<<maximum<<", absolute sum="<<sum<<'\n';
    }
    CHECK(expected.convertToFormat(QImage::Format_RGBA8888) == atlas);
}

} // namespace

int main(int argc, char* argv[])
{
    QGuiApplication application(argc, argv);
    const auto library = ui::BrushAssetLibrary::createPackaged();
    if(application.arguments().contains("--diagnose")) {
        for(const auto& asset:library->registry().assets()) {
            QImageReader reader(":/imageeditor/brush/v1/"+QString::fromStdString(asset.relativePackagedPath));
            const auto decoded=reader.read();
            const auto rgba=decoded.convertToFormat(QImage::Format_RGBA8888);
            const auto hash=QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char*>(rgba.constBits()),rgba.sizeInBytes()),QCryptographicHash::Sha256).toHex();
            std::cout<<asset.id<<" angle="<<asset.defaultRotationDegrees.value_or(-18)
                     <<" format="<<decoded.format()<<" rgba="<<hash.constData()<<'\n';
        }
    }
    CHECK(library->diagnostics().empty());
    const auto first = renderMatrix(library);
    const auto second = renderMatrix(library);
    CHECK(first.atlas == second.atlas);
    compareOrUpdateGolden(first.atlas,
        qEnvironmentVariableIntValue(
            "IMAGEEDITOR_UPDATE_BRUSH_COMPONENT_GOLDEN") == 1);

    QString contactSheetPath;
    for (int index = 1; index + 1 < argc; ++index) {
        if (QString::fromLocal8Bit(argv[index])
            == QStringLiteral("--contact-sheet")) {
            contactSheetPath = QString::fromLocal8Bit(argv[index + 1]);
        }
    }
    if (!contactSheetPath.isEmpty()) {
        CHECK(QDir().mkpath(QFileInfo(contactSheetPath).absolutePath()));
        CHECK(makeReviewSheet(first, library).save(contactSheetPath));
    }

    if (failures != 0) {
        std::cerr << failures << " brush component golden assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All brush component golden tests passed\n";
    return EXIT_SUCCESS;
}
