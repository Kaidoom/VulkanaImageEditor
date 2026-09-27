#pragma once
#include <QDialog>
#include <QJsonObject>
#include <QStringList>

class QListWidget;
class QTextBrowser;
class QStackedWidget;

namespace imageeditor::ui {
class ThirdPartyNoticesDialog final : public QDialog {
public:
    explicit ThirdPartyNoticesDialog(QWidget* parent, const QString& manifestPath = {});
    static QString defaultManifestPath();
    bool loaded() const { return loaded_; }
private:
    void showEntry(int row);
    QJsonObject manifest_;
    QListWidget* list_ {};
    QTextBrowser* body_ {};
    QTextBrowser* summary_ {};
    QStackedWidget* pages_ {};
    QStringList entrySummaries_;
    QStringList entrySearchTexts_;
    bool loaded_ {};
};
} // namespace imageeditor::ui
