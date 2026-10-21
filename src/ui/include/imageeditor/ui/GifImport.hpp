#pragma once
#include "imageeditor/core/Document.hpp"
#include <QString>
#include <QByteArray>
#include <functional>
#include <memory>

namespace imageeditor::ui {
struct GifImportResult {
    std::unique_ptr<core::Document> document;
    QString error;
    bool cancelled{};
    explicit operator bool() const noexcept { return bool(document); }
};
// The callback runs on the caller's thread. Zero total denotes metadata scanning.
// workingBytes is an admission budget, not an arbitrary frame-count limit.
GifImportResult loadGifDocument(const QString& path, const QString& frameName,
    std::function<bool(std::size_t, std::size_t)> progress = {}, std::uint64_t workingBytes = 0);
GifImportResult loadGifDocumentFromData(const QByteArray&, const QString& frameName,
    std::function<bool(std::size_t, std::size_t)> progress = {}, std::uint64_t workingBytes = 0);
bool isGifFile(const QString& path);
}
