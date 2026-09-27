#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/Theme.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QImage>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionSpinBox>
#include <QTest>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace ui = imageeditor::ui;
namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "FAIL line " << __LINE__ << ": " #condition "\n"; ++failures; } } while (false)

void settle()
{
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

void wheel(QWidget& receiver, const QPoint& angleDelta, const QPoint& pixelDelta = {})
{
    const QPoint center = receiver.rect().center();
    QWheelEvent event(center, receiver.mapToGlobal(center), pixelDelta, angleDelta,
        Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
    QCoreApplication::sendEvent(&receiver, &event);
    settle();
}

void closedCombosNeverChangeOnWheel()
{
    // Ordinary Qt controls created after theme setup cover dialogs and future
    // panels without requiring each construction site to install a filter.
    QDialog dialog;
    auto* layout = new QVBoxLayout(&dialog);
    auto* other = new QPushButton(QStringLiteral("Other focus"), &dialog);
    auto* combo = new QComboBox(&dialog);
    layout->addWidget(other);
    layout->addWidget(combo);
    for (int i = 0; i < 40; ++i) combo->addItem(QStringLiteral("Option %1").arg(i));
    combo->setCurrentIndex(20);
    dialog.show();
    dialog.activateWindow();
    settle();
    QSignalSpy changed(combo, &QComboBox::currentIndexChanged);
    QSignalSpy activated(combo, &QComboBox::activated);
    for (const bool editable : {false, true}) {
        combo->setEditable(editable);
        for (const bool focused : {false, true}) {
            if (focused) combo->setFocus(Qt::OtherFocusReason);
            else other->setFocus(Qt::OtherFocusReason);
            settle();
            CHECK(combo->hasFocus() == focused);
            const auto text = combo->currentText();
            changed.clear();
            activated.clear();
            for (const auto delta : {QPoint(0, 120), QPoint(0, -120), QPoint(120, 0)})
                wheel(*combo, delta);
            wheel(*combo, {}, QPoint(0, -30));
            if (auto* editor = combo->lineEdit()) {
                wheel(*editor, QPoint(0, 120));
                wheel(*editor, QPoint(0, -120));
                wheel(*editor, {}, QPoint(0, -30));
            }
            CHECK(combo->currentIndex() == 20);
            CHECK(combo->currentText() == text);
            CHECK(changed.empty());
            CHECK(activated.empty());
        }
    }
    // Wheel protection must leave deliberate keyboard selection working.
    combo->setEditable(false);
    combo->setFocus(Qt::OtherFocusReason);
    QTest::keyClick(combo, Qt::Key_Down);
    CHECK(combo->currentIndex() == 21);
    dialog.close();
}

void tabDoesNotNavigateControls()
{
    QDialog dialog;
    auto* layout = new QVBoxLayout(&dialog);
    auto* field = new QLineEdit;
    auto* slider = new ui::CompactValueControl;
    auto* combo = new QComboBox;
    combo->addItems({"First", "Second"});
    auto* button = new QPushButton("Action");
    auto* text = new QTextEdit;
    for (auto* control : QList<QWidget*>{field, slider, combo, button, text}) layout->addWidget(control);
    dialog.show(); dialog.activateWindow(); settle();
    for (auto* control : QList<QWidget*>{field, slider, combo, button}) {
        control->setFocus(); settle();
        auto* before = QApplication::focusWidget();
        QTest::keyClick(control, Qt::Key_Tab);
        CHECK(QApplication::focusWidget() == before);
        QTest::keyClick(control, Qt::Key_Backtab, Qt::ShiftModifier);
        CHECK(QApplication::focusWidget() == before);
    }
    text->setFocus(); QTest::keyClick(text, Qt::Key_Tab);
    CHECK(text->toPlainText() == "\t"); // Text input is not focus traversal.
    dialog.close();
}

void anOpenComboScrollsItsListWithoutCommitting()
{
    for (const bool editable : {false, true}) {
        QComboBox combo;
        combo.setEditable(editable);
        combo.setMaxVisibleItems(6);
        for (int i = 0; i < 80; ++i) combo.addItem(QStringLiteral("Popup option %1").arg(i));
        combo.setCurrentIndex(0);
        combo.resize(220, 34);
        combo.show();
        combo.activateWindow();
        combo.setFocus(Qt::OtherFocusReason);
        settle();
        combo.showPopup();
        settle();
        auto* view = combo.view();
        CHECK(view && view->isVisible());
        if (!view || !view->isVisible()) continue;
        auto* scrollbar = view->verticalScrollBar();
        CHECK(scrollbar && scrollbar->maximum() > 0);
        if (!scrollbar || scrollbar->maximum() <= 0) { combo.hidePopup(); continue; }
        QSignalSpy changed(&combo, &QComboBox::currentIndexChanged);
        QSignalSpy activated(&combo, &QComboBox::activated);
        const auto before = scrollbar->value();
        wheel(*view->viewport(), QPoint(0, -120));
        CHECK(scrollbar->value() > before);
        CHECK(combo.currentIndex() == 0);
        CHECK(changed.empty() && activated.empty());
        CHECK(view->isVisible());
        // Escape cancels the browsed list, retaining the original option.
        QTest::keyClick(view, Qt::Key_Escape);
        settle();
        CHECK(!view->isVisible());
        CHECK(combo.currentIndex() == 0);
        CHECK(changed.empty() && activated.empty());
    }
}

QStyleOptionSpinBox spinOption(QAbstractSpinBox& spin)
{
    QStyleOptionSpinBox option;
    option.initFrom(&spin);
    option.frame = spin.hasFrame();
    option.buttonSymbols = spin.buttonSymbols();
    option.stepEnabled = QAbstractSpinBox::StepUpEnabled | QAbstractSpinBox::StepDownEnabled;
    option.subControls = QStyle::SC_All;
    return option;
}

QRect spinRect(QAbstractSpinBox& spin, QStyle::SubControl part)
{
    const auto option = spinOption(spin);
    return spin.style()->subControlRect(QStyle::CC_SpinBox, &option, part, &spin);
}

bool hasArrowInk(const QImage& image, const QRect& button)
{
    // Only inspect the button interior: its outline and divider cannot make
    // a blank arrow button pass. Account for device pixels in HiDPI runs.
    const auto interior = button.adjusted(3, 3, -3, -3);
    const auto scale = image.devicePixelRatio();
    int darkest = 255;
    int lightest = 0;
    for (int y = qCeil(interior.top() * scale); y < qFloor((interior.bottom() + 1) * scale); ++y) {
        for (int x = qCeil(interior.left() * scale); x < qFloor((interior.right() + 1) * scale); ++x) {
            if (!image.rect().contains(x, y)) continue;
            const auto value = qGray(image.pixel(x, y));
            darkest = std::min(darkest, value);
            lightest = std::max(lightest, value);
        }
    }
    return lightest - darkest >= 40;
}

void manualSliderEntryDimsOnlyProgress()
{
    ui::CompactValueControl control;
    control.setRange(0, 100);
    control.setValue(80);
    control.resize(280, 32);
    control.show();
    control.setFocus();
    const auto field = control.valueFieldRect();
    QTest::mouseMove(&control, field.center());
    settle();
    const auto normal = control.grab().toImage();
    const auto palette = control.palette();
    QTest::mouseDClick(&control, Qt::LeftButton, {}, field.center());
    QTest::mouseRelease(&control, Qt::LeftButton, {}, field.center());
    settle();
    CHECK(control.isManualEntryActive());
    const auto manual = control.grab().toImage();
    const auto sample = [](const QImage& image, QPoint p) {
        return image.pixelColor(qRound(p.x() * image.devicePixelRatio()),
            qRound(p.y() * image.devicePixelRatio()));
    };
    const QPoint filled(field.left() + 8, field.center().y());
    const QPoint empty(field.right() - 8, field.center().y());
    CHECK(sample(normal, filled) != sample(manual, filled));
    CHECK(sample(normal, empty) == sample(manual, empty));
    CHECK(normal.copy(spinRect(control, QStyle::SC_SpinBoxUp))
        == manual.copy(spinRect(control, QStyle::SC_SpinBoxUp)));
    CHECK(control.palette() == palette); // Text selection retains the theme color.
    QTest::keyClick(&control, Qt::Key_Return);
    settle();
    CHECK(!control.isManualEntryActive());
    CHECK(sample(control.grab().toImage(), filled) == sample(normal, filled));
}

void colorDialogSpinButtonsRenderAndMatchTheirHitTargets(QApplication& application)
{
    for (const auto preset : {ui::ThemePreset::Dark, ui::ThemePreset::Light}) {
        ui::ThemeSettings theme;
        theme.preset = preset;
        ui::applyEditorTheme(application, theme);
        QColorDialog dialog(QColor(80, 130, 180));
        dialog.setOption(QColorDialog::DontUseNativeDialog);
        dialog.setOption(QColorDialog::ShowAlphaChannel);
        dialog.show();
        settle();
        const auto reviewPath = qEnvironmentVariable("IMAGEEDITOR_COLOR_DIALOG_REVIEW");
        if (!reviewPath.isEmpty() && preset == ui::ThemePreset::Dark)
            CHECK(dialog.grab().save(reviewPath));
        ui::CompactValueControl reference;
        reference.resize(140, 30);
        reference.ensurePolished();
        const auto referenceWidth = spinRect(reference, QStyle::SC_SpinBoxUp).width();
        auto spins = dialog.findChildren<QSpinBox*>();
        CHECK(spins.size() >= 6);
        bool testedHold = false;
        for (auto* spin : spins) {
            if (!spin->isVisible()) continue;
            const auto option = spinOption(*spin);
            const auto up = spinRect(*spin, QStyle::SC_SpinBoxUp);
            const auto down = spinRect(*spin, QStyle::SC_SpinBoxDown);
            const auto field = spinRect(*spin, QStyle::SC_SpinBoxEditField);
            CHECK(up.width() >= 14 && up.height() >= 10);
            CHECK(down.width() == up.width() && down.height() >= 10);
            CHECK(up.width() == referenceWidth);
            CHECK(spin->rect().contains(up) && spin->rect().contains(down));
            CHECK(!up.intersects(down));
            CHECK(!field.intersects(up) && !field.intersects(down));
            CHECK(spin->style()->hitTestComplexControl(QStyle::CC_SpinBox, &option,
                up.center(), spin) == QStyle::SC_SpinBoxUp);
            CHECK(spin->style()->hitTestComplexControl(QStyle::CC_SpinBox, &option,
                down.center(), spin) == QStyle::SC_SpinBoxDown);
            spin->setValue((spin->minimum() + spin->maximum()) / 2);
            settle();
            const auto image = spin->grab().toImage();
            CHECK(hasArrowInk(image, up));
            CHECK(hasArrowInk(image, down));
            const auto before = spin->value();
            QTest::mouseClick(spin, Qt::LeftButton, Qt::NoModifier, up.center());
            CHECK(spin->value() == before + spin->singleStep());
            QTest::mouseClick(spin, Qt::LeftButton, Qt::NoModifier, down.center());
            CHECK(spin->value() == before);
            if (!testedHold) {
                const auto delay = spin->style()->styleHint(QStyle::SH_SpinBox_ClickAutoRepeatThreshold,
                    &option, spin);
                QTest::mousePress(spin, Qt::LeftButton, Qt::NoModifier, up.center());
                QTest::qWait(std::max(delay, 0) + 350);
                QTest::mouseRelease(spin, Qt::LeftButton, Qt::NoModifier, up.center());
                CHECK(spin->value() > before + spin->singleStep());
                const auto afterUpHold = spin->value();
                QTest::mousePress(spin, Qt::LeftButton, Qt::NoModifier, down.center());
                QTest::qWait(std::max(delay, 0) + 350);
                QTest::mouseRelease(spin, Qt::LeftButton, Qt::NoModifier, down.center());
                CHECK(spin->value() < afterUpHold - spin->singleStep());
                testedHold = true;
            }
        }
        CHECK(testedHold);
        dialog.close();
    }
    ui::applyEditorTheme(application, ui::ThemeSettings());
}
} // namespace

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    ui::applyEditorTheme(application);
    closedCombosNeverChangeOnWheel();
    tabDoesNotNavigateControls();
    anOpenComboScrollsItsListWithoutCommitting();
    manualSliderEntryDimsOnlyProgress();
    colorDialogSpinButtonsRenderAndMatchTheirHitTargets(application);
    if (failures) {
        std::cerr << failures << " widget polish assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All widget polish tests passed\n";
    return EXIT_SUCCESS;
}
