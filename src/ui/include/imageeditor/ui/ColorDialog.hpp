#pragma once
#include <QColorDialog>
#include <QDialog>
#include <QPointer>

class QDialogButtonBox;
class QLabel;
class QPushButton;
namespace imageeditor::ui {
class ColorPicker;
class WorkspaceDialog;

// Shared editor dialog, using public Qt widgets rather than modifying Qt's
// private color-dialog layout. Keep the familiar color/option signal contract.
class ColorDialog final : public QDialog {
    Q_OBJECT
public:
    using ColorDialogOption = QColorDialog::ColorDialogOption;
    using ColorDialogOptions = QColorDialog::ColorDialogOptions;
    static constexpr auto ShowAlphaChannel = QColorDialog::ShowAlphaChannel;
    static constexpr auto DontUseNativeDialog = QColorDialog::DontUseNativeDialog;
    static constexpr auto NoButtons = QColorDialog::NoButtons;
    explicit ColorDialog(QWidget* parent = nullptr);
    explicit ColorDialog(const QColor& color, QWidget* parent = nullptr);
    ~ColorDialog() override;
    [[nodiscard]] QColor currentColor() const;
    [[nodiscard]] QColor selectedColor() const { return selected_; }
    void setCurrentColor(const QColor& color);
    void setOptions(ColorDialogOptions options);
    void setOption(ColorDialogOption option, bool on = true);
    [[nodiscard]] bool testOption(ColorDialogOption option) const { return options_.testFlag(option); }
    [[nodiscard]] ColorDialogOptions options() const { return options_; }
    static QColor getColor(const QColor& initial = Qt::white, QWidget* parent = nullptr,
                           const QString& title = {}, ColorDialogOptions options = {});
    void done(int result) override;
    int exec() override;
    void setVisible(bool visible) override;
signals:
    void currentColorChanged(const QColor& color);
    void colorSelected(const QColor& color);
private:
    ColorPicker* picker_ {};
    QPointer<WorkspaceDialog> presenter_;
    bool ownsPresenter_ {};
    QObject* screenPicker_ {};
    QPushButton* screenButton_ {};
    QDialogButtonBox* buttons_ {};
    QLabel* status_ {};
    QWidget* preview_ {};
    QColor initial_, selected_;
    ColorDialogOptions options_ {DontUseNativeDialog};
};
} // namespace imageeditor::ui
