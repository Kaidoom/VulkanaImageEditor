#include "imageeditor/ui/Localization.hpp"
#include <QApplication>
#include <QDialogButtonBox>
#include <QFile>
#include <QPushButton>
#include <iostream>
int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    application.setOrganizationName("VulkanaLocalizationProbe");
    application.setApplicationName("InstalledPackProbe");
    auto& service = imageeditor::ui::i18n::Localization::instance();
    service.initialize(); // No source-tree macro and no explicit pack-directory override.
    if (service.languages().size() != 16 || service.resolveLanguage("zh-TW") != "zh-Hant"
        || service.resolveLanguage("pt-BR") != "pt-BR") return 5;
    for (const auto& language : service.languages()) {
        if (!service.setLanguage(language.id)) return 1;
        const auto label = imageeditor::ui::i18n::text("language.label");
        if (label.isEmpty() || (language.id != "en" && label == "Language")) return 2;
        imageeditor::ui::i18n::ButtonBox buttons(QDialogButtonBox::Cancel | QDialogButtonBox::Save);
        if (buttons.button(QDialogButtonBox::Cancel)->text() != imageeditor::ui::i18n::text("dialog.button.cancel")) return 3;
        if (QFile::exists(QCoreApplication::applicationDirPath() + "/../share/vulkana-editor/translations/qtbase_"
                + language.qtCatalog + ".qm")) {
            QDialogButtonBox nativeButtons(QDialogButtonBox::Cancel);
            if (language.id != "en" && nativeButtons.button(QDialogButtonBox::Cancel)->text().remove('&') == "Cancel") return 6;
        }
        if (application.layoutDirection() != Qt::LeftToRight) return 4;
    }
    std::cout << "Installed offline packs and Qt dialog translations passed\n";
}
