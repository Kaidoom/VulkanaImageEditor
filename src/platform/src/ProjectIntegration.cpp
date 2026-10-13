#include "imageeditor/platform/ProjectIntegration.hpp"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <vector>
#ifdef Q_OS_WIN
#include <windows.h>
#include <shlobj.h>
#endif

namespace imageeditor::platform {
bool projectIntegrationAvailable(const QString& executable)
{
#ifdef Q_OS_WIN
    const auto bin = QFileInfo(executable).absoluteDir();
    return QFileInfo(executable).isFile()
        && QFileInfo(bin.filePath(QStringLiteral("vulkana-thumbnail-provider.dll"))).isFile()
        && QFileInfo(bin.filePath(QStringLiteral("vulkana-thumbnail-codec.dll"))).isFile();
#else
    Q_UNUSED(executable);
    return false;
#endif
}
#ifdef Q_OS_WIN
namespace {
const QString snapshotKey = QStringLiteral("integration/windows/projectFilesRegistration");
const QString clsid = QStringLiteral("{4EE78B14-4465-40F5-BC5E-C4F096A51B7F}");
const QString handler = QStringLiteral("\\shellex\\{E357FCCD-A995-4576-B01F-234630154E96}");
const QString progId = QStringLiteral("Vulkana.Project");
struct Entry { QString path, name; QJsonObject before, after; };
QStringList allowedPaths()
{
    return {progId, progId + "\\DefaultIcon", progId + "\\shell\\open\\command", progId + "\\Application", progId + handler,
        ".vulkana", ".vulkana\\OpenWithProgids", ".vulkana" + handler,
        "CLSID\\" + clsid, "CLSID\\" + clsid + "\\InprocServer32"};
}
QString registryPath(const QString& path) { return QStringLiteral("Software\\Classes\\") + path; }
QJsonObject value(const QString& text)
{
    const QByteArray bytes(reinterpret_cast<const char*>(text.utf16()), int((text.size() + 1) * sizeof(wchar_t)));
    return {{"present", true}, {"type", int(REG_SZ)}, {"data", QString::fromLatin1(bytes.toBase64())}};
}
LSTATUS read(const QString& path, const QString& name, QJsonObject& result, HKEY hive = HKEY_CURRENT_USER)
{
    result = {{"present", false}};
    HKEY key {};
    const auto full = registryPath(path).toStdWString();
    auto status = RegOpenKeyExW(hive, full.c_str(), 0, KEY_QUERY_VALUE, &key);
    if (status == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    if (status) return status;
    const auto field = name.toStdWString();
    DWORD type {}, size {};
    status = RegQueryValueExW(key, field.c_str(), nullptr, &type, nullptr, &size);
    if (status == ERROR_FILE_NOT_FOUND) { RegCloseKey(key); return ERROR_SUCCESS; }
    if (status || size > 65536) { RegCloseKey(key); return status ? status : ERROR_INVALID_DATA; }
    QByteArray bytes(qsizetype(size), Qt::Uninitialized);
    status = RegQueryValueExW(key, field.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(bytes.data()), &size);
    RegCloseKey(key);
    if (!status) result = {{"present", true}, {"type", int(type)}, {"data", QString::fromLatin1(bytes.toBase64())}};
    return status;
}
LSTATUS write(const QString& path, const QString& name, const QJsonObject& data)
{
    HKEY key {};
    const auto full = registryPath(path).toStdWString(), field = name.toStdWString();
    const bool present = data["present"].toBool();
    auto status = present ? RegCreateKeyExW(HKEY_CURRENT_USER, full.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr)
                          : RegOpenKeyExW(HKEY_CURRENT_USER, full.c_str(), 0, KEY_SET_VALUE, &key);
    if (!present && status == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    if (status) return status;
    const auto bytes = QByteArray::fromBase64(data["data"].toString().toLatin1());
    status = present ? RegSetValueExW(key, field.c_str(), 0, DWORD(data["type"].toInt()),
                         reinterpret_cast<const BYTE*>(bytes.constData()), DWORD(bytes.size()))
                     : RegDeleteValueW(key, field.c_str());
    RegCloseKey(key);
    return status == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : status;
}
std::vector<Entry> snapshot(QSettings& settings)
{
    std::vector<Entry> entries;
    const auto allowed = allowedPaths();
    for (const auto& item : QJsonDocument::fromJson(settings.value(snapshotKey).toByteArray()).array()) {
        const auto data = item.toObject();
        const auto path = data["path"].toString(), name = data["name"].toString();
        if (!allowed.contains(path) || (!name.isEmpty() && name != progId && name != "ThreadingModel" && name != "ApplicationName")) continue;
        entries.push_back({path, name, data["before"].toObject(), data["after"].toObject()});
    }
    return entries;
}
QByteArray serialize(const std::vector<Entry>& entries)
{
    QJsonArray data;
    for (const auto& entry : entries) data.append(QJsonObject{{"path", entry.path}, {"name", entry.name},
        {"before", entry.before}, {"after", entry.after}});
    return QJsonDocument(data).toJson(QJsonDocument::Compact);
}
void notify() { SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr); }
}
#endif
QString registerProjectFiles(QSettings& settings, const QString& executable)
{
#ifdef Q_OS_WIN
    if (!projectIntegrationAvailable(executable)) return QStringLiteral("The portable folder is missing its project-thumbnail components.");
    const auto previous = snapshot(settings);
    const auto exe = QDir::toNativeSeparators(QFileInfo(executable).absoluteFilePath());
    const auto dll = QDir::toNativeSeparators(QFileInfo(executable).absoluteDir().filePath("vulkana-thumbnail-provider.dll"));
    if (exe.contains('"') || dll.contains('"')) return QStringLiteral("Invalid application path.");
    std::vector<Entry> entries;
    auto add = [&](const QString& path, const QString& name, const QString& text) { entries.push_back({path, name, {}, value(text)}); };
    add(progId, {}, "Vulkana project");
    add(progId + "\\Application", "ApplicationName", "Vulkana Image Editor");
    add(progId + "\\DefaultIcon", {}, '"' + exe + "\",0");
    add(progId + "\\shell\\open\\command", {}, '"' + exe + "\" \"%1\"");
    add(progId + handler, {}, clsid);
    add(".vulkana\\OpenWithProgids", progId, "");
    add(".vulkana" + handler, {}, clsid);
    add("CLSID\\" + clsid, {}, "Vulkana project thumbnail");
    add("CLSID\\" + clsid + "\\InprocServer32", {}, dll);
    add("CLSID\\" + clsid + "\\InprocServer32", "ThreadingModel", "Apartment");
    QJsonObject extension;
    if (const auto status = read(".vulkana", {}, extension); status) return QStringLiteral("Cannot read project association (%1).").arg(status);
    if (!extension["present"].toBool()) {
        if (const auto status = read(".vulkana", {}, extension, HKEY_LOCAL_MACHINE); status)
            return QStringLiteral("Cannot read system project association (%1).").arg(status);
    }
    // Register Open With without taking over another application's default.
    // The protected Windows UserChoice entry is never read or overwritten.
    if (!extension["present"].toBool() || extension == value("") || extension == value(progId)) add(".vulkana", {}, progId);
    auto saved = entries;
    for (size_t i = 0; i < entries.size(); ++i) {
        auto& entry = entries[i];
        if (const auto status = read(entry.path, entry.name, entry.before); status)
            return QStringLiteral("Cannot read project registration (%1).").arg(status);
        saved[i].before = entry.before;
        for (const auto& old : previous)
            if (old.path == entry.path && old.name == entry.name && old.after == entry.before) saved[i].before = old.before;
    }
    // Save the undo journal before registry writes, so interruption is removable.
    const auto oldJournal = settings.value(snapshotKey);
    settings.setValue(snapshotKey, serialize(saved)); settings.sync();
    if (settings.status() != QSettings::NoError) return QStringLiteral("Cannot save project registration preferences.");
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        if (const auto status = write(entry.path, entry.name, entry.after); status) {
            while (i > 0) { --i; write(entries[i].path, entries[i].name, entries[i].before); }
            settings.setValue(snapshotKey, oldJournal); settings.sync();
            return QStringLiteral("Cannot register project files for this account (%1).").arg(status);
        }
    }
    settings.setValue(projectIntegrationChoiceKey, "registered"); settings.sync(); notify();
    return {};
#else
    Q_UNUSED(settings); Q_UNUSED(executable);
    return QStringLiteral("Project registration is not available for this package.");
#endif
}
QString unregisterProjectFiles(QSettings& settings)
{
#ifdef Q_OS_WIN
    auto entries = snapshot(settings);
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        QJsonObject current;
        auto status = read(it->path, it->name, current);
        // Leave later changes by another application or the user untouched.
        if (!status && current == it->after) status = write(it->path, it->name, it->before);
        if (status) return QStringLiteral("Cannot remove project registration (%1).").arg(status);
    }
    settings.remove(snapshotKey);
    settings.setValue(projectIntegrationChoiceKey, "declined"); settings.sync(); notify();
    return {};
#else
    Q_UNUSED(settings);
    return QStringLiteral("Project registration is not available for this package.");
#endif
}
}
