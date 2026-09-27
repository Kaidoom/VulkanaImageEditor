#include "imageeditor/ui/UiLayoutConfig.hpp"

#include <QFile>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextStream>
#include <QWidget>

#include <cstdlib>
#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

QString writeConfig(QTemporaryDir& directory, const QString& contents)
{
    const auto path = directory.filePath(QStringLiteral("ui-layout.ini"));
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly | QIODevice::Text));
    if (file.isOpen()) {
        QTextStream stream(&file);
        stream << contents;
    }
    return path;
}

void customPanelRangesLoadIndependently()
{
    QTemporaryDir directory;
    CHECK(directory.isValid());
    const auto path = writeConfig(directory, QStringLiteral(R"ini(
[ColorPanel]
minimumHeight=96
maximumHeight=360
[LayersPanel]
minimumHeight=284
maximumHeight=760
[PropertiesPanel]
minimumHeight=180
maximumHeight=0
)ini"));
    QStringList diagnostics;
    const auto config = imageeditor::ui::UiLayoutConfig::loadFromIni(
        path, &diagnostics);
    for (const auto& diagnostic : diagnostics) {
        std::cerr << "Unexpected config diagnostic: "
                  << diagnostic.toStdString() << '\n';
    }

    CHECK(config.color.minimum == 96);
    CHECK(config.color.maximum == 360);
    CHECK(config.layers.minimum == 284);
    CHECK(config.layers.maximum == 760);
    CHECK(config.properties.minimum == 180);
    CHECK(config.properties.maximum == QWIDGETSIZE_MAX);
    CHECK(diagnostics.empty());
}

void invalidRangesRemainResizable()
{
    QTemporaryDir directory;
    CHECK(directory.isValid());
    const auto path = writeConfig(directory, QStringLiteral(R"ini(
[ColorPanel]
minimumHeight=20
maximumHeight=10
[LayersPanel]
minimumHeight=300
maximumHeight=not-a-number
)ini"));
    QStringList diagnostics;
    const auto config = imageeditor::ui::UiLayoutConfig::loadFromIni(
        path, &diagnostics);

    CHECK(config.color.minimum == 80);
    CHECK(config.color.maximum == config.color.minimum + 1);
    CHECK(config.layers.minimum == 300);
    CHECK(config.layers.maximum == QWIDGETSIZE_MAX);
    CHECK(diagnostics.size() >= 3);
}

} // namespace

int main()
{
    customPanelRangesLoadIndependently();
    invalidRangesRemainResizable();
    if (failures != 0) {
        std::cerr << failures << " UI layout config assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All UI layout config tests passed\n";
    return EXIT_SUCCESS;
}
