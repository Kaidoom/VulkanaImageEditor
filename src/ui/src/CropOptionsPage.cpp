#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/CropOptionsPage.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/ui/ToolOptionsButton.hpp"
#include "imageeditor/ui/ToolOptionsNumber.hpp"
#include "imageeditor/ui/RasterLimits.hpp"
#include <QHBoxLayout>
#include <QScopedValueRollback>
#include <QSignalBlocker>
namespace imageeditor::ui {
CropOptionsPage::CropOptionsPage(QWidget* parent, Mode mode)
    : QWidget(parent)
{
    const bool canvas = mode == Mode::Canvas;
    setObjectName(canvas ? "CanvasResizeOptionsPage" : "CropOptionsPage");
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addStretch();
    const std::array labels { "X", "Y", "W", "H" };
    for (std::size_t i = 0; i < 4; ++i) {
        auto* n = new ToolOptionsNumber(this);
        numbers_[i] = n;
        n->setObjectName((canvas ? QStringLiteral("CanvasResize%1Control") : QStringLiteral("Crop%1Control")).arg(labels[i]));
        n->setPrefix(QString::fromLatin1(labels[i]) + ": ");
        i18n::setSuffix(n, i18n::text("crop.options.page.px"));
        n->setDecimals(canvas ? 0 : 2);
        n->setRange(i < 2 ? -1e9 : canvas ? 1 : .0001, canvas && i >= 2 ? kMaximumRasterDimension : 1e9);
        n->setSingleStep(1);
        n->setFixedWidth(140);
        if (canvas) i18n::setToolTip(n, i18n::text("canvas.resize.hint"));
        else n->setToolTip(
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
    const auto button = [this, row](const char* name, const i18n::Message& text,
                            const i18n::Message& hint, bool toggle = false,
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
    if (canvas) {
        // Keep layer-only controls out of both the canvas page and global
        // object-name lookup used by shortcuts, accessibility and tests.
        chamfer_->setObjectName("CanvasResizeUnusedChamfer");
        preview_->setObjectName("CanvasResizeUnusedSourcePreview");
        chamfer_->hide();
        preview_->hide();
    }
    auto* remove = button("CropRemove", "Reset crop",
        "Reset crop — reveal the full retained source and clear corner cuts; preserve the original source",
        false, ToolOptionsButton::Presentation::Icon);
    remove->setIcon(toolGlyph(ToolGlyph::ResetCrop));
    auto* apply = button("CropApply", "Apply", "Apply crop actions · {{FinishOperationAction}} / right-click");
    apply->setProperty("toolOptionsPrimary", true);
    auto* cancel = button("CropCancel", "Cancel",
        "Restore the crop from tool entry · Escape");
    if (canvas) {
        lock_->setObjectName("CanvasResizeAspectLock");
        i18n::setToolTip(lock_, i18n::text("canvas.resize.ratio"));
        remove->setObjectName("CanvasResizeReset");
        i18n::setText(remove, i18n::text("canvas.resize.reset"));
        i18n::setAccessibleName(remove, i18n::text("canvas.resize.reset"));
        i18n::setToolTip(remove, i18n::text("canvas.resize.reset"));
        apply->setObjectName("CanvasResizeApply");
        i18n::setToolTip(apply, i18n::text("canvas.resize.apply"));
        cancel->setObjectName("CanvasResizeCancel");
        i18n::setToolTip(cancel, i18n::text("canvas.resize.cancel"));
    }
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
