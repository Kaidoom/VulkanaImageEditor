#pragma once
#include <QString>
class QFileDevice;

namespace imageeditor::platform {
// Flush staged bytes before verification/publication. No direct-write fallback.
bool syncFile(QFileDevice& file);
// The caller must stage beside the destination, verify/release its native handle
// (destroy QTemporaryFile, whose close() retains the handle), and retain
// cleanup ownership on failure. Does not copy/delete across volumes.
bool replaceFileAtomically(const QString& staged, const QString& destination, QString& error);
}
