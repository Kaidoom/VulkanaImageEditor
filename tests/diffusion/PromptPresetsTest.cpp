#include "imageeditor/diffusion/PromptPresets.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

using namespace imageeditor::diffusion;

namespace {
PromptPreset customPreset(const QString &id = QStringLiteral("custom.portrait"))
{
    return {id, QStringLiteral("Portrait — صورة"), QStringLiteral("generate"),
            QStringLiteral("Portrait with a chosen style"),
            QStringLiteral("A portrait of {subject}, {style}. {instruction}"),
            QStringLiteral("Unwanted background: {background}")};
}

bool writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}

QByteArray readBytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}
} // namespace

class PromptPresetsTest : public QObject {
    Q_OBJECT
private slots:
    void builtInsExpandWithoutExecution();
    void invalidTemplatesAndMissingVariablesPreserveOutput();
    void customRoundTripAndMutation();
    void duplicateAndBuiltInProtection();
    void failedSavePreservesMemory();
    void invalidFilePreservesMemory_data();
    void invalidFilePreservesMemory();
};

void PromptPresetsTest::builtInsExpandWithoutExecution()
{
    const auto builtIns = PromptPresets::builtIns();
    QCOMPARE(builtIns.size(), 4);
    QString error;
    for (const auto &preset : builtIns) {
        QVERIFY2(PromptPresets::validate(preset, &error), qPrintable(error));
        ExpandedPrompt result;
        QVERIFY2(PromptPresets::expand(preset, {{QStringLiteral("instruction"), QStringLiteral("a blue teapot")},
                                              {QStringLiteral("background"), QStringLiteral("a garden")}}, &result, &error), qPrintable(error));
        QVERIFY(!result.prompt.isEmpty());
        QVERIFY(result.negativePrompt.isEmpty());
    }
    auto preset = customPreset();
    preset.promptTemplate = QStringLiteral("{{literal}} {instruction} {instruction}");
    preset.negativePromptTemplate.clear();
    QCOMPARE(PromptPresets::requiredVariables(preset), QStringList{QStringLiteral("instruction")});
    ExpandedPrompt result;
    const QString value = QStringLiteral("{background}; $(touch never-run)");
    QVERIFY2(PromptPresets::expand(preset, {{QStringLiteral("instruction"), value}}, &result, &error), qPrintable(error));
    QCOMPARE(result.prompt, QStringLiteral("{literal} ") + value + u' ' + value);
}

void PromptPresetsTest::invalidTemplatesAndMissingVariablesPreserveOutput()
{
    auto preset = customPreset();
    QString error;
    ExpandedPrompt result{QStringLiteral("untouched"), QStringLiteral("untouched negative")};
    const ExpandedPrompt original = result;
    QVERIFY(!PromptPresets::expand(preset, {}, &result, &error));
    QVERIFY(error.contains(QStringLiteral("subject")));
    QVERIFY(result == original);
    for (const auto &invalid : {QStringLiteral("{unknown}"), QStringLiteral("bad {"), QStringLiteral("bad }"), QStringLiteral("{}")}) {
        preset.promptTemplate = invalid;
        QVERIFY(!PromptPresets::validate(preset, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!PromptPresets::expand(preset, {}, &result, &error));
        QVERIFY(result == original);
    }
    preset = customPreset();
    preset.task = QStringLiteral("execute");
    QVERIFY(!PromptPresets::validate(preset, &error));
    QVERIFY(error.contains(QStringLiteral("Valid tasks")));
    preset = customPreset();
    preset.id = QStringLiteral("../invalid");
    QVERIFY(!PromptPresets::validate(preset, &error));
    preset = customPreset();
    preset.promptTemplate = QStringLiteral("{instruction}{instruction}");
    preset.negativePromptTemplate.clear();
    QVERIFY(!PromptPresets::expand(preset, {{QStringLiteral("instruction"), QString(40000, u'x')}}, &result, &error));
    QVERIFY(error.contains(QStringLiteral("65,536")));
    QVERIFY(result == original);
}

void PromptPresetsTest::customRoundTripAndMutation()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = dir.filePath(QStringLiteral("nested/presets.json"));
    PromptPresetStore store(path);
    QString error;
    QVERIFY2(store.load(&error), qPrintable(error));
    QCOMPARE(store.allPresets().size(), 4);
    auto preset = customPreset();
    QVERIFY2(store.upsert(preset, &error), qPrintable(error));
    QCOMPARE(store.customPresets().size(), 1);
    QCOMPARE(store.allPresets().size(), 5);
    PromptPresetStore reopened(path);
    QVERIFY2(reopened.load(&error), qPrintable(error));
    QVERIFY(reopened.customPresets() == store.customPresets());
    preset.promptTemplate = QStringLiteral("Updated {instruction}");
    QVERIFY2(reopened.upsert(preset, &error), qPrintable(error));
    QCOMPARE(reopened.customPresets().size(), 1);
    QCOMPARE(reopened.customPresets().front().promptTemplate, preset.promptTemplate);
    QVERIFY2(reopened.remove(preset.id, &error), qPrintable(error));
    QVERIFY(reopened.customPresets().isEmpty());
    QVERIFY2(store.load(&error), qPrintable(error));
    QVERIFY(store.customPresets().isEmpty());
}

void PromptPresetsTest::duplicateAndBuiltInProtection()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = dir.filePath(QStringLiteral("presets.json"));
    PromptPresetStore store(path);
    QString error;
    const auto preset = customPreset();
    QVERIFY2(store.upsert(preset, &error), qPrintable(error));
    const auto bytes = readBytes(path);
    QVERIFY(!store.replaceCustom({preset, preset}, &error));
    QVERIFY(error.contains(QStringLiteral("ID")));
    auto duplicateName = customPreset(QStringLiteral("different.id"));
    duplicateName.name = preset.name.toUpper();
    QVERIFY(!store.replaceCustom({preset, duplicateName}, &error));
    QVERIFY(error.contains(QStringLiteral("name")));
    const auto builtin = PromptPresets::builtIns().front();
    QVERIFY(!store.upsert(builtin, &error));
    QVERIFY(!store.remove(builtin.id, &error));
    QCOMPARE(readBytes(path), bytes);
    QCOMPARE(store.customPresets().size(), 1);
    QVERIFY(store.customPresets().front() == preset);
}

void PromptPresetsTest::failedSavePreservesMemory()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = dir.filePath(QStringLiteral("presets.json"));
    PromptPresetStore store(path);
    QString error;
    const auto preset = customPreset();
    QVERIFY2(store.upsert(preset, &error), qPrintable(error));
    QVERIFY(QFile::remove(path));
    QVERIFY(QDir().mkdir(path)); // A directory at the target makes failure deterministic, even as root.
    auto changed = preset;
    changed.name = QStringLiteral("Replacement");
    QVERIFY(!store.upsert(changed, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(store.customPresets().size(), 1);
    QVERIFY(store.customPresets().front() == preset);
}

void PromptPresetsTest::invalidFilePreservesMemory_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::newRow("broken JSON") << QByteArray("{broken");
    QTest::newRow("wrong root") << QByteArray("[]");
    QTest::newRow("future version") << QByteArray(R"({"version":2,"presets":[]})");
    QTest::newRow("fractional version") << QByteArray(R"({"version":1.5,"presets":[]})");
    QTest::newRow("unknown root key") << QByteArray(R"({"version":1,"presets":[],"models":[]})");
    QTest::newRow("wrong collection type") << QByteArray(R"({"version":1,"presets":"bad"})");
    QTest::newRow("missing preset fields") << QByteArray(R"({"version":1,"presets":[{"id":"test"}]})");
    QTest::newRow("nonobject preset") << QByteArray(R"({"version":1,"presets":[null]})");
    QTest::newRow("unknown preset field") << QByteArray(R"({"version":1,"presets":[{"id":"test","name":"Test","task":"generate","description":"","promptTemplate":"Hello","negativePromptTemplate":"","model":"not-allowed"}]})");
}

void PromptPresetsTest::invalidFilePreservesMemory()
{
    QFETCH(QByteArray, bytes);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = dir.filePath(QStringLiteral("presets.json"));
    PromptPresetStore store(path);
    QString error;
    const auto preset = customPreset();
    QVERIFY2(store.upsert(preset, &error), qPrintable(error));
    QVERIFY(writeBytes(path, bytes));
    QVERIFY(!store.load(&error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(store.customPresets().size(), 1);
    QVERIFY(store.customPresets().front() == preset);
    QCOMPARE(readBytes(path), bytes);
}

QTEST_GUILESS_MAIN(PromptPresetsTest)
#include "PromptPresetsTest.moc"
