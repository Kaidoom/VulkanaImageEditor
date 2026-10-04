#include "imageeditor/ui/ToolIcons.hpp"
#include <QPainter>
#include <QPixmap>
#include <QPainterPath>
#include <QLinearGradient>
#include <QApplication>
#include <QIconEngine>
#include <algorithm>
#include <cmath>
#include <optional>
#include <iterator>
#include <map>
#include <utility>
namespace imageeditor::ui {
QColor layerLabelColor(core::ColorLabel label)
{
    static const QColor labels[] = { Qt::transparent, QColor("#dd747d"), QColor("#dd9b66"),
        QColor("#d7c273"), QColor("#83bb92"), QColor("#839fd7"), QColor("#ba93d6") };
    const auto index = static_cast<std::size_t>(label);
    return index < std::size(labels) ? labels[index] : QColor(Qt::transparent);
}
namespace {
QPixmap rasterizedGlyph(ToolGlyph glyph, const QColor& ink, const QSize& size, qreal scale,
    QIcon::State state)
{
    // The 24-unit geometry is a vector coordinate system, not a fixed-size
    // bitmap. Render it directly into the requested physical resolution.
    QPixmap pixmap(size * scale);
    pixmap.setDevicePixelRatio(scale);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    const auto logical = pixmap.deviceIndependentSize();
    const auto side = std::min(logical.width(), logical.height());
    p.translate((logical.width() - side) / 2, (logical.height() - side) / 2);
    p.scale(side / 24, side / 24);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(ink, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    if (glyph == ToolGlyph::Person) {
        p.drawEllipse(QPointF(12, 7), 4, 4);
        QPainterPath shoulders;
        shoulders.moveTo(3, 21); shoulders.cubicTo(3, 11, 21, 11, 21, 21);
        shoulders.closeSubpath(); p.drawPath(shoulders);
    } else if (glyph == ToolGlyph::SocialX) {
        p.drawPolygon(QPolygonF{{4, 3}, {8, 3}, {20, 21}, {16, 21}});
        p.drawLine(QPointF(20, 3), QPointF(4, 21));
    } else if (glyph == ToolGlyph::Repository) {
        // A code repository/book, in the same original line style as contacts.
        p.drawRoundedRect(QRectF(3, 2, 18, 20), 2, 2);
        p.drawLine(QPointF(3, 18), QPointF(21, 18));
        p.drawPolyline(QPolygonF{{10, 7}, {7, 10}, {10, 13}});
        p.drawPolyline(QPolygonF{{14, 7}, {17, 10}, {14, 13}});
    } else if (glyph == ToolGlyph::Mail) {
        p.drawRoundedRect(QRectF(2, 5, 20, 15), 2, 2);
        p.drawPolyline(QPolygonF{{3, 6}, {12, 13}, {21, 6}});
    } else if (glyph == ToolGlyph::Globe) {
        p.drawEllipse(QRectF(2, 2, 20, 20));
        p.drawEllipse(QRectF(7, 2, 10, 20));
        p.drawLine(QPointF(3, 8), QPointF(21, 8));
        p.drawLine(QPointF(3, 16), QPointF(21, 16));
    } else if (glyph == ToolGlyph::ExternalLink) {
        p.drawPolyline(QPolygonF{{10, 4}, {3, 4}, {3, 21}, {20, 21}, {20, 14}});
        p.drawPolyline(QPolygonF{{14, 3}, {21, 3}, {21, 10}});
        p.drawLine(QPointF(10, 14), QPointF(21, 3));
    } else if (glyph == ToolGlyph::Refresh) {
        p.drawArc(QRectF(4, 4, 16, 16), 35 * 16, 285 * 16);
        p.drawPolyline(QPolygonF{{16, 8}, {20, 7}, {19, 3}});
    } else if (glyph == ToolGlyph::Download) {
        p.drawLine(QPointF(12, 3), QPointF(12, 16));
        p.drawPolyline(QPolygonF{{7, 11}, {12, 16}, {17, 11}});
        p.drawPolyline(QPolygonF{{3, 16}, {3, 21}, {21, 21}, {21, 16}});
    } else if (glyph == ToolGlyph::CheckCircle || glyph == ToolGlyph::InfoCircle) {
        p.drawEllipse(QRectF(2, 2, 20, 20));
        if (glyph == ToolGlyph::CheckCircle)
            p.drawPolyline(QPolygonF{{6, 12}, {10, 16}, {18, 8}});
        else {
            p.drawPoint(QPointF(12, 7));
            p.drawLine(QPointF(12, 11), QPointF(12, 17));
        }
    } else if (glyph == ToolGlyph::Close) {
        p.drawLine(QPointF(5, 5), QPointF(19, 19));
        p.drawLine(QPointF(19, 5), QPointF(5, 19));
    } else if (glyph == ToolGlyph::ShowSource) {
        QPainterPath eye;
        eye.moveTo(2, 12);
        eye.cubicTo(7, 4, 17, 4, 22, 12);
        eye.cubicTo(17, 20, 7, 20, 2, 12);
        p.drawPath(eye);
        p.drawEllipse(QPointF(12, 12), 3, 3);
        if (state == QIcon::Off)
            p.drawLine(QPointF(4, 3), QPointF(20, 21));
    } else if (glyph == ToolGlyph::Chamfer) {
        if (state == QIcon::On) {
            p.drawPolygon(QPolygonF{{8, 3}, {16, 3}, {21, 8}, {21, 16},
                {16, 21}, {8, 21}, {3, 16}, {3, 8}});
        } else {
            p.drawRect(QRectF(3, 3, 18, 18));
            p.setPen(QPen(ink, 1, Qt::DashLine, Qt::RoundCap));
            for (int turn = 0; turn < 4; ++turn) {
                p.drawLine(QPointF(3, 8), QPointF(8, 3));
                p.translate(12, 12);
                p.rotate(90);
                p.translate(-12, -12);
            }
        }
    } else if (glyph == ToolGlyph::ResetCrop) {
        // A retained frame with a return arrow restoring its full extent.
        p.drawPolyline(QPolygonF{{3, 9}, {3, 3}, {9, 3}});
        p.drawPolyline(QPolygonF{{15, 3}, {21, 3}, {21, 9}});
        p.drawPolyline(QPolygonF{{21, 15}, {21, 21}, {15, 21}});
        p.drawPolyline(QPolygonF{{9, 21}, {3, 21}, {3, 15}});
        QPainterPath restore;
        restore.moveTo(7, 10);
        restore.cubicTo(9, 6, 17, 7, 17, 13);
        restore.cubicTo(17, 17, 12, 19, 9, 16);
        p.drawPath(restore);
        p.drawPolyline(QPolygonF{{7, 6}, {7, 10}, {11, 10}});
    } else if (glyph == ToolGlyph::AspectLock) {
        p.drawRoundedRect(QRectF(5, 10, 14, 11), 2, 2);
        QPainterPath shackle;
        shackle.moveTo(8, 10);
        shackle.lineTo(8, 7);
        shackle.cubicTo(8, 1.5, 16, 1.5, 16, 7);
        if (state == QIcon::On)
            shackle.lineTo(16, 10);
        p.drawPath(shackle);
        p.drawLine(QPointF(12, 14), QPointF(12, 17));
    } else if (glyph == ToolGlyph::FlipHorizontal || glyph == ToolGlyph::FlipVertical) {
        // Mirrored silhouettes around a dotted axis; rotating the complete
        // glyph makes the two directions distinguishable at toolbar sizes.
        if (glyph == ToolGlyph::FlipVertical) {
            p.translate(12, 12);
            p.rotate(90);
            p.translate(-12, -12);
        }
        p.setPen(QPen(ink, 1, Qt::DashLine, Qt::RoundCap));
        p.drawLine(QPointF(12, 2), QPointF(12, 22));
        p.setPen(QPen(ink, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(ink);
        p.drawPolygon(QPolygonF{{3, 5}, {9, 12}, {3, 19}});
        p.setBrush(Qt::NoBrush);
        p.drawPolygon(QPolygonF{{21, 5}, {15, 12}, {21, 19}});
    } else if (glyph == ToolGlyph::Crop) {
        p.drawPolyline(QPolygonF{{3,7},{17,7},{17,22}});
        p.drawPolyline(QPolygonF{{7,2},{7,17},{22,17}});
        p.setPen(QPen(ink,.8,Qt::DashLine));p.drawLine(10,14,21,3);
    } else if (glyph == ToolGlyph::SelectByColor) {
        p.setPen(QPen(ink, 1.2, Qt::DashLine)); p.drawRoundedRect(QRectF(2,2,20,20),4,4);
        p.setPen(QPen(ink,1.5)); p.setBrush(ink);
        p.drawEllipse(QPointF(8,8),2.5,2.5); p.drawEllipse(QPointF(16,16),2.5,2.5);
        p.setBrush(Qt::NoBrush); p.drawEllipse(QPointF(16,8),2.5,2.5);
    } else if (glyph == ToolGlyph::QuickSelection) {
        // A dotted selection contour opens around the diagonal brush tip.
        QPainterPath selection;
        selection.moveTo(13, 3);
        selection.cubicTo(5, 1, 1, 6, 3, 12);
        selection.cubicTo(1, 17, 5, 22, 12, 21);
        p.setPen(QPen(ink, 1.3, Qt::DotLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(selection);
        p.setPen(QPen(ink, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPainterPath handle;
        handle.moveTo(10.5, 12.5);
        handle.lineTo(18, 3.5);
        handle.cubicTo(20, 1.5, 23, 4, 21, 6);
        handle.lineTo(13, 15);
        handle.closeSubpath();
        p.drawPath(handle);
        p.drawLine(QPointF(12, 10.7), QPointF(14.7, 13.1));
        QPainterPath bristles;
        bristles.moveTo(10.5, 13);
        bristles.cubicTo(7, 13, 9, 17, 5.5, 18.5);
        bristles.cubicTo(10, 20, 14, 18, 13, 15);
        bristles.closeSubpath();
        p.setBrush(ink);
        p.drawPath(bristles);
    } else if (glyph == ToolGlyph::MagicWand) {
        // A narrow wand and three distinct sparks stay clear at toolbar sizes.
        p.drawPolygon(QPolygonF{{3, 18.5}, {14.5, 7}, {17, 9.5}, {5.5, 21}});
        p.drawLine(QPointF(12, 9.5), QPointF(14.5, 12));
        p.setBrush(ink);
        p.drawPolygon(QPolygonF{{18, 1.5}, {19, 4}, {21.5, 5}, {19, 6},
            {18, 8.5}, {17, 6}, {14.5, 5}, {17, 4}});
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(8, 2), QPointF(8, 6));
        p.drawLine(QPointF(6, 4), QPointF(10, 4));
        p.drawLine(QPointF(20, 13), QPointF(20, 17));
        p.drawLine(QPointF(18, 15), QPointF(22, 15));
    } else if (glyph == ToolGlyph::Cloning || glyph == ToolGlyph::CloneStamp) {
        // A broad stamp base and tapered handle. The tool-rail variant adds a
        // small offset source target; the mode glyph keeps the stamp uncluttered.
        const bool rail = glyph == ToolGlyph::Cloning;
        p.save();
        if (rail) {
            p.translate(-0.8, 0.3);
            p.scale(0.83, 0.83);
        }
        QPainterPath handle;
        handle.moveTo(8.4, 4.2);
        handle.cubicTo(8.4, 0.8, 15.6, 0.8, 15.6, 4.2);
        handle.cubicTo(15.6, 7.2, 13.9, 8.2, 13.9, 11.4);
        handle.lineTo(10.1, 11.4);
        handle.cubicTo(10.1, 8.2, 8.4, 7.2, 8.4, 4.2);
        handle.closeSubpath();
        p.drawPath(handle);
        QPainterPath base;
        base.moveTo(5.8, 12.4);
        base.lineTo(18.2, 12.4);
        base.quadTo(20.3, 12.4, 20.8, 17.5);
        base.lineTo(3.2, 17.5);
        base.quadTo(3.7, 12.4, 5.8, 12.4);
        base.closeSubpath();
        p.drawPath(base);
        p.drawLine(QPointF(4, 21), QPointF(20, 21));
        p.restore();
        if (rail) {
            p.drawEllipse(QPointF(19, 18), 2.7, 2.7);
            p.drawLine(QPointF(19, 13.5), QPointF(19, 16));
            p.drawLine(QPointF(19, 20), QPointF(19, 22.5));
            p.drawLine(QPointF(14.5, 18), QPointF(17, 18));
            p.drawLine(QPointF(21, 18), QPointF(23, 18));
        }
    } else if (glyph == ToolGlyph::SpotHeal) {
        // Original automatic repair mark: an open blemish ring and sparkle.
        p.drawArc(QRectF(3,7,13,13),20*16,295*16);
        p.drawLine(QPointF(6.5,15.5),QPointF(9,18));
        p.drawLine(QPointF(9,18),QPointF(14,12));
        QPainterPath sparkle;
        sparkle.moveTo(17,2);sparkle.lineTo(18.5,6.5);sparkle.lineTo(23,8);
        sparkle.lineTo(18.5,9.5);sparkle.lineTo(17,14);sparkle.lineTo(15.5,9.5);
        sparkle.lineTo(11,8);sparkle.lineTo(15.5,6.5);sparkle.closeSubpath();
        p.drawPath(sparkle);
    } else if (glyph == ToolGlyph::CloneHeal) {
        // An original diagonal repair strip with a central woven patch.
        p.translate(12, 12);
        p.rotate(-42);
        p.drawRoundedRect(QRectF(-10.5, -4.8, 21, 9.6), 3.5, 3.5);
        p.drawRoundedRect(QRectF(-3.3, -3.1, 6.6, 6.2), 0.8, 0.8);
        p.setPen(Qt::NoPen);
        p.setBrush(ink);
        for (const auto x : {-7.3, -5.6, 5.6, 7.3})
            for (const auto y : {-1.5, 1.5})
                p.drawEllipse(QPointF(x, y), 0.6, 0.6);
        p.drawEllipse(QPointF(-1.1, -1.1), 0.65, 0.65);
        p.drawEllipse(QPointF(1.1, 1.1), 0.65, 0.65);
    } else if (glyph == ToolGlyph::CloneSource) {
        p.drawEllipse(QPointF(12, 12), 6.5, 6.5);
        for (int turn = 0; turn < 4; ++turn) {
            p.drawLine(QPointF(12, 2), QPointF(12, 8));
            p.translate(12, 12);
            p.rotate(90);
            p.translate(-12, -12);
        }
        p.setPen(Qt::NoPen);
        p.setBrush(ink);
        p.drawEllipse(QPointF(12, 12), 1.5, 1.5);
    } else if (glyph == ToolGlyph::CurrentAndBelow) {
        // The picked layer is solid; an arrow continues down to the lower
        // layers. Use the same diamond silhouette as the other source modes.
        p.setBrush(ink);
        p.drawPolygon(QPolygonF{{3, 6}, {10, 2}, {17, 6}, {10, 10}});
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(QPolygonF{{3, 11}, {10, 15}, {16, 11.5}});
        p.drawPolyline(QPolygonF{{3, 16}, {10, 20}, {16, 16.5}});
        p.drawLine(QPointF(20, 8), QPointF(20, 20));
        p.drawPolyline(QPolygonF{{17.5, 17.5}, {20, 20}, {22.5, 17.5}});
    } else if (glyph == ToolGlyph::CloneAligned) {
        // Matching source/destination targets retain a common offset. Breaking
        // the link for Off reinforces the independent-stroke reset behavior.
        p.drawEllipse(QPointF(6, 7), 3.7, 3.7);
        p.drawEllipse(QPointF(18, 17), 3.7, 3.7);
        if (state == QIcon::On) {
            p.drawLine(QPointF(9, 9.5), QPointF(15, 14.5));
        } else {
            p.drawLine(QPointF(9, 9.5), QPointF(10.5, 10.75));
            p.drawLine(QPointF(13.5, 13.25), QPointF(15, 14.5));
        }
        p.setPen(Qt::NoPen);
        p.setBrush(ink);
        p.drawEllipse(QPointF(6, 7), 1.1, 1.1);
        p.drawEllipse(QPointF(18, 17), 1.1, 1.1);
    } else if (glyph == ToolGlyph::FollowStrokeDirection) {
        // A narrow tip follows a curved stroke toward the arrowhead.
        QPainterPath stroke;
        stroke.moveTo(3, 20);
        stroke.cubicTo(16, 20, 8, 5, 21, 5);
        p.drawPath(stroke);
        p.drawPolyline(QPolygonF{{18, 2}, {21, 5}, {18, 8}});
        p.save();
        p.translate(11.5, 12);
        p.rotate(30);
        p.setBrush(ink);
        p.drawEllipse(QRectF(-1.5, -3.5, 3, 7));
        p.restore();
    } else if (glyph == ToolGlyph::LocalBlur) {
        // A smooth droplet and soft concentric center, distinct from Heal's
        // repair strip. Procedural alpha rings remain sharp at every DPR.
        QPainterPath drop;
        drop.moveTo(12, 2);
        drop.cubicTo(10, 7, 5, 10, 5, 15);
        drop.cubicTo(5, 24, 19, 24, 19, 15);
        drop.cubicTo(19, 10, 14, 7, 12, 2);
        p.drawPath(drop);
        for (int ring = 4; ring >= 1; --ring) {
            auto soft = ink;
            soft.setAlphaF(0.12F + 0.12F * static_cast<float>(4 - ring));
            p.setPen(Qt::NoPen); p.setBrush(soft);
            p.drawEllipse(QPointF(12, 15), ring, ring);
        }
    } else if (glyph == ToolGlyph::AdjustmentLayer) {
        for(int i=0;i<3;++i) {
            const auto x=6+i*6, y=i==1?8:15;
            p.drawLine(QPointF(x,3),QPointF(x,y-2));
            p.drawLine(QPointF(x,y+2),QPointF(x,21));
            p.drawRoundedRect(QRectF(x-2,y-2,4,4),1,1);
        }
    } else if (glyph == ToolGlyph::NewLayer) {
        p.drawPolyline(QPolygonF{{6,17},{3,17},{3,3},{16,3},{16,6}});
        p.drawRoundedRect(QRectF(7,7,14,14),1,1);
    } else if (glyph == ToolGlyph::Trash) {
        p.drawRoundedRect(QRectF(6,7,12,14),1,1);
        p.drawLine(QPointF(4,6),QPointF(20,6));
        p.drawPolyline(QPolygonF{{9,6},{9,3},{15,3},{15,6}});
        p.drawLine(QPointF(10,10),QPointF(10,18));
        p.drawLine(QPointF(14,10),QPointF(14,18));
    } else if (glyph == ToolGlyph::LayerMask) {
        // A fading coverage tile, using palette ink so both themes stay legible.
        auto middle=ink, transparent=ink;
        middle.setAlphaF(ink.alphaF()*.55F);
        transparent.setAlphaF(ink.alphaF()*.06F);
        QLinearGradient coverage(QPointF(4,8),QPointF(20,16));
        coverage.setColorAt(0,ink);
        coverage.setColorAt(.5,middle);
        coverage.setColorAt(1,transparent);
        p.setBrush(coverage);
        p.setPen(QPen(ink,1.25,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
        p.drawPolygon(QPolygonF{{12,2},{22,12},{12,22},{2,12}});
    } else if (glyph == ToolGlyph::LayerRaster) {
        p.setBrush(QApplication::palette().color(QPalette::Button));
        p.setPen(QPen(QApplication::palette().color(QPalette::Mid), 1));
        p.drawRoundedRect(QRectF(2.5, 2.5, 19, 19), 3, 3);
        p.setPen(Qt::NoPen); p.setBrush(ink);
        p.drawPolygon(QPolygonF({{3,16},{9,10},{13,14},{16,11},{21,17},{21,21},{3,21}}));
        p.drawEllipse(QPointF(16.5, 7.5), 2.3, 2.3);
    } else if (glyph==ToolGlyph::Folder) {
        QPainterPath path; path.moveTo(3,8);path.lineTo(3,6);path.quadTo(3,4,5,4);
        path.lineTo(10,4);path.lineTo(13,7);path.lineTo(20,7);path.quadTo(21,7,21,9);
        path.lineTo(21,18);path.quadTo(21,20,19,20);path.lineTo(5,20);path.quadTo(3,20,3,18);path.closeSubpath();
        p.drawPath(path);p.drawLine(3,10,21,10);
    } else if(glyph==ToolGlyph::ClippingGroup) {
        p.drawRoundedRect(QRectF(3,12,18,9),2,2);
        p.drawRoundedRect(QRectF(7,3,10,6),1,1);
        p.drawLine(12,9,12,16);p.drawLine(9,13,12,16);p.drawLine(15,13,12,16);
    } else if(glyph==ToolGlyph::Group) {
        p.drawRoundedRect(QRectF(3,8,13,13),2,2);p.drawRoundedRect(QRectF(8,3,13,13),2,2);
    } else if (glyph >= ToolGlyph::ShapeRectangle) {
        if (glyph==ToolGlyph::ShapeRectangle) p.drawRect(QRectF(4,5,16,14));
        else if (glyph==ToolGlyph::ShapeRoundedRectangle) p.drawRoundedRect(QRectF(4,5,16,14),4,4);
        else if (glyph==ToolGlyph::ShapeEllipse) p.drawEllipse(QRectF(3,5,18,14));
        else if (glyph==ToolGlyph::ShapeTriangle) p.drawPolygon(QPolygonF({{12,3},{22,21},{2,21}}));
        else if (glyph==ToolGlyph::ShapeLine) p.drawLine(QPointF(4,20),QPointF(20,4));
        else if (glyph==ToolGlyph::ShapePolygon) p.drawPolygon(QPolygonF({{12,2},{22,9},{18,21},{5,20},{2,8}}));
        else if (glyph==ToolGlyph::ShapeFill) { p.setBrush(ink); p.drawRect(QRectF(5,5,14,14)); }
        else { p.setPen(QPen(ink,3)); p.drawRect(QRectF(5,5,14,14)); }
    } else if (glyph==ToolGlyph::SelectRectangle || glyph==ToolGlyph::SelectEllipse) {
        p.setPen(QPen(ink,1.5,Qt::DashLine));
        if(glyph==ToolGlyph::SelectEllipse) p.drawEllipse(QRectF(3,4,18,16));
        else p.drawRect(QRectF(3,4,18,16));
    } else if (glyph==ToolGlyph::LassoFreehand || glyph==ToolGlyph::LassoMagnetic) {
        QPainterPath path; path.moveTo(8,18); path.cubicTo(-2,12,5,3,13,4);
        path.cubicTo(24,3,25,18,14,18); path.cubicTo(6,18,6,13,10,12);
        p.drawPath(path); p.drawLine(QPointF(10,12),QPointF(8,22));
        if (glyph==ToolGlyph::LassoMagnetic) {
            p.fillRect(QRectF(13,12,11,12),QApplication::palette().color(QPalette::Button));
            QPainterPath magnet; magnet.moveTo(15,13); magnet.lineTo(15,18);
            magnet.cubicTo(15,23,22,23,22,18); magnet.lineTo(22,13);
            p.setPen(QPen(ink,2.6)); p.drawPath(magnet);
            p.setPen(QPen(QApplication::palette().color(QPalette::Button),1)); p.drawLine(14,16,16,16); p.drawLine(21,16,23,16);
        }
    } else if (glyph==ToolGlyph::LassoPolygonal) {
        p.drawPolygon(QPolygonF({{4,17},{7,4},{20,7},{18,19}}));
        p.setBrush(QApplication::palette().color(QPalette::Button));
        for (auto point:{QPointF(4,17),QPointF(7,4),QPointF(20,7),QPointF(18,19)})
            p.drawRect(QRectF(point.x()-1.5,point.y()-1.5,3,3));
    } else if (glyph == ToolGlyph::SelectionAdjustments) {
        // Coverage boundary with outward/inward arrows, not transform handles.
        p.setPen(QPen(ink, 1.4, Qt::DashLine));
        p.drawRect(QRectF(7, 7, 10, 10));
        p.setPen(QPen(ink, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        for (int i = 0; i < 4; ++i) {
            p.save(); p.translate(12, 12); p.rotate(i * 90);
            p.drawLine(QPointF(0, -8), QPointF(0, -11));
            p.drawPolyline(QPolygonF({{-2,-9},{0,-11},{2,-9}}));
            p.restore();
        }
    } else if (glyph == ToolGlyph::Transform) {
        p.drawRect(QRectF(5, 5, 14, 14));
        p.setBrush(QApplication::palette().color(QPalette::Button));
        for (const auto pt : { QPointF(5, 5), QPointF(12, 5), QPointF(19, 5), QPointF(19, 12),
                 QPointF(19, 19), QPointF(12, 19), QPointF(5, 19), QPointF(5, 12) })
            p.drawRect(QRectF(pt.x() - 1.5, pt.y() - 1.5, 3, 3));
    } else if (glyph == ToolGlyph::MergedVisible || glyph == ToolGlyph::ActiveLayer) {
        if (glyph == ToolGlyph::MergedVisible) {
            p.drawPolyline(QPolygonF({ { 3, 12 }, { 12, 17 }, { 21, 12 } }));
            p.drawPolyline(QPolygonF({ { 3, 16 }, { 12, 21 }, { 21, 16 } }));
        }
        p.drawPolygon(QPolygonF({ { 3, 7 }, { 12, 2 }, { 21, 7 }, { 12, 12 } }));
        if (glyph == ToolGlyph::ActiveLayer) {
            p.setBrush(ink);
            p.drawEllipse(QPointF(12, 18), 2, 2);
        }
    } else if (glyph == ToolGlyph::Measure) {
        // Original diagonal ruler: a slim measuring edge with three clear
        // graduations, vector-rendered through the shared DPR-aware cache.
        p.translate(12, 12);
        p.rotate(-40);
        p.drawRoundedRect(QRectF(-10, -4, 20, 8), 1.3, 1.3);
        for (int x : {-5, 0, 5}) p.drawLine(QPointF(x, -4), QPointF(x, x == 0 ? 1 : -1));
    } else if (glyph == ToolGlyph::UnderMouse) {
        // Mouse silhouette with the primary button picked out.
        p.drawRoundedRect(QRectF(6, 2, 12, 20), 6, 6);
        p.drawLine(QPointF(6, 10), QPointF(18, 10));
        p.drawLine(QPointF(12, 2), QPointF(12, 10));
        p.setPen(Qt::NoPen);
        p.setBrush(ink);
        p.drawRoundedRect(QRectF(8, 5, 2, 3), 1, 1);
    } else if (glyph == ToolGlyph::FillSelection || glyph == ToolGlyph::FillContiguous) {
        // Shared canvas outline: all selected islands versus one connected
        // area grown from the visible seed dot.
        p.setPen(QPen(ink, 1.2, Qt::DashLine));
        p.drawRect(QRectF(3, 3, 18, 18));
        p.setPen(Qt::NoPen);
        p.setBrush(ink);
        if (glyph == ToolGlyph::FillSelection) {
            p.drawRect(QRectF(6, 6, 5, 5));
            p.drawRect(QRectF(14, 7, 4, 4));
            p.drawRect(QRectF(8, 14, 10, 4));
        } else {
            p.drawPolygon(QPolygonF({ { 6, 6 }, { 14, 6 }, { 14, 10 }, { 18, 10 },
                { 18, 18 }, { 10, 18 }, { 10, 14 }, { 6, 14 } }));
            p.setBrush(QApplication::palette().color(QPalette::Button));
            p.drawEllipse(QPointF(12, 12), 1.8, 1.8);
        }
    } else {
        p.setPen(QPen(ink, 1.4, Qt::DashLine));
        p.drawRect(QRectF(3, 6, 14, 14));
        p.setPen(QPen(ink, 1.7, Qt::SolidLine, Qt::RoundCap));
        if (glyph == ToolGlyph::Add || glyph == ToolGlyph::Subtract) {
            p.fillRect(QRectF(12, 1, 11, 11), QApplication::palette().color(QPalette::Button));
            p.drawLine(QPointF(14, 6), QPointF(22, 6));
            if (glyph == ToolGlyph::Add)
                p.drawLine(QPointF(18, 2), QPointF(18, 10));
        } else if (glyph == ToolGlyph::Intersect) {
            p.drawRect(QRectF(10, 2, 11, 11));
            p.fillRect(QRectF(10, 6, 7, 7), ink);
        }
    }
    p.end();
    return pixmap;
}
class ThemeIconEngine final : public QIconEngine {
public:
    explicit ThemeIconEngine(ToolGlyph glyph, std::optional<QColor> ink = {}) : glyph_(glyph), ink_(ink) {}
    ThemeIconEngine(QString resource, QPalette::ColorRole role) : source_(resource), role_(role) {}
    ThemeIconEngine(QString resource, const QColor& ink) : ink_(ink), source_(resource) {}
    QIconEngine* clone() const override { return new ThemeIconEngine(*this); }
    bool isNull() override { return false; }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    { return scaledPixmap(size, mode, state, 1.0); }
    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        if (size.isEmpty() || !std::isfinite(scale) || scale <= 0) return {};
        const auto palette = QApplication::palette();
        if (paletteKey_ != palette.cacheKey()) { cache_.clear(); paletteKey_ = palette.cacheKey(); }
        const auto key = std::make_tuple(size.width(), size.height(), scale, mode, state);
        if (auto it = cache_.find(key); it != cache_.end()) return it->second;
        QPixmap result;
        const auto color = ink_.value_or(palette.color(mode == QIcon::Disabled ? QPalette::Disabled : QPalette::Active, role_));
        // Do not pass explicit label/default ink through Qt's Selected icon
        // recolouring: selection changes the row background, not the label.
        if (glyph_) result = rasterizedGlyph(*glyph_, color, size, scale, state);
        else {
            result = source_.pixmap(size, scale, QIcon::Normal, state);
            QPainter painter(&result); painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
            painter.fillRect(result.rect(), color);
        }
        if (cache_.size() >= 64) cache_.clear();
        cache_.emplace(key, result); return result;
    }
    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override
    {
        const auto image = scaledPixmap(rect.size(), mode, state, painter->device()->devicePixelRatioF());
        painter->drawPixmap(rect, image);
    }
private:
    std::optional<ToolGlyph> glyph_;
    std::optional<QColor> ink_;
    QIcon source_;
    QPalette::ColorRole role_ {QPalette::Text};
    qint64 paletteKey_ {0};
    std::map<std::tuple<int, int, qreal, QIcon::Mode, QIcon::State>, QPixmap> cache_;
};
}
QIcon toolGlyph(ToolGlyph glyph)
{
    static std::map<ToolGlyph, QIcon> icons;
    if (auto found = icons.find(glyph); found != icons.end()) return found->second;
    return icons.emplace(glyph, QIcon(new ThemeIconEngine(glyph))).first->second;
}
QIcon toolGlyph(ToolGlyph glyph, const QColor& ink)
{
    static std::map<std::pair<ToolGlyph, QRgb>, QIcon> icons;
    const auto key = std::make_pair(glyph, ink.rgba());
    if (auto found = icons.find(key); found != icons.end()) return found->second;
    if (icons.size() >= 256) icons.clear();
    return icons.emplace(key, QIcon(new ThemeIconEngine(glyph, ink))).first->second;
}
QIcon themedIcon(const QString& resource, QPalette::ColorRole role)
{
    static std::map<std::pair<QString, QPalette::ColorRole>, QIcon> icons;
    const auto key = std::make_pair(resource, role);
    if (auto found = icons.find(key); found != icons.end()) return found->second;
    return icons.emplace(key, QIcon(new ThemeIconEngine(resource, role))).first->second;
}
QIcon themedIcon(const QString& resource, const QColor& ink)
{
    static std::map<std::pair<QString, QRgb>, QIcon> icons;
    const auto key = std::make_pair(resource, ink.rgba());
    if (auto found = icons.find(key); found != icons.end()) return found->second;
    if (icons.size() >= 256) icons.clear();
    return icons.emplace(key, QIcon(new ThemeIconEngine(resource, ink))).first->second;
}
}
