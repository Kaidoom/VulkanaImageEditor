#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/NewDocumentDialog.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/RecentFiles.hpp"
#include "imageeditor/platform/PlatformStartup.hpp"

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QLineEdit>
#include <QMenu>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace core = imageeditor::core;
namespace ui = imageeditor::ui;
namespace {
int failures = 0;
void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
#define CHECK(expression) \
    check(static_cast<bool>(expression), #expression, __LINE__)

void settle()
{
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

QByteArray fileBytes(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error(
            (QStringLiteral("Cannot read test file: ") + path).toStdString());
    return file.readAll();
}

struct Fixture {
    QTemporaryDir files;
    ui::MainWindow::UnsavedChoice decision { ui::MainWindow::UnsavedChoice::Cancel };
    QString destination;
    QStringList errors;
    int asks { 0 }, choices { 0 }, replacements { 0 }, progressCalls { 0 };
    bool replace { true }, continueProgress { true };
    std::function<void()> duringProgress;
    QString imagePath;
    ui::MainWindow window { nullptr, false };

    Fixture()
    {
        if (!files.isValid())
            throw std::runtime_error("Cannot create temporary lifecycle directory");
        ui::MainWindow::FileInteractions hooks;
        hooks.askUnsaved = [this] {
            ++asks;
            return decision;
        };
        hooks.chooseSavePath = [this] {
            ++choices;
            return destination;
        };
        hooks.confirmReplace = [this](const QString&) {
            ++replacements;
            return replace;
        };
        hooks.reportError = [this](const QString& error) { errors.append(error); };
        hooks.progress = [this](quint64, quint64) {
            ++progressCalls;
            if (duringProgress)
                duringProgress();
            return continueProgress;
        };
        window.setFileInteractions(std::move(hooks));
        // Do not disable prompts: persistWindowState=false must not disable
        // production save protection. Every decision is supplied explicitly.
        imagePath = files.filePath(QStringLiteral("source.png"));
        QImage image(32, 24, QImage::Format_RGBA8888);
        image.fill(QColor(21, 96, 180, 173));
        CHECK(image.save(imagePath));
        CHECK(window.openImageFromPath(imagePath));
        CHECK(asks == 0);
        CHECK(window.projectPath().isEmpty());
        CHECK(!session().document()->isModified());
    }
    ~Fixture()
    {
        // Teardown is not one of the interactions under test.
        window.setUnsavedPromptEnabled(false);
        window.close();
        settle();
    }
    core::EditorSession& session()
    {
        // Test-only mutation seam: execute real production commands, never
        // mutate command internals or manufacture a modified flag.
        return const_cast<core::EditorSession&>(window.editorSession());
    }
    void modify()
    {
        const auto id = session().document()->layers().front().id;
        const bool visible = session().document()->layer(id)->visible;
        CHECK(session().execute(
            std::make_unique<core::SetLayerVisibilityCommand>(id, !visible)));
        CHECK(session().document()->isModified());
    }
    QString saveAs(const QString& name = QStringLiteral("working.vulkana"))
    {
        destination = files.filePath(name);
        CHECK(window.saveDocument(true));
        CHECK(window.projectPath() == QFileInfo(destination).absoluteFilePath());
        CHECK(!session().document()->isModified());
        return window.projectPath();
    }
    void show()
    {
        window.resize(1280, 800);
        window.show();
        window.activateWindow();
        window.setFocus();
        settle();
    }
};

struct State {
    const core::Document* document;
    std::uint64_t content;
    core::Revision revision;
    std::optional<core::LayerId> active;
    std::size_t undo, redo, memory;
    QString path;
    bool modified, visible;
    explicit State(Fixture& f)
        : document(f.session().document())
        , content(document->contentState())
        , revision(document->revision())
        , active(f.session().activeLayer())
        , undo(f.session().history().undoDepth())
        , redo(f.session().history().redoDepth())
        , memory(f.session().history().memoryUsed())
        , path(f.window.projectPath())
        , modified(document->isModified())
        , visible(document->layers().front().visible)
    {
    }
    void verify(Fixture& f) const
    {
        CHECK(f.session().document() == document);
        CHECK(f.session().document()->contentState() == content);
        CHECK(f.session().document()->revision() == revision);
        CHECK(f.session().activeLayer() == active);
        CHECK(f.session().history().undoDepth() == undo);
        CHECK(f.session().history().redoDepth() == redo);
        CHECK(f.session().history().memoryUsed() == memory);
        CHECK(f.window.projectPath() == path);
        CHECK(f.session().document()->isModified() == modified);
        CHECK(f.session().document()->layers().front().visible == visible);
    }
};

void saveAsCancellationFailureAndAssociation()
{
    Fixture f;
    f.modify();
    const State unsaved(f);
    CHECK(!f.window.saveDocument(true)); // Destination picker cancellation.
    unsaved.verify(f);
    CHECK(f.errors.empty());
    const auto original = f.saveAs();
    const auto previousFile = fileBytes(original);
    f.modify();
    const State modified(f);
    f.destination.clear();
    CHECK(!f.window.saveDocument(true));
    modified.verify(f);
    f.destination = original;
    f.replace = false;
    const auto confirmations = f.replacements;
    CHECK(!f.window.saveDocument(true));
    CHECK(f.replacements == confirmations + 1);
    modified.verify(f);
    CHECK(fileBytes(original) == previousFile);

    f.replace = true;
    f.destination = f.files.filePath(QStringLiteral("missing-parent/failure.vulkana"));
    CHECK(!f.window.saveDocument(true));
    CHECK(!f.errors.empty());
    modified.verify(f);
    CHECK(fileBytes(original) == previousFile);
    f.errors.clear();
    f.continueProgress = false;
    const auto progressBefore = f.progressCalls;
    CHECK(!f.window.saveDocument()); // Cancel replacing the associated file.
    CHECK(f.progressCalls > progressBefore);
    modified.verify(f);
    CHECK(fileBytes(original) == previousFile);
    CHECK(f.errors.empty());

    f.continueProgress = true;
    const auto choices = f.choices;
    CHECK(f.window.saveDocument()); // Existing association requires no picker.
    CHECK(f.choices == choices);
    CHECK(f.window.projectPath() == original);
    CHECK(!f.session().document()->isModified());
    auto loaded = ui::loadProject(original);
    CHECK(loaded);
    if (loaded)
        CHECK(loaded.document->layers().front().visible == f.session().document()->layers().front().visible);
    const auto saved = fileBytes(original);
    const auto second = f.saveAs(QStringLiteral("copy.vulkana"));
    CHECK(second != original);
    CHECK(QFileInfo::exists(second));
    CHECK(fileBytes(original) == saved);
    CHECK(f.errors.empty());
}

void openingImageDoesNotTurnSaveIntoImageOverwrite()
{
    Fixture f;
    const auto png = fileBytes(f.imagePath);
    f.modify();
    f.destination = f.imagePath; // Deliberately choose the original PNG name.
    CHECK(f.window.saveDocument());
    const auto project = f.imagePath + QStringLiteral(".vulkana");
    CHECK(f.window.projectPath() == QFileInfo(project).absoluteFilePath());
    CHECK(QFileInfo::exists(project));
    CHECK(fileBytes(f.imagePath) == png);
    CHECK(f.replacements == 0);
    CHECK(f.choices == 1);
    CHECK(!f.session().document()->isModified());
    auto loaded = ui::loadProject(project);
    CHECK(loaded);
    if (loaded)
        CHECK((loaded.document->canvas().extent == core::Extent2u { 32, 24 }));
    CHECK(f.errors.empty());
}

void closeChoicesKeepOrFinishDocument()
{
    enum class Case { Cancel,
        Discard,
        Save,
        SavePickerCancel,
        SaveFailure };
    for (const auto mode : { Case::Cancel, Case::Discard, Case::Save,
             Case::SavePickerCancel, Case::SaveFailure }) {
        Fixture f;
        f.modify();
        f.show();
        const State before(f);
        f.decision = mode == Case::Cancel ? ui::MainWindow::UnsavedChoice::Cancel
            : mode == Case::Discard
            ? ui::MainWindow::UnsavedChoice::Discard
            : ui::MainWindow::UnsavedChoice::Save;
        if (mode == Case::Save)
            f.destination = f.files.filePath(QStringLiteral("close.vulkana"));
        if (mode == Case::SaveFailure)
            f.destination = f.files.filePath(QStringLiteral("missing-parent/close.vulkana"));
        const bool accepted = f.window.close();
        const bool shouldClose = mode == Case::Discard || mode == Case::Save;
        CHECK(accepted == shouldClose);
        CHECK(f.window.isVisible() != shouldClose);
        CHECK(f.asks == 1);
        if (mode == Case::Save) {
            CHECK(QFileInfo::exists(f.destination));
            CHECK(f.window.projectPath() == QFileInfo(f.destination).absoluteFilePath());
            CHECK(!f.session().document()->isModified());
            CHECK(f.session().history().undoDepth() == before.undo);
        } else {
            before.verify(f);
        }
        CHECK(f.errors.isEmpty() == (mode != Case::SaveFailure));
    }
}

void failedOpenAndCancelledOpenPreserveDocument()
{
    Fixture f;
    const auto path = f.saveAs();
    f.modify();
    // Preserve a real redo branch as well as the currently modified state.
    const auto id = f.session().document()->layers().front().id;
    CHECK(f.session().execute(
        std::make_unique<core::SetLayerOpacityCommand>(id, .5f)));
    CHECK(f.session().undo());
    const State before(f);
    const auto original = f.window.activeDocumentId();
    f.decision = ui::MainWindow::UnsavedChoice::Cancel;
    CHECK(f.window.openImageFromPath(f.imagePath)); // Open never discards the existing tab.
    CHECK(f.asks == 0);
    CHECK(f.window.activateDocument(original));
    before.verify(f);
    CHECK(f.errors.empty());

    f.decision = ui::MainWindow::UnsavedChoice::Discard;
    CHECK(!f.window.openImageFromPath(
        f.files.filePath(QStringLiteral("missing.vulkana"))));
    before.verify(f);
    CHECK(!f.errors.empty());
    f.errors.clear();
    const auto broken = f.files.filePath(QStringLiteral("damaged.vulkana"));
    {
        QFile file(broken);
        CHECK(file.open(QIODevice::WriteOnly));
        CHECK(file.write("not a project") == 13);
    }
    CHECK(!f.window.openImageFromPath(broken));
    before.verify(f);
    CHECK(!f.errors.empty());
    f.errors.clear();
    f.continueProgress = false;
    const auto unloaded = f.files.filePath("unloaded.vulkana");
    CHECK(QFile::copy(path, unloaded));
    CHECK(!f.window.openImageFromPath(unloaded));
    before.verify(f);
    CHECK(f.errors.empty());
    f.continueProgress = true;
    CHECK(f.window.openImageFromPath(path));
    CHECK(f.window.projectPath() == path);
    before.verify(f); // Existing project is focused, including unsaved history.
    CHECK(f.window.openImageFromPath(unloaded));
    CHECK(!f.session().document()->isModified() && f.session().history().undoDepth() == 0);
    CHECK(f.session().document()->layers().front().visible);
    CHECK(f.errors.empty());
}

void saveBeforeOpenMustSucceed()
{
    Fixture f;
    f.modify();
    const State before(f);
    const auto original = f.window.activeDocumentId();
    f.decision = ui::MainWindow::UnsavedChoice::Save;
    CHECK(f.window.openImageFromPath(f.imagePath));
    CHECK(f.asks == 0 && f.choices == 0);
    CHECK(f.window.activateDocument(original));
    before.verify(f);
    f.destination = f.files.filePath(QStringLiteral("missing-parent/before-open.vulkana"));
    CHECK(f.window.openImageFromPath(f.imagePath));
    CHECK(f.asks == 0 && f.errors.empty());
    CHECK(f.window.activateDocument(original));
    before.verify(f);
    f.destination = f.files.filePath(QStringLiteral("before-open.vulkana"));
    CHECK(f.window.openImageFromPath(f.imagePath));
    CHECK(!QFileInfo::exists(f.destination)); // No implicit Save from Open.
    CHECK(f.window.projectPath()
            .isEmpty()); // Image open clears the native association.
    CHECK(!f.session().document()->isModified());
    CHECK(f.session().history().undoDepth() == 0);
    CHECK(f.window.activateDocument(original));
    before.verify(f);
    CHECK(f.errors.empty());
}

void newDialog(Fixture& f,
    const std::function<void(ui::NewDocumentDialog&)>& interact)
{
    auto* action = f.window.findChild<QAction*>(QStringLiteral("NewDocumentAction"));
    CHECK(action);
    if (!action)
        return;
    bool visited = false;
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, [&] {
        CHECK(false); // A regression must fail instead of hanging in a modal loop.
        for (auto* widget : QApplication::allWidgets())
            if (auto* dialog = qobject_cast<QDialog*>(widget))
                dialog->reject();
    });
    watchdog.start(2000);
    QTimer::singleShot(0, &f.window, [&] {
        ui::NewDocumentDialog* dialog = nullptr;
        for (auto* candidate : f.window.findChildren<QDialog*>())
            if (auto* card = dynamic_cast<ui::NewDocumentDialog*>(candidate); card && !card->isHidden())
                dialog = card;
        CHECK(dialog);
        if (!dialog)
            return;
        visited = true;
        interact(*dialog);
    });
    action->trigger();
    watchdog.stop();
    CHECK(visited);
}

void newDocumentGuardOccursOnlyAfterDialogAcceptance()
{
    Fixture f;
    f.saveAs();
    f.modify();
    const State before(f);
    const auto original = f.window.activeDocumentId();
    newDialog(f, [](ui::NewDocumentDialog& dialog) { dialog.reject(); });
    CHECK(f.asks == 0);
    before.verify(f);
    f.decision = ui::MainWindow::UnsavedChoice::Cancel;
    newDialog(f, [](ui::NewDocumentDialog& dialog) { dialog.accept(); });
    CHECK(f.asks == 0 && f.window.documentCount() == 2);
    CHECK(f.window.activateDocument(original));
    before.verify(f);
    f.decision = ui::MainWindow::UnsavedChoice::Discard;
    newDialog(f, [](ui::NewDocumentDialog& dialog) {
        auto* width = dialog.findChild<QDoubleSpinBox*>(
            QStringLiteral("CanvasWidthSpinBox"));
        auto* height = dialog.findChild<QDoubleSpinBox*>(
            QStringLiteral("CanvasHeightSpinBox"));
        CHECK(width && height);
        if (width && height) {
            width->setValue(47);
            height->setValue(31);
        }
        dialog.accept();
    });
    CHECK(f.asks == 0 && f.window.documentCount() == 3);
    CHECK((f.session().document()->canvas().extent == core::Extent2u { 47, 31 }));
    CHECK(f.session().document()->isModified());
    CHECK(f.window.projectPath().isEmpty());
    CHECK(f.session().history().undoDepth() == 0);
    CHECK(f.session().history().redoDepth() == 0);
    CHECK(f.errors.empty());
}

void recentMenuUsesTheSameUnsavedGuard()
{
    Fixture source;
    const auto project = source.saveAs();
    ui::RecentFiles recents;
    recents.clear();
    recents.recordSuccess(project, ui::RecentFileKind::Project);
    Fixture
        f; // Reads the isolated preferences containing the seeded recent item.
    f.modify();
    const State before(f);
    const auto original = f.window.activeDocumentId();
    QMenu* recent = nullptr;
    for (auto* menu : f.window.findChildren<QMenu*>())
        if (menu->title().remove('&') == QStringLiteral("Open Recent"))
            recent = menu;
    CHECK(recent);
    if (!recent)
        return;
    CHECK(QMetaObject::invokeMethod(recent, "aboutToShow", Qt::DirectConnection));
    CHECK(!recent->actions().empty());
    auto* entry = recent->actions().empty() ? nullptr : recent->actions().front()->menu();
    CHECK(entry && !entry->actions().empty());
    if (!entry || entry->actions().empty())
        return;
    auto* open = entry->actions().front();
    f.decision = ui::MainWindow::UnsavedChoice::Cancel;
    open->trigger();
    CHECK(f.asks == 0 && f.window.documentCount() == 2);
    CHECK(f.window.activateDocument(original));
    before.verify(f);
    f.decision = ui::MainWindow::UnsavedChoice::Discard;
    open->trigger();
    CHECK(f.asks == 0 && f.window.documentCount() == 2);
    CHECK(f.window.projectPath() == project);
    CHECK(!f.session().document()->isModified());
    CHECK(f.session().history().undoDepth() == 0);
    CHECK(f.errors.empty());
    recents.clear();
}

void saveShortcutsReachPersistenceFromEditableFields()
{
    Fixture f;
    f.show();
    auto* save = f.window.findChild<QAction*>(QStringLiteral("SaveDocumentAction"));
    auto* saveAs = f.window.findChild<QAction*>(QStringLiteral("SaveDocumentAsAction"));
    CHECK(save && saveAs);
    if (!save || !saveAs)
        return;
    CHECK(save->shortcuts().contains(QKeySequence(QStringLiteral("Ctrl+S"))));
    CHECK(saveAs->shortcuts().contains(QKeySequence(QStringLiteral("Ctrl+Shift+S"))));
    f.modify();
    f.destination = f.files.filePath(QStringLiteral("shortcut.vulkana"));
    QTest::keyClick(&f.window, Qt::Key_S, Qt::ControlModifier);
    settle();
    CHECK(f.choices == 1);
    CHECK(QFileInfo::exists(f.destination));
    CHECK(f.window.projectPath() == f.destination);
    CHECK(!f.session().document()->isModified());

    // Editable QWidget ownership must not swallow file shortcuts, while the
    // field itself remains untouched. Text-specific edits use separate tests.
    QLineEdit field(&f.window);
    field.setText(QStringLiteral("unchanged"));
    field.show();
    field.setFocus();
    settle();
    f.modify();
    const auto choices = f.choices;
    const auto progressBefore = f.progressCalls;
    QTest::keyClick(&field, Qt::Key_S, Qt::ControlModifier);
    settle();
    CHECK(f.progressCalls > progressBefore);
    CHECK(f.choices == choices);
    CHECK(field.text() == QStringLiteral("unchanged"));
    CHECK(!f.session().document()->isModified());
    f.destination = f.files.filePath(QStringLiteral("shortcut-copy.vulkana"));
    QTest::keyClick(&field, Qt::Key_S, Qt::ControlModifier | Qt::ShiftModifier);
    settle();
    CHECK(f.choices == choices + 1);
    CHECK(f.window.projectPath() == f.destination);
    CHECK(QFileInfo::exists(f.destination));
    CHECK(field.text() == QStringLiteral("unchanged"));
    CHECK(f.errors.empty());
}

void progressCannotMutateDocumentThroughMoveOrUndo()
{
    Fixture f;
    const auto path = f.saveAs();
    f.show();
    auto* move = f.window.findChild<QAction*>(QStringLiteral("ToolAction_move"));
    CHECK(move);
    if (!move)
        return;
    move->trigger();
    f.modify();
    const auto id = f.session().document()->layers().front().id;
    CHECK(f.session().execute(
        std::make_unique<core::SetLayerOpacityCommand>(id, .5f)));
    CHECK(f.session().undo());
    move->trigger(); // Synchronize action enablement after test-only commands.
    settle();
    auto* x = f.window.findChild<QDoubleSpinBox*>(QStringLiteral("MoveXControl"));
    QAction* undo = nullptr;
    for (auto* action : f.window.findChildren<QAction*>())
        if (action->shortcuts().contains(QKeySequence(QStringLiteral("Ctrl+Z"))))
            undo = action;
    CHECK(x && undo);
    if (!x || !undo)
        return;
    CHECK(x->isEnabled());
    CHECK(undo->isEnabled());
    const auto original = f.session().document()->layer(id)->localToDocument;
    const auto opacity = f.session().document()->layer(id)->opacity;
    const State before(f);
    const auto originalX = x->value();
    bool attempted = false;
    f.duringProgress = [&] {
        if (attempted)
            return;
        attempted = true;
        // Model the native stepper/reentrant QAction paths, which are not
        // protected merely by filtering mouse/keyboard input to the canvas.
        x->setValue(originalX + 13);
        undo->trigger();
        QTest::keyClick(&f.window, Qt::Key_Z, Qt::ControlModifier);
        settle();
        before.verify(f);
        CHECK(f.session().document()->layer(id)->localToDocument == original);
        CHECK(f.session().document()->layer(id)->opacity == opacity);
    };
    const bool saved = f.window.saveDocument();
    f.duringProgress = {};
    CHECK(saved);
    CHECK(attempted);
    CHECK(f.errors.empty());
    CHECK(f.session().document()->contentState() == before.content);
    CHECK(f.session().document()->revision() == before.revision);
    CHECK(f.session().history().undoDepth() == before.undo);
    CHECK(f.session().history().redoDepth() == before.redo);
    CHECK(f.session().history().memoryUsed() == before.memory);
    CHECK(!f.session().document()->isModified());
    CHECK(f.session().document()->layer(id)->localToDocument == original);
    CHECK(x->value() == originalX);
    const auto loaded = ui::loadProject(path);
    CHECK(loaded);
    if (loaded) {
        CHECK(loaded.document->layers().front().localToDocument == original);
        CHECK(loaded.document->layers().front().visible == before.visible);
        CHECK(loaded.document->layers().front().opacity == opacity);
    }
}
void updateRestartGuards()
{
    Fixture first, second;
    first.window.show(); second.window.show();
    first.modify(); second.modify();
    bool launched=false;
    const auto launch=[&](const QString&,const QStringList&){launched=true;return false;};
    first.decision=ui::MainWindow::UnsavedChoice::Discard;
    second.decision=ui::MainWindow::UnsavedChoice::Cancel;
    CHECK(!first.window.restartAfterUpdate(QCoreApplication::applicationFilePath(),launch));
    CHECK(!launched&&first.window.isVisible()&&second.window.isVisible());
    CHECK(first.session().document()->isModified()&&second.session().document()->isModified());
    second.decision=ui::MainWindow::UnsavedChoice::Discard;
    CHECK(!first.window.restartAfterUpdate(QCoreApplication::applicationFilePath(),launch));
    CHECK(launched&&first.window.isVisible()&&second.window.isVisible());
    CHECK(first.session().document()->isModified()&&second.session().document()->isModified());
    CHECK(first.errors.size()==1&&first.errors.back().contains("restart"));
    namespace platform = imageeditor::platform;
    // QApplication consumes -platform before UI creation. Restart must retain
    // only that explicit choice, never turn a session-derived backend into one.
    platform::retainPlatformArguments(platform::inspectPlatformStartup(
        {"vulkana", "-platform", "xcb"}, QProcessEnvironment{}));
    QStringList arguments;
    const auto capture = [&](const QString&, const QStringList& args) { arguments = args; return false; };
    CHECK(!first.window.restartAfterUpdate(QCoreApplication::applicationFilePath(), capture));
    CHECK(arguments == QStringList({"-platform", "xcb", "--wait-for-instance-exit"}));
    platform::retainPlatformArguments(platform::inspectPlatformStartup({"vulkana"}, QProcessEnvironment{}));
    CHECK(!first.window.restartAfterUpdate(QCoreApplication::applicationFilePath(), capture));
    CHECK(arguments == QStringList({"--wait-for-instance-exit"}));
}

void externalLaunchesUseExistingWorkspace()
{
    Fixture f;
    f.show();
    const auto project = f.saveAs();
    f.modify();
    const auto original = f.window.activeDocumentId();
    const State before(f);
    const auto waitFor = [](const auto& condition) {
        QElapsedTimer timer; timer.start();
        while (!condition() && timer.elapsed() < 2000) QTest::qWait(10);
        CHECK(condition());
    };
    f.window.receiveExternalLaunch({});
    settle();
    CHECK(f.window.documentCount() == 1);
    before.verify(f);
    f.window.receiveExternalLaunch({f.imagePath, f.imagePath});
    waitFor([&] { return f.window.documentCount() == 3; });
    CHECK(f.asks == 0); // no discard prompt; original dirty tab is retained
    f.window.receiveExternalLaunch({project});
    waitFor([&] { return f.window.activeDocumentId() == original; });
    CHECK(f.window.documentCount() == 3); // canonical project path focuses instead of reloading
    before.verify(f);

    // Explicit New is not the startup chooser. A second launch must wait for
    // the user to finish it, not cancel their entered size choices.
    newDialog(f, [&](ui::NewDocumentDialog& dialog) {
        f.window.receiveExternalLaunch({f.imagePath});
        QTest::qWait(230);
        CHECK(dialog.isVisible());
        CHECK(f.window.documentCount() == 3);
        dialog.reject();
    });
    waitFor([&] { return f.window.documentCount() == 4; });

    // Startup New Canvas, in contrast, yields to a file-manager launch.
    bool timedOut = false;
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, [&] {
        timedOut = true;
        for (auto* dialog : f.window.findChildren<QDialog*>()) dialog->reject();
    });
    watchdog.start(2000);
    QTimer::singleShot(0, &f.window, [&] { f.window.receiveExternalLaunch({f.imagePath}); });
    f.window.showStartupDocument();
    watchdog.stop();
    CHECK(!timedOut);
    waitFor([&] { return f.window.documentCount() == 5; });
    CHECK(f.errors.empty());
    CHECK(f.window.activateDocument(original));
    before.verify(f);
}
} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QApplication::setStyle(QStringLiteral("Fusion"));
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir preferences;
    if (!preferences.isValid())
        return EXIT_FAILURE;
    QCoreApplication::setOrganizationName(
        QStringLiteral("ImageEditorLifecycleTests"));
    QCoreApplication::setApplicationName(QStringLiteral("ProjectLifecycle"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
        preferences.path());
    try {
        saveAsCancellationFailureAndAssociation();
        openingImageDoesNotTurnSaveIntoImageOverwrite();
        closeChoicesKeepOrFinishDocument();
        failedOpenAndCancelledOpenPreserveDocument();
        saveBeforeOpenMustSucceed();
        newDocumentGuardOccursOnlyAfterDialogAcceptance();
        recentMenuUsesTheSameUnsavedGuard();
        saveShortcutsReachPersistenceFromEditableFields();
        progressCannotMutateDocumentThroughMoveOrUndo();
        updateRestartGuards();
        externalLaunchesUseExistingWorkspace();
    } catch (const std::exception& error) {
        std::cerr << "Unexpected exception: " << error.what() << '\n';
        ++failures;
    }
    std::cout << (failures ? "Project lifecycle checks failed: "
                           : "Project lifecycle checks passed: ")
              << failures << '\n';
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
