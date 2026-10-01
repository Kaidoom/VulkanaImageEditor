#include "imageeditor/ui/LayerListView.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/ui/Theme.hpp"

#include <QAbstractItemModel>
#include <QApplication>
#include <QDrag>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QHelpEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolTip>
#include <utility>

namespace imageeditor::ui {
namespace {
    class LayerRowDelegate final : public QStyledItemDelegate {
    public:
        using QStyledItemDelegate::QStyledItemDelegate;
        void initStyleOption(QStyleOptionViewItem* option,const QModelIndex& index) const override {
            QStyledItemDelegate::initStyleOption(option,index);
            option->decorationSize={index.data(LayerListModel::HasMaskRole).toBool()?74:34,34};
        }
        QRect decorationRect(QStyleOptionViewItem option,const QModelIndex& index) const {
            option.rect.adjust(index.data(Qt::UserRole+2).toInt(),0,0,0);
            initStyleOption(&option,index);
            return (option.widget?option.widget->style():QApplication::style())->subElementRect(QStyle::SE_ItemViewItemDecoration,&option,option.widget);
        }
        bool helpEvent(QHelpEvent* event,QAbstractItemView* view,
            const QStyleOptionViewItem& option,const QModelIndex& index) override {
            if(event->type()==QEvent::ToolTip && decorationRect(option,index).contains(event->pos())) {
                QToolTip::hideText();
                return true; // Content/mask thumbnails stay unobstructed; status shows the target help.
            }
            return QStyledItemDelegate::helpEvent(event,view,option,index);
        }
        void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
            const QModelIndex& index) const override
        {
            auto indented = option;
            indented.rect.adjust(index.data(Qt::UserRole + 2).toInt(), 2, -7, -2);
            // Qt retains the icon/check-indicator spacing; only the displayed
            // name becomes editable, leaving the row controls untouched.
            QStyledItemDelegate::updateEditorGeometry(editor, indented, index);
        }
        void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
        {
            const bool selected = option.state.testFlag(QStyle::State_Selected);
            const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
            const auto selectionInk = themeColor(ThemeColor::LayerSelection);
            // Keep the row's existing 2px vertical margins and full-width
            // selection extent, including indentation. Only the paint changes.
            const auto row = option.rect.adjusted(0, 2, 0, -2);
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            if (selected || hovered) {
                auto tint = selected ? selectionInk : option.palette.color(QPalette::Text);
                tint.setAlphaF(selected ? .22F : .04F);
                painter->setBrush(tint);
                painter->drawRoundedRect(row, 7, 7);
            }
            painter->restore();
            auto indented = option;
            indented.rect.adjust(index.data(Qt::UserRole + 2).toInt(), 0, 0, 0);
            // The delegate owns selection/hover chrome. Let Qt keep all its
            // item geometry and foreground painting, without a second opaque
            // selection fill or focus outline over our tint.
            indented.state &= ~(QStyle::State_Selected | QStyle::State_MouseOver | QStyle::State_HasFocus);
            if (selected)
                indented.palette.setColor(QPalette::Text, themeColor(ThemeColor::SelectedText));
            const bool hidden = index.data(Qt::UserRole + 6).isValid() && !index.data(Qt::UserRole + 6).toBool();
            painter->save();
            if (hidden) painter->setOpacity(painter->opacity() * .64);
            QStyledItemDelegate::paint(painter, indented, index);
            painter->restore();
            // Fade artwork/name, not the owned eye state. An inherited-hidden
            // child can retain an enabled eye; repaint just that indicator.
            if (hidden && index.data(Qt::CheckStateRole).isValid()) {
                auto check = indented;
                initStyleOption(&check, index);
                const auto* style = option.widget ? option.widget->style() : QApplication::style();
                const auto eye = style->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &check, option.widget);
                painter->save();
                painter->setClipRect(eye, Qt::IntersectClip);
                QStyledItemDelegate::paint(painter, indented, index);
                painter->restore();
            }
            painter->save();
            if (index.data(Qt::UserRole + 3).toBool()) {
                const auto x = indented.rect.left() - 10;
                const auto y = option.rect.center().y();
                auto color = option.palette.text().color();
                if (hidden) color.setAlphaF(color.alphaF() * .64F);
                painter->setPen(QPen(color, 1.5));
                const auto expanded = index.data(Qt::UserRole + 4).toBool();
                painter->drawPolyline(expanded ? QPolygonF { { double(x - 4), double(y - 2) }, { double(x), double(y + 2) }, { double(x + 4), double(y - 2) } }
                                               : QPolygonF { { double(x - 2), double(y - 4) }, { double(x + 2), double(y) }, { double(x - 2), double(y + 4) } });
            }
            const auto label = index.data(Qt::UserRole + 5).toInt();
            if (label > 0 && label < 7)
                painter->fillRect(QRect(option.rect.right() - 4, option.rect.top() + 5, 3, option.rect.height() - 10), layerLabelColor(core::ColorLabel(label)));
            painter->restore();
            // Primary vs additional selected items remains readable without
            // an enclosing border; label bars on the right keep their meaning.
            // Ancestor folders carry only the strip. Their text/background
            // stays unselected unless the folder itself belongs to the set.
            if (selected || index.data(LayerListModel::SelectedDescendantRole).toBool())
                painter->fillRect(QRect(row.left(), row.top(), selected && index.data(Qt::UserRole + 1).toBool() ? 3 : 2,
                    row.height()), selectionInk);
        }
    };
}

LayerListView::LayerListView(QWidget* parent)
    : QListView(parent)
{
    setItemDelegate(new LayerRowDelegate(this));
}

QRect LayerListView::thumbnailRect(const QModelIndex& index,bool mask) const
{
    QStyleOptionViewItem option;initViewItemOption(&option);option.rect=visualRect(index);
    const auto decoration=static_cast<LayerRowDelegate*>(itemDelegate())->decorationRect(option,index);
    return {decoration.x()+(mask?40:0),decoration.y(),34,34};
}

void LayerListView::setModel(QAbstractItemModel* next)
{
    if (next == model()) return;
    finishRename(false);
    for (const auto& connection : renameModelConnections_) disconnect(connection);
    renameModelConnections_ = {};
    QListView::setModel(next);
    if (next) {
        renameModelConnections_ = {
            connect(next, &QAbstractItemModel::modelReset, this, &LayerListView::updateRenameGeometry),
            connect(next, &QAbstractItemModel::layoutChanged, this, &LayerListView::updateRenameGeometry),
            connect(next, &QAbstractItemModel::rowsRemoved, this, &LayerListView::updateRenameGeometry),
            connect(next, &QAbstractItemModel::rowsInserted, this, &LayerListView::updateRenameGeometry)
        };
    }
}

QLineEdit* LayerListView::renameEditor() const noexcept { return renameEditor_.data(); }

bool LayerListView::beginRename(core::LayerId id)
{
    if (renameEditor_ && renameId_ == id) {
        if (onRenameEditorChanged) onRenameEditorChanged(renameEditor_);
        return true;
    }
    finishRename(true);
    auto* layers = dynamic_cast<LayerListModel*>(model());
    if (!layers) return false;
    const QSignalBlocker selectionGuard(selectionModel());
    if (!layers->revealItem(id)) return false;
    const auto index = layers->index(layers->rowForLayer(id), 0);
    if (!index.isValid()) return false;
    scrollTo(index, QAbstractItemView::EnsureVisible);
    QStyleOptionViewItem option;
    initViewItemOption(&option);
    option.rect = visualRect(index);
    auto* created = itemDelegate()->createEditor(viewport(), option, index);
    auto* editor = qobject_cast<QLineEdit*>(created);
    if (!editor) { delete created; return false; }
    renameEditor_ = editor;
    renameId_ = id;
    renameOriginal_ = index.data(Qt::EditRole).toString();
    editor->setObjectName(QStringLiteral("LayerNameEditor"));
    editor->setProperty("layerId", QVariant::fromValue<qulonglong>(id));
    editor->setAccessibleName(tr("Layer name"));
    editor->setToolTip(tr("Enter to rename · Escape to cancel · click elsewhere to confirm"));
    itemDelegate()->setEditorData(editor, index);
    updateRenameGeometry();
    editor->installEventFilter(this);
    // Deliberately don't use QAbstractItemView's focus-out commit: native
    // surface handoffs are not edits, and a model reset must never retarget
    // this editor to another row. Completion below always uses the pinned ID.
    editor->show();
    editor->raise();
    if (onRenameEditorChanged) onRenameEditorChanged(editor);
    else editor->setFocus(Qt::OtherFocusReason);
    editor->selectAll();
    const QPointer<QLineEdit> openingEditor = editor;
    const auto original = renameOriginal_;
    // Context-menu teardown may finish restoring focus after its Rename
    // action returned. Complete the initial selection once that unwinds, but
    // never overwrite typing that already arrived in the meantime.
    QTimer::singleShot(0, this, [this, openingEditor, original] {
        if (openingEditor && renameEditor_ == openingEditor
            && !openingEditor->isModified() && openingEditor->text() == original) {
            if (onRenameEditorChanged) onRenameEditorChanged(openingEditor);
            openingEditor->selectAll();
        }
    });
    return true;
}

void LayerListView::finishRename(bool commit)
{
    if (!renameEditor_) return;
    auto editor = renameEditor_;
    const auto id = renameId_;
    const auto name = editor->text().trimmed();
    const auto original = renameOriginal_;
    // Metadata publication can synchronously reset the model. Remove editing
    // ownership and its native keyboard bridge before invoking that command.
    renameEditor_.clear();
    renameId_ = 0;
    renameOriginal_.clear();
    editor->removeEventFilter(this);
    if (onRenameEditorChanged) onRenameEditorChanged(nullptr);
    editor->hide();
    editor->deleteLater();
    viewport()->update();
    if (commit && !name.isEmpty() && name != original)
        if (auto* layers = dynamic_cast<LayerListModel*>(model())) layers->renameItem(id, name);
}

void LayerListView::updateRenameGeometry()
{
    if (!renameEditor_) return;
    auto* layers = dynamic_cast<LayerListModel*>(model());
    const auto row = layers ? layers->rowForLayer(renameId_) : -1;
    if (row < 0) { finishRename(false); return; }
    const auto index = layers->index(row, 0);
    QStyleOptionViewItem option;
    initViewItemOption(&option);
    option.rect = visualRect(index);
    itemDelegate()->updateEditorGeometry(renameEditor_, option, index);
}

bool LayerListView::eventFilter(QObject* target, QEvent* event)
{
    if (target == renameEditor_ && (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape || key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            event->accept();
            if (event->type() == QEvent::KeyPress && !key->isAutoRepeat()) finishRename(key->key() != Qt::Key_Escape);
            return true;
        }
    }
    return QListView::eventFilter(target, event);
}

void LayerListView::resizeEvent(QResizeEvent* event)
{
    QListView::resizeEvent(event);
    updateRenameGeometry();
}

void LayerListView::scrollContentsBy(int dx, int dy)
{
    QListView::scrollContentsBy(dx, dy);
    updateRenameGeometry();
}

QRect LayerListView::visibilityIndicatorRect(const QModelIndex& index) const
{
    if (!index.isValid()) {
        return { };
    }

    QStyleOptionViewItem option;
    initViewItemOption(&option);
    option.rect = visualRect(index);
    option.rect.adjust(index.data(Qt::UserRole + 2).toInt(), 0, 0, 0);
    option.features |= QStyleOptionViewItem::HasCheckIndicator;
    option.checkState = static_cast<Qt::CheckState>(
        index.data(Qt::CheckStateRole).toInt());
    option.text = index.data(Qt::DisplayRole).toString();
    if (!option.text.isEmpty()) {
        option.features |= QStyleOptionViewItem::HasDisplay;
    }
    const auto decoration = qvariant_cast<QIcon>(
        index.data(Qt::DecorationRole));
    if (!decoration.isNull()) {
        option.features |= QStyleOptionViewItem::HasDecoration;
        option.icon = decoration;
        option.decorationSize = iconSize();
    }
    return style()->subElementRect(
        QStyle::SE_ItemViewItemCheckIndicator, &option, this);
}

bool LayerListView::event(QEvent* event)
{
    if (event && (event->type() == QEvent::ApplicationDeactivate || event->type() == QEvent::WindowDeactivate || event->type() == QEvent::FocusOut || event->type() == QEvent::Hide || event->type() == QEvent::TouchCancel)) {
        cancelVisibilityGesture();
        deferredSingleIndex_ = QPersistentModelIndex {};
        disclosureGesture_ = false;
    }
    return QListView::event(event);
}

void LayerListView::mousePressEvent(QMouseEvent* event)
{
    const auto row = indexAt(event->position().toPoint());
    if(row.isValid()&&onEditingTargetRequested&&row.data(LayerListModel::HasMaskRole).toBool()
        &&event->button()==Qt::LeftButton) {
        const bool mask=thumbnailRect(row,true).contains(event->position().toPoint());
        if(mask||thumbnailRect(row,false).contains(event->position().toPoint())) {
            onEditingTargetRequested(row.row(),mask);
            // Keep Qt's drag baseline, but never let its selection callbacks switch the target back.
            {const QSignalBlocker guard(selectionModel());QListView::mousePressEvent(event);}
            event->accept();return;
        }
    }
    if (event->button() == Qt::LeftButton && row.data(Qt::UserRole + 3).toBool()) {
        const auto r = visualRect(row);
        if (event->position().x() < r.left() + row.data(Qt::UserRole + 2).toInt()) {
            disclosureGesture_ = true;
            const QSignalBlocker guard(selectionModel());
            if (auto* list = dynamic_cast<LayerListModel*>(model()))
                list->toggleExpanded(row.data(Qt::UserRole).toULongLong());
            event->accept();
            return;
        }
    }
    if (event->button() == Qt::RightButton) {
        // Use the ordinary context-selection path for thumbnails too. Target
        // switching belongs to the context menu, not press-time file settling
        // (which cancels this new pointer capture before Qt opens the popup).
        if (row.isValid() && onContextSelectionRequested)
            onContextSelectionRequested(row.row());
        event->accept();
        return;
    }
    if (beginVisibilityGesture(event)) {
        return;
    }
    if (event->button() == Qt::LeftButton && !row.isValid() && onEmptySpacePressed) {
        deferredSingleIndex_ = QPersistentModelIndex {};
        onEmptySpacePressed();
        event->accept();
        return; // Session owns this policy too; never let Qt clear the primary.
    }
    const auto index = indexAt(event->position().toPoint());
    if (event->button() == Qt::LeftButton && index.isValid() && onRowSelectionRequested) {
        const auto* layers = dynamic_cast<const LayerListModel*>(model());
        const bool defer = event->modifiers() == Qt::NoModifier && selectionModel()->isSelected(index)
            && (layers ? layers->hasMultipleSelectedItems() : selectionModel()->selectedIndexes().size() > 1);
        {
            // Let Qt retain drag/reorder mechanics, but the session owns the
            // stable-ID range anchor and all pointer selection changes.
            const QSignalBlocker guard(selectionModel());
            QListView::mousePressEvent(event);
        }
        if (defer)
            deferredSingleIndex_ = index;
        else
            onRowSelectionRequested(index.row(), event->modifiers());
        return;
    }
    QListView::mousePressEvent(event);
}

QItemSelectionModel::SelectionFlags LayerListView::selectionCommand(const QModelIndex& index, const QEvent* event) const
{
    if (onRowSelectionRequested && event && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseButtonDblClick))
        return QItemSelectionModel::NoUpdate;
    return QListView::selectionCommand(index, event);
}

void LayerListView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && (event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))) {
        mousePressEvent(event); // Qt's second press is delivered as DblClick.
        return;
    }
    // Qt emits MouseButtonDblClick instead of a second MouseButtonPress. Treat
    // it as another independent eye activation so rapid hide/show never drops
    // the second click.
    if (beginVisibilityGesture(event)) {
        return;
    }
    QListView::mouseDoubleClickEvent(event);
}

void LayerListView::mouseMoveEvent(QMouseEvent* event)
{
    if (visibilityGestureActive_) {
        event->accept();
        return;
    }
    QListView::mouseMoveEvent(event);
}

void LayerListView::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && deferredSingleIndex_.isValid()) {
        const auto index = std::exchange(deferredSingleIndex_, QPersistentModelIndex {});
        if (onRowSelectionRequested)
            onRowSelectionRequested(index.row(), Qt::NoModifier);
    }
    if (disclosureGesture_ && event->button() == Qt::LeftButton) {
        disclosureGesture_ = false;
        event->accept();
        return;
    }
    if (visibilityGestureActive_ && event->button() == Qt::LeftButton) {
        cancelVisibilityGesture();
        event->accept();
        return;
    }
    QListView::mouseReleaseEvent(event);
}

void LayerListView::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) {
        const auto i = currentIndex();
        if (i.data(Qt::UserRole + 3).toBool() && i.data(Qt::UserRole + 4).toBool() != (event->key() == Qt::Key_Right)) {
            const QSignalBlocker guard(selectionModel());
            if (auto* list = dynamic_cast<LayerListModel*>(model()))
                list->toggleExpanded(i.data(Qt::UserRole).toULongLong());
            event->accept();
            return;
        }
    }
    // Keyboard navigation follows the same stable-ID anchor as pointer ranges,
    // rather than leaving an independent Qt selection behind the session.
    if (onRowSelectionRequested && model() && model()->rowCount() > 0) {
        int row = std::max(0, currentIndex().row());
        const auto key = event->key();
        const auto page = std::max(1, viewport()->height() / std::max(1, visualRect(model()->index(row, 0)).height()));
        if (key == Qt::Key_Up)
            --row;
        else if (key == Qt::Key_Down)
            ++row;
        else if (key == Qt::Key_Home)
            row = 0;
        else if (key == Qt::Key_End)
            row = model()->rowCount() - 1;
        else if (key == Qt::Key_PageUp)
            row -= page;
        else if (key == Qt::Key_PageDown)
            row += page;
        else if (key != Qt::Key_Space) {
            QListView::keyPressEvent(event);
            return;
        }
        onRowSelectionRequested(std::clamp(row, 0, model()->rowCount() - 1),
            key == Qt::Key_Space ? event->modifiers() : event->modifiers() & Qt::ShiftModifier);
        event->accept();
        return;
    }
    QListView::keyPressEvent(event);
}

bool LayerListView::beginVisibilityGesture(QMouseEvent* event)
{
    if (!event || event->button() != Qt::LeftButton || !model()) {
        return false;
    }
    const QModelIndex index = indexAt(event->position().toPoint());
    const QVariant checkState = index.data(Qt::CheckStateRole);
    if (!index.isValid() || !checkState.isValid()
        || !index.flags().testFlag(Qt::ItemIsUserCheckable)
        || !visibilityIndicatorRect(index).contains(
            event->position().toPoint())) {
        return false;
    }

    const auto current = static_cast<Qt::CheckState>(
        checkState.toInt());
    const auto next = current == Qt::Checked
        ? Qt::Unchecked
        : Qt::Checked;
    if (!model()->setData(index, next, Qt::CheckStateRole)) {
        return false;
    }

    visibilityGestureActive_ = true;
    event->accept();
    return true;
}

void LayerListView::cancelVisibilityGesture() noexcept
{
    visibilityGestureActive_ = false;
}

void LayerListView::dragMoveEvent(QDragMoveEvent* event)
{
    // Retain Qt's autoscroll/state machinery, but own slot feedback/admission.
    setDropIndicatorShown(false);
    QListView::dragMoveEvent(event);
    const auto index = indexAt(event->position().toPoint());
    dropRow_ = -1;
    dropParent_ = { };
    dropText_.clear();
    dropHint_ = { };
    if (index.isValid()) {
        const auto r = visualRect(index);
        const auto y = event->position().y();
        const auto indent = index.data(Qt::UserRole + 2).toInt();
        if (index.data(Qt::UserRole + 3).toBool() && y > r.top() + r.height() * .25 && y < r.bottom() - r.height() * .25) {
            dropParent_ = index;
            dropHint_ = r.adjusted(indent, 1, -3, -1);
            dropText_ = QStringLiteral("Into %1").arg(index.data().toString());
        } else {
            const bool below = y >= r.center().y();
            dropRow_ = below ? -2 : index.row();
            dropParent_ = below ? index : QModelIndex { };
            dropHint_ = QRect(r.left() + indent, below ? r.bottom() - 1 : r.top(), r.width() - indent - 3, 2);
        }
    }
    if (!index.isValid())
        dropHint_ = QRect(2, model()->rowCount() ? visualRect(model()->index(model()->rowCount() - 1, 0)).bottom() : 2, viewport()->width() - 4, 2);
    const auto* layers = dynamic_cast<LayerListModel*>(model());
    const auto action = layers && layers->onCanTransfer && layers->onCanTransfer(event->mimeData()) ? Qt::CopyAction : Qt::MoveAction;
    if (model()->canDropMimeData(event->mimeData(), action, dropRow_, 0, dropParent_)) {
        event->setDropAction(action);
        event->accept();
    } else {
        event->ignore();
        dropHint_ = { };
        dropText_.clear();
    }
    viewport()->update();
}
void LayerListView::dragLeaveEvent(QDragLeaveEvent* event)
{
    dropHint_ = { };
    dropText_.clear();
    viewport()->update();
    QListView::dragLeaveEvent(event);
}
void LayerListView::dropEvent(QDropEvent* event)
{
    const auto* layers = dynamic_cast<LayerListModel*>(model());
    const auto action = layers && layers->onCanTransfer && layers->onCanTransfer(event->mimeData()) ? Qt::CopyAction : Qt::MoveAction;
    if (!dropHint_.isEmpty() && model()->dropMimeData(event->mimeData(), action, dropRow_, 0, dropParent_)) {
        event->setDropAction(action);
        event->accept();
    } else
        event->ignore();
    dropHint_ = { };
    dropParent_ = { };
    dropText_.clear();
    viewport()->update();
    setState(NoState);
}
void LayerListView::startDrag(Qt::DropActions)
{
    deferredSingleIndex_ = QPersistentModelIndex {};
    const auto indexes = selectionModel()->selectedIndexes();
    if (indexes.isEmpty())
        return;
    auto* drag = new QDrag(this);
    drag->setMimeData(model()->mimeData(indexes));
    QPixmap preview(180, 36);
    preview.fill(palette().color(QPalette::AlternateBase));
    QPainter p(&preview);
    p.setPen(palette().text().color());
    p.drawText(preview.rect().adjusted(10, 0, -6, 0), Qt::AlignVCenter, indexes.size() == 1 ? indexes.front().data().toString() : tr("%1 selected items").arg(indexes.size()));
    p.end();
    drag->setPixmap(preview);
    drag->setHotSpot({ 12, 18 });
    // The model already performs one atomic move. QAbstractItemView's default
    // startDrag can attempt a second source-row removal after MoveAction.
    drag->exec(Qt::MoveAction | Qt::CopyAction, Qt::MoveAction);
    drag->deleteLater();
    dropHint_ = { };
    dropText_.clear();
    setState(NoState);
    viewport()->update();
}
void LayerListView::paintEvent(QPaintEvent* event)
{
    QListView::paintEvent(event);
    if (dropHint_.isEmpty())
        return;
    QPainter painter(viewport());
    const auto accent = palette().color(QPalette::Highlight).lighter(155);
    painter.setPen(QPen(accent, 2));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(dropHint_);
    if (!dropText_.isEmpty()) {
        const QRect label(dropHint_.left() + 2, dropHint_.top() + 2, dropHint_.width() - 4, 22);
        painter.fillRect(label, palette().color(QPalette::AlternateBase));
        painter.setPen(palette().color(QPalette::HighlightedText));
        painter.drawText(label.adjusted(5, 0, -5, 0), Qt::AlignVCenter, dropText_);
    }
}

} // namespace imageeditor::ui
