#pragma once

#include <QLabel>
#include <QTimer>
#include <cstdint>
#include <functional>
#include <optional>

namespace imageeditor::ui {
struct MemoryStatusSnapshot {
    std::optional<std::uint64_t> residentBytes;
    std::uint64_t textureCacheBytes{}, stagingBytes{};
};

// A low-frequency readout only: sampling must never render or build caches.
class MemoryStatusLabel final : public QLabel {
public:
    using Sampler = std::function<MemoryStatusSnapshot()>;
    explicit MemoryStatusLabel(Sampler, QWidget* parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
    void paintEvent(QPaintEvent*) override;
private:
    void sample();
    Sampler sampler_;
    QTimer timer_;
    QString ramText_;
};
}
