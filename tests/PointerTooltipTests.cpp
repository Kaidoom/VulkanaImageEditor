#include "imageeditor/render/PointerTooltip.hpp"
#include <QGuiApplication>
#include <QPainter>
#include <cmath>
#include <iostream>
#include <limits>

namespace r = imageeditor::render;
int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        if (!ok) { std::cerr << "FAIL: " << name << '\n'; ++failures; }
    };
    check(r::pointerSizeText({245.2,25.6}) == "H: 26 px, W: 245 px", "rounded size order");
    check(r::pointerSizeText({-245.2,-25.6}) == "H: 26 px, W: 245 px", "positive dimensions through flips");
    check(r::pointerSizeText({0,0}) == "H: 0 px, W: 0 px", "zero scale size");
    check(r::pointerSizeText({NAN,0}).empty(), "invalid input");
    check(r::pointerAngleText(-0.001) == "Angle: 0.0°", "no negative zero");
    check(r::pointerAngleText(-24.25) == "Angle: -24.3°", "signed rounded angle");
    check(r::pointerAngleText(15) == "Angle: 15.0°", "snap angle keeps decimal");
    check(r::pointerAngleText(9) == "Angle: 9.0°" && r::pointerAngleText(9.2) == "Angle: 9.2°",
        "whole and fractional angles retain the same character count");
    check(r::pointerAngleText(INFINITY).empty(), "invalid angle");
    const QSizeF viewport(800,600), size(180,28);
    const auto below = r::pointerTooltipRect({100,100},size,viewport);
    check(below.top() > 100 && below.left() > 100, "offset below pointer");
    for (const auto pointer : {QPointF(0,0),QPointF(799,599),QPointF(-500,-500),QPointF(900,900)}) {
        const auto rect = r::pointerTooltipRect(pointer,size,viewport);
        check(rect.left() >= 0 && rect.right() <= viewport.width()
            && rect.top() >= 0 && rect.bottom() <= viewport.height(), "viewport edge placement");
    }
    check(r::pointerTooltipRect({799,599},size,viewport).bottom() < 599, "flip above bottom edge");
    check(r::pointerTooltipRect({NAN,0},size,viewport).isEmpty(), "invalid position");
    const QRectF workspace(80, 24, 540, 520);
    for (const auto pointer : {QPointF(90,30), QPointF(590,480), QPointF(780,580)}) {
        const auto rect = r::pointerTooltipRect(pointer, size, workspace);
        check(workspace.contains(rect), "readout avoids dock columns and ruler strips");
    }
    r::PointerTooltipCache cache;
    auto font = QGuiApplication::font(); font.setPixelSize(13); font.setWeight(QFont::Medium);
    cache.update(r::pointerAngleText(9),1,font);
    const auto wholeAngleSize = cache.logicalSize();
    cache.update(r::pointerAngleText(9.2),1,font);
    check(cache.logicalSize() == wholeAngleSize, "whole/fractional readout has stable size");
    check(cache.update("H: 26 px, W: 245 px",1,font), "initial raster");
    auto revision = cache.revision(); auto image = cache.image();
    check(!image.isNull() && image.format() == QImage::Format_RGBA8888, "straight rgba raster");
    check(!cache.update("H: 26 px, W: 245 px",1,font) && cache.revision() == revision
        && cache.image().cacheKey() == image.cacheKey(), "no work for unchanged text");
    const auto logical = cache.logicalSize();
    bool white = false, translucent = false;
    for (int y=0; y<image.height(); ++y) for (int x=0; x<image.width(); ++x) {
        const auto color = image.pixelColor(x,y);
        white |= color.red() > 240 && color.green() > 240 && color.blue() > 240 && color.alpha() > 240;
        translucent |= color.red() < 40 && color.alpha() > 180 && color.alpha() < 240;
    }
    check(white && translucent, "white text and translucent dark background");
    check(image.pixelColor(0,0).alpha() == 0, "rounded transparent corners");
    check(cache.update("H: 26 px, W: 245 px",1.5,font) && cache.logicalSize() == logical,
        "DPR-only rebuild preserves logical size");
    check(cache.image().width() == int(std::ceil(logical.width()*1.5)), "fractional DPR raster");
    check(cache.update("Angle: 26°",2,font), "generic Unicode text");
    check(cache.update(std::string(20000,'W'),4,font) && cache.image().width() <= 2560,
        "bounded long-label memory");
    check(cache.update("",1,font) && cache.image().isNull(), "empty label clears cache");
    if (const auto path = qEnvironmentVariable("IMAGEEDITOR_TEST_TOOLTIP_PREVIEW"); !path.isEmpty()) {
        QImage sheet(900,360,QImage::Format_ARGB32_Premultiplied);
        sheet.fill(QColor("#40454e")); QPainter painter(&sheet);
        painter.setPen(Qt::white); painter.setFont(font);
        const std::array texts {"H: 26 px, W: 245 px", "Angle: -24.3°", "Other tool information"};
        for (std::size_t i=0; i<texts.size(); ++i) {
            cache.update(texts[i],2,font);
            painter.drawText(QPointF(30,40+100*double(i)), QStringLiteral("Pointer tooltip · cached 2× raster"));
            painter.drawImage(QPointF(45,58+100*double(i)),cache.image());
        }
        painter.end(); check(sheet.save(path), "save review sheet");
    }
    std::cout << "Pointer tooltip cache/layout tests: " << failures << " failures\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
