#include "imageeditor/core/AdjustmentCommands.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/ExportDialog.hpp"
#include "imageeditor/ui/FileDialogLocations.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/Theme.hpp"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QVulkanInstance>
#include <QUrl>
#include <iostream>

class ExportUrlReceiver final : public QObject {
    Q_OBJECT
public:
    QList<QUrl> urls;
public slots:
    void open(const QUrl& url) { urls.append(url); }
};

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace r = imageeditor::render;
namespace {
int failures = 0;
#define CHECK(value)                                                                                         \
    do {                                                                                                     \
        if (!(value)) {                                                                                      \
            ++failures;                                                                                      \
            std::cerr << "FAIL " << __LINE__ << ": " << #value << '\n';                                      \
        }                                                                                                    \
    } while (false)
QByteArray read(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return { };
    return f.readAll();
}
u::ExportDialog* exportDialog()
{
    for (auto* widget : QApplication::allWidgets())
        if (widget->objectName() == "ExportDialog" && widget->isVisible())
            return dynamic_cast<u::ExportDialog*>(widget);
    return nullptr;
}
struct State {
    const c::Document* doc;
    c::Revision revision;
    std::uint64_t content;
    std::size_t undo, redo;
    std::optional<c::LayerId> active;
    bool dirty;
    QString path;
    explicit State(u::MainWindow& w)
        : doc(w.editorSession().document())
        , revision(doc->revision())
        , content(doc->contentState())
        , undo(w.editorSession().history().undoDepth())
        , redo(w.editorSession().history().redoDepth())
        , active(w.editorSession().activeLayer())
        , dirty(doc->isModified())
        , path(w.projectPath())
    {
    }
    void verify(u::MainWindow& w) const
    {
        CHECK(doc == w.editorSession().document());
        CHECK(revision == doc->revision());
        CHECK(content == doc->contentState());
        CHECK(undo == w.editorSession().history().undoDepth());
        CHECK(redo == w.editorSession().history().redoDepth());
        CHECK(active == w.editorSession().activeLayer());
        CHECK(dirty == doc->isModified());
        CHECK(path == w.projectPath());
    }
};
void dialogControls()
{
    QTemporaryDir files;
    CHECK(files.isValid());
    const QSize nativeSize(128, 96);
    u::ExportSettings initial;
    initial.format = u::ExportFormat::WebP;
    initial.destination = files.filePath("controls.webp");
    initial.size = QSize(256, 192);
    initial.aspectLocked = false;
    initial.pngMatte = true;
    initial.pngMatteColor = QColor("#123456");
    initial.jpegMatteColor = QColor("#abcdef");
    initial.pngEffort = 1;
    initial.jpegQuality = 37;
    initial.webpLossless = true;
    initial.webpQuality = 24;
    initial.webpEffort = 1;
    u::ExportDialog dialog(initial, nativeSize);
    CHECK(dialog.windowTitle() == "Export");
    CHECK(dialog.findChild<QLabel*>("ToolTitle")->text() == "Export");
    auto* close = dialog.findChild<QPushButton*>("ExportHeaderClose");
    auto* cancel = dialog.findChild<QPushButton*>("ExportCancel");
    CHECK(close && !close->icon().isNull() && cancel && cancel->isHidden());
    CHECK(dialog.findChild<QLabel*>("ExportStatus")->alignment() == Qt::AlignCenter);
    auto* width = dialog.findChild<QDoubleSpinBox*>("ExportWidth");
    auto* height = dialog.findChild<QDoubleSpinBox*>("ExportHeight");
    auto* scale = dialog.findChild<QDoubleSpinBox*>("ExportScale");
    auto* destination = dialog.findChild<QLineEdit*>("ExportDestination");
    auto* reset = dialog.findChild<QPushButton*>("ExportReset");
    auto* write = dialog.findChild<QPushButton*>("ExportWrite");
    auto* aspect = dialog.findChild<QToolButton*>("ExportAspectLock");
    CHECK(width && height && scale && destination && reset && write && aspect);
    if (!width || !height || !scale || !destination || !reset || !write || !aspect)
        return;
    for (auto* field : { width, height, scale }) {
        CHECK(!field->keyboardTracking());
        auto* editor = field->findChild<QLineEdit*>();
        CHECK(editor && !editor->isReadOnly());
        if (!editor || editor->isReadOnly())
            return;
    }
    int settingsChanges = 0, exportRequests = 0, cancelRequests = 0, closeRequests = 0;
    dialog.onSettingsChanged = [&] { ++settingsChanges; };
    dialog.onExportRequested = [&] { ++exportRequests; };
    dialog.onCancelRequested = [&] { ++cancelRequests; };
    dialog.onCloseRequested = [&] { ++closeRequests; dialog.done(QDialog::Rejected); };
    dialog.show();
    dialog.activateWindow();
    CHECK(QTest::qWaitForWindowActive(&dialog));
    const auto preview = [](QSize size) {
        QImage image(size, QImage::Format_RGBA8888);
        image.fill(QColor("#56789a"));
        return image;
    };
    const auto typeNumber = [](QDoubleSpinBox* field, const QString& value) {
        auto* editor = field->findChild<QLineEdit*>();
        QTest::mouseClick(editor, Qt::LeftButton);
        QTest::keyClick(editor, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClicks(editor, value);
    };
    CHECK(!write->isEnabled());
    CHECK(reset->text() == "Reset Settings");
    dialog.setPreview(preview(initial.size), 512);
    CHECK(write->isEnabled());

    // Reset covers hidden format controls and matte colors as well as size,
    // while emitting one coherent change and retaining the chosen output.
    QTest::mouseClick(reset, Qt::LeftButton);
    u::ExportSettings defaults;
    defaults.format = initial.format;
    defaults.destination = initial.destination;
    defaults.size = nativeSize;
    CHECK(dialog.settings() == defaults);
    CHECK(settingsChanges == 1);
    CHECK(width->value() == 128 && height->value() == 96 && scale->value() == 100);
    CHECK(aspect->isChecked());
    CHECK(dialog.findChild<QComboBox*>("ExportFormat")->currentIndex() == int(initial.format));
    CHECK(destination->text() == initial.destination);
    CHECK(dialog.findChild<QDoubleSpinBox*>("ExportPngEffort")->value() == defaults.pngEffort);
    CHECK(!dialog.findChild<QCheckBox*>("ExportPngMatte")->isChecked());
    CHECK(!dialog.findChild<QPushButton*>("ExportPngMatteColor")->isEnabled());
    CHECK(dialog.findChild<QDoubleSpinBox*>("ExportJpegQuality")->value() == defaults.jpegQuality);
    CHECK(dialog.findChild<QComboBox*>("ExportWebpMode")->currentIndex() == 0);
    CHECK(dialog.findChild<QDoubleSpinBox*>("ExportWebpQuality")->isEnabled());
    CHECK(dialog.findChild<QDoubleSpinBox*>("ExportWebpQuality")->value() == defaults.webpQuality);
    CHECK(dialog.findChild<QDoubleSpinBox*>("ExportWebpEffort")->value() == defaults.webpEffort);
    CHECK(!write->isEnabled());
    dialog.setPreview(preview(nativeSize), 400);
    CHECK(write->isEnabled());

    // Ordinary single-click typing must leave the committed settings alone
    // until Enter, and a late preview must not enable Export during the edit.
    int before = settingsChanges;
    typeNumber(scale, "150");
    CHECK(dialog.settings().size == nativeSize);
    CHECK(settingsChanges == before);
    CHECK(!write->isEnabled());
    dialog.setPreview(preview(nativeSize), 400);
    CHECK(!write->isEnabled());
    QTest::keyClick(scale->findChild<QLineEdit*>(), Qt::Key_Return);
    CHECK(dialog.settings().size == QSize(192, 144));
    CHECK(width->value() == 192 && height->value() == 144 && scale->value() == 150);
    CHECK(settingsChanges == before + 1);
    CHECK(!write->isEnabled());
    CHECK(dialog.isVisible() && exportRequests == 0);

    // Only a decoded result with matching dimensions and actual bytes is ready.
    dialog.setPreview(preview(nativeSize), 400);
    CHECK(!write->isEnabled());
    dialog.setPreview(preview(dialog.settings().size), 0);
    CHECK(!write->isEnabled());
    dialog.setPreview(preview(dialog.settings().size), -1);
    CHECK(!write->isEnabled());
    dialog.setPreview(QImage(), 400);
    CHECK(!write->isEnabled());
    dialog.setPreview(preview(dialog.settings().size), 600);
    CHECK(write->isEnabled());

    // Exact pixel entry commits on focus-out and keeps the original ratio.
    before = settingsChanges;
    typeNumber(width, "257");
    CHECK(dialog.settings().size == QSize(192, 144));
    CHECK(settingsChanges == before && !write->isEnabled());
    QTest::mouseClick(destination, Qt::LeftButton);
    CHECK(dialog.settings().size == QSize(257, 193));
    CHECK(width->value() == 257 && height->value() == 193);
    CHECK(settingsChanges == before + 1 && !write->isEnabled());
    typeNumber(height, "99");
    QTest::keyClick(height->findChild<QLineEdit*>(), Qt::Key_Return);
    CHECK(dialog.settings().size == QSize(132, 99));
    CHECK(width->value() == 132 && height->value() == 99);
    CHECK(exportRequests == 0);

    // Reject oversized output without allocating it, then recover through the
    // same text-entry path after the coordinator reports validation failure.
    typeNumber(scale, "10000");
    QTest::keyClick(scale->findChild<QLineEdit*>(), Qt::Key_Return);
    CHECK(dialog.settings().size == QSize(12800, 9600));
    const auto error = u::validateExport(dialog.settings(), nativeSize);
    CHECK(error.contains("budget"));
    CHECK(!write->isEnabled());
    dialog.setError(error);
    CHECK(!write->isEnabled() && reset->isEnabled());
    typeNumber(scale, "100");
    QTest::keyClick(scale->findChild<QLineEdit*>(), Qt::Key_Return);
    CHECK(dialog.settings().size == nativeSize);
    CHECK(u::validateExport(dialog.settings(), nativeSize).isEmpty());
    CHECK(!write->isEnabled());
    dialog.setPreview(preview(nativeSize), 400);
    CHECK(write->isEnabled());

    // Preview availability never overrides destination validation.
    for (const auto& invalid : { QString(), files.path(), files.filePath("project.vulkana"),
             files.filePath("missing/output.webp") }) {
        dialog.setDestination(invalid);
        CHECK(!write->isEnabled());
        dialog.setPreview(preview(nativeSize), 400);
        CHECK(!write->isEnabled());
    }
    dialog.setDestination(initial.destination);
    CHECK(!write->isEnabled());
    dialog.setPreview(preview(nativeSize), 400);
    CHECK(write->isEnabled());

    dialog.setProgress("Encoding preview");
    CHECK(cancel->isHidden());
    CHECK(!write->isEnabled() && reset->isEnabled());
    dialog.setPreview(preview(nativeSize), 400);
    CHECK(write->isEnabled());
    dialog.setProgress("Writing export", true);
    CHECK(cancel->isVisible() && close->isEnabled());
    cancel->click();
    CHECK(cancelRequests == 1 && closeRequests == 0 && dialog.isVisible());
    CHECK(!write->isEnabled() && !reset->isEnabled());
    CHECK(!width->isEnabled() && !destination->isEnabled());
    const auto writingSettings = dialog.settings();
    before = settingsChanges;
    reset->click();
    CHECK(dialog.settings() == writingSettings && settingsChanges == before);
    dialog.setError("Write failed");
    CHECK(cancel->isHidden());
    CHECK(!write->isEnabled() && reset->isEnabled() && width->isEnabled());
    dialog.setPreview(preview(nativeSize), 400);
    CHECK(write->isEnabled());
    QTest::mouseClick(write, Qt::LeftButton);
    CHECK(exportRequests == 1);
    dialog.setCancelled(true);
    CHECK(cancel->isHidden() && write->isEnabled() && width->isEnabled());
    close->click();
    CHECK(closeRequests == 1 && cancelRequests == 1 && !dialog.isVisible());
}
void exportConfirmation()
{
    QTemporaryDir files;
    u::ExportSettings settings;
    settings.size = QSize(32, 24);
    settings.destination = files.filePath(QString::fromUtf8("finished <画像>.png"));
    QImage image(settings.size, QImage::Format_RGBA8888);
    image.fill(Qt::white);
    CHECK(image.save(settings.destination));
    u::ExportDialog dialog(settings, settings.size);
    dialog.show();
    const auto unchanged = dialog.settings();
    ExportUrlReceiver receiver;
    QDesktopServices::setUrlHandler("file", &receiver, "open");
    int closed = 0;
    QObject::connect(&dialog, &QDialog::finished, [&] { ++closed; });
    for (int action = 0; action < 4; ++action) {
        dialog.setExported(settings.destination, settings.size, 1234);
        QCoreApplication::processEvents();
        auto* box = dialog.findChild<QWidget*>("ExportSuccessOverlay");
        CHECK(box && box->isVisible() && !box->isWindow());
        if (!box)
            break;
        CHECK(!QApplication::activeModalWidget());
        CHECK(box->parentWidget() == &dialog && box->geometry() == dialog.rect());
        CHECK(!dialog.findChild<QPushButton*>("ExportWrite")->isEnabled());
        CHECK(box->findChildren<QPushButton*>().size() == 3); // Two actions and X, no OK.
        dialog.resize(dialog.size() + QSize(40, 20));
        CHECK(box->geometry() == dialog.rect());
        if (action == 0)
            box->findChild<QPushButton*>("ExportSuccessOpenImage")->click();
        else if (action == 1)
            box->findChild<QPushButton*>("ExportSuccessBrowse")->click();
        else if (action == 2)
            box->findChild<QPushButton*>("ExportSuccessClose")->click();
        else
            QTest::keyClick(box, Qt::Key_Escape);
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        CHECK(!QApplication::activeModalWidget());
        CHECK(dialog.isVisible() && closed == 0);
        CHECK(dialog.settings() == unchanged);
        CHECK(dialog.findChild<QPushButton*>("ExportWrite")->isEnabled());
    }
    CHECK(receiver.urls == QList<QUrl>({ QUrl::fromLocalFile(settings.destination),
                              QUrl::fromLocalFile(files.path()) }));
    QDesktopServices::unsetUrlHandler("file");
}

void workflow(QVulkanInstance* instance, const QString& artifacts)
{
    QTemporaryDir files;
    u::MainWindow window(instance, true, false);
    window.setUnsavedPromptEnabled(false);
    auto* exportAction = window.findChild<QAction*>("ExportImageAction");
    CHECK(exportAction && exportAction->text() == "Export…" && !exportAction->icon().isNull());
    window.resize(1380, 920);
    window.show();
    QTest::qWait(150);
    QImage source(128, 96, QImage::Format_RGBA8888);
    for (int y = 0; y < 96; ++y)
        for (int x = 0; x < 128; ++x)
            source.setPixelColor(x, y, QColor(x * 2, y * 2, 120, (x + y) % 256));
    const auto sourcePath = files.filePath("source.png");
    CHECK(source.save(sourcePath));
    CHECK(window.openImageFromPath(sourcePath));
    CHECK(u::openDocumentDirectory() == files.path());
    CHECK(QSettings().value("files/lastOpenedDocument").toString() == sourcePath);
    auto& session = const_cast<c::EditorSession&>(window.editorSession());
    const auto id = session.activeLayer().value();
    auto adjustments = std::make_shared<c::AdjustmentStack>();
    auto& exposure = adjustments->items[std::size_t(c::AdjustmentType::Exposure)];
    exposure.enabled = true;
    exposure.parameters = c::ExposureParameters { .7 };
    CHECK(session.execute(std::make_unique<c::SetLayerAdjustmentsCommand>(
        id, session.document()->layer(id)->adjustments, adjustments)));
    u::MainWindow::FileInteractions hooks;
    hooks.chooseSavePath = [&] { return files.filePath("original.vulkana"); };
    hooks.confirmReplace = [](auto) { return true; };
    hooks.reportError = [](const QString& e) {
        std::cerr << e.toStdString() << '\n';
        CHECK(false);
    };
    window.setFileInteractions(hooks);
    CHECK(window.saveDocument(true));
    CHECK(session.execute(std::make_unique<c::SetLayerOpacityCommand>(id, .7F)));
    CHECK(session.execute(std::make_unique<c::SetLayerOpacityCommand>(id, .5F)));
    CHECK(session.undo());
    const State baseline(window);
    r::CanvasWindow* canvas = nullptr;
    for (auto* native : QGuiApplication::allWindows())
        if (native->objectName() == "VulkanCanvasWindow")
            canvas = dynamic_cast<r::CanvasWindow*>(native);
    CHECK(canvas);
    if (canvas) {
        canvas->setAdjustmentBypassLayer(id);
        canvas->setCropPreviewLayer(id);
    }
    QTest::qWait(150);
    const auto stats = canvas->rendererStats();
    const auto size = canvas->size();
    const auto zoom = canvas->zoom();
    const auto output = files.filePath(QString::fromUtf8("finished-画像.PNG"));
    u::ExportSettings canonicalSettings;
    canonicalSettings.size = source.size();
    const auto canonical = u::renderExport(*session.document(), canonicalSettings);
    CHECK(canonical);
    // Drive the same modal card/action as the application. Timers keep running
    // through its nested loop and source-render event pumping.
    int stage = 0;
    bool saved = false;
    QTimer driver;
    driver.setInterval(20);
    QElapsedTimer elapsed;
    elapsed.start();
    QObject::connect(&driver, &QTimer::timeout, &window, [&] {
        auto* dialog = exportDialog();
        if (!dialog)
            return;
        if (elapsed.elapsed() > 15000) {
            CHECK(false);
            dialog->reject();
            return;
        }
        auto* status = dialog->findChild<QLabel*>("ExportStatus");
        if (stage == 0) {
            CHECK(!dialog->isWindow());
            CHECK(dialog->settings().size == source.size());
            CHECK(dialog->settings().format == u::ExportFormat::Png);
            QTest::keyClick(canvas, Qt::Key_Z, Qt::ControlModifier);
            baseline.verify(window);
            dialog->setDestination(output);
            // Rapid settings changes must publish only the final encoded size.
            auto* width = dialog->findChild<QDoubleSpinBox*>("ExportWidth");
            width->setValue(71);
            width->setValue(93);
            width->setValue(128);
            stage = 1;
        } else if (stage == 1 && status->text().contains("actual encoded size")) {
            auto* write = dialog->findChild<QPushButton*>("ExportWrite");
            CHECK(write->isEnabled());
            // Exercise typing through the real modal canvas shield, not just
            // a standalone widget. Even a cached preview stays disabled until
            // a same-value edit is finished; invalid sizes recover via Reset.
            auto* scaleEditor = dialog->findChild<QDoubleSpinBox*>("ExportScale")->findChild<QLineEdit*>();
            QTest::mouseClick(scaleEditor, Qt::LeftButton);
            QTest::keyClick(scaleEditor, Qt::Key_A, Qt::ControlModifier);
            QTest::keyClicks(scaleEditor, "100");
            CHECK(!write->isEnabled());
            QTest::keyClick(scaleEditor, Qt::Key_Return);
            CHECK(write->isEnabled());
            QTest::mouseClick(scaleEditor, Qt::LeftButton);
            QTest::keyClick(scaleEditor, Qt::Key_A, Qt::ControlModifier);
            QTest::keyClicks(scaleEditor, "10000");
            QTest::keyClick(scaleEditor, Qt::Key_Return);
            CHECK(dialog->settings().size == QSize(12800, 9600));
            CHECK(!write->isEnabled());
            stage = 10;
        } else if (stage == 10 && status->text().contains("budget")) {
            CHECK(!dialog->findChild<QPushButton*>("ExportWrite")->isEnabled());
            dialog->findChild<QPushButton*>("ExportReset")->click();
            CHECK(dialog->settings().size == source.size());
            stage = 11;
        } else if (stage == 11 && status->text().contains("actual encoded size")) {
            CHECK(status->text().startsWith("128 × 96"));
            CHECK(dialog->findChild<QPushButton*>("ExportWrite")->isEnabled());
            if (!artifacts.isEmpty()) {
                QDir().mkpath(artifacts);
                CHECK(dialog->grab().save(artifacts + "/export-dialog.png"));
            }
            dialog->findChild<QPushButton*>("ExportWrite")->click();
            stage = 2;
        } else if (stage == 2 && status->text().startsWith("Exported")) {
            auto* box = dialog->findChild<QWidget*>("ExportSuccessOverlay");
            CHECK(box && box->isVisible() && !box->isWindow());
            if (box) {
                if (!artifacts.isEmpty())
                    CHECK(dialog->grab().save(artifacts + "/export-success.png"));
                box->findChild<QPushButton*>("ExportSuccessClose")->click();
            }
            CHECK(dialog->isVisible());
            CHECK(QFileInfo::exists(output));
            const QImage decoded(output);
            CHECK(decoded.size() == canonical.image.size());
            for (int y = 0; y < decoded.height(); ++y)
                for (int x = 0; x < decoded.width(); ++x)
                    CHECK(decoded.pixelColor(x, y) == canonical.image.pixelColor(x, y));
            saved = true;
            dialog->reject();
            stage = 3;
        }
    });
    driver.start();
    window.findChild<QAction*>("ExportImageAction")->trigger();
    driver.stop();
    CHECK(saved);
    baseline.verify(window);
    CHECK(u::hasExportPreferences());
    CHECK(QSettings().value("export/v1/destination").toString() == output);
    CHECK(canvas->size() == size && canvas->zoom() == zoom);
    CHECK(canvas->rendererStats().uploadedBytes == stats.uploadedBytes);
    const auto bytes = read(output);
    CHECK(!bytes.isEmpty());
    if (!artifacts.isEmpty()) {
        QFile copy(artifacts + "/workflow.png");
        CHECK(copy.open(QIODevice::WriteOnly));
        CHECK(copy.write(bytes) == bytes.size());
    }

    // Export Again always asks before replacing; cancellation leaves bytes and
    // last-success preferences intact, and the document remains dirty with redo.
    stage = 0;
    elapsed.restart();
    QObject::disconnect(&driver, nullptr, &window, nullptr);
    QObject::connect(&driver, &QTimer::timeout, &window, [&] {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            CHECK(box->windowTitle() == "Replace exported image?");
            box->done(QMessageBox::Cancel);
            stage = 1;
            return;
        }
        if (auto* dialog = exportDialog(); dialog && (stage == 1 || elapsed.elapsed() > 15000)) {
            CHECK(stage == 1);
            dialog->reject();
        }
    });
    driver.start();
    window.exportImage(true);
    driver.stop();
    CHECK(stage == 1);
    CHECK(read(output) == bytes);
    baseline.verify(window);

    // Explicit extension reconciliation, actual JPEG container, no accidental
    // write under the PNG path; independent format preferences persist.
    const auto mismatched = files.filePath("reconciled.png");
    stage = 0;
    elapsed.restart();
    QObject::disconnect(&driver, nullptr, &window, nullptr);
    QTimer modalResponder;
    modalResponder.setInterval(20);
    QObject::connect(&modalResponder, &QTimer::timeout, &window, [&] {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            CHECK(box->windowTitle() == "Match image extension?");
            box->button(QMessageBox::Yes)->click();
        }
    });
    QObject::connect(&driver, &QTimer::timeout, &window, [&] {
        if (QApplication::activeModalWidget())
            return;
        auto* dialog = exportDialog();
        if (!dialog)
            return;
        if (elapsed.elapsed() > 15000) {
            CHECK(false);
            dialog->reject();
            return;
        }
        auto* status = dialog->findChild<QLabel*>("ExportStatus");
        if (stage == 0) {
            dialog->setDestination(mismatched);
            dialog->findChild<QComboBox*>("ExportFormat")->setCurrentIndex(1);
            stage = 1;
        } else if (stage == 1 && status->text().contains("actual encoded size")) {
            stage = 2;
            dialog->findChild<QPushButton*>("ExportWrite")->click();
        } else if (stage == 2 && status->text().startsWith("Exported")) {
            stage = 3;
            dialog->findChild<QPushButton*>("ExportSuccessClose")->click();
            dialog->reject();
        }
    });
    modalResponder.start();
    driver.start();
    window.exportImage();
    driver.stop();
    modalResponder.stop();
    CHECK(stage == 3);
    CHECK(!QFileInfo::exists(mismatched));
    CHECK(read(files.filePath("reconciled.jpg")).startsWith(QByteArray::fromHex("ffd8")));
    baseline.verify(window);

    // A cancelled encode/render never writes or replaces last-success prefs.
    const auto lastDestination = QSettings().value("export/v1/destination");
    // Cancel stops an explicitly requested export, unlike the header X/Escape.
    // It must keep the panel usable, without publishing or persisting anything.
    stage = 0;
    elapsed.restart();
    QObject::disconnect(&driver, nullptr, &window, nullptr);
    QObject::connect(&driver, &QTimer::timeout, &window, [&] {
        auto* dialog = exportDialog();
        if (!dialog) return;
        if (elapsed.elapsed() > 15000) {
            CHECK(false);
            dialog->reject();
            return;
        }
        auto* cancel = dialog->findChild<QPushButton*>("ExportCancel");
        if (stage == 0) {
            stage = 1; // Rendering pumps events; don't reenter this setup.
            dialog->setDestination(files.filePath("cancel-stay.webp"));
            dialog->findChild<QComboBox*>("ExportFormat")->setCurrentIndex(2);
            dialog->findChild<QComboBox*>("ExportWebpMode")->setCurrentIndex(1);
            dialog->findChild<QDoubleSpinBox*>("ExportWidth")->setValue(1024);
            // Same path as Export Again while preparation is still in flight.
            dialog->onExportRequested();
            CHECK(cancel->isVisible());
            cancel->click();
            stage = 2;
        } else if (stage == 2 && cancel->isHidden()) {
            CHECK(dialog->isVisible());
            CHECK(dialog->findChild<QLineEdit*>("ExportDestination")->isEnabled());
            CHECK(dialog->findChild<QPushButton*>("ExportReset")->isEnabled());
            CHECK(!QFileInfo::exists(files.filePath("cancel-stay.webp")));
            CHECK(QSettings().value("export/v1/destination") == lastDestination);
            stage = 3;
            dialog->findChild<QPushButton*>("ExportHeaderClose")->click();
        }
    });
    driver.start();
    window.exportImage();
    driver.stop();
    CHECK(stage == 3);
    baseline.verify(window);

    stage = 0;
    elapsed.restart();
    QObject::disconnect(&driver, nullptr, &window, nullptr);
    QObject::connect(&driver, &QTimer::timeout, &window, [&] {
        auto* dialog = exportDialog();
        if (!dialog)
            return;
        if (stage == 0) {
            dialog->setDestination(files.filePath("cancel.png"));
            dialog->findChild<QDoubleSpinBox*>("ExportWidth")->setValue(4096);
            stage = 1;
        } else if (dialog->findChild<QLabel*>("ExportStatus")->text().startsWith("Rendering")
            || elapsed.elapsed() > 3000) {
            stage = 2;
            dialog->reject();
        }
    });
    driver.start();
    window.exportImage();
    driver.stop();
    CHECK(stage == 2);
    CHECK(!QFileInfo::exists(files.filePath("cancel.png")));
    CHECK(QSettings().value("export/v1/destination") == lastDestination);
    baseline.verify(window);
    // Cancellation during a codec operation retains the modal shield until
    // the worker has acknowledged it, then returns without publishing bytes.
    stage = 0;
    elapsed.restart();
    QObject::disconnect(&driver, nullptr, &window, nullptr);
    QObject::connect(&driver, &QTimer::timeout, &window, [&] {
        auto* dialog = exportDialog();
        if (!dialog)
            return;
        if (stage == 0) {
            dialog->setDestination(files.filePath("cancel-encode.webp"));
            dialog->findChild<QComboBox*>("ExportFormat")->setCurrentIndex(2);
            dialog->findChild<QComboBox*>("ExportWebpMode")->setCurrentIndex(1);
            dialog->findChild<QDoubleSpinBox*>("ExportWidth")->setValue(2048);
            stage = 1;
        } else if (stage == 1 && dialog->findChild<QLabel*>("ExportStatus")->text().startsWith("Encoding")) {
            stage = 2;
            dialog->reject();
        } else if (elapsed.elapsed() > 15000) {
            CHECK(false);
            dialog->reject();
        }
    });
    driver.start();
    window.exportImage();
    driver.stop();
    CHECK(stage == 2);
    CHECK(!QFileInfo::exists(files.filePath("cancel-encode.webp")));
    baseline.verify(window);
    window.close();
    QTest::qWait(150);
}
} // namespace
int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("ImageEditorTests");
    QCoreApplication::setApplicationName("ExportWorkflow");
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    u::applyEditorTheme(app);
    dialogControls();
    exportConfirmation();
    const auto args = app.arguments();
    const auto index = args.indexOf("--artifacts");
    const auto artifacts = index >= 0 ? args.value(index + 1) : QString { };
    std::atomic_uint64_t warnings { 0 }, errors { 0 };
    if (args.contains("--wayland-validation")) {
        QVulkanInstance instance;
        instance.setApiVersion(QVersionNumber(1, 2));
        instance.setLayers({ QByteArrayLiteral("VK_LAYER_KHRONOS_validation") });
        instance.installDebugOutputFilter(
            [&](QVulkanInstance::DebugMessageSeverityFlags severity,
                QVulkanInstance::DebugMessageTypeFlags type, const void* message) {
                if (!type.testFlag(QVulkanInstance::ValidationMessage))
                    return false;
                if (severity.testFlag(QVulkanInstance::ErrorSeverity))
                    ++errors;
                else if (severity.testFlag(QVulkanInstance::WarningSeverity))
                    ++warnings;
                else
                    return false;
                const auto* data = static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(message);
                std::cerr << "Export Vulkan: " << (data ? data->pMessage : "unknown") << '\n';
                return true;
            });
        CHECK(instance.create());
        if (instance.isValid())
            workflow(&instance, artifacts);
        instance.destroy();
        CHECK(warnings == 0 && errors == 0);
        std::cout << "Export Vulkan: " << warnings << " warnings, " << errors << " errors\n";
    } else
        workflow(nullptr, artifacts);
    std::cout << (failures ? "Export workflow FAILED\n" : "Export workflow passed\n");
    return failures ? 1 : 0;
}

#include "ExportWorkflowTests.moc"
