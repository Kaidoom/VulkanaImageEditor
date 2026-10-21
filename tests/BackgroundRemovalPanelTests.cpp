#include "imageeditor/ui/BackgroundRemovalPanel.hpp"
#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/Theme.hpp"

#include <QApplication>
#include <QComboBox>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QTest>

#include <iostream>

namespace ui = imageeditor::ui;
namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { ++failures; \
    std::cerr << "Line " << __LINE__ << ": " #condition "\n"; } } while (false)

void controlsAndHold()
{
    QWidget host;
    host.resize(600, 600);
    ui::BackgroundRemovalPanel panel(&host);
    panel.resize(panel.sizeHint());
    panel.move(10, 10);
    host.show();
    panel.show();
    QCoreApplication::processEvents();
    auto* model = panel.findChild<QComboBox*>("BackgroundRemovalModel");
    auto* view = panel.findChild<QComboBox*>("BackgroundRemovalView");
    auto* original = panel.findChild<QPushButton*>("BackgroundRemovalOriginal");
    auto* apply = panel.findChild<QPushButton*>("BackgroundRemovalApply");
    auto* cancel = panel.findChild<QPushButton*>("BackgroundRemovalCancel");
    auto* rerun = panel.findChild<QPushButton*>("BackgroundRemovalRegenerate");
    auto* status = panel.findChild<QLabel*>("BackgroundRemovalStatus");
    auto* timing = panel.findChild<QLabel*>("BackgroundRemovalTiming");
    auto* hint = panel.findChild<QLabel*>("BackgroundRemovalMaskHint");
    CHECK(model && view && original && apply && cancel && rerun && status && timing && hint);
    if (!model || !view || !original || !apply || !cancel || !rerun || !status || !timing || !hint)
        return;
    CHECK(!panel.isWindow());
    CHECK(panel.sizeHint().width() == 316);
    CHECK(!apply->isEnabled() && !original->isEnabled() && !rerun->isEnabled());

    QString selected;
    int modelChanges = 0;
    panel.onModelChanged = [&](QString id) { selected = std::move(id); ++modelChanges; };
    panel.setModels({
        {QStringLiteral("small"), QStringLiteral("Small Model"), QStringLiteral("small.onnx")},
        {QStringLiteral("large"), QStringLiteral("Large Model"), QStringLiteral("large.onnx")}
    });
    CHECK(modelChanges == 0);
    CHECK(panel.selectedModel() == QStringLiteral("small"));
    CHECK(!rerun->isEnabled() && !rerun->isVisible());
    model->setCurrentIndex(1);
    CHECK(modelChanges == 1 && selected == QStringLiteral("large"));
    CHECK(!apply->isEnabled());

    int regenerations = 0;
    panel.onRegenerate = [&] { ++regenerations; };
    panel.setError(QStringLiteral("Temporary error"));
    CHECK(rerun->isEnabled() && rerun->isVisible());
    CHECK(rerun->text() == QStringLiteral("Retry"));
    rerun->click();
    CHECK(regenerations == 1);
    panel.setBusy(true);
    CHECK(model->isEnabled() && cancel->isEnabled());
    CHECK(!apply->isEnabled() && !rerun->isEnabled());
    CHECK(status->text().contains(QStringLiteral("Removing background")));
    model->setCurrentIndex(0);
    CHECK(modelChanges == 2 && selected == QStringLiteral("small"));

    ui::BackgroundRemovalResult result;
    result.mask = QImage(120, 80, QImage::Format_Grayscale8);
    result.mask.fill(255);
    result.timings.initializationMs = 250;
    result.timings.inferenceMs = 1234;
    result.timings.preprocessingMs = 12;
    result.timings.postprocessingMs = 20;
    result.timings.totalMs = 1516;
    result.details = QStringLiteral("Graph details");
    panel.setResult(result);
    CHECK(apply->isEnabled() && original->isEnabled() && !rerun->isEnabled());
    CHECK(!rerun->isVisible());
    CHECK(status->text() == QStringLiteral("Preview ready"));
    CHECK(timing->text().contains(QStringLiteral("1.52 s")));
    CHECK(timing->toolTip().contains(QStringLiteral("Inference 1.23 s")));
    CHECK(timing->toolTip().contains(result.details));

    int applications = 0;
    panel.onApply = [&] { ++applications; };
    apply->click();
    CHECK(applications == 1);
    ui::BackgroundRemovalPanel::View currentView{};
    panel.onViewChanged = [&](auto value) { currentView = value; };
    view->setCurrentIndex(3);
    CHECK(currentView == ui::BackgroundRemovalPanel::View::Mask);
    CHECK(panel.view() == currentView);

    bool held = false;
    int originalChanges = 0;
    panel.onOriginalChanged = [&](bool value) { held = value; ++originalChanges; };
    QTest::mousePress(original, Qt::LeftButton);
    CHECK(held);
    QTest::mouseRelease(original, Qt::LeftButton);
    CHECK(!held && originalChanges == 2);
    QTest::keyPress(original, Qt::Key_Space);
    CHECK(held);
    QKeyEvent repeat(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier, {}, true);
    QCoreApplication::sendEvent(original, &repeat);
    CHECK(held);
    QTest::keyRelease(original, Qt::Key_Space);
    CHECK(!held);
    QTest::keyPress(original, Qt::Key_Return);
    CHECK(held);
    QFocusEvent focusOut(QEvent::FocusOut);
    QCoreApplication::sendEvent(original, &focusOut);
    CHECK(!held && !original->isDown());
    QTest::keyRelease(original, Qt::Key_Return);

    QTest::mousePress(original, Qt::LeftButton);
    CHECK(held);
    QEvent deactivate(QEvent::ApplicationDeactivate);
    QCoreApplication::sendEvent(qApp, &deactivate);
    CHECK(!held);
    QTest::mouseRelease(original, Qt::LeftButton);
    QTest::keyPress(original, Qt::Key_Space);
    CHECK(held);
    panel.hide();
    CHECK(!held);
    panel.show();

    panel.setTarget(QStringLiteral("<b>Untouched layer name</b>"), true);
    CHECK(hint->text().contains(QStringLiteral("existing mask")));
    CHECK(panel.findChild<QLabel*>("BackgroundRemovalTarget")->textFormat() == Qt::PlainText);
    panel.setResult(result, false);
    CHECK(!apply->isEnabled());
    apply->click();
    CHECK(applications == 1);
    result.error = QStringLiteral("Unsupported test operator.");
    panel.setResult(result);
    CHECK(status->text() == result.error && !apply->isEnabled());
    CHECK(timing->text().isEmpty());
    CHECK(rerun->isEnabled() && rerun->isVisible());
    const QString longError = QStringLiteral("Unsupported operator: ")
        + QStringLiteral("complete model initialization diagnostic ").repeated(50);
    panel.resize(260, panel.sizeHint().height());
    panel.setError(longError);
    QCoreApplication::processEvents();
    CHECK(status->height() <= status->fontMetrics().lineSpacing() * 3);
    CHECK(status->text().size() < longError.size());
    CHECK(status->toolTip() == longError);
    CHECK(status->accessibleDescription() == longError);
    CHECK(status->text().contains(QChar(0x2026)));
    CHECK(cancel->geometry().bottom() < panel.height());
    int cancellations = 0;
    panel.onCancel = [&] { ++cancellations; };
    panel.setBusy(true);
    cancel->click();
    CHECK(cancellations == 1);
    panel.setModels({});
    CHECK(!model->isEnabled() && !apply->isEnabled() && !rerun->isEnabled());
    CHECK(status->toolTip().contains(QStringLiteral("models are unavailable")));
}

void localization()
{
    ui::BackgroundRemovalPanel panel;
    panel.resize(panel.sizeHint());
    panel.setModels({{QStringLiteral("u2netp"), QStringLiteral("U²-NetP"),
                      QStringLiteral("u2netp.onnx")},
                     {QStringLiteral("birefnet-lite-int8"), QStringLiteral("BiRefNet-lite INT8"),
                      QStringLiteral("birefnet-lite-int8.onnx")}});
    auto* models = panel.findChild<QComboBox*>("BackgroundRemovalModel");
    auto* view = panel.findChild<QComboBox*>("BackgroundRemovalView");
    auto* original = panel.findChild<QPushButton*>("BackgroundRemovalOriginal");
    auto* status = panel.findChild<QLabel*>("BackgroundRemovalStatus");
    auto* timing = panel.findChild<QLabel*>("BackgroundRemovalTiming");
    auto* hint = panel.findChild<QLabel*>("BackgroundRemovalMaskHint");
    auto& localization = ui::i18n::Localization::instance();
    ui::BackgroundRemovalResult result;
    result.mask = QImage(20, 30, QImage::Format_Grayscale8);
    result.mask.fill(255);
    result.timings.totalMs = 1234;
    panel.setResult(result);
    panel.setTarget(QStringLiteral("Original layer name"), true);
    const auto englishHint = hint->text();
    CHECK(localization.setLanguage(QStringLiteral("fr")));
    CHECK(panel.accessibleName() == QStringLiteral("Supprimer l’arrière-plan"));
    CHECK(view->itemText(3) == QStringLiteral("Masque seul"));
    CHECK(status->toolTip() == QStringLiteral("Aperçu prêt"));
    CHECK(timing->text() == QStringLiteral("Durée : 1.23 s"));
    CHECK(hint->text() != englishHint);
    CHECK(models->currentData() == QStringLiteral("u2netp"));
    CHECK(models->itemText(0) == QStringLiteral("U²-NetP"));
    CHECK(models->itemText(1) == QStringLiteral("BiRefNet-lite INT8"));
    CHECK(localization.setLanguage(QStringLiteral("ar")));
    CHECK(panel.layoutDirection() == Qt::RightToLeft);
    CHECK(status->toolTip() == QStringLiteral("المعاينة جاهزة"));
    CHECK(timing->text().contains(QStringLiteral("\u20661.23\u2069")));
    CHECK(original->text() == ui::i18n::text("main.window.refinement.hold.for.original"));
    CHECK(localization.setLanguage(QStringLiteral("en")));
    CHECK(hint->text() == englishHint);
    panel.setBusy(true);
    CHECK(timing->text().isEmpty());
    CHECK(localization.setLanguage(QStringLiteral("fr")));
    CHECK(timing->text().isEmpty());
    CHECK(localization.setLanguage(QStringLiteral("en")));
}
} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    ui::i18n::Localization::instance().initialize();
    ui::i18n::Localization::instance().setLanguage(QStringLiteral("en"));
    ui::applyEditorTheme(app);
    controlsAndHold();
    localization();
    if (!failures)
        std::cout << "Background removal panel: model changes, timing, safe apply, cancellation and original comparison passed\n";
    return failures ? 1 : 0;
}
