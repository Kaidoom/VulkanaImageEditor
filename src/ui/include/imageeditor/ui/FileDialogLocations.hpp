#pragma once
#include <QString>

namespace imageeditor::ui {
// Explicit existing-directory fallbacks; never depend on process working dir.
[[nodiscard]] QString existingFileDialogDirectory(
    const QString& remembered, const QString& pictures, const QString& application);
[[nodiscard]] QString openDocumentDirectory();
[[nodiscard]] QString lastExportDirectory();
void rememberOpenedDocument(const QString& path);
void rememberExportDirectory(const QString& path);
}
