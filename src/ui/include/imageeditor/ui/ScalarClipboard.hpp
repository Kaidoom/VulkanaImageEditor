#pragma once

#include "imageeditor/core/ScalarClipboard.hpp"
#include <QByteArray>

namespace imageeditor::ui {
inline constexpr auto scalarClipboardMime = "application/x-vulkana-scalar-region-v1";
[[nodiscard]] QByteArray encodeScalarClipboard(const core::ScalarFragment&);
[[nodiscard]] std::optional<core::ScalarFragment> decodeScalarClipboard(const QByteArray&);
}
