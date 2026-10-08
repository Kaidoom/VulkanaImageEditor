#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/ToolIcons.hpp"

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QPixmap>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
using namespace imageeditor::ui;
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "FAIL line " << __LINE__ << ": " #condition "\n"; ++failures; } } while (false)

bool containsInk(const QImage& image, QColor ink)
{
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const auto pixel = image.pixelColor(x, y);
            if (pixel.alpha() >= 200 && std::abs(pixel.red() - ink.red()) <= 2
                && std::abs(pixel.green() - ink.green()) <= 2
                && std::abs(pixel.blue() - ink.blue()) <= 2) return true;
        }
    }
    return false;
}

QImage pixels(QPixmap pixmap)
{
    auto result = pixmap.toImage();
    result.setDevicePixelRatio(1);
    return result;
}

void iconsRenderRequestedSizesAndPreserveExplicitTint()
{
    const QColor ink("#D96BAB");
    std::vector<QIcon> icons;
    for (const auto glyph : {ToolGlyph::LayerRaster, ToolGlyph::Folder, ToolGlyph::Group,
             ToolGlyph::ShapeRectangle, ToolGlyph::ShapeRoundedRectangle, ToolGlyph::ShapeEllipse,
             ToolGlyph::ShapeTriangle, ToolGlyph::ShapeLine, ToolGlyph::ShapePolygon,
             ToolGlyph::ObjectSelection}) {
        icons.push_back(toolGlyph(glyph, ink));
    }
    icons.push_back(themedIcon(QStringLiteral(":/icons/text.svg"), ink));
    for (std::size_t i = 0; i < icons.size(); ++i) {
        const auto& icon = icons[i];
        CHECK(!icon.isNull());
        for (const auto logicalSize : {18, 24, 32, 34, 40}) {
            for (const auto dpr : {1.0, 1.25, 1.5, 2.0, 3.0}) {
                const auto failuresBefore = failures;
                const auto expectedPhysical = QSize(qRound(logicalSize * dpr), qRound(logicalSize * dpr));
                const auto normal = icon.pixmap(QSize(logicalSize, logicalSize), dpr, QIcon::Normal);
                const auto selected = icon.pixmap(QSize(logicalSize, logicalSize), dpr, QIcon::Selected);
                CHECK(normal.size() == expectedPhysical);
                CHECK(selected.size() == expectedPhysical);
                CHECK(std::abs(normal.devicePixelRatioF() - dpr) < 1e-8);
                CHECK(std::abs(selected.devicePixelRatioF() - dpr) < 1e-8);
                CHECK(std::abs(normal.deviceIndependentSize().width() - logicalSize) <= 0.5 / dpr + 1e-8);
                CHECK(containsInk(normal.toImage(), ink));
                CHECK(containsInk(selected.toImage(), ink));
                CHECK(pixels(selected) == pixels(normal));
                // Cover QIcon::paint, used by item delegates, as well as
                // pixmap requests. Fractional physical rounding can crop the
                // final half-pixel, so compare the integral target cases.
                if (logicalSize != 34) {
                    QPixmap painted(expectedPhysical);
                    painted.setDevicePixelRatio(dpr);
                    painted.fill(Qt::transparent);
                    QPainter painter(&painted);
                    icon.paint(&painter, QRect(0, 0, logicalSize, logicalSize), Qt::AlignCenter, QIcon::Selected);
                    painter.end();
                    CHECK(pixels(painted) == pixels(selected));
                }
                if (failures != failuresBefore)
                    std::cerr << "  icon " << i << ", logical size " << logicalSize << ", DPR " << dpr << '\n';
            }
        }
        // Equivalent physical requests must rasterize the same geometry.
        // This catches choosing differently sized pre-rendered atlas entries
        // and resampling them instead of drawing at the requested resolution.
        CHECK(pixels(icon.pixmap(QSize(32, 32), 1.25)) == pixels(icon.pixmap(QSize(40, 40), 1.0)));
        CHECK(pixels(icon.pixmap(QSize(40, 40), 1.5)) == pixels(icon.pixmap(QSize(20, 20), 3.0)));
    }
}

void paletteIconsFollowThemeChangesWhileExplicitIconsRetainTheirInk(QApplication& application)
{
    const QColor explicitInk("#D96BAB");
    const auto fixedGlyph = toolGlyph(ToolGlyph::Folder, explicitInk);
    const auto fixedText = themedIcon(QStringLiteral(":/icons/text.svg"), explicitInk);
    const auto paletteGlyph = toolGlyph(ToolGlyph::Folder);
    const auto paletteText = themedIcon(QStringLiteral(":/icons/text.svg"));
    ThemeSettings theme;
    theme.preset = ThemePreset::Custom;
    for (const auto ink : {QColor("#CB6A43"), QColor("#599BC8")}) {
        theme.custom.at(static_cast<std::size_t>(ThemeColor::Text)) = ink;
        applyEditorTheme(application, theme);
        CHECK(containsInk(paletteGlyph.pixmap(QSize(40, 40), 1.25).toImage(), ink));
        CHECK(containsInk(paletteText.pixmap(QSize(40, 40), 1.25).toImage(), ink));
        CHECK(containsInk(fixedGlyph.pixmap(QSize(40, 40), 1.25, QIcon::Selected).toImage(), explicitInk));
        CHECK(containsInk(fixedText.pixmap(QSize(40, 40), 1.25, QIcon::Selected).toImage(), explicitInk));
    }
    applyEditorTheme(application, ThemeSettings());
}

void saveOptionalReviewSheet()
{
    const auto path = qEnvironmentVariable("IMAGEEDITOR_ICON_REVIEW_PATH");
    if (path.isEmpty()) return;
    const auto defaultInk = themeColor(ThemeColor::Thumbnail);
    const auto labelInk = layerLabelColor(imageeditor::core::ColorLabel::Red);
    const std::vector<QString> names {QStringLiteral("Raster"), QStringLiteral("Folder"),
        QStringLiteral("Text"), QStringLiteral("Shape"), QStringLiteral("Object")};
    const auto icons = [](QColor ink) {
        return std::vector<QIcon> {toolGlyph(ToolGlyph::LayerRaster, ink), toolGlyph(ToolGlyph::Folder, ink),
            themedIcon(QStringLiteral(":/icons/text.svg"), ink), toolGlyph(ToolGlyph::ShapeRectangle, ink),
            toolGlyph(ToolGlyph::ObjectSelection, ink)};
    };
    const auto defaults = icons(defaultInk), labels = icons(labelInk);
    QImage sheet(1310, 70 + int(names.size()) * 170 + 20, QImage::Format_ARGB32_Premultiplied);
    sheet.fill(themeColor(ThemeColor::Background));
    QPainter painter(&sheet);
    painter.setPen(themeColor(ThemeColor::Text));
    painter.drawText(QRect(12, 8, 1286, 30), Qt::AlignLeft | Qt::AlignVCenter,
        QStringLiteral("Native physical pixels: 32px default thumbnail / 40px red label (selected)"));
    const std::vector<qreal> scales {1, 1.25, 1.5, 2, 3};
    for (std::size_t column = 0; column < scales.size(); ++column) {
        const auto left = 90 + int(column) * 244;
        painter.drawText(QRect(left, 40, 238, 25), Qt::AlignCenter, QStringLiteral("DPR %1").arg(scales[column]));
        for (std::size_t row = 0; row < defaults.size(); ++row) {
            const auto top = 70 + int(row) * 170;
            if (column == 0)
                painter.drawText(QRect(10, top, 75, 140), Qt::AlignLeft | Qt::AlignVCenter, names[row]);
            painter.fillRect(QRect(left, top, 238, 160), themeColor(ThemeColor::Surface));
            const auto normal = pixels(defaults[row].pixmap(QSize(32, 32), scales[column]));
            const auto selected = pixels(labels[row].pixmap(QSize(40, 40), scales[column], QIcon::Selected));
            painter.drawImage(QPoint(left + 54 - normal.width() / 2, top + 75 - normal.height() / 2), normal);
            painter.drawImage(QPoint(left + 170 - selected.width() / 2, top + 75 - selected.height() / 2), selected);
            painter.drawText(QRect(left + 4, top + 132, 105, 24), Qt::AlignCenter, QStringLiteral("32px"));
            painter.drawText(QRect(left + 113, top + 132, 122, 24), Qt::AlignCenter, QStringLiteral("40px"));
        }
    }
    painter.end();
    CHECK(sheet.save(path));
}
} // namespace

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    applyEditorTheme(application);
    iconsRenderRequestedSizesAndPreserveExplicitTint();
    paletteIconsFollowThemeChangesWhileExplicitIconsRetainTheirInk(application);
    saveOptionalReviewSheet();
    if (failures) return EXIT_FAILURE;
    std::cout << "Tool icon tests passed\n";
    return EXIT_SUCCESS;
}
