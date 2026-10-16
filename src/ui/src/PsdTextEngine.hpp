#pragma once
#include <QMap>
#include <QVariant>

namespace imageeditor::ui::psdwrite {
// Export-local modern text records. Both TySh and Txt2 are generated from the
// same resolved runs; imported glyph caches are never reused after edits.
class TextEngine {
public:
  int append(const QVariantMap &body, const QVariantMap &resources);
  QByteArray bytes() const;

private:
  QMap<QString, int> fontIndices_;
  QVariantList fonts_, frames_, objects_;
};
} // namespace imageeditor::ui::psdwrite
