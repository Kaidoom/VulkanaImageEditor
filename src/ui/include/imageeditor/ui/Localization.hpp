#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTranslator>
#include <QVariant>
#include <QHash>
#include <QMutex>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QAction>
#include <QMenu>
#include <QMenuBar>
#include <QTabWidget>
#include <functional>
#include <memory>
#include <vector>
class QFormLayout;

namespace imageeditor::ui::i18n {
struct Language { QString id, nativeName; bool rtl {}, reviewPending {}; QString qtCatalog {}; };
// Empty means no installed match. Exposed independently of pack loading so
// future language/script choices can be tested without advertising empty packs.
QString matchLanguage(const QStringList& candidates, const QList<Language>& available);
class QtUiTranslator final : public QTranslator {
public:
    QString translate(const char* context, const char* source, const char* comment = nullptr, int n = -1) const override;
};

// Only presentation strings carry a resolver. Document strings and serialized
// names stay QString/std::string and must never be looked up by their contents.
class Message : public QString {
public:
    Message() = default;
    Message(const char* literal) : QString(QString::fromUtf8(literal)) {}
    Message(const QString& literal) : QString(literal) {}
    Message(QString value, std::function<QString()> resolver, QByteArray id = {})
        : QString(std::move(value)), resolver_(std::move(resolver)), id_(std::move(id)) {}
    QString resolved() const { return resolver_ ? resolver_() : QString(*this); }
    Message refreshed() const { return {resolved(), resolver_, id_}; }
    bool localized() const { return bool(resolver_); }
    QString source() const;
    Message toUpper() const { const auto f = [self=*this] { return self.resolved().toUpper(); }; return {f(),f,id_}; }
    Message toLower() const { const auto f = [self=*this] { return self.resolved().toLower(); }; return {f(),f,id_}; }
    Message toHtmlEscaped() const { const auto f = [self=*this] { return self.resolved().toHtmlEscaped(); }; return {f(),f,id_}; }
    friend Message operator+(const Message& a, const QString& b) {
        const auto f = [a,b] { return a.resolved() + b; }; return {f(),f,a.id_};
    }
    friend Message operator+(const Message& a, const char* b) { return a + QString::fromUtf8(b); }
    friend Message operator+(const QString& a, const Message& b) {
        const auto f = [a,b] { return a + b.resolved(); }; return {f(),f};
    }
    friend Message operator+(QString&& a, const Message& b) {
        const auto f = [a=std::move(a),b] { return a + b.resolved(); }; return {f(),f};
    }
    friend Message operator+(const char* a, const Message& b) {
        const auto f = [a=QString::fromUtf8(a),b] { return a + b.resolved(); }; return {f(),f};
    }
    friend Message operator+(const Message& a, const Message& b) {
        const auto f = [a,b] { return a.resolved() + b.resolved(); }; return {f(),f};
    }
    Message& operator+=(const Message& other) { *this = *this + other; return *this; }
    Message& operator+=(const QString& other) { *this = *this + other; return *this; }
    Message& operator+=(const char* other) { *this = *this + other; return *this; }
    template<class... Args> Message arg(Args... args) const {
        const auto resolve = [self = *this, args...] { return self.resolved().arg(argument(args)...); };
        return {resolve(), resolve};
    }
private:
    static QString argument(const Message& value) { return value.resolved(); }
    template<class T> static T argument(const T& value) { return value; }
    std::function<QString()> resolver_;
    QByteArray id_;
};

class Localization final : public QObject {
    Q_OBJECT
public:
    static Localization& instance();
    void initialize(QString trustedDirectory = {});
    QList<Language> languages() const;
    QString resolveLanguage(const QString& requested) const;
    QString language() const { return language_; }
    QString preference() const { return preference_; }
    bool isRtl() const;
    bool setLanguage(const QString& requested);
    // A requested switch waits for the existing interaction boundary, not an
    // artificial commit/reset of a text composition or painting gesture.
    void requestLanguage(QString requested);
    std::function<bool()> canSwitch;
    QString lookup(const char* id, int n = -1) const;
    QString source(const char* id) const;
    Message historyText(std::string_view canonicalLabel) const;
    void bind(QObject*, const QByteArray& property, Message);
    void direction(QWidget*); // Opt in only content/forms, never workspace docks.
    bool isBound(QObject*, const char*) const;
    void setShortcutFormatter(std::function<QString(QString)> formatter);
signals:
    void languageChanged();
private:
    Localization();
    bool eventFilter(QObject*, QEvent*) override;
    void tryPending();
    QString directory_, language_ = QStringLiteral("en"), preference_ = QStringLiteral("system");
    QString pending_;
    bool initialized_ {}, switching_ {};
    QPointer<QObject> composing_;
    QTranslator fallback_, english_, translated_;
    QtUiTranslator qt_;
    QList<Language> languages_;
    mutable QHash<QByteArray, QString> staticText_;
    mutable QRecursiveMutex textMutex_;
    QHash<QString, QByteArray> historyIds_;
    std::function<QString(QString)> shortcutFormatter_;
    QString display(const Message&) const;
    struct Binding { QPointer<QObject> object; QByteArray property; Message message; QString last; };
    std::vector<Binding> bindings_;
    std::vector<QPointer<QWidget>> directions_;
    void refresh();
};
Message text(const char* id, int n = -1);
// Presentation only: keep versions, ASCII identifiers and similar tokens in
// reading order inside RTL prose. Never store isolates in document data.
Message ltrToken(QString value);
void property(QObject*, const char*, Message);
inline void setText(QObject* o, Message m) { property(o, "text", std::move(m)); }
inline void setToolTip(QObject* o, Message m) { property(o, "toolTip", std::move(m)); }
inline void setWindowTitle(QObject* o, Message m) { property(o, "windowTitle", std::move(m)); }
inline void setAccessibleName(QObject* o, Message m) { property(o, "accessibleName", std::move(m)); }
inline void setPlaceholderText(QObject* o, Message m) { property(o, "placeholderText", std::move(m)); }
inline void setPrefix(QObject* o, Message m) { property(o, "prefix", std::move(m)); }
inline void setSuffix(QObject* o, Message m) { property(o, "suffix", std::move(m)); }
template<class T, class... Args> T* make(Message message, Args&&... args) {
    auto* object = new T(QString(message), std::forward<Args>(args)...);
    property(object, "text", std::move(message));
    return object;
}
template<class T, class... Args> T* make(const QIcon& icon, Message message, Args&&... args) {
    auto* object = new T(icon, QString(message), std::forward<Args>(args)...);
    property(object, "text", std::move(message)); return object;
}
template<class Owner, class... Args> QAction* addAction(Owner* owner, Message message, Args&&... args) {
    auto* action = owner->addAction(QString(message), std::forward<Args>(args)...);
    setText(action, std::move(message)); return action;
}
template<class Owner, class... Args> QAction* addAction(Owner* owner, const QIcon& icon, Message message, Args&&... args) {
    auto* action = owner->addAction(icon, QString(message), std::forward<Args>(args)...);
    setText(action, std::move(message)); return action;
}
template<class Owner> QMenu* addMenu(Owner* owner, Message message) {
    auto* menu = owner->addMenu(QString(message));
    property(menu, "title", std::move(message)); return menu;
}
void addItem(QComboBox*, Message, const QVariant& data = {});
void addItems(QComboBox*, std::initializer_list<Message>);
void addItems(QComboBox*, const QList<Message>&);
int addTab(QTabWidget*, QWidget*, Message);
void addRow(QFormLayout*, Message, QWidget*);
// Keep Qt's button roles/default-button behavior, but use the app's selected
// language, not the desktop platform theme's standard-button wording.
class ButtonBox final : public QDialogButtonBox {
public:
    explicit ButtonBox(StandardButtons, QWidget* parent = nullptr);
protected:
    bool event(QEvent*) override;
};
} // namespace imageeditor::ui::i18n
