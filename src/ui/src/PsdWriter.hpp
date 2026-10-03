#pragma once
#include "imageeditor/ui/PsdExport.hpp"
#include <QIODevice>
#include <QRect>
#include <QVariant>

namespace imageeditor::ui::psdwrite {
struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};
struct Cancelled {};
void require(bool, const char *);
void check(const std::atomic_bool &);
void u16(QByteArray &, int);
void u32(QByteArray &, quint32);
void f64(QByteArray &, double);
void unicode(QByteArray &, const QString &);
QVariantMap enumeration(const char *type, const char *value);
QVariantMap unit(double, const char * = "#Pxl");
QVariantMap rgb(core::Rgba8);
QByteArray descriptor(const QVariantMap &, const char *classId);
QByteArray engine(const QVariant &);
QByteArray blend(core::BlendMode, bool descriptor = false);
struct Channel {
  int id{};
  qint64 offset{};
  quint32 length{};
};
struct Record {
  QRect bounds;
  QString name;
  QByteArray blend{"norm"};
  int opacity{255}, label{}, section{};
  bool visible{true}, clipping{false};
  QRect maskBounds;
  int maskOutside{255};
  bool mask{false}, maskEnabled{true};
  QMap<QByteArray, QByteArray> tags;
  std::vector<Channel> channels;
};
// One spool shared by all records. Only one source raster and compression row
// need to be resident; channel data is copied to atomic output in bounded
// chunks.
Channel spoolChannel(QIODevice &, int id, const QImage &, int component,
                     const std::atomic_bool &, std::uint64_t fileLimit);
void write(QIODevice &, QIODevice &spool, const std::vector<Record> &,
           const QImage &composite, core::CanvasSpec, const std::atomic_bool &,
           std::uint64_t fileLimit);
// Independent seek-based structural walk before publication; no full-file copy.
void verify(QIODevice &, core::CanvasSpec, int records);
} // namespace imageeditor::ui::psdwrite
