#pragma once
#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/ui/QtTextLayoutService.hpp"
#include "imageeditor/ui/EditorShortcuts.hpp"
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <functional>
#include <map>

class QComboBox;
class QPushButton;
class QInputMethodEvent;
class QKeyEvent;
class QMouseEvent;
namespace imageeditor::render {
class CanvasWindow;
}
namespace imageeditor::ui {
class ColorDialog;
class OverlayDockWorkspace;
class ToolOptionsButton;
class ToolOptionsNumber;
class FontFamilyPicker;
class TextController final : public QObject {
public:
    TextController(
        core::EditorSession&, render::CanvasWindow*, OverlayDockWorkspace*, QObject* parent);
    ~TextController() override;
    void setSession(core::EditorSession&);
    void forgetSession(core::EditorSession&);
    [[nodiscard]] bool active() const noexcept { return id_ != 0; }
    [[nodiscard]] core::LayerId layerId() const noexcept { return id_; }
    bool press(core::Vec2d, Qt::KeyboardModifiers, bool doubleClick);
    bool editLayer(core::LayerId);
    void drag(core::Vec2d);
    void finish();
    void finishNumeric(bool cancel = false);
    void prepareOverlayInteraction(QWidget*, const QMouseEvent&);
    bool keyEvent(QKeyEvent*, bool editableWidgetOwnsInput);
    void setShortcutBindings(const ShortcutBindings& bindings);
    bool inputEvent(QEvent*);
    void history(bool redo);
    void prepareCaches();
    void viewportChanged();
    void refreshPresentation();
    void patchStyle(core::TextStylePatch, std::uint64_t grouping = 0);
    [[nodiscard]] const core::TextStyle& insertionStyle() const { return insertion_; }
    [[nodiscard]] QTextCursor cursor() const { return cursor_; }
    std::function<void()> onChanged;
    std::function<void()> onActivateText;
    std::function<void(QString)> onError;

private:
    ShortcutBindings shortcuts_ = defaultShortcutBindings();
    bool eventFilter(QObject*, QEvent*) override;
    void guardEdit(const std::function<void()>&);
    bool handleKey(QKeyEvent*, bool);
    void applyStyle(core::TextStylePatch, std::uint64_t);
    void alignParagraphs(core::TextAlignment);
    void createOverlay();
    void synchronizeControls();
    void placeOverlay();
    void publishOverlays();
    void deliberateCaret();
    void replaceSelection(const QString&, const QString&, bool groupTyping);
    void mutate(core::TextLayer, core::TextEditHint before, core::TextEditHint after,
        const QString& label, std::uint64_t key = 0);
    core::TextEditHint hint() const;
    void restoreCursor(core::TextEditHint);
    core::AffineTransform transform() const;
    bool containsEditingPoint(core::Vec2d) const;
    void composition(QInputMethodEvent*);
    void commitComposition();
    void clearComposition();
    void showColor();
    void returnCanvasFocus();
    void setOverlayInput(QWidget*);
    void updateInputGeometry();
    void showEditing();
    bool settleCreationHistory();
    void applyAlignment(core::TextAlignment);
    void resetGroup();
    std::uint64_t typingGroup(const QString& kind);
    QtTextLayout* displayLayout() const;
    core::EditorSession* session_;
    render::CanvasWindow* canvas_;
    OverlayDockWorkspace* workspace_;
    std::unique_ptr<QtTextLayout> layout_, preeditLayout_;
    QTextCursor cursor_;
    core::TextStyle insertion_;
    core::LayerId id_ { };
    std::optional<core::Layer> provisional_;
    // The first creation session is retractable without destroying pre-existing
    // redo. Its individual commands still use EditorSession's normal history.
    std::optional<core::History> historyBeforeCreation_;
    std::uint64_t contentBeforeCreation_ {0};
    std::optional<core::LayerId> previousActive_;
    std::map<core::LayerId, std::pair<core::Revision, std::unique_ptr<QtTextLayout>>> layouts_;
    std::map<core::EditorSession*, decltype(layouts_)> inactiveLayouts_;
    QTimer blink_, densityTimer_;
    QElapsedTimer typingClock_;
    QString typingKind_, preedit_;
    int preeditCursor_ { };
    bool preeditCaretVisible_ { true }, caretVisible_ { true }, syncing_ { false },
        publishing_ { false };
    std::uint64_t group_ { 1 }, numericGroup_ { };
    struct NumericEdit {
        core::TextLayer before;
        core::TextEditHint cursor;
        core::TextStyle insertion;
        bool selected;
    };
    std::optional<NumericEdit> numericEdit_;
    QWidget* overlay_ { };
    FontFamilyPicker* family_ { };
    QComboBox* styles_ { };
    ToolOptionsNumber* size_ { };
    ToolOptionsButton *bold_ { }, *italic_ { }, *alignLeft_ { }, *alignCenter_ { },
        *alignRight_ { };
    QPushButton* color_ { };
    QPointer<ColorDialog> colorDialog_;
    core::Vec2d lastOrigin_;
    double lastZoom_ { -1 }, lastDpr_ { -1 };
    QSize lastViewport_;
};
}
