#include "imageeditor/ui/CropOptionsPage.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/ui/ToolOptionsButton.hpp"
#include "imageeditor/ui/ToolOptionsNumber.hpp"
#include <QHBoxLayout>
#include <QScopedValueRollback>
#include <QSignalBlocker>
namespace imageeditor::ui {
CropOptionsPage::CropOptionsPage(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("CropOptionsPage");
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addStretch();
    const std::array labels { "X", "Y", "W", "H" };
    for (std::size_t i = 0; i < 4; ++i) {
        auto* n = new ToolOptionsNumber(this);
        numbers_[i] = n;
        n->setObjectName(QStringLiteral("Crop%1Control").arg(labels[i]));
        n->setPrefix(QString::fromLatin1(labels[i]) + ": ");
        n->setSuffix(" px");
        n->setDecimals(2);
        n->setRange(i < 2 ? -1e9 : .0001, 1e9);
        n->setSingleStep(1);
        n->setFixedWidth(140);
        n->setToolTip(
            i < 2 ? "Crop origin in stable layer-local pixels"
                  : "Crop size in layer-local pixels; content does not scale");
        n->onInteractionFinished = [this] {
            if (onActionFinished)
                onActionFinished();
        };
        row->addWidget(n);
        connect(n, &QDoubleSpinBox::valueChanged, this, [this, n, i](double value) {
            auto r = frame_;
            if (i == 0)
                r.x = value;
            else if (i == 1)
                r.y = value;
            else if (i == 2) {
                if (aspectLocked() && r.width > 0)
                    r.height *= value / r.width;
                r.width = value;
            } else {
                if (aspectLocked() && r.height > 0)
                    r.width *= value / r.height;
                r.height = value;
            }
            {
                const QScopedValueRollback guard(publishing_, n);
                if (onChanged)
                    onChanged(r);
            }
            if (!n->interactionActive() && onActionFinished)
                onActionFinished();
        });
    }
    const auto button = [this, row](const char* name, const char* text,
                            const char* hint, bool toggle = false,
                            ToolOptionsButton::Presentation presentation = ToolOptionsButton::Presentation::Text) {
        auto* b = new ToolOptionsButton(name, text, hint,
            toggle ? ToolOptionsButton::Kind::Toggle
                   : ToolOptionsButton::Kind::Action,
            this, presentation);
        row->addWidget(b);
        return b;
    };
    lock_ = button(
        "CropAspectLock", "Lock ratio",
        "Lock ratio — keep crop aspect ratio; Shift temporarily toggles the constraint", true,
        ToolOptionsButton::Presentation::Icon);
    lock_->setIcon(toolGlyph(ToolGlyph::AspectLock));
    chamfer_ = button("CropChamfer", "Chamfer",
        "Chamfer — corner handles cut diagonally; Alt mirrors the opposite corner. Turn off to resize the frame; cuts stay editable.", true,
        ToolOptionsButton::Presentation::Icon);
    chamfer_->setIcon(toolGlyph(ToolGlyph::Chamfer));
    connect(chamfer_, &QToolButton::toggled, this, [this] {
        finishNumericInput();
        if (onChamferChanged)
            onChamferChanged();
    });
    preview_ = button(
        "CropShowSource", "Show source",
        "Show source — toggle a faint preview of retained content outside the crop; editor only", true,
        ToolOptionsButton::Presentation::Icon);
    preview_->setIcon(toolGlyph(ToolGlyph::ShowSource));
    preview_->setChecked(true);
    auto* remove = button("CropRemove", "Reset crop",
        "Reset crop — reveal the full retained source and clear corner cuts; preserve the original source",
        false, ToolOptionsButton::Presentation::Icon);
    remove->setIcon(toolGlyph(ToolGlyph::ResetCrop));
    auto* apply = button("CropApply", "Apply", "Apply crop actions · {{FinishOperationAction}} / right-click");
    apply->setProperty("toolOptionsPrimary", true);
    auto* cancel = button("CropCancel", "Cancel",
        "Restore the crop from tool entry · Escape");
    row->addStretch();
    connect(preview_, &QToolButton::toggled, this, [this] {
        if (onPreviewChanged)
            onPreviewChanged();
    });
    connect(remove, &QToolButton::clicked, this, [this] {
        finishNumericInput();
        if (onRemove)
            onRemove();
    });
    connect(apply, &QToolButton::clicked, this, [this] {
        finishNumericInput();
        if (onApply)
            onApply();
    });
    connect(cancel, &QToolButton::clicked, this, [this] {
        if (onCancel)
            onCancel();
    });
}
void CropOptionsPage::setFrame(core::RectD r)
{
    frame_ = r;
    const std::array values { r.x, r.y, r.width, r.height };
    for (std::size_t i = 0; i < 4; ++i)
        if (numbers_[i] != publishing_) {
            const QSignalBlocker guard(numbers_[i]);
            numbers_[i]->setValue(values[i]);
        }
}
void CropOptionsPage::finishNumericInput()
{
    for (auto* n : numbers_) {
        n->interpretText();
        n->clearFocus();
        n->finishInteraction();
    }
}
bool CropOptionsPage::aspectLocked() const { return lock_->isChecked(); }
bool CropOptionsPage::showSource() const { return preview_->isChecked(); }
bool CropOptionsPage::chamferEnabled() const { return chamfer_->isChecked(); }
void CropOptionsPage::setChamferEnabled(bool value)
{
    const QSignalBlocker guard(chamfer_);
    chamfer_->setChecked(value);
}
} // namespace imageeditor::ui
