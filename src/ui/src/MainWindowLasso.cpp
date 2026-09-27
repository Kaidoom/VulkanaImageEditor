#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include <QStatusBar>
#include <QTimer>
#include <algorithm>
#include <array>
#include <cmath>

namespace imageeditor::ui {
namespace {
    double length(core::Vec2d v) { return std::hypot(v.x, v.y); }
    // Compare geometry, not stored vertex count: exact collinear compression can
    // move an endpoint even when a long prefix is unchanged.
    double commonLength(std::span<const core::Vec2d> a, std::span<const core::Vec2d> b)
    {
        if (a.empty() || b.empty() || a.front() != b.front())
            return 0;
        std::size_t i = 1, j = 1;
        auto p = a.front();
        double distance = 0;
        while (i < a.size() && j < b.size()) {
            const auto u = a[i] - p, v = b[j] - p;
            const double lu = length(u), lv = length(v);
            if (lu < 1e-9) {
                ++i;
                continue;
            }
            if (lv < 1e-9) {
                ++j;
                continue;
            }
            if (std::abs(u.x * v.y - u.y * v.x) > 1e-7 * lu * lv || u.x * v.x + u.y * v.y <= 0)
                break;
            const double step = std::min(lu, lv);
            p = p + u * (step / lu);
            distance += step;
            if (lu <= lv + 1e-9)
                ++i;
            if (lv <= lu + 1e-9)
                ++j;
        }
        return distance;
    }
    std::pair<std::vector<core::Vec2d>, std::vector<core::Vec2d>> splitPath(
        std::span<const core::Vec2d> points, double at)
    {
        std::vector<core::Vec2d> prefix { points.front() }, tail;
        for (std::size_t i = 1; i < points.size(); ++i) {
            const auto d = points[i] - points[i - 1];
            const double distance = length(d);
            if (distance <= at) {
                prefix.push_back(points[i]);
                at -= distance;
                continue;
            }
            const auto cut = points[i - 1] + d * (at / distance);
            prefix.push_back(cut);
            tail.push_back(cut);
            tail.insert(tail.end(), points.begin() + std::ptrdiff_t(i), points.end());
            return { prefix, tail };
        }
        tail.push_back(points.back());
        return { prefix, tail };
    }
}
bool MainWindow::anchoredLassoActive() const
{
    return selectionGesture_ && selectionGesture_->lasso && !selectionGesture_->moving
        && selectionGesture_->mode != core::LassoMode::Freehand
        && canvasWindow_->selectionConstructionActive();
}
void MainWindow::beginAnchoredLasso(core::Vec2d point)
{
    auto& g = *selectionGesture_;
    g.anchored.addVertex(point);
    g.pointer = point;
    g.guide.append(point);
    g.live = { point };
    g.closing = { point };
    if (g.mode == core::LassoMode::Magnetic) {
        g.reference = std::make_unique<core::PinnedDocumentSampler>(
            *session().document(), session().activeLayer(), magneticSource_);
        auto* reference = g.reference.get();
        g.edges = std::make_unique<core::MagneticEdgeCache>(reference->extent(),
            [reference](int x, int y) { return reference->sample({ x + 0.5, y + 0.5 }); });
    }
    canvasWindow_->setSelectionConstructionActive(true);
    publishLassoPreview();
    statusBar()->showMessage(
        QStringLiteral("Click anchors · %1 removes · %2/double-click closes · Escape cancels")
            .arg(shortcutLabel(shortcuts_, "RemovePointAction"), shortcutLabel(shortcuts_, "FinishOperationAction")));
}
void MainWindow::moveAnchoredLasso(core::Vec2d point)
{
    if (!anchoredLassoActive())
        return;
    try {
        auto& g = *selectionGesture_;
        if (g.reference && !g.reference->matches(*session().document()))
            throw std::runtime_error("Magnetic reference changed; selection unchanged");
        if (g.finishing || g.anchorRequested) {
            g.deferredPointer = point;
            return;
        }
        if (g.pointer == point)
            return;
        if (g.mode == core::LassoMode::Polygonal) {
            core::FreehandSelectionPath validation;
            validation.append(point);
            g.pointer = point;
            g.live = { g.anchored.points().back(), point };
            g.closing = { point, g.anchored.points().front() };
        } else {
            g.guide.append(point);
            g.pointer = point;
            ++g.requestedRevision;
            // Even during fast input, only one pending search and one newest
            // pointer are retained. Bound an unusually dense unfinished guide.
            if (g.guide.points().size() >= 510)
                g.anchorRequested = true;
            magneticTimer_->start();
        }
        publishLassoPreview();
    } catch (const std::exception& error) {
        finishSelectionGesture(true);
        statusBar()->showMessage(
            QStringLiteral("Lasso cancelled: %1").arg(QString::fromUtf8(error.what())), 6000);
    }
}
void MainWindow::anchorLasso(core::Vec2d point, bool manual)
{
    if (!anchoredLassoActive())
        return;
    auto& g = *selectionGesture_;
    if (g.finishing)
        return;
    if (g.anchorRequested) {
        if (g.queuedAnchors.size() < 32)
            g.queuedAnchors.push_back({ point, manual, false });
        else
            statusBar()->showMessage(
                QStringLiteral("Magnetic is catching up; wait before placing more anchors"), 3000);
        return;
    }
    const auto points = g.anchored.points();
    if (points.size() >= 3 && length(point - points.front()) * canvasWindow_->zoom() <= 8) {
        closeLasso(points.front());
        return;
    }
    moveAnchoredLasso(point);
    if (!anchoredLassoActive())
        return;
    try {
        auto& edit = *selectionGesture_;
        if (edit.mode == core::LassoMode::Polygonal || manual) {
            edit.anchored.addVertex(point);
            edit.guide = { };
            edit.guide.append(point);
            edit.live = { point };
            edit.closing = { point, edit.anchored.points().front() };
            edit.previousLive.clear();
            edit.search.reset();
            edit.solvingClosing = false;
            edit.anchorRequested = false;
            ++edit.requestedRevision;
            magneticTimer_->stop();
            publishLassoPreview();
            continueLassoRequests();
        } else {
            edit.anchorRequested = true;
            magneticTimer_->start();
        }
    } catch (const std::exception& error) {
        finishSelectionGesture(true);
        statusBar()->showMessage(
            QStringLiteral("Lasso cancelled: %1").arg(QString::fromUtf8(error.what())), 6000);
    }
}
void MainWindow::closeLasso(core::Vec2d point)
{
    if (!anchoredLassoActive())
        return;
    if (selectionGesture_->finishing)
        return;
    if (selectionGesture_->anchorRequested) {
        auto& queued = selectionGesture_->queuedAnchors;
        if (queued.size() < 32)
            queued.push_back({ point, false, true });
        else
            statusBar()->showMessage(QStringLiteral("Magnetic is catching up; wait before closing"), 3000);
        magneticTimer_->start();
        return;
    }
    moveAnchoredLasso(point);
    if (!anchoredLassoActive())
        return;
    auto& g = *selectionGesture_;
    g.finishing = true;
    if (g.mode == core::LassoMode::Polygonal)
        commitAnchoredLasso();
    else
        magneticTimer_->start();
}
void MainWindow::continueLassoRequests()
{
    if (!anchoredLassoActive())
        return;
    auto& g = *selectionGesture_;
    if (g.anchorRequested || g.finishing)
        return;
    if (!g.queuedAnchors.empty()) {
        const auto next = g.queuedAnchors.front();
        g.queuedAnchors.pop_front();
        if (next.close)
            closeLasso(next.point);
        else
            anchorLasso(next.point, next.manual);
    } else if (g.deferredPointer) {
        const auto point = *g.deferredPointer;
        g.deferredPointer.reset();
        moveAnchoredLasso(point);
    }
}
void MainWindow::removeLassoAnchor()
{
    if (!anchoredLassoActive())
        return;
    auto& g = *selectionGesture_;
    g.search.reset();
    magneticTimer_->stop();
    g.anchorRequested = g.finishing = g.solvingClosing = false;
    g.deferredPointer.reset();
    g.queuedAnchors.clear();
    if (!g.anchored.removeLastAnchor() || g.anchored.points().empty()) {
        finishSelectionGesture(true);
        return;
    }
    const auto pointer = g.pointer;
    g.pathEdges = std::make_shared<std::vector<core::SelectionEdge>>();
    g.previewAnchoredEdges = 0;
    g.pointer = g.anchored.points().back();
    g.guide = { };
    g.guide.append(g.pointer);
    g.live = { g.pointer };
    g.previousLive.clear();
    g.closing.clear();
    ++g.requestedRevision;
    publishLassoPreview();
    moveAnchoredLasso(pointer);
}
void MainWindow::publishLassoPreview()
{
    if (!anchoredLassoActive())
        return;
    auto& g = *selectionGesture_;
    const auto points = g.anchored.points();
    if (points.empty())
        return;
    auto& edges = *g.pathEdges;
    const auto stable = points.size() - 1;
    edges.resize(stable);
    for (auto i = std::min(stable, g.previewAnchoredEdges); i < stable; ++i)
        edges[i] = { points[i], points[i + 1] };
    g.previewAnchoredEdges = stable;
    auto last = points.back();
    const auto append = [&](std::span<const core::Vec2d> path) {
        for (auto p : path)
            if (p != last) {
                edges.push_back({ last, p });
                last = p;
            }
    };
    append(g.live);
    const std::array pointer { g.pointer };
    append(pointer);
    const auto closeStart = edges.size();
    if (!g.closing.empty() && g.closing.front() == g.pointer && g.closing.back() == points.front())
        append(g.closing);
    else {
        const std::array close { points.front() };
        append(close);
    }
    const auto closing = edges.size() - closeStart;
    const auto anchors = g.anchored.anchors();
    for (auto p : anchors)
        edges.push_back({ p, p });
    canvasWindow_->setSelectionPathPreview(g.pathEdges, g.operation, closing, stable, anchors.size());
}
void MainWindow::advanceMagneticLasso()
{
    if (!anchoredLassoActive() || selectionGesture_->mode != core::LassoMode::Magnetic)
        return;
    try {
        auto& g = *selectionGesture_;
        if (!session().document() || !g.reference->matches(*session().document())
            || session().document()->selection() != g.before)
            throw std::runtime_error("Magnetic reference changed; selection unchanged");
        if (g.search && g.searchRevision != g.requestedRevision) {
            g.search.reset();
            g.solvingClosing = false;
        }
        if (!g.search) {
            g.search = std::make_unique<core::LiveWireSearch>(*g.edges, g.guide.points(), magneticRadius_);
            g.searchRevision = g.requestedRevision;
            g.solvingClosing = false;
        }
        if (!g.search->step(32768)) {
            magneticTimer_->start();
            return;
        }
        if (!g.solvingClosing) {
            g.previousLive = std::move(g.live);
            g.live.assign(g.search->path().begin(), g.search->path().end());
            const bool fallback = g.search->result() == core::LiveWireSearch::Result::ManualFallback;
            g.search.reset();
            statusBar()->showMessage(fallback
                    ? QStringLiteral(
                          "Magnetic: manual segment (weak/outside/large search) · Ctrl+click forces straight")
                    : QStringLiteral("Magnetic: edge following · Click anchors · Ctrl+click straight · "
                                     "%1 removes · %2 closes")
                        .arg(shortcutLabel(shortcuts_, "RemovePointAction"), shortcutLabel(shortcuts_, "FinishOperationAction")));
            g.closing = { g.pointer, g.anchored.points().front() };
            const double gap = length(g.pointer - g.anchored.points().front());
            // The visible closing edge is locally magnetic near the first
            // anchor, otherwise an explicit straight chord. Commit uses this
            // exact preview, never an unseen whole-image closing search.
            if (gap > 0 && gap <= magneticRadius_ * 3) {
                g.search = std::make_unique<core::LiveWireSearch>(*g.edges, g.closing, magneticRadius_);
                g.solvingClosing = true;
                publishLassoPreview();
                magneticTimer_->start();
                return;
            }
        } else {
            g.closing.assign(g.search->path().begin(), g.search->path().end());
            g.search.reset();
            g.solvingClosing = false;
        }
        if (g.finishing) {
            publishLassoPreview();
            commitAnchoredLasso();
            return;
        }
        if (g.anchorRequested) {
            g.anchored.addSegment(g.live);
            g.anchorRequested = false;
            g.guide = { };
            g.guide.append(g.pointer);
            g.live = { g.pointer };
            g.previousLive.clear();
            publishLassoPreview();
            continueLassoRequests();
            return;
        }
        const double cooled = commonLength(g.live, g.previousLive);
        if (cooled >= 64 + magneticRadius_) {
            auto [prefix, tail] = splitPath(g.live, cooled - magneticRadius_ * 2);
            if (g.anchored.addSegment(prefix)) {
                g.live = std::move(tail);
                g.guide = { };
                for (auto p : g.live)
                    g.guide.append(p);
                g.previousLive.clear();
            }
        }
        publishLassoPreview();
    } catch (const std::exception& error) {
        finishSelectionGesture(true);
        statusBar()->showMessage(
            QStringLiteral("Lasso cancelled: %1").arg(QString::fromUtf8(error.what())), 6000);
    }
}
void MainWindow::commitAnchoredLasso()
{
    if (!anchoredLassoActive())
        return;
    try {
        auto& g = *selectionGesture_;
        std::vector<core::Vec2d> polygon(g.anchored.points().begin(), g.anchored.points().end());
        const auto append = [&](std::span<const core::Vec2d> points) {
            for (auto p : points)
                if (polygon.empty() || p != polygon.back())
                    polygon.push_back(p);
        };
        append(g.live);
        const std::array pointer { g.pointer };
        append(pointer);
        append(g.closing);
        if (polygon.size() > 1 && polygon.back() == polygon.front())
            polygon.pop_back();
        g.path = { };
        for (auto p : polygon)
            g.path.append(p);
        magneticTimer_->stop();
        g.search.reset();
        canvasWindow_->setSelectionConstructionActive(false);
        pointerRouter_->cancelCapture();
        finishSelectionGesture(false);
    } catch (const std::exception& error) {
        finishSelectionGesture(true);
        statusBar()->showMessage(
            QStringLiteral("Lasso cancelled: %1").arg(QString::fromUtf8(error.what())), 6000);
    }
}
}
