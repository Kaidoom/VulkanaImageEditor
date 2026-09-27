#include "imageeditor/ui/LayerListView.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/ToolIcons.hpp"

#include <QApplication>
#include <QFont>
#include <QImage>
#include <QItemSelectionModel>
#include <QPainter>
#include <QStandardItemModel>
#include <QTest>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace ui = imageeditor::ui;
namespace core = imageeditor::core;
namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "FAIL line " << __LINE__ << ": " #condition "\n"; ++failures; } } while (false)

constexpr int primaryRole = Qt::UserRole + 1;
constexpr int indentRole = Qt::UserRole + 2;
constexpr int folderRole = Qt::UserRole + 3;
constexpr int expandedRole = Qt::UserRole + 4;
constexpr int labelRole = Qt::UserRole + 5;
constexpr int visibleRole = Qt::UserRole + 6;
const QColor artworkColor("#30D9B8");

void settle()
{
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

int colorDistance(QColor a, QColor b)
{
    return std::max({std::abs(a.red() - b.red()), std::abs(a.green() - b.green()),
        std::abs(a.blue() - b.blue())});
}

QRect colorBounds(const QImage& image, const QRect& area, QColor color, int tolerance = 2)
{
    QRect result;
    for (int y = area.top(); y <= area.bottom(); ++y) {
        for (int x = area.left(); x <= area.right(); ++x) {
            if (colorDistance(image.pixelColor(x, y), color) <= tolerance)
                result = result.united(QRect(x, y, 1, 1));
        }
    }
    return result;
}

int colorPixelCount(const QImage& image, const QRect& area, QColor color, int tolerance = 16)
{
    int count = 0;
    for (int y = area.top(); y <= area.bottom(); ++y)
        for (int x = area.left(); x <= area.right(); ++x)
            if (colorDistance(image.pixelColor(x, y), color) <= tolerance) ++count;
    return count;
}

struct Fixture {
    QStandardItemModel model;
    ui::LayerListView view;

    Fixture()
    {
        view.setObjectName(QStringLiteral("LayerList"));
        view.setModel(&model);
        view.setSelectionMode(QAbstractItemView::ExtendedSelection);
        view.setEditTriggers(QAbstractItemView::NoEditTriggers);
        view.setFocusPolicy(Qt::NoFocus);
        view.setMouseTracking(true);
        view.setIconSize({34, 34});
        view.resize(368, 342);

        // A mode-invariant thumbnail makes geometry observable without testing
        // platform-specific antialiasing or QIcon's generated selected tint.
        QPixmap artwork(34, 34);
        artwork.fill(Qt::transparent);
        {
            QPainter painter(&artwork);
            painter.fillRect(QRect(6, 6, 22, 22), artworkColor);
        }
        QIcon icon;
        for (const auto mode : {QIcon::Normal, QIcon::Selected, QIcon::Active, QIcon::Disabled})
            icon.addPixmap(artwork, mode);
        const std::array names {"Normal layer", "Primary selection", "Selected child", "Normal child", "Expanded folder"};
        for (int row = 0; row < int(names.size()); ++row) {
            auto* item = new QStandardItem(icon, QString::fromLatin1(names[std::size_t(row)]));
            item->setCheckable(true);
            item->setCheckState(Qt::Checked);
            item->setData(row == 1, primaryRole);
            item->setData(row == 2 || row == 3 ? 32 : row == 4 ? 18 : 0, indentRole);
            item->setData(row == 4, folderRole);
            item->setData(true, expandedRole);
            item->setData(int(core::ColorLabel::Blue), labelRole);
            item->setData(true, visibleRole);
            model.appendRow(item);
        }
        view.selectionModel()->select(model.index(1, 0), QItemSelectionModel::Select);
        view.selectionModel()->select(model.index(2, 0), QItemSelectionModel::Select);
        view.show();
        settle();
        clearHover();
    }

    QRect rowRect(int row) const { return view.visualRect(model.index(row, 0)); }
    QRect eyeRect(int row) const { return view.visibilityIndicatorRect(model.index(row, 0)); }

    void hover(int row)
    {
        QTest::mouseMove(view.viewport(), rowRect(row).center());
        settle();
    }

    void clearHover()
    {
        QTest::mouseMove(view.viewport(), QPoint(view.viewport()->width() / 2, view.viewport()->height() - 2));
        settle();
    }

    QImage render()
    {
        settle();
        // QWidget::render targets logical pixels, including on a HiDPI host.
        QImage image(view.viewport()->size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        view.viewport()->render(&image);
        return image;
    }

    QColor background(const QImage& image, int row) const
    {
        const auto rect = rowRect(row);
        return image.pixelColor(rect.right() - 15, rect.center().y());
    }
};

void selectedRowsUseLayerSelectionAndKeepTheirContentGeometry(Fixture& fixture)
{
    const auto normal = fixture.render();
    const auto selection = ui::themeColor(ui::ThemeColor::LayerSelection);
    const auto accent = ui::themeColor(ui::ThemeColor::Accent);
    const auto base = fixture.background(normal, 0);
    const auto tinted = fixture.background(normal, 1);
    CHECK(selection != accent);
    CHECK(colorDistance(tinted, base) > 1);
    // The interior remains closer to the row surface than to the solid token.
    CHECK(colorDistance(tinted, base) < colorDistance(tinted, selection));
    CHECK(tinted == fixture.background(normal, 2));

    for (const auto row : {1, 2}) {
        const auto rect = fixture.rowRect(row);
        const int stripWidth = row == 1 ? 3 : 2;
        for (int x = 0; x < stripWidth; ++x)
            CHECK(normal.pixelColor(rect.left() + x, rect.center().y()) == selection);
        CHECK(normal.pixelColor(rect.left() + stripWidth, rect.center().y()) == tinted);
        // A primary marker belongs only on the left; sample the previous
        // outline's top/bottom/right locations away from glyphs and labels.
        // Rounded corners may blend back into the surface, but no edge may
        // introduce a stronger border than the subtle interior tint.
        for (const int x : {rect.center().x(), rect.right() - 12}) {
            CHECK(colorDistance(normal.pixelColor(x, rect.top() + 2), base)
                <= colorDistance(tinted, base) + 1);
            CHECK(colorDistance(normal.pixelColor(x, rect.bottom() - 3), base)
                <= colorDistance(tinted, base) + 1);
        }
        CHECK(colorDistance(normal.pixelColor(rect.right() - 3, rect.top() + 2), base)
            <= colorDistance(tinted, base) + 1);
        CHECK(normal.pixelColor(rect.right() - 3, rect.center().y())
            == ui::layerLabelColor(core::ColorLabel::Blue));
    }

    const auto eye = fixture.eyeRect(0);
    const auto childEye = fixture.eyeRect(3);
    const auto icon = colorBounds(normal, fixture.rowRect(0), artworkColor);
    const auto childIcon = colorBounds(normal, fixture.rowRect(3), artworkColor);
    CHECK(!icon.isEmpty());
    CHECK(icon.size() == QSize(22, 22));
    CHECK(childIcon.size() == icon.size());
    CHECK(childIcon.left() - icon.left() == 32);
    CHECK(childEye.size() == eye.size());
    CHECK(childEye.left() - eye.left() == 32);
    const auto row = fixture.rowRect(0);
    fixture.model.item(0)->setData(true, primaryRole);
    fixture.view.selectionModel()->select(fixture.model.index(0, 0), QItemSelectionModel::Select);
    const auto selected = fixture.render();
    CHECK(fixture.rowRect(0) == row);
    CHECK(fixture.eyeRect(0) == eye);
    CHECK(colorBounds(selected, row, artworkColor) == icon);
    const QRect label(row.right() - 4, row.top() + 5, 3, row.height() - 10);
    CHECK(selected.copy(label) == normal.copy(label));
    fixture.view.selectionModel()->select(fixture.model.index(0, 0), QItemSelectionModel::Deselect);
    fixture.model.item(0)->setData(false, primaryRole);
}

void selectedTextUsesItsThemeToken(Fixture& fixture)
{
    // Thin system fonts can consist almost entirely of RGB subpixel fringes.
    // Use solid glyph interiors only for this color assertion, then restore
    // the app font for all geometry checks and the optional visual review.
    const auto originalFont = fixture.view.font();
    auto font = originalFont;
    font.setStyleStrategy(QFont::NoSubpixelAntialias);
    font.setWeight(QFont::DemiBold);
    fixture.view.setFont(font);
    const auto image = fixture.render();
    for (const auto row : {1, 2}) {
        const auto rect = fixture.rowRect(row);
        const auto textArea = QRect(rect.left() + 85 + fixture.model.item(row)->data(indentRole).toInt(),
            rect.top() + 10, rect.width() - 150, rect.height() - 20);
        CHECK(colorPixelCount(image, textArea, ui::themeColor(ui::ThemeColor::SelectedText)) > 8);
    }
    fixture.view.setFont(originalFont);
    settle();
}

void hoverIsSubtleAndDoesNotOverrideSelection(Fixture& fixture)
{
    fixture.clearHover();
    const auto normal = fixture.render();
    fixture.hover(0);
    const auto hover = fixture.render();
    const auto difference = colorDistance(fixture.background(normal, 0), fixture.background(hover, 0));
    CHECK(difference > 0 && difference <= 16);
    CHECK(fixture.background(hover, 0) != fixture.background(normal, 1));
    CHECK(colorBounds(hover, fixture.rowRect(0), artworkColor)
        == colorBounds(normal, fixture.rowRect(0), artworkColor));
    fixture.hover(1);
    const auto selectedHover = fixture.render();
    CHECK(selectedHover.copy(fixture.rowRect(1)) == normal.copy(fixture.rowRect(1)));
    fixture.clearHover();
}

void layerSelectionIsIndependentOfOtherSelectionColors(QApplication& application,
    Fixture& fixture, const ui::ThemeSettings& settings)
{
    fixture.clearHover();
    const auto original = fixture.render();
    auto changed = settings;
    changed.custom[std::size_t(ui::ThemeColor::Selection)] = QColor("#BA721F");
    changed.custom[std::size_t(ui::ThemeColor::Accent)] = QColor("#4854F0");
    ui::applyEditorTheme(application, changed);
    const auto otherTokensChanged = fixture.render();
    for (const auto row : {1, 2})
        CHECK(otherTokensChanged.copy(fixture.rowRect(row)) == original.copy(fixture.rowRect(row)));

    changed.custom[std::size_t(ui::ThemeColor::LayerSelection)] = QColor("#AA6D9C");
    ui::applyEditorTheme(application, changed);
    const auto layerTokenChanged = fixture.render();
    CHECK(ui::themeColor(ui::ThemeColor::Selection) == QColor("#BA721F"));
    CHECK(ui::themeColor(ui::ThemeColor::Accent) == QColor("#4854F0"));
    CHECK(layerTokenChanged.copy(fixture.rowRect(0)) == otherTokensChanged.copy(fixture.rowRect(0)));
    for (const auto row : {1, 2}) {
        const auto rect = fixture.rowRect(row);
        CHECK(layerTokenChanged.pixelColor(rect.left(), rect.center().y()) == QColor("#AA6D9C"));
        CHECK(fixture.background(layerTokenChanged, row) != fixture.background(otherTokensChanged, row));
        const QRect label(rect.right() - 4, rect.top() + 5, 3, rect.height() - 10);
        CHECK(layerTokenChanged.copy(label) == original.copy(label));
    }
    ui::applyEditorTheme(application, settings);
}

void saveReview(const std::vector<QImage>& panels)
{
    const auto path = qEnvironmentVariable("IMAGEEDITOR_LAYER_ROW_REVIEW");
    if (path.isEmpty()) return;
    constexpr int gutter = 16;
    constexpr int header = 42;
    const int width = panels.front().width();
    const int height = panels.front().height();
    QImage sheet(3 * width + 4 * gutter, height + header + gutter, QImage::Format_ARGB32_Premultiplied);
    sheet.fill(QColor("#303030"));
    {
        QPainter painter(&sheet);
        painter.setPen(Qt::white);
        const std::array titles {"Dark", "Light", "Custom: teal layer / gold UI / pink accent"};
        for (int i = 0; i < int(panels.size()); ++i) {
            const auto x = gutter + i * (width + gutter);
            painter.drawText(QRect(x, 0, width, header), Qt::AlignVCenter,
                QString::fromLatin1(titles[std::size_t(i)]));
            painter.drawImage(x, header, panels[std::size_t(i)]);
        }
    }
    CHECK(sheet.save(path));
    std::cout << "Layer row review: " << path.toStdString() << '\n';
}
} // namespace

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    ui::applyEditorTheme(application);
    std::vector<QImage> panels;
    for (const auto preset : {ui::ThemePreset::Dark, ui::ThemePreset::Light, ui::ThemePreset::Custom}) {
        ui::ThemeSettings settings;
        settings.preset = preset;
        if (preset == ui::ThemePreset::Custom) {
            settings.custom[std::size_t(ui::ThemeColor::LayerSelection)] = QColor("#218D7A");
            settings.custom[std::size_t(ui::ThemeColor::Selection)] = QColor("#917A20");
            settings.custom[std::size_t(ui::ThemeColor::Accent)] = QColor("#EA4BA2");
            settings.custom[std::size_t(ui::ThemeColor::SelectedText)] = QColor("#BFFFE8");
        }
        ui::applyEditorTheme(application, settings);
        Fixture fixture;
        selectedRowsUseLayerSelectionAndKeepTheirContentGeometry(fixture);
        selectedTextUsesItsThemeToken(fixture);
        hoverIsSubtleAndDoesNotOverrideSelection(fixture);
        if (preset == ui::ThemePreset::Custom)
            layerSelectionIsIndependentOfOtherSelectionColors(application, fixture, settings);
        fixture.hover(0);
        panels.push_back(fixture.render());
    }
    saveReview(panels);
    if (failures) std::cerr << failures << " layer row style checks failed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
