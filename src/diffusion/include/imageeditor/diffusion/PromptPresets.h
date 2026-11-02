#pragma once

#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

namespace imageeditor::diffusion {

// Instructions describe intent; model capabilities and supplied images/masks
// determine which editing operations can actually be performed.
struct PromptPreset {
    QString id;
    QString name;
    QString task;
    QString description;
    QString promptTemplate;
    QString negativePromptTemplate;

    bool operator==(const PromptPreset &) const = default;
};

struct ExpandedPrompt {
    QString prompt;
    QString negativePrompt;

    bool operator==(const ExpandedPrompt &) const = default;
};

class PromptPresets {
public:
    static QVector<PromptPreset> builtIns();
    static QStringList supportedTasks();
    static QStringList supportedVariables();
    // Templates use {instruction}, {background}, {subject}, or {style}.
    // Double braces produce literal braces. Substitution is never recursive.
    static bool validate(const PromptPreset &preset, QString *error = nullptr);
    static QStringList requiredVariables(const PromptPreset &preset);
    static bool expand(const PromptPreset &preset, const QMap<QString, QString> &variables,
                       ExpandedPrompt *result, QString *error = nullptr);
};

class PromptPresetStore {
public:
    explicit PromptPresetStore(QString filePath);

    static QString defaultFilePath();
    const QString &filePath() const { return m_filePath; }
    const QVector<PromptPreset> &customPresets() const { return m_custom; }
    QVector<PromptPreset> allPresets() const;

    // A missing file is an empty custom collection. Invalid files leave the
    // current collection unchanged. Built-in presets cannot be overwritten.
    bool load(QString *error = nullptr);
    // Each mutation validates and atomically saves before updating memory.
    bool replaceCustom(const QVector<PromptPreset> &presets, QString *error = nullptr);
    bool upsert(const PromptPreset &preset, QString *error = nullptr);
    bool remove(const QString &id, QString *error = nullptr);

private:
    QString m_filePath;
    QVector<PromptPreset> m_custom;
};

} // namespace imageeditor::diffusion
