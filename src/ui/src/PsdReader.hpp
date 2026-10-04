#pragma once
// All binary descriptors stay behind this IO boundary; never project payloads.
#include "imageeditor/ui/PsdImport.hpp"
#include <QByteArray>
#include <QColorSpace>
#include <QRect>
#include <QVariant>
#include <stdexcept>

namespace imageeditor::ui::psd {
struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};
struct Unsupported : Error {
  using Error::Error;
};
struct Cancelled : Error {
  Cancelled() : Error("Cancelled") {}
};
void check(const std::shared_ptr<PsdJob> &);
struct Reader {
  QByteArray storage;
  QByteArrayView data;
  qsizetype pos{};
  explicit Reader(QByteArrayView input) : data(input) {}
  explicit Reader(QByteArray input) : storage(std::move(input)), data(storage) {}
  qsizetype left() const { return data.size() - pos; }
  QByteArrayView take(qsizetype);
  Reader section(qsizetype n) { return Reader(take(n)); }
  void skip(qsizetype n) { (void)take(n); }
  quint8 u8();
  quint16 u16();
  quint32 u32();
  qint32 i32();
  double f64();
  QByteArray key();
  QString unicode();
};
QVariantMap descriptor(Reader &, int depth = 0);
QVariantMap versionedDescriptor(Reader &);
QVariantMap engineData(QByteArrayView);
QString utf16(QByteArrayView);
double number(const QVariant &, double fallback = 0);
QString enumeration(const QVariant &);
struct Channel {
  int id;
  qsizetype offset{}, length{};
};
struct Mask {
  QRect bounds;
  int outside{255};
  quint8 flags{};
  int density{255};
  double feather{};
  bool present{}, real{};
};
struct Record {
  QRect bounds;
  QString name;
  QByteArray blend;
  int opacity{255}, fill{255}, section{}, parent{-1};
  bool visible{true}, clipping{}, blendIf{}, knockout{};
  Mask mask;
  std::vector<Channel> channels;
  QMap<QByteArray, QByteArray> tags;
  QStringList issues;
};
QByteArray decodeChannel(QByteArrayView, int width, int height,
                         const std::shared_ptr<PsdJob> &);
std::shared_ptr<PsdSource> parse(QByteArray, const std::shared_ptr<PsdJob> &,
                                 PsdLimits);
QImage pixels(const PsdSource &, const Record &,
              const std::shared_ptr<PsdJob> &);
QImage composite(const PsdSource &, const std::shared_ptr<PsdJob> &);
} // namespace imageeditor::ui::psd
namespace imageeditor::ui {
struct PsdSource {
  QByteArray bytes;
  QSize size;
  double ppi{72};
  double globalLightAngle{120}, globalLightAltitude{30};
  int channels{}, depth{}, mode{};
  qsizetype compositeOffset{};
  bool realComposite{true}, compositeAlpha{};
  QColorSpace colorSpace;
  QString colorDescription, fingerprint;
  std::vector<psd::Record> records;
};
} // namespace imageeditor::ui
