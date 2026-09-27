#include "imageeditor/core/DocumentCommands.hpp"

namespace imageeditor::core {

ChangeCanvasSpecCommand::ChangeCanvasSpecCommand(CanvasSpec after)
    : after_(after)
{
}

bool ChangeCanvasSpecCommand::apply(Document& document)
{
    if (!before_) {
        before_ = document.canvas();
        beforeSelection_ = document.selection();
        beforeRemembered_ = document.lastSelection();
        if (beforeSelection_) (void)beforeSelection_->boundaryEdges();
        if (beforeRemembered_) (void)beforeRemembered_->boundaryEdges();
    }
    return document.setCanvas(after_);
}

bool ChangeCanvasSpecCommand::undo(Document& document)
{
    if (!before_ || !document.setCanvas(*before_)) return false;
    document.setSelection(beforeSelection_);
    document.setLastSelection(beforeRemembered_);
    return true;
}

} // namespace imageeditor::core
