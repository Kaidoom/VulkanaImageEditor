#pragma once

#include "imageeditor/ui/MainWindow.hpp"
#include <QElapsedTimer>
#include <QTest>

namespace imageeditor::test {
// Match the application's startup sequence: native editor input and imports
// begin after the graphics gate has presented its first frame. Offscreen
// fixtures have no graphics gate and return immediately.
inline bool waitForGraphicsStartup(ui::MainWindow& window)
{
    QElapsedTimer deadline;
    deadline.start();
    while (!window.startupGraphicsReady() && deadline.elapsed() < 60000)
        QTest::qWait(10);
    return window.startupGraphicsReady();
}
}
