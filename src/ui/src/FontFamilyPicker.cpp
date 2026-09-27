#include "imageeditor/ui/FontFamilyPicker.hpp"
#include "imageeditor/ui/PopupOwnership.hpp"

#include <QAbstractItemView>
#include <QCompleter>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QStringListModel>
#include <QWheelEvent>
#include <QScreen>
#include <QWindow>
#include <algorithm>

namespace imageeditor::ui {
namespace {
QString fold(const QString& text)
{
    return text.normalized(QString::NormalizationForm_KC).toCaseFolded().simplified();
}
const QStringList& startupFamilies()
{
    // Constructed with the cached text overlay during application startup.
    // No font enumeration, disk reads or font loading in the search path.
    static const auto families = QFontDatabase::families();
    return families;
}
class RankedCompleter final : public QCompleter {
public:
    using QCompleter::QCompleter;
    QStringList splitPath(const QString&) const override
    {
        // The owner already did token matching and ranking. Do not let Qt's
        // single-prefix matching exclude reordered tokens such as "emoji noto".
        return { QString { } };
    }
};
}

FontFamilyPicker::FontFamilyPicker(QWidget* parent)
    : FontFamilyPicker(startupFamilies(), parent)
{
}
FontFamilyPicker::FontFamilyPicker(const QStringList& families, QWidget* parent)
    : QComboBox(parent)
    , matches_(new QStringListModel(this))
    , completion_(new RankedCompleter(matches_, this))
{
    for (const auto& family : families) {
        const auto folded = fold(family);
        if (!folded.isEmpty())
            entries_.push_back({ family, folded, folded.split(' ', Qt::SkipEmptyParts) });
    }
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
        return a.folded == b.folded ? a.family < b.family : a.folded < b.folded;
    });
    entries_.erase(std::unique(entries_.begin(), entries_.end(),
                       [](const Entry& a, const Entry& b) { return a.folded == b.folded; }),
        entries_.end());
    for (const auto& entry : entries_)
        allFamilies_.push_back(entry.family);
    setEditable(true);
    setInsertPolicy(QComboBox::NoInsert);
    setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    setModel(new QStringListModel(allFamilies_, this));
    completion_->setCompletionMode(QCompleter::PopupCompletion);
    completion_->setCaseSensitivity(Qt::CaseInsensitive);
    completion_->setMaxVisibleItems(12);
    setCompleter(completion_);
    completionPopup()->installEventFilter(this);
    completionPopup()->setMouseTracking(true);
    connect(completionPopup(), &QAbstractItemView::entered, this, [this](const QModelIndex& index) {
        if (!index.isValid() || !(index.flags() & Qt::ItemIsEnabled))
            return;
        // One highlighted row, just like an ordinary combo. Suppress the
        // completer's inline-text signal: hovering must not replace the query.
        const QSignalBlocker block(completion_);
        completionPopup()->setCurrentIndex(index);
        if (searching_ && !lineEdit()->text().simplified().isEmpty())
            candidate_ = index.data().toString();
    });
    lineEdit()->installEventFilter(this);
    installEventFilter(this);
    connect(lineEdit(), &QLineEdit::textEdited, this, &FontFamilyPicker::filter);
    connect(completion_, qOverload<const QString&>(&QCompleter::activated), this,
        &FontFamilyPicker::choose);
    connect(completion_, qOverload<const QString&>(&QCompleter::highlighted), this,
        [this](const QString& family) {
            if (searching_ && !lineEdit()->text().simplified().isEmpty()
                && allFamilies_.contains(family))
                candidate_ = family;
        });
    // Do not bind QComboBox::activated: Qt also emits it from editingFinished
    // for exact matches on focus loss. Only the actual result popup accepts a
    // choice; arbitrary focus changes must remain a pending search.
    restoreDisplay();
}
QStringList FontFamilyPicker::matches(const QString& query) const
{
    const auto normalized = fold(query);
    const auto tokens = normalized.split(' ', Qt::SkipEmptyParts);
    struct Match {
        int rank;
        const Entry* entry;
    };
    std::vector<Match> matched;
    matched.reserve(entries_.size());
    for (const auto& entry : entries_) {
        int rank = entry.folded == normalized     ? -10000
            : entry.folded.startsWith(normalized) ? -1000
                                                  : 0;
        bool accepted = true;
        for (const auto& token : tokens) {
            if (!entry.folded.contains(token)) {
                accepted = false;
                break;
            }
            if (std::any_of(entry.words.begin(), entry.words.end(),
                    [&](const QString& word) { return word.startsWith(token); }))
                rank -= 10;
        }
        if (accepted)
            matched.push_back({ rank, &entry });
    }
    // entries_ is sorted; ties have a stable order across searches.
    std::stable_sort(matched.begin(), matched.end(),
        [](const Match& a, const Match& b) { return a.rank < b.rank; });
    QStringList result;
    result.reserve(qsizetype(matched.size()));
    for (const auto& match : matched)
        result.push_back(match.entry->family);
    return result;
}
void FontFamilyPicker::filter(const QString& query)
{
    searching_ = true;
    const auto filtered = matches(query);
    // Keep completion data separate from the combo's display model. Resetting
    // the latter rewrites QLineEdit text/caret and destroys its local undo stack.
    matches_->setStringList(filtered);
    candidate_
        = query.simplified().isEmpty() || filtered.isEmpty() ? QString { } : filtered.front();
    lineEdit()->setToolTip(candidate_.isEmpty()
            ? tr("No font selected. Click back into text to keep its existing font.")
            : tr("Best match: %1. Select a result or click back into text to apply.")
                  .arg(candidate_));
    // Present synchronously, before QLineEdit's automatic completion can assign
    // a native popup anchor relative to our logical (subsurface) overlay window.
    presentPopup();
}
void FontFamilyPicker::setDisplayedFamily(const QString& family, bool mixed)
{
    if (searching_)
        return;
    displayed_ = mixed ? tr("Mixed fonts") : family;
    restoreDisplay();
}
void FontFamilyPicker::restoreDisplay()
{
    const QSignalBlocker comboBlock(this), editBlock(lineEdit());
    if (matches_->stringList() != allFamilies_)
        matches_->setStringList(allFamilies_);
    setCurrentIndex(int(allFamilies_.indexOf(displayed_)));
    lineEdit()->setText(displayed_);
    lineEdit()->setToolTip(tr("Search fonts by name fragments in any order."));
}
void FontFamilyPicker::choose(const QString& family)
{
    if (!allFamilies_.contains(family))
        return;
    searching_ = false;
    candidate_.clear();
    displayed_ = family;
    hidePopup();
    restoreDisplay();
    if (onFamilyChosen)
        onFamilyChosen(family);
    if (onReturnToText)
        onReturnToText();
}
void FontFamilyPicker::finishSearch(bool accept)
{
    if (!searching_)
        return;
    if (accept && !candidate_.isEmpty()) {
        const auto selected = candidate_;
        choose(selected);
        return;
    }
    searching_ = false;
    candidate_.clear();
    hidePopup();
    restoreDisplay();
}
QAbstractItemView* FontFamilyPicker::completionPopup() const { return completion_->popup(); }
void FontFamilyPicker::presentPopup()
{
    auto* owner = popupTopLevelOwner(this);
    if (owner && owner != window()) {
        // Qt 6's QCompleter::complete sets its Wayland transient owner from
        // widget()->window(), which is our child panel plane. Keep Qt's model,
        // key handling and focus proxy, but position this overlay-hosted popup
        // explicitly relative to the real desktop owner. No native child is
        // detached and the text editor remains the completer's input widget.
        auto* popup = completion_->popup();
        preparePopupOwnership(popup, this);
        const int rows = std::min(completion_->maxVisibleItems(), completion_->completionCount());
        if (rows == 0) { popup->hide(); return; }
        const auto available = screen()->availableGeometry();
        const int frame = popup->frameWidth() * 2;
        const int rowHeight = std::max(1, popup->sizeHintForRow(0));
        const int height = std::min(available.height(), rows * rowHeight + frame);
        // Match the existing control-width popup, not the longest font name.
        // The text input can be inset within the combo by the active theme.
        const int width = std::min(available.width(), this->width());
        auto position = QPoint(mapToGlobal(QPoint()).x(),
            lineEdit()->mapToGlobal(QPoint(0, lineEdit()->height() + 2)).y());
        if (position.y() + height > available.bottom() + 1)
            position.setY(mapToGlobal(QPoint()).y() - height);
        position.setX(std::clamp(position.x(), available.left(), available.right() + 1 - width));
        position.setY(std::clamp(position.y(), available.top(), available.bottom() + 1 - height));
        popup->setGeometry(QRect(position, QSize(width, height)));
        (void)owner->winId(); (void)popup->winId();
        popup->windowHandle()->setTransientParent(owner->windowHandle());
        popup->show();
        return;
    }
    // An explicit widget-local rectangle uses mapped global placement instead
    // of Qt's Wayland combo anchor, whose logical top-level origin differs from
    // the native shell parent of our overlay. Keep the query above the results.
    const auto* anchor = completion_->widget();
    completion_->complete(QRect(mapTo(anchor, QPoint(0, 2)), size()));
}
void FontFamilyPicker::showPopup()
{
    completion_->setCompletionPrefix({ });
    presentPopup();
    const auto selected = searching_ ? candidate_ : displayed_;
    const auto row = std::max(0, int(matches_->stringList().indexOf(selected)));
    completion_->popup()->setCurrentIndex(completion_->completionModel()->index(row, 0));
}
void FontFamilyPicker::hidePopup()
{
    completion_->popup()->hide();
    QComboBox::hidePopup();
}
void FontFamilyPicker::wheelEvent(QWheelEvent* event)
{
    // Scrolling the field must not silently cycle its displayed family. The
    // results view still handles its own wheel input for browsing the list.
    event->ignore();
}
bool FontFamilyPicker::eventFilter(QObject* target, QEvent* event)
{
    if (target == completionPopup() && event->type() == QEvent::MouseButtonPress && searching_) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        const auto global = mouse->globalPosition().toPoint();
        if (mouse->button() == Qt::LeftButton
            && !completionPopup()->rect().contains(completionPopup()->mapFromGlobal(global))
            && onOutsidePress && onOutsidePress(global))
            return true;
    }
    if ((target == this || target == lineEdit()) && event->type() == QEvent::ShortcutOverride) {
        // Search owns typing, Space, clipboard and undo, even if Qt reports the
        // editable combo wrapper rather than its line editor as the focus widget.
        event->accept();
        return true;
    }
    if ((target == this || target == lineEdit()) && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if ((key->key() == Qt::Key_Down || key->key() == Qt::Key_Up)
            && !completionPopup()->isVisible()) {
            showPopup();
            return true;
        }
        if (key->key() == Qt::Key_Escape) {
            finishSearch(false);
            if (onReturnToText)
                onReturnToText();
            return true;
        }
    }
    return QComboBox::eventFilter(target, event);
}
} // namespace imageeditor::ui
