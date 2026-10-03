#include "imageeditor/platform/AvailableMemory.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/MemoryStatusLabel.hpp"
#include <QApplication>
#include <QHBoxLayout>
#include <QStatusBar>
#include <QTest>
#include <iostream>

namespace u = imageeditor::ui;
int failures{};
#define CHECK(x) do { if (!(x)) { ++failures; std::cerr << __LINE__ << ": " #x "\n"; } } while (false)
struct PaintCounter : QObject {
    int paints{};
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::Paint) ++paints;
        return false;
    }
};
int main(int argc, char** argv) {
    QApplication app(argc, argv);
#ifdef Q_OS_LINUX
    const auto resident = imageeditor::platform::processResidentMemoryBytes();
    CHECK(resident && *resident > 0);
#endif
    constexpr std::uint64_t MiB = 1024ULL * 1024;
    int samples = 0;
    u::MemoryStatusSnapshot value{512 * MiB, 3072 * MiB, 64 * MiB};
    QWidget host;
    auto* layout = new QHBoxLayout(&host);
    auto* memory = new u::MemoryStatusLabel([&] { ++samples; return value; }, &host);
    layout->addWidget(memory);
    CHECK(samples == 0);
    host.resize(500, 45);
    host.show();
    QTest::qWait(30);
    CHECK(samples == 1);
    CHECK(memory->text() == "RAM 512.0 MiB · GPU cache 3.0 GiB");
    CHECK(memory->toolTip().contains("not total VRAM"));
    CHECK(memory->toolTip().contains("64.0 MiB"));
    CHECK(memory->focusPolicy() == Qt::NoFocus);
    const auto hint = memory->sizeHint();
    value.residentBytes = 1536 * MiB;
    QTest::qWait(1200);
    CHECK(samples >= 2 && samples <= 3);
    CHECK(memory->text().startsWith("RAM 1.5 GiB"));
    CHECK(memory->sizeHint() == hint);
    PaintCounter counter;
    memory->installEventFilter(&counter);
    QTest::qWait(1200);
    CHECK(counter.paints == 0); // Unchanged samples do not repaint.
    memory->removeEventFilter(&counter);
    host.hide();
    const auto hiddenSamples = samples;
    QTest::qWait(1200);
    CHECK(samples == hiddenSamples);
    value.residentBytes.reset();
    host.show();
    QCoreApplication::processEvents();
    CHECK(memory->text().startsWith("RAM unavailable"));
    host.resize(130, 45);
    QCoreApplication::processEvents();
    CHECK(memory->width() < hint.width()); // Narrow bars can elide, not grow the window.
    host.hide();

    u::MainWindow window(nullptr, false);
    window.setUnsavedPromptEnabled(false);
    window.resize(1300, 850);
    window.show();
    QCoreApplication::processEvents();
    auto* status = window.findChild<QLabel*>("MemoryStatus");
    CHECK(status && status->isVisible());
    CHECK(status && status->text().startsWith("RAM "));
    const auto* document = window.editorSession().document();
    const auto revision = document ? document->revision() : 0;
    const auto undo = window.editorSession().history().undoDepth();
    window.statusBar()->showMessage("Status message must not hide memory", 3000);
    QTest::qWait(1200);
    CHECK(status && status->isVisible());
    CHECK(!document || document->revision() == revision);
    CHECK(window.editorSession().history().undoDepth() == undo);
    std::cout << "Memory status failures: " << failures << '\n';
    return failures ? 1 : 0;
}
