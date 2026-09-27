#pragma once
#include "imageeditor/core/Layer.hpp"
#include <QByteArray>
#include <optional>
namespace imageeditor::ui {
inline constexpr auto kTextClipboardMime = "application/x-imageeditor-rich-text-v1";
QByteArray encodeTextClipboard(const core::TextLayer&);
std::optional<core::TextLayer> decodeTextClipboard(const QByteArray&);
}
