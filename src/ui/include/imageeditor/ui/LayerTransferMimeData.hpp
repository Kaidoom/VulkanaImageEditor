#pragma once
#include "imageeditor/ui/DocumentContext.hpp"
#include "imageeditor/core/LayerStructureCommands.hpp"
#include <QMimeData>
namespace imageeditor::ui {
// Process-local, typed payload. No pointers encoded in untrusted MIME bytes.
class LayerTransferMimeData final : public QMimeData {
public:
    std::shared_ptr<DocumentContext> source;
    mutable core::LayerTransfer content;
    core::Vec2d anchor;
    mutable bool consumed {false};
    mutable DocumentInstanceId destination {0};
};
}
