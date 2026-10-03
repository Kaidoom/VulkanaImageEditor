#include "imageeditor/platform/AvailableMemory.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <limits>
#ifdef Q_OS_LINUX
#include <unistd.h>
#endif

namespace imageeditor::platform {
std::optional<std::uint64_t> availableMemoryBytes() {
  std::optional<std::uint64_t> available;
#ifdef Q_OS_LINUX
  QFile meminfo("/proc/meminfo");
  if (meminfo.open(QIODevice::ReadOnly))
    for (const auto &line : meminfo.readAll().split('\n'))
      if (line.startsWith("MemAvailable:")) {
        bool ok = false;
        const auto kib = line.simplified().split(' ').value(1).toULongLong(&ok);
        if (ok && kib < (1ULL << 40))
          available = kib * 1024;
        break;
      }
  QFile membership("/proc/self/cgroup");
  if (membership.open(QIODevice::ReadOnly))
    for (const auto &line : membership.readAll().split('\n')) {
      if (!line.startsWith("0::/"))
        continue;
      const QString root = "/sys/fs/cgroup";
      auto path = QDir::cleanPath(root + QString::fromUtf8(line.mid(3)));
      while (path == root || path.startsWith(root + '/')) {
        QFile maximum(path + "/memory.max"), current(path + "/memory.current");
        if (maximum.open(QIODevice::ReadOnly) && current.open(QIODevice::ReadOnly)) {
          bool maxOk = false, usedOk = false;
          const auto cap = maximum.read(64).trimmed().toULongLong(&maxOk);
          const auto used = current.read(64).trimmed().toULongLong(&usedOk);
          if (maxOk && usedOk) {
            const auto headroom = cap > used ? cap - used : 0;
            available = available ? std::min(*available, std::uint64_t(headroom))
                                  : headroom;
          }
        }
        if (path == root)
          break;
        path = QFileInfo(path).path();
      }
    }
#endif
  return available;
}
std::uint64_t availableWorkingMemoryBytes() {
  const auto available = availableMemoryBytes();
  return available ? *available - *available / 4 : 1536ULL * 1024 * 1024;
}
std::optional<std::uint64_t> processResidentMemoryBytes() {
#ifdef Q_OS_LINUX
  QFile statm("/proc/self/statm");
  if (statm.open(QIODevice::ReadOnly)) {
    const auto fields = statm.read(128).simplified().split(' ');
    bool ok = false;
    const auto residentPages = fields.value(1).toULongLong(&ok);
    static const long pageSize = ::sysconf(_SC_PAGESIZE);
    if (ok && pageSize > 0 && residentPages <=
        std::numeric_limits<std::uint64_t>::max() / std::uint64_t(pageSize))
      return residentPages * std::uint64_t(pageSize);
  }
#endif
  return std::nullopt;
}
}
