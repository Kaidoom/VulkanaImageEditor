#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/MemoryStatusLabel.hpp"
#include <QHideEvent>
#include <QPainter>
#include <QShowEvent>
#include <QStyle>

namespace imageeditor::ui {
namespace {
QString memorySize(std::uint64_t bytes) {
    constexpr auto MiB = 1024ULL * 1024;
    constexpr auto GiB = 1024 * MiB;
    constexpr auto TiB = 1024 * GiB;
    if (bytes >= TiB) return QStringLiteral("%1 TiB").arg(double(bytes) / TiB, 0, 'f', 1);
    if (bytes >= GiB) return QStringLiteral("%1 GiB").arg(double(bytes) / GiB, 0, 'f', 1);
    return QStringLiteral("%1 MiB").arg(double(bytes) / MiB, 0, 'f', 1);
}
}
MemoryStatusLabel::MemoryStatusLabel(Sampler sampler, QWidget* parent)
    : QLabel(parent), sampler_(std::move(sampler)) {
    setObjectName(QStringLiteral("MemoryStatus"));
    i18n::setAccessibleName(this, i18n::text("memory.status.label.application.memory.usage"));
    setTextFormat(Qt::PlainText);
    setTextInteractionFlags(Qt::NoTextInteraction);
    setFocusPolicy(Qt::NoFocus);
    setContentsMargins(8, 0, 8, 0);
    setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    timer_.setInterval(1000);
    timer_.setTimerType(Qt::CoarseTimer);
    connect(&timer_, &QTimer::timeout, this, [this] {
        if (isVisible() && !window()->isMinimized()) sample();
    });
}
void MemoryStatusLabel::sample() {
    const auto value = sampler_();
    const auto ram = value.residentBytes ? memorySize(*value.residentBytes) : i18n::text("memory.status.label.unavailable");
    ramText_ = i18n::text("memory.status.label.ram.1").arg(ram);
    const auto text = i18n::text("memory.status.label.1.gpu.cache.2").arg(ramText_, memorySize(value.textureCacheBytes));
    if (text != this->text()) setText(text);
    const auto detail = i18n::text("memory.status.label.ram.1.os.reported.resident.memory.for.the.entire")
        .arg(ram, memorySize(value.textureCacheBytes), memorySize(value.stagingBytes));
    if (toolTip() != detail) setToolTip(detail);
}
QSize MemoryStatusLabel::sizeHint() const {
    // Stable width prevents changing measurements from shifting adjacent zoom.
    return {fontMetrics().horizontalAdvance(i18n::text("memory.status.label.ram.999.9.mib.gpu.cache.999.9.mib")) + 16,
            QLabel::sizeHint().height()};
}
QSize MemoryStatusLabel::minimumSizeHint() const { return {0, sizeHint().height()}; }
void MemoryStatusLabel::showEvent(QShowEvent* event) {
    QLabel::showEvent(event);
    sample();
    timer_.start();
}
void MemoryStatusLabel::hideEvent(QHideEvent* event) {
    timer_.stop();
    QLabel::hideEvent(event);
}
void MemoryStatusLabel::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const auto full = fontMetrics().horizontalAdvance(text()) <= contentsRect().width();
    const auto visible = fontMetrics().elidedText(full ? text() : ramText_, Qt::ElideRight, contentsRect().width());
    style()->drawItemText(&painter, contentsRect(), int(alignment()), palette(), isEnabled(), visible, QPalette::WindowText);
}
}
