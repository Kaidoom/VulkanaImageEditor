#include "imageeditor/ui/ColorDialog.hpp"
#include "imageeditor/ui/Theme.hpp"
#include <QApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QElapsedTimer>
#include <QPushButton>
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
    imageeditor::ui::applyEditorTheme(app);
    auto bus = QDBusConnection::sessionBus();
    Portal portal;
    if (!bus.registerService("org.freedesktop.portal.Desktop")
        || !bus.registerObject("/org/freedesktop/portal/desktop", &portal, QDBusConnection::ExportAllSlots)) return 1;
    imageeditor::ui::ColorDialog dialog(QColor(10, 20, 30, 91));
    dialog.setOption(imageeditor::ui::ColorDialog::ShowAlphaChannel);
    dialog.show();
    auto* button = dialog.findChild<QPushButton*>("PickScreenColor");
    const auto start = [&] {
        const auto previous = portal.calls;
        // Headless screen buttons are intentionally disabled; exercise the same
        // asynchronous request handler without requiring a desktop input grab.
        QMetaObject::invokeMethod(button, "clicked", Qt::DirectConnection);
        CHECK(waitFor([&] { return portal.calls == previous + 1; }));
        CHECK(!button->isEnabled());
    };
    start(); portal.respond(0);
    CHECK(waitFor([&] { return button->isEnabled(); }));
    CHECK(dialog.currentColor() == QColor(51, 102, 204, 91));
    start(); portal.respond(1);
    CHECK(waitFor([&] { return button->isEnabled(); }));
    CHECK(dialog.currentColor() == QColor(51, 102, 204, 91));
    start(); portal.respond(0, true);
    CHECK(waitFor([&] { return button->isEnabled(); }));
    CHECK(dialog.currentColor() == QColor(51, 102, 204, 91));
    start(); const auto obsolete = portal.path; dialog.reject();
    CHECK(waitFor([&] { return portal.request.closed == 1; }));
    dialog.show(); start();
    dialog.setCurrentColor(QColor(80, 90, 100, 91));
    portal.respond(0, false, obsolete); QTest::qWait(30);
    CHECK(dialog.currentColor() == QColor(80, 90, 100, 91) && !button->isEnabled());
    portal.respond(0); CHECK(waitFor([&] { return button->isEnabled(); }));
    CHECK(dialog.currentColor() == QColor(51, 102, 204, 91));
    start(); dialog.close(); CHECK(waitFor([&] { return portal.request.closed == 2; }));
    if (!failures) std::cout << "Color portal: asynchronous sample, cancel, invalid response, close, and stale-result isolation passed\n";
    return failures ? 1 : 0;
}
#include "ColorPortalTests.moc"
