#include "imageeditor/ui/FontFamilyPicker.hpp"
#include "imageeditor/ui/Theme.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QImage>
#include <QLabel>
#include <QStyleOptionComboBox>
#include <QKeyEvent>
#include <QLineEdit>
#include <QTest>
#include <QWheelEvent>

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace u = imageeditor::ui;
namespace {
int failures = 0;
void check(bool value, const char* expression, int line)
{
    if (!value) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
#define CHECK(value) check(static_cast<bool>(value), #value, __LINE__)

QStringList families()
{
    // Injected names make ranking independent of the machine's installed fonts.
    return { QStringLiteral("Zebra Monospace"), QStringLiteral("Noto Sans"),
        QStringLiteral("Quasimono"), QStringLiteral("Monospace"), QStringLiteral("Mono Test"),
        QStringLiteral("Noto Color Emoji"), QStringLiteral("Mono"), QStringLiteral("Noto") };
}

struct Fixture {
    QWidget window;
    u::FontFamilyPicker* picker;
    QStringList choices;
    int returns = 0;

    Fixture()
    {
        auto* layout = new QHBoxLayout(&window);
        picker = new u::FontFamilyPicker(families(), &window);
        picker->setFixedWidth(164); // Same width contract as the text overlay.
        picker->setDisplayedFamily(QStringLiteral("Noto Sans"));
        picker->onFamilyChosen = [this](const QString& family) { choices.push_back(family); };
        picker->onReturnToText = [this] { ++returns; };
        layout->addWidget(picker);
        window.show();
        QApplication::processEvents();
    }

    ~Fixture()
    {
        picker->hidePopup();
        window.hide();
        QApplication::processEvents();
    }

    void search(const QString& query)
    {
        picker->hidePopup();
        picker->lineEdit()->setFocus();
        QTest::keyClick(picker->lineEdit(), Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(picker->lineEdit(), Qt::Key_Backspace);
        QTest::keyClicks(picker->lineEdit(), query);
        QApplication::processEvents();
    }

    QStringList visibleMatches() const
    {
        QStringList result;
        const auto* model = picker->completionPopup()->model();
        for (int i = 0; i < model->rowCount(); ++i)
            result.push_back(model->index(i, 0).data().toString());
        return result;
    }

    void checkPopupBelowSearch() const
    {
        const auto* popup = picker->completionPopup();
        CHECK(popup->isVisible());
        const auto fieldBottom = picker->lineEdit()->mapToGlobal(
            QPoint(0, picker->lineEdit()->height())).y();
        const auto popupTop = popup->mapToGlobal(QPoint {}).y();
        CHECK(popupTop >= fieldBottom);
        CHECK(popupTop <= fieldBottom + 8);
    }

    QModelIndex popupIndex(const QString& family) const
    {
        const auto* model = picker->completionPopup()->model();
        for (int i = 0; i < model->rowCount(); ++i) {
            const auto index = model->index(i, 0);
            if (index.data().toString() == family)
                return index;
        }
        return { };
    }
};

void matchingAndRanking()
{
    auto names = families();
    u::FontFamilyPicker picker(names, nullptr);
    const QStringList expectedMono { QStringLiteral("Mono"), QStringLiteral("Mono Test"),
        QStringLiteral("Monospace"), QStringLiteral("Zebra Monospace"),
        QStringLiteral("Quasimono") };
    CHECK(picker.matches(QStringLiteral("mono")) == expectedMono);
    CHECK(picker.matches(QStringLiteral("mOnO")) == expectedMono);
    CHECK(picker.matches(QStringLiteral("Noto")).front() == QStringLiteral("Noto"));
    CHECK(picker.matches(QStringLiteral("Noto Sans")).front() == QStringLiteral("Noto Sans"));

    const auto emoji = QStringLiteral("Noto Color Emoji");
    CHECK(picker.matches(QStringLiteral("noto")).contains(emoji));
    for (const auto& query :
        { QStringLiteral("noto emoji"), QStringLiteral("emoji noto"), QStringLiteral("color emoj"),
            QStringLiteral("  EMOJ\t NoTo  "), QStringLiteral("emoji emoji noto") }) {
        CHECK(picker.matches(query) == QStringList { emoji });
    }
    CHECK(picker.matches(QStringLiteral("not a real family")).isEmpty());
    CHECK(picker.matches(QStringLiteral("noto mono")).isEmpty());
    CHECK(picker.matches(QString { }).size() == names.size());
    CHECK(picker.matches(QStringLiteral(" \t ")) == picker.matches(QString { }));

    std::reverse(names.begin(), names.end());
    u::FontFamilyPicker reverse(names, nullptr);
    CHECK(reverse.matches(QStringLiteral("mono")) == expectedMono);
    CHECK(reverse.matches(QString { }) == picker.matches(QString { }));
    for (int i = 0; i < 10; ++i)
        CHECK(picker.matches(QStringLiteral("mono")) == expectedMono);

    u::FontFamilyPicker normalized(
        { QStringLiteral("Ｎｏｔｏ Color Emoji"), QStringLiteral("Straße Sans"),
            QStringLiteral("  "), QStringLiteral("Mono"), QStringLiteral("mono") },
        nullptr);
    CHECK(normalized.matches(QStringLiteral("noto emoji"))
        == QStringList { QStringLiteral("Ｎｏｔｏ Color Emoji") });
    CHECK(normalized.matches(QStringLiteral("STRAßE"))
        == QStringList { QStringLiteral("Straße Sans") });
    CHECK(normalized.matches(QStringLiteral("mono")).size() == 1);
    CHECK(normalized.matches(QString { }).size() == 3);
}

void actualTypingIsDeferredAndFiltered()
{
    Fixture f;
    CHECK(f.picker->isEditable());
    CHECK(f.picker->insertPolicy() == QComboBox::NoInsert);
    f.search(QStringLiteral("mono"));
    CHECK(f.picker->searching());
    CHECK(f.picker->lineEdit()->text() == QStringLiteral("mono"));
    CHECK(f.picker->candidate() == QStringLiteral("Mono"));
    CHECK(f.visibleMatches() == f.picker->matches(QStringLiteral("mono")));
    CHECK(f.choices.isEmpty());
    CHECK(f.returns == 0);
    f.checkPopupBelowSearch(); // Automatic completion from actual typed keys.

    // Rendering/controller synchronization must not overwrite a pending query.
    f.picker->setDisplayedFamily(QStringLiteral("Monospace"));
    f.picker->setDisplayedFamily(QString { }, true);
    CHECK(f.picker->lineEdit()->text() == QStringLiteral("mono"));
    CHECK(f.picker->candidate() == QStringLiteral("Mono"));
    CHECK(f.choices.isEmpty());
    f.picker->showPopup();
    QApplication::processEvents();
    f.checkPopupBelowSearch();
    CHECK(f.visibleMatches() == f.picker->matches(QStringLiteral("mono")));
    f.picker->hidePopup();
    CHECK(f.picker->searching());
    CHECK(f.choices.isEmpty());
    CHECK(f.returns == 0); // Dismissing suggestions is not accepting a font.

    f.window.move(80, 120);
    f.picker->showPopup();
    QApplication::processEvents();
    f.checkPopupBelowSearch(); // Explicit reopening resolves the current anchor.
    f.picker->hidePopup();

    f.picker->finishSearch(false);
    CHECK(!f.picker->searching());
    CHECK(f.picker->lineEdit()->text() == QStringLiteral("Noto Sans"));
    CHECK(f.picker->candidate().isEmpty());
    CHECK(f.choices.isEmpty());

    f.search(QStringLiteral("Mono"));
    f.picker->hidePopup();
    f.window.setFocusPolicy(Qt::StrongFocus);
    f.window.setFocus();
    QApplication::processEvents();
    CHECK(f.picker->searching());
    CHECK(f.choices.isEmpty()); // Exact-match focus loss is not popup activation.
    CHECK(f.returns == 0);
    f.picker->finishSearch(false);
}

void acceptanceCancellationAndNoMatch()
{
    Fixture f;
    f.search(QStringLiteral("emoji noto"));
    f.picker->finishSearch(true);
    CHECK(!f.picker->searching());
    CHECK(f.choices == QStringList { QStringLiteral("Noto Color Emoji") });
    CHECK(f.returns == 1);
    CHECK(f.picker->lineEdit()->text() == QStringLiteral("Noto Color Emoji"));
    f.picker->finishSearch(true);
    CHECK(f.choices.size() == 1); // An already-finished search cannot reapply.

    f.search(QStringLiteral("mono"));
    f.picker->finishSearch(false);
    CHECK(f.picker->lineEdit()->text() == QStringLiteral("Noto Color Emoji"));
    CHECK(f.choices.size() == 1);
    CHECK(f.returns == 1);

    f.search(QStringLiteral("no such font family"));
    CHECK(f.picker->searching());
    CHECK(f.picker->candidate().isEmpty());
    CHECK(f.visibleMatches().isEmpty());
    f.picker->finishSearch(true);
    CHECK(!f.picker->searching());
    CHECK(f.picker->lineEdit()->text() == QStringLiteral("Noto Color Emoji"));
    CHECK(f.choices.size() == 1);
    CHECK(f.returns == 1);

    f.search(QString { });
    CHECK(f.picker->searching());
    CHECK(f.picker->candidate().isEmpty());
    f.picker->finishSearch(true);
    CHECK(f.picker->lineEdit()->text() == QStringLiteral("Noto Color Emoji"));
    CHECK(f.choices.size() == 1);

    f.picker->setDisplayedFamily(QStringLiteral("Requested Unavailable Family"));
    f.search(QStringLiteral("no match"));
    f.picker->finishSearch(true);
    CHECK(f.picker->lineEdit()->text() == QStringLiteral("Requested Unavailable Family"));
    f.picker->setDisplayedFamily(QString { }, true);
    f.search(QStringLiteral("no match"));
    f.picker->finishSearch(false);
    CHECK(f.picker->lineEdit()->text() == QStringLiteral("Mixed fonts"));
    CHECK(f.choices.size() == 1);
}

void explicitCompletionSelectionFiresOnce()
{
    Fixture f;
    f.search(QStringLiteral("mono"));
    f.picker->showPopup();
    QApplication::processEvents();
    auto* popup = f.picker->completionPopup();
    const auto clicked = f.popupIndex(QStringLiteral("Monospace"));
    CHECK(clicked.isValid());
    if (!clicked.isValid())
        return;
    popup->scrollTo(clicked);
    const auto hit = popup->visualRect(clicked);
    CHECK(!hit.isEmpty());
    QTest::mouseClick(popup->viewport(), Qt::LeftButton, { }, hit.center());
    QApplication::processEvents();
    CHECK(f.choices == QStringList { QStringLiteral("Monospace") });
    CHECK(f.returns == 1);
    CHECK(!f.picker->searching());
    CHECK(!popup->isVisible());

    f.search(QStringLiteral("noto"));
    f.picker->showPopup();
    QApplication::processEvents();
    const auto selected = f.popupIndex(QStringLiteral("Noto Color Emoji"));
    CHECK(selected.isValid());
    if (!selected.isValid())
        return;
    popup->setCurrentIndex(selected);
    QTest::keyClick(popup, Qt::Key_Return);
    QApplication::processEvents();
    CHECK(f.choices
        == QStringList({ QStringLiteral("Monospace"), QStringLiteral("Noto Color Emoji") }));
    CHECK(f.returns == 2);
    CHECK(!f.picker->searching());
    CHECK(f.picker->lineEdit()->text() == QStringLiteral("Noto Color Emoji"));
}

void hoverHighlightsWithoutEditingOrAccepting()
{
    Fixture f;
    for (bool searching : { false, true }) {
        if (searching)
            f.search(QStringLiteral("mono"));
        f.picker->showPopup();
        QApplication::processEvents();
        auto* popup = f.picker->completionPopup();
        const auto query = f.picker->lineEdit()->text();
        for (const auto& family : { QStringLiteral("Monospace"), QStringLiteral("Mono Test") }) {
            const auto index = f.popupIndex(family);
            CHECK(index.isValid());
            popup->scrollTo(index);
            QTest::mouseMove(popup->viewport(), popup->visualRect(index).center());
            QApplication::processEvents();
            CHECK(popup->currentIndex() == index);
            CHECK(popup->selectionModel()->selectedIndexes().size() == 1);
            CHECK(popup->selectionModel()->isSelected(index));
            CHECK(f.picker->lineEdit()->text() == query);
            CHECK(f.choices.isEmpty());
            CHECK(f.returns == 0);
            if (searching)
                CHECK(f.picker->candidate() == family);
        }
        f.picker->hidePopup();
    }
    f.picker->showPopup();
    QTest::keyClick(f.picker->completionPopup(), Qt::Key_Return);
    CHECK(f.choices == QStringList { QStringLiteral("Mono Test") });
    CHECK(f.returns == 1);
}

void shortcutOwnershipAndStableGeometry()
{
    Fixture f;
    const auto geometry = f.picker->geometry();
    const auto heightHint = f.picker->sizeHint().height();
    f.search(QStringLiteral("mono"));
    QKeyEvent keyOverride(QEvent::ShortcutOverride, Qt::Key_T, Qt::ControlModifier);
    keyOverride.ignore();
    QApplication::sendEvent(f.picker, &keyOverride);
    CHECK(keyOverride.isAccepted());
    QKeyEvent textOverride(QEvent::ShortcutOverride, Qt::Key_B, { });
    textOverride.ignore();
    QApplication::sendEvent(f.picker->lineEdit(), &textOverride);
    CHECK(textOverride.isAccepted());
    f.picker->hidePopup();
    QTest::keyClick(f.picker->lineEdit(), Qt::Key_Escape);
    QApplication::processEvents();
    CHECK(!f.picker->searching());
    CHECK(f.choices.isEmpty());
    CHECK(f.returns == 1);
    CHECK(f.picker->lineEdit()->text() == QStringLiteral("Noto Sans"));

    f.search(QStringLiteral("mono"));
    f.picker->hidePopup();
    const auto query = f.picker->lineEdit()->text();
    QTest::keyClick(f.picker->lineEdit(), Qt::Key_Z, Qt::ControlModifier);
    QApplication::processEvents();
    CHECK(f.picker->lineEdit()->text() != query);
    CHECK(f.choices.isEmpty()); // Query undo belongs solely to the line edit.
    f.picker->finishSearch(false);

    for (const auto& display : { QStringLiteral("Mono"), QStringLiteral("Noto Color Emoji"),
             QStringLiteral("A very long requested but unavailable font family name") }) {
        f.picker->setDisplayedFamily(display);
        f.window.layout()->activate();
        CHECK(f.picker->geometry() == geometry);
        CHECK(f.picker->sizeHint().height() == heightHint);
    }
    f.picker->setDisplayedFamily({ }, true);
    f.window.layout()->activate();
    CHECK(f.picker->geometry() == geometry);
    f.search(QStringLiteral("missing font"));
    f.window.layout()->activate();
    CHECK(f.picker->geometry() == geometry);
    f.picker->finishSearch(false);
    CHECK(f.picker->lineEdit()->text() == QStringLiteral("Mixed fonts"));
    CHECK(f.choices.isEmpty());
}

void wheelDoesNotChangeFontOrSearch()
{
    Fixture f;
    for (bool searching : { false, true }) {
        if (searching) {
            f.search(QStringLiteral("mono"));
            f.picker->hidePopup();
        }
        for (bool focused : { false, true }) {
            if (focused)
                f.picker->setFocus();
            else
                f.picker->clearFocus();
            QApplication::processEvents();
            const auto index = f.picker->currentIndex();
            const auto text = f.picker->lineEdit()->text();
            const auto candidate = f.picker->candidate();
            for (QWidget* target : { static_cast<QWidget*>(f.picker),
                     static_cast<QWidget*>(f.picker->lineEdit()) }) {
                for (int delta : { -120, 120 }) {
                    const QPointF pos = target->rect().center();
                    QWheelEvent wheel(pos, target->mapToGlobal(pos), {}, QPoint(0, delta),
                        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
                    QApplication::sendEvent(target, &wheel);
                    QApplication::processEvents();
                    CHECK(f.picker->currentIndex() == index);
                    CHECK(f.picker->lineEdit()->text() == text);
                    CHECK(f.picker->candidate() == candidate);
                    CHECK(f.picker->searching() == searching);
                    CHECK(f.choices.isEmpty());
                }
            }
        }
    }
}

void sharedComboAppearanceAndArrowInteraction()
{
    CHECK(!QImage(QStringLiteral(":/theme/chevron-down.svg")).isNull());
    CHECK(!QImage(QStringLiteral(":/theme/chevron-down-disabled.svg")).isNull());
    QWidget sheet;
    auto* layout = new QGridLayout(&sheet);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(14);
    auto* font = new u::FontFamilyPicker(families(), &sheet);
    font->setFixedWidth(164);
    font->setDisplayedFamily(QStringLiteral("Noto Sans"));
    auto* smoothing = new QComboBox(&sheet);
    smoothing->setFixedWidth(220);
    smoothing->addItems({ QStringLiteral("None"), QStringLiteral("Weighted") });
    auto* disabled = new QComboBox(&sheet);
    disabled->addItem(QStringLiteral("Unavailable"));
    disabled->setEnabled(false);
    layout->addWidget(new QLabel(QStringLiteral("Font family"), &sheet), 0, 0);
    layout->addWidget(font, 0, 1);
    layout->addWidget(new QLabel(QStringLiteral("Smoothing"), &sheet), 1, 0);
    layout->addWidget(smoothing, 1, 1);
    layout->addWidget(new QLabel(QStringLiteral("Disabled"), &sheet), 2, 0);
    layout->addWidget(disabled, 2, 1);
    sheet.show();
    QApplication::processEvents();
    QStyleOptionComboBox comboOption;
    comboOption.initFrom(smoothing);
    comboOption.editable = false;
    CHECK(smoothing->style()->styleHint(QStyle::SH_ComboBox_Popup, &comboOption, smoothing) == 0);
    font->showPopup();
    QApplication::processEvents();
    const auto fontRowHeight = font->completionPopup()->sizeHintForRow(0);
    const auto fontHighlight = font->completionPopup()->palette().color(QPalette::Highlight);
    const auto output = qEnvironmentVariable("IMAGEEDITOR_COMBO_SCREENSHOT");
    if (!output.isEmpty())
        CHECK(font->completionPopup()->grab().save(output + QStringLiteral(".font-popup.png")));
    font->hidePopup();
    const auto size = smoothing->size();
    QTest::mouseClick(smoothing, Qt::LeftButton, {},
        QPoint(smoothing->width() - 13, smoothing->height() / 2));
    QApplication::processEvents();
    CHECK(smoothing->view()->isVisible());
    CHECK(std::abs(smoothing->view()->sizeHintForRow(0) - fontRowHeight) <= 2);
    CHECK(smoothing->view()->palette().color(QPalette::Highlight) == fontHighlight);
    CHECK(smoothing->view()->selectionModel()->isSelected(smoothing->view()->currentIndex()));
    if (!output.isEmpty())
        CHECK(smoothing->view()->grab().save(output + QStringLiteral(".smoothing-popup.png")));
    const auto option = smoothing->model()->index(1, 0);
    const auto itemPoint = smoothing->view()->visualRect(option).center();
    CHECK(smoothing->view()->viewport()->rect().contains(itemPoint));
    // Model the pointer travelling from the arrow into the results. Qt guards
    // the opening click's release until this movement (or its timeout).
    QTest::mouseMove(smoothing->view()->viewport(), itemPoint);
    QTest::mouseClick(smoothing->view()->viewport(), Qt::LeftButton, {}, itemPoint);
    QApplication::processEvents();
    CHECK(smoothing->currentIndex() == 1);
    CHECK(smoothing->size() == size);
    CHECK(font->lineEdit()->geometry().right() < font->width() - 20);
    if (!output.isEmpty())
        CHECK(sheet.grab().save(output));
}
}

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    u::applyEditorTheme(application);
    matchingAndRanking();
    actualTypingIsDeferredAndFiltered();
    acceptanceCancellationAndNoMatch();
    explicitCompletionSelectionFiresOnce();
    hoverHighlightsWithoutEditingOrAccepting();
    shortcutOwnershipAndStableGeometry();
    wheelDoesNotChangeFontOrSearch();
    sharedComboAppearanceAndArrowInteraction();
    if (failures)
        std::cerr << failures << " font picker assertion(s) failed\n";
    else
        std::cout
            << "Font picker matching, deferred search, completion and geometry tests passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
