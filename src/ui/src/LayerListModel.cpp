#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/ui/LayerTransferMimeData.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/ColorMath.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/ui/Theme.hpp"
#include <QMimeData>
#include <QApplication>
#include <QIconEngine>
#include <QPainter>
#include <QTimer>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <unordered_set>
#include <unordered_map>

namespace imageeditor::ui {
namespace {
    constexpr auto mimeType = "application/x-imageeditor-layer-id";
    QColor thumbnailInk(core::ColorLabel label)
    {
        return label == core::ColorLabel::None ? themeColor(ThemeColor::Thumbnail) : layerLabelColor(label);
    }
    // Content is a bounded raster preview; its badge stays vector-sharp at the
    // actual row size/DPR. Selected rows must not tint either the artwork or ink.
    class GroupThumbnailIconEngine final : public QIconEngine {
    public:
        GroupThumbnailIconEngine(QImage content, QColor ink) : content_(std::move(content)), ink_(ink) {}
        QIconEngine* clone() const override { return new GroupThumbnailIconEngine(*this); }
        bool isNull() override { return false; }
        QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
        { return scaledPixmap(size, mode, state, 1.0); }
        QPixmap scaledPixmap(const QSize& size, QIcon::Mode, QIcon::State, qreal dpr) override
        {
            if (size.isEmpty() || !std::isfinite(dpr) || dpr <= 0) return {};
            const auto palette = QApplication::palette();
            if (paletteKey_ != palette.cacheKey()) { cache_.clear(); paletteKey_ = palette.cacheKey(); }
            const auto key = std::make_tuple(size.width(), size.height(), dpr);
            if (const auto found = cache_.find(key); found != cache_.end()) return found->second;
            QPixmap result(size * dpr); result.setDevicePixelRatio(dpr); result.fill(Qt::transparent);
            QPainter painter(&result);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            const QRectF rect(QPointF(), result.deviceIndependentSize());
            painter.drawImage(rect, content_);
            const QRect badge(qRound(rect.width() * .625), qRound(rect.height() * .625),
                qRound(rect.width() * .375), qRound(rect.height() * .375));
            painter.fillRect(badge, palette.color(QPalette::AlternateBase));
            toolGlyph(ToolGlyph::Group, ink_).paint(&painter, badge);
            painter.end();
            if (cache_.size() >= 16) cache_.clear();
            cache_.emplace(key, result);
            return result;
        }
        void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override
        { painter->drawPixmap(rect, scaledPixmap(rect.size(), mode, state, painter->device()->devicePixelRatioF())); }
    private:
        QImage content_;
        QColor ink_;
        qint64 paletteKey_ {};
        std::map<std::tuple<int, int, qreal>, QPixmap> cache_;
    };
    std::vector<core::LayerId> dragIds(const QMimeData* data)
    {
        std::vector<core::LayerId> result;
        if (!data || !data->hasFormat(mimeType))
            return result;
        const auto bytes = data->data(mimeType);
        if (bytes.size() > 100000)
            return result;
        for (const auto& token : bytes.split(',')) {
            bool ok = false;
            const auto id = token.toULongLong(&ok);
            if (!ok || !id)
                return { };
            result.push_back(id);
        }
        return result;
    }
}
LayerListModel::LayerListModel(QObject* parent)
    : QAbstractListModel(parent)
{
    thumbnailClock_.start();
}
void LayerListModel::setSession(core::EditorSession* session)
{
    beginResetModel();
    session_ = session;
    ++sessionGeneration_; thumbnailRefreshQueued_ = false;
    collapsed_.clear();
    thumbnails_.clear();
    rowThumbnails_.clear();
    rebuildRows();
    endResetModel();
}
void LayerListModel::rebuildRows()
{
    rows_.clear();
    selectedAncestorFolders_ = selectedAncestorFolders();
    if (!session_ || !session_->document())
        return;
    const auto& tree = session_->document()->tree();
    const auto visit = [&](auto&& self, const auto& ids, int depth) -> void {
        for (auto it = ids.rbegin(); it != ids.rend(); ++it) {
            rows_.push_back({ *it, depth });
            const auto* c = tree.container(*it);
            if (c && core::isExpandable(c->kind) && expanded(*it))
                self(self, c->children, depth + 1);
        }
    };
    visit(visit, tree.roots, 0);
    std::erase_if(thumbnails_, [&](const auto& pair) { return !tree.container(pair.first)&&!session_->document()->layer(pair.first); });
    std::erase_if(rowThumbnails_, [&](const auto& pair) { return !session_->document()->layer(pair.first); });
}
void LayerListModel::refresh()
{
    beginResetModel();
    rebuildRows();
    endResetModel();
}
std::set<core::LayerId> LayerListModel::selectedAncestorFolders() const
{
    std::set<core::LayerId> result;
    if (!session_ || !session_->document()) return result;
    // Build parent lookup once per selection sync, never by scanning hidden
    // descendants for every painted row. Collapsed state is intentionally
    // irrelevant; selected groups also indicate their containing folders.
    std::unordered_map<core::LayerId, core::LayerId> parents;
    std::unordered_set<core::LayerId> folders;
    for (const auto& container : session_->document()->tree().containers) {
        if (core::isExpandable(container.kind)) folders.insert(container.id);
        for (auto child : container.children) parents.emplace(child, container.id);
    }
    for (auto id : session_->selectedLayers()) {
        for (std::size_t depth = 0; depth < core::LayerTree::maxDepth; ++depth) {
            const auto parent = parents.find(id);
            if (parent == parents.end()) break;
            id = parent->second;
            if (folders.contains(id)) result.insert(id);
        }
    }
    return result;
}
void LayerListModel::refreshSelectionIndicators()
{
    auto next = selectedAncestorFolders();
    if (next == selectedAncestorFolders_) return;
    selectedAncestorFolders_ = std::move(next);
    if (rowCount())
        emit dataChanged(index(0), index(rowCount() - 1), {SelectedDescendantRole});
}
bool LayerListModel::expanded(core::LayerId id) const { return !collapsed_.contains(id); }
std::vector<core::LayerId> LayerListModel::collapsedFolderIds() const
{
    std::vector<core::LayerId> result;
    if (session_ && session_->document())
        for (auto id : collapsed_)
            if (const auto* folder = session_->document()->tree().container(id);
                folder && core::isExpandable(folder->kind))
                result.push_back(id);
    return result;
}
void LayerListModel::restoreCollapsedFolderIds(std::span<const core::LayerId> ids)
{
    std::set<core::LayerId> restored;
    if (session_ && session_->document() && ids.size() <= core::LayerTree::maxItems)
        for (auto id : ids)
            if (const auto* folder = session_->document()->tree().container(id);
                folder && core::isExpandable(folder->kind))
                restored.insert(id);
    collapsed_.swap(restored);
}
bool LayerListModel::hasMultipleSelectedItems() const
{
    return session_ && session_->selectedLayers().size() > 1;
}
bool LayerListModel::revealItem(core::LayerId id)
{
    if (!session_ || !session_->document() || !session_->document()->containsItem(id)) return false;
    const auto& tree = session_->document()->tree();
    std::vector<core::LayerId> ancestors;
    auto placement = tree.placement(id);
    while (placement && placement->parent) {
        const auto* parent = tree.container(placement->parent);
        if (!parent || parent->kind == core::ContainerKind::Group) return false;
        ancestors.push_back(parent->id);
        placement = tree.placement(parent->id);
    }
    bool changed = false;
    for (auto ancestor : ancestors) changed |= collapsed_.erase(ancestor) != 0;
    if (changed) {
        refresh();
        if (onExpansionChanged) onExpansionChanged();
    }
    return rowForLayer(id) >= 0;
}
bool LayerListModel::renameItem(core::LayerId id, const QString& name)
{
    if (!session_ || !session_->document() || !session_->document()->containsItem(id)) return false;
    return onRenameRequested && onRenameRequested(id, name);
}
void LayerListModel::toggleExpanded(core::LayerId id)
{
    const auto* c = session_ && session_->document() ? session_->document()->tree().container(id) : nullptr;
    if (!c || !core::isExpandable(c->kind))
        return;
    beginResetModel();
    if (!collapsed_.erase(id))
        collapsed_.insert(id);
    rebuildRows();
    endResetModel();
    if (onExpansionChanged)
        onExpansionChanged();
}
int LayerListModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : int(rows_.size()); }
int LayerListModel::rowIndent(int row) const
{
    if (row < 0 || row >= int(rows_.size()))
        return 0;
    const auto* c = session_->document()->tree().container(rows_[std::size_t(row)].id);
    return rows_[std::size_t(row)].depth * 16 + (c && core::isExpandable(c->kind) ? 18 : 0);
}
const core::Layer* LayerListModel::layerAt(int row) const
{
    const auto id = layerIdAt(row);
    return id ? session_->document()->layer(*id) : nullptr;
}
std::optional<core::LayerId> LayerListModel::layerIdAt(int row) const
{
    return row < 0 || row >= int(rows_.size()) ? std::nullopt : std::optional(rows_[std::size_t(row)].id);
}
int LayerListModel::rowForLayer(core::LayerId id) const
{
    for (std::size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].id == id)
            return int(i);
    return -1;
}
QVariant LayerListModel::data(const QModelIndex& index, int role) const
{
    const auto id = layerIdAt(index.row());
    if (!index.isValid() || !id || !session_ || !session_->document())
        return { };
    const auto* l = layerAt(index.row());
    const auto* c = session_->document()->tree().container(*id);
    if (!l && !c)
        return { };
    switch (role) {
    case Qt::DisplayRole:
    case Qt::EditRole:
        return QString::fromStdString(l ? l->name : c->name);
    case Qt::CheckStateRole:
        return (l ? l->visible : c->visible) ? Qt::Checked : Qt::Unchecked;
    case Qt::DecorationRole: {
        const auto ink = thumbnailInk(c ? c->colorLabel : core::ColorLabel(l->colorLabel));
        if (c) {
            if (c->kind == core::ContainerKind::ClippingMaskGroup)
                return toolGlyph(ToolGlyph::ClippingGroup, ink);
            if (c->kind == core::ContainerKind::Folder)
                return toolGlyph(ToolGlyph::Folder, ink);
            return groupThumbnail(*id);
        }
        return layerThumbnail(*l);
    }
    case Qt::ToolTipRole:
        if(c&&c->kind==core::ContainerKind::ClippingMaskGroup)return tr("Clipping mask group · Bottommost child is the base · Reorder to change the base");
        if(l&&l->mask)return tr("Content thumbnail: edit layer · Mask thumbnail: edit coverage\nWhite reveals · Black hides · Gray is partial · Right-click for mask actions");
        if (c)
            return QStringLiteral("%1 · %2 immediate items\n%3").arg(c->kind == core::ContainerKind::Folder ? "Folder" : "Pass-through group").arg(c->children.size()).arg(c->kind == core::ContainerKind::Folder ? "Organizes content; no opacity or transform. Drop inside to reparent." : "Move/Transform moves all members, including hidden layers. Ungroup keeps editable content.");
        return QStringLiteral("%1\nF2 to rename · right-click for organization").arg(std::holds_alternative<core::RasterLayer>(l->payload) ? "Raster layer" : std::holds_alternative<core::TextLayer>(l->payload) ? "Editable text layer"
                                                                                                                                                                                                                  : "Editable shape layer");
    case Qt::UserRole:
        return QVariant::fromValue<qulonglong>(*id);
    case Qt::UserRole + 1:
        return session_->activeLayer() == id;
    case Qt::UserRole + 2:
        return rowIndent(index.row());
    case Qt::UserRole + 3:
        return c && core::isExpandable(c->kind);
    case Qt::UserRole + 4:
        return expanded(*id);
    case Qt::UserRole + 5:
        return l ? int(l->colorLabel) : int(c->colorLabel);
    case Qt::UserRole + 6:
        return session_->document()->isEffectivelyVisible(*id);
    case SelectedDescendantRole:
        return selectedAncestorFolders_.contains(*id);
    case HasMaskRole:
        return l&&bool(l->mask);
    case ClippingBaseRole: {
        const auto& tree=session_->document()->tree();
        const auto p=tree.placement(*id);
        const auto* parent=p?tree.container(p->parent):nullptr;
        return parent && parent->kind==core::ContainerKind::ClippingMaskGroup && !parent->children.empty() && parent->children.front()==*id;
    }
    default:
        return { };
    }
}
Qt::ItemFlags LayerListModel::flags(const QModelIndex& index) const
{
    if (!index.isValid())
        return Qt::ItemIsDropEnabled;
    auto flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled | Qt::ItemIsEditable;
    flags |= Qt::ItemIsUserCheckable;
    if (index.data(Qt::UserRole + 3).toBool())
        flags |= Qt::ItemIsDropEnabled;
    return flags;
}
bool LayerListModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    const auto id = layerIdAt(index.row());
    if (!id)
        return false;
    if (role == Qt::EditRole)
        return renameItem(*id, value.toString());
    if (role != Qt::CheckStateRole || !onVisibilityChanged)
        return false;
    if (!onVisibilityChanged(*id, value.toInt() == Qt::Checked))
        return false;
    if (rowCount())
        emit dataChanged(this->index(0), this->index(rowCount() - 1), { Qt::CheckStateRole, Qt::DecorationRole, Qt::UserRole + 6 });
    return true;
}
QStringList LayerListModel::mimeTypes() const { return { mimeType, "application/x-vulkana-layer-transfer" }; }
QMimeData* LayerListModel::mimeData(const QModelIndexList& indexes) const
{
    std::vector<core::LayerId> ids;
    for (const auto& i : indexes)
        if (auto id = layerIdAt(i.row()))
            ids.push_back(*id);
    // A collapsed row is not in Qt's visual selection model; the session can
    // still select it. A real multi-row drag must not lose those targets.
    if (session_ && std::ranges::all_of(ids, [&](auto id) { return session_->isLayerSelected(id); }))
        for (auto id : session_->selectedLayers())
            if (rowForLayer(id) < 0)
                ids.push_back(id);
    if (session_ && session_->document())
        ids = session_->document()->tree().normalize(ids);
    auto* result = onCaptureTransfer ? onCaptureTransfer(ids) : new QMimeData;
    if (!result) result = new QMimeData;
    QByteArray bytes;
    for (auto id : ids) {
        if (!bytes.isEmpty())
            bytes += ',';
        bytes += QByteArray::number(qulonglong(id));
    }
    if (!bytes.isEmpty())
        result->setData(mimeType, bytes);
    return result;
}
std::optional<core::ItemPlacement> LayerListModel::dropPlacement(int row, const QModelIndex& parent) const
{
    if (!session_ || !session_->document())
        return { };
    const auto& tree = session_->document()->tree();
    if (parent.isValid() && row == -2) {
        const auto id = layerIdAt(parent.row());
        return id ? tree.placement(*id) : std::nullopt;
    }
    if (parent.isValid() && row < 0) {
        const auto id = layerIdAt(parent.row());
        const auto* c = id ? tree.container(*id) : nullptr;
        if (c && core::isExpandable(c->kind))
            return core::ItemPlacement { c->id, c->children.size() };
        return { };
    }
    if (row < 0 || row >= rowCount())
        return core::ItemPlacement { 0, 0 };
    // Above a displayed row = immediately above that item in its own parent.
    auto p = tree.placement(*layerIdAt(row));
    if (p)
        ++p->index;
    return p;
}
bool LayerListModel::canDropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column, const QModelIndex& parent) const
{
    if (action == Qt::IgnoreAction)
        return true;
    if (const auto* transfer = dynamic_cast<const LayerTransferMimeData*>(data);
        transfer && (!transfer->source || transfer->source->closed || transfer->consumed
            || !session_ || transfer->source->session.document() != session_->document()))
        return onCanTransfer && onCanTransfer(data) && dropPlacement(row, parent).has_value();
    if (action != Qt::MoveAction || (column != 0 && column != -1))
        return false;
    const auto p = dropPlacement(row, parent);
    const auto ids = dragIds(data);
    if (!p || ids.empty())
        return false;
    const auto& tree = session_->document()->tree();
    for (auto id : ids)
        if (!session_->document()->containsItem(id) || id == p->parent || tree.isAncestor(id, p->parent))
            return false;
    return true;
}
bool LayerListModel::dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column, const QModelIndex& parent)
{
    if (action == Qt::IgnoreAction)
        return true;
    if (!canDropMimeData(data, action, row, column, parent))
        return false;
    if (onCanTransfer && onCanTransfer(data)) return onTransfer && onTransfer(data, *dropPlacement(row, parent));
    auto ids = dragIds(data);
    const auto p = *dropPlacement(row, parent);
    if (onReparentRequested)
        return onReparentRequested(std::move(ids), p);
    if (onMoveRequested && ids.size() == 1 && session_->document()->tree().containers.empty()) {
        const auto from = session_->document()->tree().placement(ids.front())->index;
        const auto to = p.index - (from < p.index ? 1U : 0U);
        if (from == to)
            return true;
        if (!onMoveRequested(ids.front(), to))
            return false;
        refresh();
        return true;
    }
    return false;
}
Qt::DropActions LayerListModel::supportedDropActions() const { return Qt::MoveAction | Qt::CopyAction; }
Qt::DropActions LayerListModel::supportedDragActions() const { return Qt::MoveAction | Qt::CopyAction; }

QIcon LayerListModel::layerThumbnail(const core::Layer& layer) const
{
    const auto surface=core::renderedSurface(layer);
    size_t stamp=surface?size_t(surface->id()^(surface->revision()*1099511628211ULL)):0;
    stamp^=size_t(QApplication::palette().cacheKey());
    QIcon content;
    if(std::holds_alternative<core::AdjustmentLayer>(layer.payload))content=toolGlyph(ToolGlyph::AdjustmentLayer,thumbnailInk(core::ColorLabel(layer.colorLabel)));
    else if(const auto found=thumbnails_.find(layer.id);found!=thumbnails_.end()&&found->second.stamp==stamp)
        content=found->second.icon;
    else {
        constexpr int side=96;
        QImage image(side,side,QImage::Format_RGBA8888);image.fill(Qt::transparent);
        try {
            auto source=layer;source.mask.reset();source.crop.reset();source.localToDocument={};
            const auto bounds=core::layerSourceBounds(source);
            if(surface&&!bounds.empty()) {
                core::PreparedLayerSampler sampler(source,false);
                const double scale=std::max(bounds.width,bounds.height)/side;
                for(int y=0;y<side;++y)for(int x=0;x<side;++x) {
                    const core::Vec2d p{bounds.x+bounds.width*.5+(x+.5-side*.5)*scale,
                        bounds.y+bounds.height*.5+(y+.5-side*.5)*scale};
                    const auto c=sampler.sample(p);
                    const auto channel=[&](int i){return c[3]>0?core::linearToSrgb(c[size_t(i)]/c[3]):uint8_t(0);};
                    image.setPixelColor(x,y,QColor(channel(0),channel(1),channel(2),core::alphaToByte(c[3])));
                }
            }
        }catch(const std::exception&) { }
        content=QIcon(QPixmap::fromImage(image));
        thumbnails_[layer.id]={stamp,0,content,thumbnailClock_.elapsed()};
    }
    // Both thumbnails share the same fixed row geometry. Target chrome is not
    // cached with content, so switching targets never recomputes source pixels.
    const bool hasMask=bool(layer.mask);
    const bool active=session_->activeLayer()==layer.id;
    size_t rowStamp=size_t(content.cacheKey());
    const auto hash=[&](uint64_t value){rowStamp^=size_t(value)+0x9e3779b9+(rowStamp<<6)+(rowStamp>>2);};
    hash(uint64_t(QApplication::palette().cacheKey()));hash(themeColor(ThemeColor::Accent).rgba());
    hash(active);hash(active&&session_->editingLayerMask());
    if(hasMask){hash(layer.mask->coverage->revision());hash(layer.mask->enabled);}
    if(const auto cached=rowThumbnails_.find(layer.id);cached!=rowThumbnails_.end()&&cached->second.stamp==rowStamp)
        return cached->second.icon;
    QPixmap preview((hasMask?74:34)*3,34*3);preview.setDevicePixelRatio(3);preview.fill(Qt::transparent);
    QPainter painter(&preview);
    for(int y=1;y<33;y+=4)for(int x=1;x<33;x+=4)
        painter.fillRect(QRect(x,y,4,4),QColor((x/4+y/4)%2?100:75,(x/4+y/4)%2?100:75,(x/4+y/4)%2?100:75));
    content.paint(&painter,{1,1,32,32});
    // Color labels use the row-edge stripe, never paint over source previews.
    if(hasMask) {
        const auto e=layer.mask->coverage->extent();
        QImage mask(96,96,QImage::Format_Grayscale8);
        for(int y=0;y<96;++y)for(int x=0;x<96;++x)
            mask.scanLine(y)[x]=layer.mask->coverage->coverageAtDocumentPixel(int(uint64_t(x)*e.width/96),int(uint64_t(y)*e.height/96));
        painter.drawImage(QRect(41,1,32,32),mask);
        if(!layer.mask->enabled){painter.setPen(QPen(QColor(230,75,65),2));painter.drawLine(42,31,72,2);}
    }
    painter.setBrush(Qt::NoBrush);
    for(int i=0;i<(hasMask?2:1);++i) {
        const bool editing=active&&(i==1)==session_->editingLayerMask();
        painter.setPen(QPen(editing?themeColor(ThemeColor::Accent):QApplication::palette().color(QPalette::Mid),editing?2:1));
        painter.drawRoundedRect(QRectF(i*40+1,1,32,32),2,2);
    }
    painter.end();
    QIcon result;
    for(auto mode:{QIcon::Normal,QIcon::Selected,QIcon::Active})result.addPixmap(preview,mode);
    rowThumbnails_[layer.id]={rowStamp,0,result,thumbnailClock_.elapsed()};
    return result;
}

QIcon LayerListModel::groupThumbnail(core::LayerId id) const
{
    const auto& doc = *session_->document();
    const auto leaves = doc.tree().descendants(id);
    const auto ink = thumbnailInk(doc.tree().container(id)->colorLabel);
    // Like a raster thumbnail, retain the group's content preview when the
    // row (or an ancestor) is hidden. Internal child gates still shape it.
    const auto visibleInGroup = [&](core::LayerId leaf) {
        if (!doc.layer(leaf)->visible) return false;
        for (std::size_t depth = 0; depth <= core::LayerTree::maxDepth; ++depth) {
            const auto p = doc.tree().placement(leaf);
            if (!p) return false;
            if (p->parent == id) return true;
            const auto* parent = doc.tree().container(p->parent);
            if (!parent || !parent->visible) return false;
            leaf = parent->id;
        }
        return false;
    };
    const std::unordered_set members(leaves.begin(),leaves.end());
    auto previewLeaves = leaves;
    const bool needsBackdrop = std::ranges::any_of(leaves,[&](auto leaf) {
        const auto& l = *doc.layer(leaf);
        return visibleInGroup(leaf) && l.opacity > 0 && (l.blendMode != core::BlendMode::Normal
            || (std::holds_alternative<core::AdjustmentLayer>(l.payload)&&core::compileAdjustmentStack(l.adjustments).active));
    });
    if (needsBackdrop && !leaves.empty()) {
        // A pass-through group's appearance includes its backdrop. Do not
        // invent isolated blending for the thumbnail. Crop to member bounds,
        // but sample the ordered document prefix through its final member.
        previewLeaves.clear();
        for (const auto& l : doc.layers()) {
            previewLeaves.push_back(l.id);
            if (l.id == leaves.back()) break;
        }
    }
    const auto visibleForPreview = [&](core::LayerId leaf) {
        return members.contains(leaf) ? visibleInGroup(leaf) : doc.isEffectivelyVisible(leaf);
    };
    std::size_t stamp = 1469598103934665603ULL;
    const auto hash = [&](std::uint64_t value) { stamp = (stamp ^ value) * 1099511628211ULL; };
    hash(static_cast<std::uint64_t>(QApplication::palette().cacheKey()));
    hash(ink.rgba());
    for(const auto& c:doc.tree().containers){hash(c.id);hash(std::uint64_t(c.kind));for(auto child:c.children)hash(child);}
    const auto appearanceStamp = stamp;
    for (auto leaf : previewLeaves) {
        const auto& layer = *doc.layer(leaf);
        const auto s = core::renderedSurface(layer);
        hash(leaf);
        hash(visibleForPreview(layer.id));
        hash(std::bit_cast<std::uint32_t>(layer.opacity));
        hash(static_cast<std::uint32_t>(layer.blendMode));
        hash(layer.adjustmentRevision);
        hash(layer.filterRevision);
        hash(layer.effectRevision);
        if(const auto* adjustment=std::get_if<core::AdjustmentLayer>(&layer.payload))hash(std::uint64_t(adjustment->scope));
        if(layer.mask){hash(layer.mask->coverage->revision());hash(layer.mask->enabled);hash(layer.mask->outside);}
        if(layer.effectCache)for(const auto& m:layer.effectCache->masks)if(m)hash(m->coverage->revision());
        hash(layer.crop.has_value());
        if(layer.crop)for(double v:layer.crop->corners)hash(std::bit_cast<std::uint64_t>(v));
        if (layer.crop)
            for (const auto value : {layer.crop->x, layer.crop->y, layer.crop->width, layer.crop->height})
                hash(std::bit_cast<std::uint64_t>(value));
        if (s) {
            hash(s->id());
            hash(s->revision());
        }
        const auto t = core::renderTransform(layer);
        for (auto value : { t.m00, t.m01, t.m02, t.m10, t.m11, t.m12, t.m20, t.m21, t.m22 })
            hash(std::bit_cast<std::uint64_t>(value));
    }
    const auto now = thumbnailClock_.elapsed();
    if (const auto it = thumbnails_.find(id); it != thumbnails_.end()) {
        if (it->second.stamp == stamp)
            return it->second.icon;
        if (it->second.appearanceStamp == appearanceStamp && now - it->second.renderedAt < 160) {
            if (!thumbnailRefreshQueued_) {
                thumbnailRefreshQueued_ = true;
                auto* self = const_cast<LayerListModel*>(this);
                const auto generation=sessionGeneration_;
                QTimer::singleShot(170, self, [self,generation] {if(self->sessionGeneration_!=generation)return;self->thumbnailRefreshQueued_=false;
                    if(self->rowCount())emit self->dataChanged(self->index(0),self->index(self->rowCount()-1),{Qt::DecorationRole}); });
            }
            return it->second.icon;
        }
    }
    // Up to a 40-logical-pixel thumbnail at 3x, independently of the canvas.
    // Sampling is bounded and content changes retain the existing 160 ms throttle.
    constexpr int thumbnailPixels = 120;
    QImage image(thumbnailPixels, thumbnailPixels, QImage::Format_RGBA8888);
    image.fill(QApplication::palette().color(QPalette::Button));
    // The canvas coordinator prepares effects asynchronously. Painting a row
    // must never run a spatial convolution or publish an unfiltered thumbnail.
    if(std::ranges::any_of(previewLeaves,[&](auto leaf){
        const auto& l=*doc.layer(leaf);
        return visibleForPreview(leaf)&&((core::hasActiveSpatialFilters(l.filters)&&!core::layerSpatialFilterCacheValid(l))||!core::layerEffectCacheValid(l));
    })) {
        if(const auto it=thumbnails_.find(id);it!=thumbnails_.end())return it->second.icon;
        return QIcon(new GroupThumbnailIconEngine(std::move(image),ink));
    }
    try {
        double left = 1e100, top = left, right = -left, bottom = -left;
        for (auto leaf : leaves) {
            const auto& l = *doc.layer(leaf);
            const auto s = core::renderedSurface(l);
            if (!s || !visibleInGroup(l.id) || !std::isfinite(l.opacity) || l.opacity <= 0)
                continue;
            const auto r=core::layerStyledBounds(l);if(r.empty())continue;
            const auto t = l.localToDocument;
            for (auto p : std::array { t.map({r.x,r.y}),t.map({r.right(),r.y}),t.map({r.right(),r.bottom()}),t.map({r.x,r.bottom()}) }) {
                left = std::min(left, p.x);
                top = std::min(top, p.y);
                right = std::max(right, p.x);
                bottom = std::max(bottom, p.y);
            }
        }
        if (std::isfinite(left) && right > left && bottom > top) {
            const auto scale = (thumbnailPixels * .85) / std::max(right - left, bottom - top);
            std::vector<const core::Layer*> prepared;
            std::vector<core::Layer> previewLayers;previewLayers.reserve(previewLeaves.size());
            const core::AffineTransform fit { scale, 0, thumbnailPixels * .075 - left * scale,
                0, scale, thumbnailPixels * .075 - top * scale };
            for (auto leaf : previewLeaves) {
                const auto* l = doc.layer(leaf);
                previewLayers.push_back(*l);
                previewLayers.back().visible=visibleForPreview(leaf)&&(bool(core::renderedSurface(*l))||std::holds_alternative<core::AdjustmentLayer>(l->payload));
            }
            for(const auto& l:previewLayers)prepared.push_back(&l);
            core::PinnedDocumentSampler sampler(prepared, core::Extent2u { thumbnailPixels, thumbnailPixels }, fit,{},&doc.tree());
            for (int y = 0; y < thumbnailPixels; ++y)
                for (int x = 0; x < thumbnailPixels; ++x) {
                    const auto c = sampler.sample({ x + .5, y + .5 });
                    image.setPixelColor(x, y, QColor(c.red, c.green, c.blue, c.alpha));
                }
        }
    } catch (const std::exception&) { /* Disposable caches may be unavailable until next revision. */
    }
    QIcon icon(new GroupThumbnailIconEngine(std::move(image), ink));
    thumbnails_[id] = { stamp, appearanceStamp, icon, now };
    return icon;
}
} // namespace imageeditor::ui
