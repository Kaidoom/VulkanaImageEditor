#pragma once

#include <QImage>
#include <QString>
#include <atomic>
#include <memory>
#include <vector>

namespace imageeditor::ui {

struct BackgroundRemovalModel {
    QString id;
    QString name;
    QString relativePath;
};

struct BackgroundRemovalTimings {
    double initializationMs { };
    double preprocessingMs { };
    double inferenceMs { };
    double postprocessingMs { };
    double totalMs { };
};

struct BackgroundRemovalResult {
    QImage mask; // Grayscale8 coverage in the original, untransformed layer resolution.
    BackgroundRemovalTimings timings;
    QString error;
    bool cancelled { false };
    QString details;
};

// A worker owns the offline CPU engine and calls it serially. Only one model
// session is retained; no downloads or GPU providers.
class BackgroundRemovalEngine {
public:
    BackgroundRemovalEngine(); // Verified, installed model bundle.
    // Explicit trusted roots support offline diagnostics and synthetic fixtures.
    explicit BackgroundRemovalEngine(QString modelDirectory, bool verifyIntegrity = false);
    ~BackgroundRemovalEngine();
    BackgroundRemovalEngine(const BackgroundRemovalEngine&) = delete;
    BackgroundRemovalEngine& operator=(const BackgroundRemovalEngine&) = delete;

    [[nodiscard]] static std::vector<BackgroundRemovalModel> models();
    [[nodiscard]] BackgroundRemovalResult evaluate(
        const QString& id, const QImage& source, const std::atomic_bool& cancel);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
