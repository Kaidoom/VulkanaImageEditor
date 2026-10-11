#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/platform/ApplicationPaths.hpp"
#include <QApplication>
#include <QAbstractSpinBox>
#include <QFile>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QDir>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibraryInfo>
#include <QLocale>
#include <QPersistentModelIndex>
#include <QSettings>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QTimer>
#include <algorithm>

static void initializeLocalizationResources() { Q_INIT_RESOURCE(localization); }
namespace imageeditor::ui::i18n {
QString QtUiTranslator::translate(const char* context, const char* source, const char* comment, int n) const {
    // Qt posts application language-change events after installTranslator().
    // Prevent even a transient global RTL flip: it can reorder QMainWindow
    // toolbars/docks before setLayoutDirection(LTR) is reached.
    if (qstrcmp(source, "QT_LAYOUT_DIRECTION") == 0) return QStringLiteral("LTR");
    return QTranslator::translate(context, source, comment, n);
}
Localization& Localization::instance() { static Localization service; return service; }
Localization::Localization() { initializeLocalizationResources(); }
void Localization::initialize(QString trustedDirectory) {
    if (initialized_) return;
    initialized_ = true;
    // UI language does not change numeric syntax. ASCII digits, decimal point,
    // and no grouping remain stable in all editor controls and file formats.
    QLocale::setDefault(QLocale::c());
    if (!fallback_.load(QStringLiteral(":/i18n/en.qm"))) qWarning("Embedded English catalog is unavailable");
    directory_ = std::move(trustedDirectory);
    if (directory_.isEmpty()) {
#ifdef VULKANA_DEVELOPMENT_TRANSLATIONS
        directory_ = QStringLiteral(VULKANA_DEVELOPMENT_TRANSLATIONS);
#else
        directory_ = platform::installedDataPath(QStringLiteral("translations"));
#endif
    }
    // Always ship metadata; optional/invalid packs are not advertised.
    QFile metadata(QDir(directory_).filePath(QStringLiteral("languages.json")));
    if (!metadata.exists()) metadata.setFileName(QStringLiteral(":/i18n/languages.json"));
    languages_.append({QStringLiteral("en"), QStringLiteral("English"), false, false});
    if (metadata.open(QIODevice::ReadOnly)) {
        for (const auto& value : QJsonDocument::fromJson(metadata.readAll()).array()) {
            const auto o = value.toObject(); const auto id = o["id"].toString();
            if (id == "en" || !o["enabled"].toBool(true)) continue;
            // Metadata is data, not a pathname or an instruction to load plugins.
            if (!QRegularExpression(QStringLiteral("^[a-z]{2,3}(?:[-_][A-Za-z]{2,4})?$")).match(id).hasMatch()
                || o["nativeName"].toString().isEmpty()
                || std::any_of(languages_.begin(), languages_.end(), [&](const auto& l) { return l.id == id; })) continue;
            QTranslator probe;
            const auto qtCatalog = o["qtCatalog"].toString(QString(id).replace('-', '_'));
            if (!QRegularExpression(QStringLiteral("^[a-z]{2,3}(?:_[A-Za-z]{2,4})?$")).match(qtCatalog).hasMatch()) continue;
            if (probe.load(QDir(directory_).filePath(id + ".qm")) && !probe.isEmpty())
                languages_.append({id, o["nativeName"].toString(), o["rtl"].toBool(), o["reviewPending"].toBool(), qtCatalog});
        }
    }
    (void)english_.load(QDir(directory_).filePath(QStringLiteral("en.qm")));
    QFile history(QStringLiteral(":/i18n/history-ids.json"));
    if (history.open(QIODevice::ReadOnly)) {
        const auto values = QJsonDocument::fromJson(history.readAll()).object();
        for (auto it = values.begin(); it != values.end(); ++it) historyIds_.insert(it.key(), it.value().toString().toUtf8());
    }
    if (qApp) qApp->installEventFilter(this);
    setLanguage(QSettings().value(QStringLiteral("preferences/ui/language"), QStringLiteral("system")).toString());
}
QList<Language> Localization::languages() const { return languages_; }
QString matchLanguage(const QStringList& candidates, const QList<Language>& available) {
    const auto exact = [&](QString candidate) -> QString {
        candidate.replace('_', '-');
        for (const auto& language : available)
            if (QString(language.id).replace('_', '-').compare(candidate, Qt::CaseInsensitive) == 0) return language.id;
        return {};
    };
    for (auto candidate : candidates) {
        candidate.replace('_', '-');
        if (auto found = exact(candidate); !found.isEmpty()) return found;
        const QLocale requested(candidate);
        if (requested.language() != QLocale::C) {
            // Qt supplies likely scripts/territories: zh-TW/HK -> Hant,
            // zh-CN/SG -> Hans. An explicit script still takes precedence.
            for (const auto& language : available) {
                const auto parts = QString(language.id).replace('_', '-').split('-');
                if (parts.size() != 2) continue;
                const QLocale offered(language.id);
                if (offered.language() != requested.language()) continue;
                if ((parts[1].size() == 4 && offered.script() == requested.script())
                    || (parts[1].size() == 2 && offered.territory() == requested.territory())) return language.id;
            }
            if (auto found = exact(QLocale::languageToCode(requested.language())); !found.isEmpty()) return found;
        }
    }
    return {};
}
QString Localization::resolveLanguage(const QString& requested) const {
    if (requested == "qps" && QFile::exists(QDir(directory_).filePath("qps.qm"))) return requested;
    const auto found = matchLanguage(requested == "system" ? QLocale::system().uiLanguages() : QStringList{requested}, languages_);
    return found.isEmpty() ? QStringLiteral("en") : found;
}
bool Localization::isRtl() const {
    for (const auto& language : languages_) if (language.id == language_) return language.rtl;
    return false;
}
bool Localization::setLanguage(const QString& requested) {
    if (!initialized_) initialize();
    const QMutexLocker lock(&textMutex_);
    const auto resolved = resolveLanguage(requested);
    switching_ = true;
    if (qApp) { qApp->removeTranslator(&qt_); qApp->removeTranslator(&translated_); }
    (void)translated_.load(QDir(directory_).filePath(resolved + ".qm"));
    (void)english_.load(QDir(directory_).filePath(QStringLiteral("en.qm")));
    language_ = resolved; preference_ = requested;
    staticText_.clear();
    if (resolved != "en" && resolved != "qps") {
        QString catalog = QString(resolved).replace('-', '_');
        for (const auto& language : languages_) if (language.id == resolved) catalog = language.qtCatalog;
        const auto qtName = QStringLiteral("qtbase_") + catalog;
        if (!qt_.load(QDir(directory_).filePath(qtName + ".qm")))
            (void)qt_.load(qtName, QLibraryInfo::path(QLibraryInfo::TranslationsPath));
        if (qApp) qApp->installTranslator(&qt_);
    }
    // Never let Qt's QT_LAYOUT_DIRECTION mirror the document workspace.
    if (qApp) qApp->setLayoutDirection(Qt::LeftToRight);
    refresh(); emit languageChanged(); switching_ = false;
    return requested == "system" || requested == resolved || !matchLanguage({requested}, languages_).isEmpty();
}
void Localization::requestLanguage(QString requested) { pending_ = std::move(requested); tryPending(); }
void Localization::tryPending() {
    if (pending_.isEmpty() || switching_) return;
    if (composing_ || QApplication::mouseButtons() != Qt::NoButton || (canSwitch && !canSwitch())) return;
    const auto requested = std::exchange(pending_, {}); setLanguage(requested);
}
bool Localization::eventFilter(QObject* receiver, QEvent* event) {
    if (event->type() == QEvent::InputMethod) {
        const auto* input = static_cast<QInputMethodEvent*>(event);
        composing_ = input->preeditString().isEmpty() ? nullptr : receiver;
    }
    if (!pending_.isEmpty() && (event->type() == QEvent::InputMethod || event->type() == QEvent::MouseButtonRelease
        || event->type() == QEvent::KeyRelease || event->type() == QEvent::FocusOut))
        QTimer::singleShot(0, this, [this] { tryPending(); });
    return false;
}
QString Localization::lookup(const char* id, int n) const {
    if (!initialized_) const_cast<Localization*>(this)->initialize();
    const QMutexLocker lock(&textMutex_);
    const QByteArray key(id);
    if (n < 0) {
        const auto found = staticText_.constFind(key);
        if (found != staticText_.cend()) return *found;
    }
    auto result = language_ == "en" ? QString{} : translated_.translate(nullptr, id, nullptr, n);
    if (result.isEmpty()) result = english_.translate(nullptr, id, nullptr, n);
    if (result.isEmpty()) result = fallback_.translate(nullptr, id, nullptr, n);
    if (result.isEmpty()) { qWarning("Missing localization ID: %s", id); return QStringLiteral("Unavailable text"); }
    if (n >= 0) result.replace(QStringLiteral("%n"), QString::number(n));
    else staticText_.insert(key, result);
    return result;
}
Message text(const char* id, int n) {
    const auto resolve = [key = QByteArray(id), n] { return Localization::instance().lookup(key.constData(), n); };
    return {resolve(), resolve, QByteArray(id)};
}
Message ltrToken(QString value) {
    const auto resolve = [value = std::move(value)] {
        return Localization::instance().isRtl() ? QStringLiteral("\u2066") + value + QStringLiteral("\u2069") : value;
    };
    return {resolve(), resolve};
}
QString Localization::source(const char* id) const { const QMutexLocker lock(&textMutex_); return fallback_.translate(nullptr, id); }
Message Localization::historyText(std::string_view label) const {
    const auto value = QString::fromUtf8(label.data(), qsizetype(label.size()));
    const auto found = historyIds_.constFind(value);
    // This adapter is only for existing core command labels, never filenames,
    // layer names or arbitrary document strings. Canonical commands remain Qt-free.
    return found == historyIds_.cend() ? Message(value) : text(found->constData());
}
QString Message::source() const { return id_.isEmpty() ? QString(*this) : Localization::instance().source(id_); }
void Localization::bind(QObject* object, const QByteArray& property, Message message) {
    std::erase_if(bindings_, [](const Binding& b) { return b.object.isNull(); });
    auto found = std::find_if(bindings_.begin(), bindings_.end(), [&](const Binding& b) { return b.object == object && b.property == property; });
    const auto value = display(message);
    if (qobject_cast<QAction*>(object)) object->setProperty(property, value);
    else { const QSignalBlocker block(object); object->setProperty(property, value); }
    if (message.localized() && property == "text") {
        if (auto* label = qobject_cast<QLabel*>(object))
            label->setLayoutDirection(isRtl() ? Qt::RightToLeft : Qt::LeftToRight);
    }
    if (!message.localized()) { if (found != bindings_.end()) bindings_.erase(found); return; }
    if (found == bindings_.end()) bindings_.push_back({object, property, std::move(message), value});
    else { found->message = std::move(message); found->last = value; }
}
void property(QObject* object, const char* name, Message message) { Localization::instance().bind(object, name, std::move(message)); }
QString Localization::display(const Message& message) const {
    auto value = message.resolved();
    return shortcutFormatter_ && value.contains(QStringLiteral("{{")) ? shortcutFormatter_(value) : value;
}
bool Localization::isBound(QObject* object, const char* name) const {
    return std::any_of(bindings_.begin(), bindings_.end(), [&](const auto& b) { return b.object == object && b.property == name; });
}
void Localization::setShortcutFormatter(std::function<QString(QString)> formatter) {
    shortcutFormatter_ = std::move(formatter); refresh();
}
void Localization::refresh() {
    std::erase_if(bindings_, [](const Binding& b) { return !b.object || b.object->property(b.property).toString() != b.last; });
    for (auto& b : bindings_) {
        b.last = display(b.message);
        // QAction::changed updates default-action toolbuttons. Text/tooltip
        // changes never emit triggered/toggled; suppressing changed leaves
        // cached button labels and accessibility text in the previous language.
        if (qobject_cast<QAction*>(b.object)) b.object->setProperty(b.property, b.last);
        else { const QSignalBlocker block(b.object); b.object->setProperty(b.property, b.last); }
        if (b.property == "text") {
            if (auto* label = qobject_cast<QLabel*>(b.object))
                label->setLayoutDirection(isRtl() ? Qt::RightToLeft : Qt::LeftToRight);
        }
    }
    std::erase_if(directions_, [](const auto& widget) { return widget.isNull(); });
    for (auto widget : directions_) widget->setLayoutDirection(isRtl() ? Qt::RightToLeft : Qt::LeftToRight);
}
void Localization::direction(QWidget* widget) {
    if (std::find(directions_.begin(), directions_.end(), widget) == directions_.end()) directions_.push_back(widget);
    widget->setLayoutDirection(isRtl() ? Qt::RightToLeft : Qt::LeftToRight);
    // Direction is a presentation choice, never a different number syntax.
    for (auto* number : widget->findChildren<QAbstractSpinBox*>()) {
        number->setLayoutDirection(Qt::LeftToRight); number->setLocale(QLocale::c());
    }
}
void addItem(QComboBox* combo, Message message, const QVariant& data) {
    combo->addItem(message.resolved(), data);
    if (!message.localized()) return;
    const QPersistentModelIndex item(combo->model()->index(combo->count() - 1, combo->modelColumn(), combo->rootModelIndex()));
    // Review/font controls repopulate often. Removed entries must not retain
    // translated callbacks for the whole lifetime of the dialog.
    auto* lifetime = new QObject(combo);
    QObject::connect(combo->model(), &QAbstractItemModel::modelReset, lifetime, &QObject::deleteLater);
    QObject::connect(combo->model(), &QAbstractItemModel::rowsRemoved, lifetime, [lifetime, item] {
        if (!item.isValid()) lifetime->deleteLater();
    });
    QObject::connect(&Localization::instance(), &Localization::languageChanged, lifetime, [combo, item, message] {
        if (!item.isValid()) return;
        // The view must receive DisplayRole changes to invalidate cached text
        // layout/row geometry (including fallback-font metrics for Arabic).
        // Suppress only parameter signals from the field, not model updates.
        const QSignalBlocker block(combo);
        combo->model()->setData(item, message.resolved(), Qt::DisplayRole);
        combo->update();
    });
}
void addItems(QComboBox* combo, std::initializer_list<Message> items) { for (const auto& item : items) addItem(combo, item); }
void addItems(QComboBox* combo, const QList<Message>& items) { for (const auto& item : items) addItem(combo, item); }
int addTab(QTabWidget* tabs, QWidget* page, Message message) {
    const auto index = tabs->addTab(page, message.resolved());
    if (message.localized()) QObject::connect(&Localization::instance(), &Localization::languageChanged, page, [tabs, page, message] {
        const auto i = tabs->indexOf(page); if (i >= 0) { const QSignalBlocker block(tabs); tabs->setTabText(i, message.resolved()); }
    });
    return index;
}
void addRow(QFormLayout* form, Message message, QWidget* field) {
    auto* label = make<QLabel>(std::move(message)); label->setBuddy(field);
    form->addRow(label, field);
}
ButtonBox::ButtonBox(StandardButtons buttons, QWidget* parent) : QDialogButtonBox(buttons, parent) {
    for (const auto& [kind, id] : {std::pair{Ok, "dialog.button.ok"},
            {Cancel, "dialog.button.cancel"}, {Apply, "dialog.button.apply"}, {Close, "dialog.button.close"}})
        if (auto* b = button(kind)) setText(b, text(id));
}
bool ButtonBox::event(QEvent* event) {
    // These texts (including custom Create/Apply labels) are already live-bound.
    // Qt's queued LanguageChange would otherwise replace them with platform
    // labels after our refresh, and detach the bindings on the next switch.
    if (event->type() == QEvent::LanguageChange) return QWidget::event(event);
    return QDialogButtonBox::event(event);
}
} // namespace imageeditor::ui::i18n
