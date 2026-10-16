#pragma once
#include "PsdReader.hpp"
#include <QMap>
#include <QRawFont>

namespace imageeditor::ui {
inline int psdFontWeight(const QRawFont& font) {
  // Qt's raw-font metadata can include synthetic styling. For a static face,
  // the OS/2 table identifies the weight actually supplied by the font file.
  if (font.fontTable("fvar").isEmpty()) {
    try {
      const auto data = font.fontTable("OS/2");
      psd::Reader reader(data); reader.skip(4);
      return reader.u16();
    } catch (const psd::Error&) {}
  }
  return font.weight();
}
inline bool psdFontItalic(const QRawFont& font) {
  if (font.fontTable("fvar").isEmpty()) {
    try {
      const auto data = font.fontTable("OS/2");
      psd::Reader reader(data); reader.skip(62);
      return bool(reader.u16() & (1 | 512)); // Italic or oblique fsSelection.
    } catch (const psd::Error&) {}
  }
  return font.style() != QFont::StyleNormal;
}
// PSD references PostScript faces, including named variable-font instances.
// The base name (name ID 6) alone turns e.g. Noto Sans Bold into Regular.
inline QString psdFontName(const QRawFont& font) {
  QMap<int, QString> names;
  try {
    const auto data = font.fontTable("name");
    psd::Reader reader(data);
    reader.skip(2);
    const auto count = reader.u16(), start = reader.u16();
    if (count > 4096) return {};
    for (int i = 0; i < count; ++i) {
      const auto platform = reader.u16();
      reader.skip(2);
      const auto language = reader.u16(), id = reader.u16(), length = reader.u16(), offset = reader.u16();
      psd::Reader value(data);
      value.skip(qsizetype(start) + offset);
      const auto bytes = value.take(length);
      const auto name = platform == 0 || platform == 3 ? psd::utf16(bytes) : QString::fromLatin1(bytes);
      if (!names.contains(id) || (platform == 3 && language == 0x409)) names[id] = name;
    }
    const auto variations = font.fontTable("fvar");
    if (!variations.isEmpty()) {
      psd::Reader fvar(variations);
      if (fvar.u16() != 1 || fvar.u16() != 0) return names.value(6);
      const auto startAxes = fvar.u16();
      fvar.skip(2);
      const auto axes = fvar.u16(), axisSize = fvar.u16(), instances = fvar.u16(), instanceSize = fvar.u16();
      if (axes > 64 || instances > 4096 || axisSize < 20 || instanceSize < 4 + 4 * axes) return {};
      for (int i = 0; i < instances; ++i) {
        psd::Reader instance(variations);
        instance.skip(qsizetype(startAxes) + qsizetype(axes) * axisSize + qsizetype(i) * instanceSize);
        const auto subfamily = instance.u16();
        if (names.value(subfamily).compare(font.styleName(), Qt::CaseInsensitive) != 0) continue;
        instance.skip(2 + 4 * axes);
        if (instanceSize >= 6 + 4 * axes) {
          const auto postscript = instance.u16();
          if (postscript != 0xffff && !names.value(postscript).isEmpty()) return names.value(postscript);
        }
        // Adobe Technical Note #5902: named instance prefix + '-' + subfamily,
        // with only ASCII alphanumerics retained in the subfamily component.
        const auto asciiName = [](const QString& value) {
          QString result;
          for (const auto c : value)
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) result += c;
          return result;
        };
        const auto prefix = names.contains(25) ? names.value(25) : asciiName(names.value(16));
        const auto suffix = asciiName(names.value(subfamily));
        if (!prefix.isEmpty() && !suffix.isEmpty() && prefix.size() + suffix.size() + 1 <= 127) return prefix + '-' + suffix;
        return {}; // Do not silently identify a nondefault face as Regular.
      }
    }
    return names.value(6);
  } catch (const psd::Error&) {
    return {};
  }
}
} // namespace imageeditor::ui
