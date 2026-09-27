#pragma once

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

// Offline tests never depend on private files or call the official service.
// Keep this override alive until all services in a test have been destroyed.
class TestServiceConfig {
public:
    TestServiceConfig()
        : previous_(qgetenv("VULKANA_SERVICE_CONFIG")), wasSet_(qEnvironmentVariableIsSet("VULKANA_SERVICE_CONFIG"))
    {
        write({{"version", 1}, {"domain", "https://updates.example"}, {"apiKey", "test-client-token"}});
        qputenv("VULKANA_SERVICE_CONFIG", path().toUtf8());
    }
    ~TestServiceConfig()
    {
        if (wasSet_) qputenv("VULKANA_SERVICE_CONFIG", previous_);
        else qunsetenv("VULKANA_SERVICE_CONFIG");
    }
    QString path() const { return directory_.filePath("services.json"); }
    bool write(const QJsonObject& object) { return writeBytes(QJsonDocument(object).toJson()); }
    bool writeBytes(const QByteArray& bytes)
    {
        QFile file(path());
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    }
private:
    QTemporaryDir directory_;
    QByteArray previous_;
    bool wasSet_;
};
