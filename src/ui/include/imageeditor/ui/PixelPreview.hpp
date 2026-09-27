#pragma once
#include "imageeditor/core/Document.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include <QObject>
#include <QString>
#include <functional>
#include <memory>

namespace imageeditor::ui {
// Owner-thread coordinator. One worker owns frozen sources; stale jobs never
// publish. Presentation metadata/selection/zoom are intentionally not cache keys.
class PixelPreview final : public QObject {
public:
    struct Profile { double freezeMs{},transferMs{};MergeProfile evaluation; };
    explicit PixelPreview(QObject* parent=nullptr);
    ~PixelPreview() override;
    void setEnabled(bool);
    void setDocumentInstance(std::uint64_t);
    void forgetDocument(std::uint64_t);
    void request(const core::DocumentSnapshot&);
    [[nodiscard]] bool busy() const;
    [[nodiscard]] std::uint64_t completedRenders() const;
    [[nodiscard]] std::uint64_t frozenBytes() const;
    [[nodiscard]] Profile lastProfile() const;
    std::function<void(std::shared_ptr<const core::RasterSurface>,QString)> onReady;
private:
    struct State;
    std::unique_ptr<State> state_;
    void advance();
};
}
