#include "imageeditor/diffusion/PromptPresets.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

#include <utility>

namespace imageeditor::diffusion {
namespace {
constexpr qint64 maximumStoreBytes = 8 * 1024 * 1024;
constexpr qsizetype maximumPromptCharacters = 65536;

bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}

bool parseTemplate(const QString &text, const QMap<QString, QString> *variables,
                   QString *expanded, QStringList *required, QString *error)
{
    QString output;
    const auto supported = PromptPresets::supportedVariables();
    for (qsizetype i = 0; i < text.size();) {
        const QChar ch = text.at(i);
        if (ch != u'{' && ch != u'}') {
            output += ch;
            ++i;
            continue;
        }
        if (i + 1 < text.size() && text.at(i + 1) == ch) {
            output += ch;
            i += 2;
            continue;
        }
        if (ch == u'}') return fail(error, QStringLiteral("Unmatched '}' in prompt template; use '}}' for a literal brace."));
        const qsizetype end = text.indexOf(u'}', i + 1);
        if (end < 0) return fail(error, QStringLiteral("Unmatched '{' in prompt template; use '{{' for a literal brace."));
        const QString key = text.mid(i + 1, end - i - 1);
        if (!supported.contains(key)) {
            return fail(error, QStringLiteral("Unknown prompt variable '%1'. Supported variables: %2.")
                                   .arg(key, supported.join(QStringLiteral(", "))));
        }
        if (required && !required->contains(key)) required->append(key);
        if (variables) {
            const auto it = variables->constFind(key);
            if (it == variables->cend() || it.value().trimmed().isEmpty()) {
                return fail(error, QStringLiteral("Provide a value for prompt variable '%1'.").arg(key));
            }
            if (it.value().size() > maximumPromptCharacters || it.value().contains(QChar::Null)) {
                return fail(error, QStringLiteral("Prompt variable '%1' is too long or contains a null character.").arg(key));
            }
            output += it.value();
        }
        i = end + 1;
        if (output.size() > maximumPromptCharacters) return fail(error, QStringLiteral("Expanded prompt exceeds 65,536 characters."));
    }
    if (output.size() > maximumPromptCharacters) return fail(error, QStringLiteral("Expanded prompt exceeds 65,536 characters."));
    if (expanded) *expanded = output;
    return true;
}

bool validateCollection(const QVector<PromptPreset> &presets, QString *error)
{
    QSet<QString> ids;
    QSet<QString> names;
    for (const auto &preset : PromptPresets::builtIns()) {
        ids.insert(preset.id);
        names.insert(preset.name.toCaseFolded());
    }
    for (const auto &preset : presets) {
        if (!PromptPresets::validate(preset, error)) return false;
        if (ids.contains(preset.id)) return fail(error, QStringLiteral("Preset ID '%1' is already used. Built-in presets cannot be overwritten.").arg(preset.id));
        if (names.contains(preset.name.toCaseFolded())) return fail(error, QStringLiteral("Preset name '%1' is already used.").arg(preset.name));
        ids.insert(preset.id);
        names.insert(preset.name.toCaseFolded());
    }
    return true;
}

QJsonObject toJson(const PromptPreset &preset)
{
    return {{QStringLiteral("id"), preset.id}, {QStringLiteral("name"), preset.name},
            {QStringLiteral("task"), preset.task}, {QStringLiteral("description"), preset.description},
            {QStringLiteral("promptTemplate"), preset.promptTemplate},
            {QStringLiteral("negativePromptTemplate"), preset.negativePromptTemplate}};
}

bool fromJson(const QJsonValue &value, PromptPreset *preset, QString *error)
{
    if (!value.isObject()) return fail(error, QStringLiteral("Each custom preset must be an object."));
    const auto obj = value.toObject();
    const QStringList keys{QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("task"),
                           QStringLiteral("description"), QStringLiteral("promptTemplate"),
                           QStringLiteral("negativePromptTemplate")};
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (!keys.contains(it.key())) return fail(error, QStringLiteral("Unknown preset field '%1'.").arg(it.key()));
    }
    for (const auto &key : keys) {
        if (!obj.value(key).isString()) return fail(error, QStringLiteral("Preset field '%1' must be a string.").arg(key));
    }
    *preset = {obj.value(keys[0]).toString(), obj.value(keys[1]).toString(), obj.value(keys[2]).toString(),
               obj.value(keys[3]).toString(), obj.value(keys[4]).toString(), obj.value(keys[5]).toString()};
    return true;
}
} // namespace

QVector<PromptPreset> PromptPresets::builtIns()
{
    return {
        {QStringLiteral("builtin.generate"), QStringLiteral("Generate image"), QStringLiteral("generate"),
         QStringLiteral("Describe the image to generate."), QStringLiteral("{instruction}"), {}},
        {QStringLiteral("builtin.fill"), QStringLiteral("Generative fill"), QStringLiteral("fill"),
         QStringLiteral("An editing instruction for a selected region. Requires image and mask support; the prompt alone does not restrict edits."),
         QStringLiteral("Replace the selected area with {instruction}. Preserve the rest of the image, including its lighting and composition."), {}},
        {QStringLiteral("builtin.background-replace"), QStringLiteral("Replace background"), QStringLiteral("background-replace"),
         QStringLiteral("Requires a reference image. Use a mask to protect the subject when the model supports masked editing."),
         QStringLiteral("Replace the background with {background}. Keep the foreground subject unchanged, preserving its pose, fine details and proportions. Match the lighting naturally."), {}},
        {QStringLiteral("builtin.background-remove"), QStringLiteral("Remove background"), QStringLiteral("background-remove"),
         QStringLiteral("Requires a reference image and a model that can return transparency. An instruction is not a segmentation mask."),
         QStringLiteral("Remove the background while keeping the subject, its fine details, pose and colors unchanged. Leave the background transparent."), {}}
    };
}

QStringList PromptPresets::supportedTasks()
{
    return {QStringLiteral("generate"), QStringLiteral("edit"), QStringLiteral("fill"),
            QStringLiteral("background-replace"), QStringLiteral("background-remove")};
}

QStringList PromptPresets::supportedVariables()
{
    return {QStringLiteral("instruction"), QStringLiteral("background"), QStringLiteral("subject"), QStringLiteral("style")};
}

bool PromptPresets::validate(const PromptPreset &preset, QString *error)
{
    if (error) error->clear();
    static const QRegularExpression idPattern(QStringLiteral("\\A[A-Za-z0-9][A-Za-z0-9._-]{0,127}\\z"));
    if (!idPattern.match(preset.id).hasMatch()) return fail(error, QStringLiteral("Preset ID must contain 1–128 letters, digits, dots, underscores or hyphens, starting with a letter or digit."));
    if (preset.name.trimmed().isEmpty() || preset.name != preset.name.trimmed() || preset.name.size() > 256 || preset.name.contains(QChar::Null)) {
        return fail(error, QStringLiteral("Preset name must contain 1–256 characters without leading/trailing spaces or null characters."));
    }
    if (!supportedTasks().contains(preset.task)) return fail(error, QStringLiteral("Unknown preset task '%1'. Valid tasks: %2.").arg(preset.task, supportedTasks().join(QStringLiteral(", "))));
    if (preset.description.size() > 4096 || preset.description.contains(QChar::Null)) return fail(error, QStringLiteral("Preset description is too long or contains a null character."));
    if (preset.promptTemplate.trimmed().isEmpty()) return fail(error, QStringLiteral("A prompt template is required."));
    for (const auto &text : {preset.promptTemplate, preset.negativePromptTemplate}) {
        if (text.size() > maximumPromptCharacters || text.contains(QChar::Null)) return fail(error, QStringLiteral("Prompt templates must contain at most 65,536 characters and no null characters."));
        if (!parseTemplate(text, nullptr, nullptr, nullptr, error)) return false;
    }
    return true;
}

QStringList PromptPresets::requiredVariables(const PromptPreset &preset)
{
    QStringList required;
    if (!validate(preset)) return required;
    parseTemplate(preset.promptTemplate, nullptr, nullptr, &required, nullptr);
    parseTemplate(preset.negativePromptTemplate, nullptr, nullptr, &required, nullptr);
    return required;
}

bool PromptPresets::expand(const PromptPreset &preset, const QMap<QString, QString> &variables,
                           ExpandedPrompt *result, QString *error)
{
    if (!result) return fail(error, QStringLiteral("An expanded-prompt destination is required."));
    if (!validate(preset, error)) return false;
    for (auto it = variables.begin(); it != variables.end(); ++it) {
        if (!supportedVariables().contains(it.key())) return fail(error, QStringLiteral("Unknown prompt variable '%1'.").arg(it.key()));
    }
    ExpandedPrompt expanded;
    if (!parseTemplate(preset.promptTemplate, &variables, &expanded.prompt, nullptr, error)
        || !parseTemplate(preset.negativePromptTemplate, &variables, &expanded.negativePrompt, nullptr, error)) return false;
    *result = std::move(expanded);
    return true;
}

PromptPresetStore::PromptPresetStore(QString filePath) : m_filePath(std::move(filePath)) {}

QString PromptPresetStore::defaultFilePath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("diffusion/prompt-presets.json"));
}

QVector<PromptPreset> PromptPresetStore::allPresets() const
{
    auto result = PromptPresets::builtIns();
    result += m_custom;
    return result;
}

bool PromptPresetStore::load(QString *error)
{
    if (error) error->clear();
    if (m_filePath.isEmpty()) return fail(error, QStringLiteral("A prompt preset file path is required."));
    QFile file(m_filePath);
    if (!file.exists()) {
        m_custom.clear();
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) return fail(error, QStringLiteral("Cannot read prompt presets: %1").arg(file.errorString()));
    if (file.size() > maximumStoreBytes) return fail(error, QStringLiteral("Prompt preset file exceeds 8 MiB."));
    const auto bytes = file.read(maximumStoreBytes + 1);
    if (file.error() != QFileDevice::NoError) return fail(error, QStringLiteral("Cannot read prompt presets: %1").arg(file.errorString()));
    if (bytes.size() > maximumStoreBytes) return fail(error, QStringLiteral("Prompt preset file exceeds 8 MiB."));
    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) return fail(error, QStringLiteral("Invalid prompt preset JSON: %1").arg(parseError.errorString()));
    const auto root = doc.object();
    if (root.size() != 2 || !root.contains(QStringLiteral("version")) || !root.contains(QStringLiteral("presets"))) {
        return fail(error, QStringLiteral("Prompt preset file must contain only 'version' and 'presets'."));
    }
    if (!root.value(QStringLiteral("version")).isDouble() || root.value(QStringLiteral("version")).toDouble() != 1.0) return fail(error, QStringLiteral("Unsupported prompt preset format version; expected 1."));
    if (!root.value(QStringLiteral("presets")).isArray()) return fail(error, QStringLiteral("Prompt preset 'presets' field must be an array."));
    QVector<PromptPreset> presets;
    for (const auto &value : root.value(QStringLiteral("presets")).toArray()) {
        PromptPreset preset;
        if (!fromJson(value, &preset, error)) return false;
        presets.append(std::move(preset));
    }
    if (!validateCollection(presets, error)) return false;
    m_custom = std::move(presets);
    return true;
}

bool PromptPresetStore::replaceCustom(const QVector<PromptPreset> &presets, QString *error)
{
    if (error) error->clear();
    if (!validateCollection(presets, error)) return false;
    if (m_filePath.isEmpty()) return fail(error, QStringLiteral("A prompt preset file path is required."));
    QJsonArray array;
    for (const auto &preset : presets) array.append(toJson(preset));
    const QByteArray bytes = QJsonDocument(QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("presets"), array}}).toJson(QJsonDocument::Indented);
    if (bytes.size() > maximumStoreBytes) return fail(error, QStringLiteral("Prompt preset collection exceeds 8 MiB."));
    const auto directory = QFileInfo(m_filePath).absoluteDir();
    if (!QDir().mkpath(directory.absolutePath())) return fail(error, QStringLiteral("Cannot create the prompt preset directory."));
    QSaveFile file(m_filePath);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) return fail(error, QStringLiteral("Cannot save prompt presets: %1").arg(file.errorString()));
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) return fail(error, QStringLiteral("Cannot set prompt preset file permissions: %1").arg(file.errorString()));
    if (file.write(bytes) != bytes.size() || !file.commit()) return fail(error, QStringLiteral("Cannot save prompt presets: %1").arg(file.errorString()));
    m_custom = presets;
    return true;
}

bool PromptPresetStore::upsert(const PromptPreset &preset, QString *error)
{
    auto updated = m_custom;
    bool found = false;
    for (auto &existing : updated) {
        if (existing.id == preset.id) {
            existing = preset;
            found = true;
            break;
        }
    }
    if (!found) updated.append(preset);
    return replaceCustom(updated, error);
}

bool PromptPresetStore::remove(const QString &id, QString *error)
{
    auto updated = m_custom;
    for (auto it = updated.begin(); it != updated.end(); ++it) {
        if (it->id == id) {
            updated.erase(it);
            return replaceCustom(updated, error);
        }
    }
    return fail(error, QStringLiteral("Custom prompt preset '%1' does not exist. Built-in presets cannot be removed.").arg(id));
}

} // namespace imageeditor::diffusion
