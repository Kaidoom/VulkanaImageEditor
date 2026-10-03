#pragma once
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/PdfExport.hpp"
#include "imageeditor/ui/PsdExport.hpp"
#include <QByteArray>
#include <QColor>
#include <QSize>
#include <atomic>

namespace imageeditor::ui {
inline constexpr int kMaximumExportDimension = 32768;
enum class ExportFormat { Png, Jpeg, WebP, Pdf, Psd };
struct ExportSettings {
    ExportFormat format { ExportFormat::Png };
    QString destination;
    QSize size;
    bool aspectLocked { true };
    bool pngMatte { false };
    QColor pngMatteColor { Qt::white };
    QColor jpegMatteColor { Qt::white };
    int pngEffort { 6 }; // zlib effort 0..9, never visual quality
    int jpegQuality { 92 };
    bool webpLossless { false };
    int webpQuality { 90 };
    int webpEffort { 4 }; // libwebp method 0..6; alpha always lossless
    PdfExportOptions pdf;
    PsdExportOptions psd;
    bool operator==(const ExportSettings&) const = default;
};
struct EncodedExport {
    QByteArray bytes;
    QImage decoded; // Actual decoded encoded bytes, not a pristine-source preview.
    QString error;
    bool cancelled { false };
    explicit operator bool() const { return !bytes.isEmpty() && error.isEmpty() && !cancelled; }
};
struct ExportWriteResult {
    QString error;
    bool cancelled { false };
    explicit operator bool() const { return error.isEmpty() && !cancelled; }
};
[[nodiscard]] QByteArray exportFormatName(ExportFormat);
[[nodiscard]] QColor exportMatteColor(const ExportSettings&);
// Returns an explicit destination proposal; the UI must show/confirm changes.
[[nodiscard]] QString exportPathForFormat(const QString&, ExportFormat);
[[nodiscard]] QString validateExport(const ExportSettings&, QSize canvasSize);
[[nodiscard]] QString validateExportDestination(const QString&, const QString& projectPath = { });
// Owner thread only, under editor mutation exclusion. The result is detached
// from mutable document pixels and safe to pass to a codec worker.
[[nodiscard]] FlattenedDocumentResult renderExport(
    const core::Document&, const ExportSettings&, FlattenedDocumentProgress = { });
// Worker-safe. Input must be an immutable renderExport result. Metadata is
// freshly constructed; no source EXIF/GPS/orientation is propagated.
[[nodiscard]] EncodedExport encodeExport(
    const QImage&, const ExportSettings&, const std::atomic_bool& cancel);
[[nodiscard]] ExportWriteResult writeExportAtomically(
    const QString&, const QByteArray&, const std::atomic_bool& cancel);
[[nodiscard]] ExportSettings loadExportPreferences(QSize canvasSize, const QString& suggestedName);
void saveExportPreferences(const ExportSettings&);
[[nodiscard]] bool hasExportPreferences();
} // namespace imageeditor::ui
