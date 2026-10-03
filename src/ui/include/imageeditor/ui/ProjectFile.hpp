#pragma once
#include "imageeditor/core/Document.hpp"
#include <QJsonObject>
#include <QStringList>
#include <functional>
#include <memory>

namespace imageeditor::ui {
using ProjectProgress = std::function<bool(quint64, quint64)>;
struct ProjectIoResult {
    QString error;
    bool cancelled { false };
    explicit operator bool() const noexcept { return error.isEmpty() && !cancelled; }
};
struct ProjectLoadResult {
    std::unique_ptr<core::Document> document;
    QJsonObject metadata; // Opaque compatible JSON; not canonical core data.
    QString error;
    QStringList warnings;
    bool cancelled { false };
    explicit operator bool() const noexcept { return document != nullptr; }
};
// Caller excludes document mutations throughout save. No shared mutable
// snapshot is advertised as immutable; work uses bounded row/chunk buffers.
ProjectIoResult saveProject(const QString&, const core::Document&,
    const QJsonObject& metadata = { }, ProjectProgress = { });
struct ProjectLoadLimits {
    // Zero selects available system/cgroup headroom, not a fixed project cap.
    quint64 workingBytes { 0 };
};
ProjectLoadResult loadProject(const QString&, ProjectProgress = { }, ProjectLoadLimits = { });
}
