#pragma once

#include <QComboBox>
#include <QStringList>
#include <functional>
#include <vector>

class QAbstractItemView;
class QCompleter;
class QStringListModel;
class QWheelEvent;

namespace imageeditor::ui {

// Search is widget state, not a font edit. Only explicit acceptance calls the
// owner; rebuilding/filtering the cached family list never applies formatting.
class FontFamilyPicker final : public QComboBox {
public:
    explicit FontFamilyPicker(QWidget* parent = nullptr);
    FontFamilyPicker(const QStringList& families, QWidget* parent);
    void setDisplayedFamily(const QString& family, bool mixed = false);
    [[nodiscard]] bool searching() const noexcept { return searching_; }
    [[nodiscard]] QString candidate() const { return candidate_; }
    [[nodiscard]] QStringList matches(const QString& query) const;
    [[nodiscard]] QAbstractItemView* completionPopup() const;
    // A canvas-return click accepts the candidate; an unmatched/empty query
    // simply restores the previous displayed family. Neither changes the caret.
    void finishSearch(bool accept);
    void showPopup() override;
    void hidePopup() override;
    std::function<void(const QString&)> onFamilyChosen;
    std::function<void()> onReturnToText;
    // Qt normally consumes an outside completion-popup press. Let the owner
    // recognize its text canvas and resolve search on that same click.
    std::function<bool(QPoint)> onOutsidePress;

private:
    void wheelEvent(QWheelEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;
    void filter(const QString& query);
    void presentPopup();
    void choose(const QString& family);
    void restoreDisplay();
    struct Entry {
        QString family, folded;
        QStringList words;
    };
    std::vector<Entry> entries_;
    QStringList allFamilies_;
    QStringListModel* matches_;
    QCompleter* completion_;
    QString displayed_, candidate_;
    bool searching_ = false;
};

} // namespace imageeditor::ui
