#include "imageeditor/ui/ColorDialog.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/EffectsPanel.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include <QApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QElapsedTimer>
#include <QPushButton>
#include <QSignalSpy>
#include <QSettings>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QTimer>
#include <QTest>
#include <functional>
#include <iostream>

// Runs on its own dbus-run-session bus, never the user's real portal service.
class Request final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Request")
public:
    int closed {};
public slots:
    void Close() { ++closed; }
};
class Portal final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Screenshot")
public:
    int calls {};
    QString path;
    Request request;
    void respond(uint result, bool malformed = false, QString responsePath = {}) {
        QDBusArgument rgb; rgb.beginStructure(); rgb << 0.2 << 0.4 << 0.8; rgb.endStructure();
        auto signal = QDBusMessage::createSignal(responsePath.isEmpty() ? path : responsePath,
            "org.freedesktop.portal.Request", "Response");
        signal << result << QVariantMap{{"color", malformed ? QVariant("invalid") : QVariant::fromValue(rgb)}};
        QDBusConnection::sessionBus().send(signal);
    }
public slots:
    QDBusObjectPath PickColor(const QString&, const QVariantMap& options, const QDBusMessage& message) {
        auto sender = message.service(); sender.remove(0, 1); sender.replace('.', '_');
        path = "/org/freedesktop/portal/desktop/request/" + sender + '/' + options.value("handle_token").toString();
        QDBusConnection::sessionBus().registerObject(path, &request, QDBusConnection::ExportAllSlots);
        ++calls;
        return QDBusObjectPath(path);
    }
};
namespace {
int failures {};
#define CHECK(c) do { if (!(c)) { ++failures; std::cerr << "Line " << __LINE__ << ": " #c "\n"; } } while(false)
bool waitFor(const std::function<bool()>& predicate) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < 1500) QTest::qWait(5);
    return predicate();
}
}
int main(int argc, char** argv) {
    // Avoid desktop theme initialization starting the real portal on the
    // isolated bus before this test registers its deterministic service.
    qputenv("QT_QPA_PLATFORMTHEME", "generic");
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    QCoreApplication::setOrganizationName("ImageEditorTests");
    QCoreApplication::setApplicationName("ColorPortal");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    imageeditor::ui::applyEditorTheme(app);
    auto bus = QDBusConnection::sessionBus();
    Portal portal;
    if (!bus.registerService("org.freedesktop.portal.Desktop")
        || !bus.registerObject("/org/freedesktop/portal/desktop", &portal, QDBusConnection::ExportAllSlots)) return 1;
    namespace u = imageeditor::ui;
    namespace c = imageeditor::core;
    u::MainWindow host(nullptr, false, false);
    host.setUnsavedPromptEnabled(false);
    host.resize(1100,800);
    host.show();
    QApplication::processEvents();
    auto* workspace = dynamic_cast<u::OverlayDockWorkspace*>(host.findChild<QWidget*>("CanvasWorkspace"));
    if (!workspace) return 1;
    const auto start = [&](u::ColorDialog& dialog) {
        auto* button = dialog.findChild<QPushButton*>("PickScreenColor");
        auto* shield = workspace->modalOverlay();
        CHECK(button && dialog.isVisible() && shield && shield->isVisible());
        if (!button || !shield) return;
        const auto previous = portal.calls;
        // Headless controls are disabled; invoke the real handoff on our isolated bus.
        QMetaObject::invokeMethod(button, "clicked", Qt::DirectConnection);
        CHECK(!dialog.isVisible() && !shield->isVisible());
        CHECK(waitFor([&] { return portal.calls == previous + 1; }));
        CHECK(!button->isEnabled() && !shield->isVisible());
    };

    u::ColorDialog dialog(QColor(10,20,30,91), &host);
    dialog.setOption(u::ColorDialog::ShowAlphaChannel);
    QSignalSpy finished(&dialog, &QDialog::finished);
    QSignalSpy selected(&dialog, &u::ColorDialog::colorSelected);
    dialog.show(); start(dialog); portal.respond(0);
    CHECK(waitFor([&] { return finished.count()==1; }));
    CHECK(!dialog.isVisible() && !workspace->modalOverlay() && selected.count()==1);
    CHECK(dialog.selectedColor()==QColor(51,102,204,91));
    dialog.show(); start(dialog); portal.respond(1);
    CHECK(waitFor([&] { return finished.count()==2; }));
    CHECK(dialog.result()==QDialog::Rejected && !dialog.isVisible() && !workspace->modalOverlay() && selected.count()==1);
    dialog.show(); start(dialog); portal.respond(0,true);
    CHECK(waitFor([&] { return dialog.isVisible(); }));
    CHECK(dialog.findChild<QPushButton*>("PickScreenColor")->isEnabled() && finished.count()==2);
    start(dialog); const auto obsolete=portal.path; dialog.reject();
    CHECK(waitFor([&] { return portal.request.closed==1; }));
    dialog.show(); start(dialog);
    portal.respond(0,false,obsolete); QTest::qWait(30);
    CHECK(!dialog.isVisible() && selected.count()==1 && finished.count()==3);
    portal.respond(0); CHECK(waitFor([&] { return finished.count()==4; }));
    dialog.show(); start(dialog); dialog.close();
    CHECK(waitFor([&] { return portal.request.closed==2; }));
    CHECK(!workspace->modalOverlay());
    dialog.show();
    const int previousCalls = portal.calls;
    QMetaObject::invokeMethod(dialog.findChild<QPushButton*>("PickScreenColor"), "clicked", Qt::DirectConnection);
    dialog.reject();
    QTest::qWait(50);
    CHECK(portal.calls==previousCalls && !workspace->modalOverlay());

    // Asynchronous working-color caller: successful picks publish immediately.
    auto* primary = host.findChild<QPushButton*>("ForegroundColorButton");
    if (!primary) return 1;
    primary->click();
    auto* working = host.findChild<u::ColorDialog*>("WorkingColorDialog");
    if (!working) return 1;
    start(*working); portal.respond(0);
    CHECK(waitFor([&] { return !workspace->modalOverlay(); }));
    CHECK((host.editorSession().colors().primary==c::Rgba8{51,102,204,255}));

    // Effects use a caller-owned presenter and a synchronous edit transaction.
    auto* effects = dynamic_cast<u::EffectsPanel*>(host.findChild<QWidget*>("EffectsPanelContent"));
    if (!effects || !host.editorSession().activeLayer()) return 1;
    const auto id = *host.editorSession().activeLayer();
    const auto depth = host.editorSession().history().undoDepth();
    const auto pickEffect = [&](auto openColor, bool cancel) {
        QTimer pick;
        pick.setSingleShot(true);
        QObject::connect(&pick,&QTimer::timeout,&host,[&]{
            auto* shield = workspace->modalOverlay();
            auto* effectDialog = shield ? shield->findChild<u::ColorDialog*>() : nullptr;
            if (!effectDialog) { CHECK(false); return; }
            effectDialog->setCurrentColor(Qt::red); // Exercise live preview and rollback.
            start(*effectDialog);
            portal.respond(cancel ? 1 : 0);
        });
        pick.start(0);
        openColor();
        CHECK(!workspace->modalOverlay());
    };
    const auto overlayColor = [&] { effects->onColorRequested(c::LayerEffectType::ColorOverlay,false); };
    pickEffect(overlayColor,false);
    const auto effectsAfter = host.editorSession().document()->layer(id)->effects;
    CHECK((effectsAfter && effectsAfter->items[std::size_t(c::LayerEffectType::ColorOverlay)].color==c::Rgba8{51,102,204,255}));
    CHECK(host.editorSession().history().undoDepth()==depth+1);
    pickEffect(overlayColor,true);
    CHECK(c::equivalentLayerEffects(host.editorSession().document()->layer(id)->effects,effectsAfter));
    CHECK(host.editorSession().history().undoDepth()==depth+1);
    pickEffect([&] { effects->onGradientColorRequested(1); },false);
    const auto gradient = host.editorSession().document()->layer(id)->effects;
    const auto stops = c::effectiveGradientStops(gradient->items[std::size_t(c::LayerEffectType::GradientOverlay)]);
    CHECK((stops.size()==2 && stops[1].color==c::Rgba8{51,102,204,255}));
    CHECK(host.editorSession().history().undoDepth()==depth+2);
    return failures ? 1 : 0;
}
#include "ColorPortalTests.moc"
