#include "imageeditor/ui/ShapeOptionsPage.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/ui/ToolOptionsButton.hpp"
#include "imageeditor/ui/ToolOptionsNumber.hpp"
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace imageeditor::ui {
namespace {
    QIcon swatch(core::Rgba8 c)
    {
        QPixmap p(30, 18);
        p.fill(Qt::transparent);
        QPainter paint(&p);
        for (int y = 0; y < 18; y += 5)
            for (int x = 0; x < 30; x += 5)
                paint.fillRect(x, y, 5, 5, ((x / 5 + y / 5) % 2) ? QColor(110, 115, 125) : QColor(65, 69, 78));
        paint.fillRect(p.rect(), QColor(c.red, c.green, c.blue, c.alpha));
        return QIcon(p);
    }
}
ShapeOptionsPage::ShapeOptionsPage(QWidget* parent)
    : QWidget(parent)
    , modes_(new QWidget)
    , modeGroup_(new QButtonGroup(this))
{
    setObjectName(QStringLiteral("ShapeOptionsPage"));
    modes_->setObjectName(QStringLiteral("ShapeModes"));
    auto* modesRow = new QHBoxLayout(modes_);
    modesRow->setContentsMargins(0, 0, 0, 0);
    modesRow->setSpacing(4);
    const std::array labels { "Rectangle", "Rounded rectangle", "Ellipse", "Triangle", "Line", "Polygon" };
    for (std::size_t i = 0; i < labels.size(); ++i) {
        auto* b = new ToolOptionsButton(QStringLiteral("ShapeMode%1").arg(i), QString::fromLatin1(labels[i]),
            QStringLiteral("%1 — create a new editable shape").arg(QString::fromLatin1(labels[i])),
            ToolOptionsButton::Kind::Toggle, modes_, ToolOptionsButton::Presentation::Icon);
        b->setIcon(toolGlyph(static_cast<ToolGlyph>(static_cast<int>(ToolGlyph::ShapeRectangle) + static_cast<int>(i))));
        modeGroup_->addButton(b, static_cast<int>(i));
        modesRow->addWidget(b);
    }
    setCreationMode(core::ShapeKind::Rectangle);
    connect(modeGroup_, &QButtonGroup::idClicked, this, [this](int id) {
        finishNumericInput();
        if (onModeChanged)
            onModeChanged(static_cast<core::ShapeKind>(id));
    });
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addStretch();
    auto toggle = [this, row](const char* name, const char* hint, ToolGlyph icon) {
        auto* b = new ToolOptionsButton(QString::fromLatin1(name), QString::fromLatin1(hint), QString::fromLatin1(hint),
            ToolOptionsButton::Kind::Toggle, this, ToolOptionsButton::Presentation::Icon);
        b->setIcon(toolGlyph(icon));
        row->addWidget(b);
        return b;
    };
    fill_ = toggle("ShapeFillEnabled", "Fill — disable for a hollow shape", ToolGlyph::ShapeFill);
    fillColor_ = new ToolOptionsButton(QStringLiteral("ShapeFillColor"), QStringLiteral("Fill color"), QStringLiteral("Independent shape fill color and alpha"), ToolOptionsButton::Kind::Action, this, ToolOptionsButton::Presentation::Icon);
    row->addWidget(fillColor_);
    stroke_ = toggle("ShapeStrokeEnabled", "Stroke — centered outline", ToolGlyph::ShapeStroke);
    strokeColor_ = new ToolOptionsButton(QStringLiteral("ShapeStrokeColor"), QStringLiteral("Stroke color"), QStringLiteral("Independent shape stroke color and alpha"), ToolOptionsButton::Kind::Action, this, ToolOptionsButton::Presentation::Icon);
    row->addWidget(strokeColor_);
    auto number = [this, row](const char* name, const char* label, double maximum) {
        auto* n = new ToolOptionsNumber(this);
        n->setObjectName(QString::fromLatin1(name));
        n->setPrefix(QString::fromLatin1(label) + QStringLiteral(": "));
        n->setSuffix(QStringLiteral(" px"));
        n->setRange(0, maximum);
        n->setDecimals(2);
        n->setFixedWidth(145);
        row->addWidget(n);
        n->onInteractionFinished = [this] {if(!updating_&&onNumericFinished)onNumericFinished(true); };
        n->onInteractionCancelled = [this] {if(!updating_&&onNumericFinished)onNumericFinished(false); };
        return n;
    };
    width_ = number("ShapeStrokeWidth", "Stroke", core::maximumShapeStyleDimension);
    radius_ = number("ShapeCornerRadius", "Radius", core::maximumShapeStyleDimension);
    auto* transform = new ToolOptionsButton(QStringLiteral("ShapeTransform"), QStringLiteral("Transform"),
        QStringLiteral("Explicit transform session · {{LayerTransformAction}} · {{FinishOperationAction}} applies · Escape cancels the session"),
        ToolOptionsButton::Kind::Action, this, ToolOptionsButton::Presentation::Icon);
    transform->setIcon(toolGlyph(ToolGlyph::Transform));
    row->addWidget(transform);
    row->addStretch();
    connect(transform, &QToolButton::clicked, this, [this] {finishNumericInput();if(onTransformRequested)onTransformRequested(); });
    connect(fillColor_, &QToolButton::clicked, this, [this] {finishNumericInput();if(onColorRequested)onColorRequested(true); });
    connect(strokeColor_, &QToolButton::clicked, this, [this] {finishNumericInput();if(onColorRequested)onColorRequested(false); });
    connect(fill_, &QToolButton::toggled, this, [this](bool b) {if(updating_)return;finishNumericInput();shape_.fillEnabled=b;publish();if(onNumericFinished)onNumericFinished(true); });
    connect(stroke_, &QToolButton::toggled, this, [this](bool b) {if(updating_)return;finishNumericInput();shape_.strokeEnabled=b;publish();if(onNumericFinished)onNumericFinished(true); });
    connect(width_, &QDoubleSpinBox::valueChanged, this, [this](double v) {if(updating_)return;shape_.strokeWidth=v;publish();if(!width_->interactionActive()&&onNumericFinished)onNumericFinished(true); });
    connect(radius_, &QDoubleSpinBox::valueChanged, this, [this](double v) {if(updating_)return;shape_.cornerRadius=v;publish();if(!radius_->interactionActive()&&onNumericFinished)onNumericFinished(true); });
    numbers_[0] = width_;
    numbers_[1] = radius_;
    refresh();
}
void ShapeOptionsPage::populateProperties(QVBoxLayout* layout)
{
    auto* title = new QLabel(QStringLiteral("LOCAL GEOMETRY"));
    title->setObjectName(QStringLiteral("SectionLabel"));
    layout->addWidget(title);
    auto make = [this, layout](const char* name, const char* prefix) {
        auto* n = new ToolOptionsNumber(this);
        n->setObjectName(QString::fromLatin1(name));
        n->setPrefix(QString::fromLatin1(prefix));
        n->setRange(0, core::maximumShapeDimension);
        n->setDecimals(2);
        layout->addWidget(n);
        n->onInteractionFinished = [this] {if(!updating_&&onNumericFinished)onNumericFinished(true); };
        n->onInteractionCancelled = [this] {if(!updating_&&onNumericFinished)onNumericFinished(false); };
        return n;
    };
    geometryWidth_ = make("ShapeGeometryWidth", "Width: ");
    geometryHeight_ = make("ShapeGeometryHeight", "Height: ");
    for (auto* n : { geometryWidth_, geometryHeight_ }) {
        n->setSuffix(QStringLiteral(" px"));
        n->setToolTip(QStringLiteral("Intrinsic geometry before layer transforms. Changes geometry, not stroke width; {{LayerTransformAction}} scales both together."));
    }
    auto resizeGeometry = [this](ToolOptionsNumber* n, bool x) {
        if (updating_ || !editable_)
            return;
        auto& dimension = x ? shape_.size.width : shape_.size.height;
        const double before = dimension;
        dimension = n->value();
        for (std::size_t i = 0; i < shape_.points.size(); ++i) {
            auto& point = shape_.points[i];
            auto& coordinate = x ? point.x : point.y;
            const auto normalized = before > 0         ? coordinate / before
                : shape_.kind == core::ShapeKind::Line ? double(i)
                                                       : 0.0;
            coordinate = std::clamp(normalized * dimension, 0.0, dimension);
        }
        publish();
        if (!n->interactionActive() && onNumericFinished)
            onNumericFinished(true);
    };
    connect(geometryWidth_, &QDoubleSpinBox::valueChanged, this, [this, resizeGeometry] { resizeGeometry(geometryWidth_, true); });
    connect(geometryHeight_, &QDoubleSpinBox::valueChanged, this, [this, resizeGeometry] { resizeGeometry(geometryHeight_, false); });
    auto* verticesTitle = new QLabel(QStringLiteral("VERTICES"));
    verticesTitle->setObjectName(QStringLiteral("SectionLabel"));
    layout->addWidget(verticesTitle);
    vertex_ = make("ShapeVertexIndex", "Vertex: ");
    vertex_->setDecimals(0);
    vertexX_ = make("ShapeVertexX", "X: ");
    vertexY_ = make("ShapeVertexY", "Y: ");
    vertexX_->setSuffix(QStringLiteral(" px"));
    vertexY_->setSuffix(QStringLiteral(" px"));
    numbers_[2] = vertex_;
    numbers_[3] = vertexX_;
    numbers_[4] = vertexY_;
    numbers_[5] = geometryWidth_;
    numbers_[6] = geometryHeight_;
    connect(vertex_, &QDoubleSpinBox::valueChanged, this, [this] {if(updating_)return;if(onNumericFinished)onNumericFinished(true);refreshVertex(); });
    auto change = [this](ToolOptionsNumber* n, bool x) {
        if (updating_ || shape_.points.empty())
            return;
        auto i = std::min(shape_.points.size() - 1, static_cast<std::size_t>(std::max(0.0, vertex_->value() - 1)));
        (x ? shape_.points[i].x : shape_.points[i].y) = n->value();
        publish();
        if (!n->interactionActive() && onNumericFinished)
            onNumericFinished(true);
    };
    connect(vertexX_, &QDoubleSpinBox::valueChanged, this, [this, change] { change(vertexX_, true); });
    connect(vertexY_, &QDoubleSpinBox::valueChanged, this, [this, change] { change(vertexY_, false); });
    auto* help = new QLabel(QStringLiteral("Line and polygon vertices use the local geometry frame. X points right; Y points down. Enlarge Width/Height to extend this frame. Whole-layer transforms preserve these coordinates."));
    help->setWordWrap(true);
    help->setObjectName(QStringLiteral("MutedLabel"));
    layout->addWidget(help);
    refresh();
}
void ShapeOptionsPage::setCreationMode(core::ShapeKind kind)
{
    if (auto* b = modeGroup_->button(static_cast<int>(kind)))
        b->setChecked(true);
}
void ShapeOptionsPage::setConstructionActive(bool active)
{
    setEnabled(!active);
    modes_->setEnabled(!active);
    if (vertex_)
        for (auto* n : { vertex_, vertexX_, vertexY_ })
            n->setEnabled(!active && editable_ && !shape_.points.empty());
    if (geometryWidth_)
        for (auto* n : { geometryWidth_, geometryHeight_ })
            n->setEnabled(!active && editable_);
}
void ShapeOptionsPage::setShape(const core::ShapeLayer& shape, bool editable)
{
    shape_ = shape;
    editable_ = editable;
    refresh();
}
void ShapeOptionsPage::publish()
{
    if (onShapeChanged)
        onShapeChanged(shape_);
    refresh();
}
void ShapeOptionsPage::refresh()
{
    const QScopedValueRollback guard(updating_, true);
    fill_->setChecked(shape_.fillEnabled);
    stroke_->setChecked(shape_.strokeEnabled);
    fill_->setEnabled(shape_.kind != core::ShapeKind::Line);
    fillColor_->setIcon(swatch(shape_.fillColor));
    strokeColor_->setIcon(swatch(shape_.strokeColor));
    fillColor_->setEnabled(shape_.fillEnabled && shape_.kind != core::ShapeKind::Line);
    strokeColor_->setEnabled(shape_.strokeEnabled);
    width_->setValue(shape_.strokeWidth);
    width_->setEnabled(shape_.strokeEnabled);
    radius_->setValue(shape_.cornerRadius);
    radius_->setEnabled(shape_.kind == core::ShapeKind::RoundedRectangle);
    if (geometryWidth_) {
        geometryWidth_->setValue(shape_.size.width);
        geometryHeight_->setValue(shape_.size.height);
        geometryWidth_->setEnabled(editable_);
        geometryHeight_->setEnabled(editable_);
    }
    refreshVertex();
}
void ShapeOptionsPage::refreshVertex()
{
    if (!vertex_)
        return;
    const QScopedValueRollback guard(updating_, true);
    const bool enabled = editable_ && !shape_.points.empty();
    vertex_->setEnabled(enabled);
    vertexX_->setEnabled(enabled);
    vertexY_->setEnabled(enabled);
    vertex_->setRange(1, static_cast<double>(std::max(std::size_t { 1 }, shape_.points.size())));
    // Qt rounds spinbox limits to their display precision. Round inward so a
    // displayed maximum can never exceed the authoritative subpixel frame.
    vertexX_->setMaximum(std::floor(shape_.size.width * 100) / 100);
    vertexY_->setMaximum(std::floor(shape_.size.height * 100) / 100);
    if (enabled) {
        const auto i = static_cast<std::size_t>(vertex_->value() - 1);
        vertexX_->setValue(shape_.points[i].x);
        vertexY_->setValue(shape_.points[i].y);
    }
}
void ShapeOptionsPage::finishNumericInput()
{
    for (auto* n : numbers_)
        if (n) {
            n->interpretText();
            if (n->hasFocus())
                n->clearFocus();
            n->finishInteraction();
        }
    if (!updating_ && onNumericFinished)
        onNumericFinished(true);
}
}
