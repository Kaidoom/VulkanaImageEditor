# Linguist/Python are build tools only. Runtime loads local .qm packs via Qt.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(VULKANA_LRELEASE NAMES lrelease-qt6 lrelease6 lrelease
    HINTS /usr/lib64/qt6/bin /usr/lib/qt6/bin REQUIRED)
set(VULKANA_TRANSLATIONS_DIR "${PROJECT_BINARY_DIR}/translations")
file(GLOB _catalogs CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/localization/*.ts")
set(_qm_files "${VULKANA_TRANSLATIONS_DIR}/en.qm" "${VULKANA_TRANSLATIONS_DIR}/qps.qm")
foreach(_catalog IN LISTS _catalogs)
    get_filename_component(_language "${_catalog}" NAME_WE)
    list(APPEND _qm_files "${VULKANA_TRANSLATIONS_DIR}/${_language}.qm")
endforeach()
list(REMOVE_DUPLICATES _qm_files)
add_custom_command(OUTPUT ${_qm_files} "${VULKANA_TRANSLATIONS_DIR}/history-ids.json"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/localization.py" compile
        --output "${VULKANA_TRANSLATIONS_DIR}" --lrelease "${VULKANA_LRELEASE}"
    DEPENDS ${_catalogs} "${PROJECT_SOURCE_DIR}/localization/languages.json"
        "${PROJECT_SOURCE_DIR}/tools/localization.py" VERBATIM)
add_custom_target(vulkana_language_packs DEPENDS ${_qm_files} "${VULKANA_TRANSLATIONS_DIR}/history-ids.json")
set_source_files_properties("${VULKANA_TRANSLATIONS_DIR}/en.qm" PROPERTIES GENERATED TRUE)
file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/localization.qrc" CONTENT
    "<RCC><qresource prefix=\"/i18n\"><file alias=\"en.qm\">${VULKANA_TRANSLATIONS_DIR}/en.qm</file><file alias=\"languages.json\">${PROJECT_SOURCE_DIR}/localization/languages.json</file><file alias=\"history-ids.json\">${VULKANA_TRANSLATIONS_DIR}/history-ids.json</file></qresource></RCC>")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/localization/languages.json")
file(READ "${PROJECT_SOURCE_DIR}/localization/languages.json" _language_metadata)
string(JSON _language_count LENGTH "${_language_metadata}")
math(EXPR _language_last "${_language_count} - 1")
set(_shipping_qms "${VULKANA_TRANSLATIONS_DIR}/qps.qm")
set(_qt_catalogs)
foreach(_index RANGE ${_language_last})
    string(JSON _enabled GET "${_language_metadata}" ${_index} enabled)
    if(_enabled)
        string(JSON _id GET "${_language_metadata}" ${_index} id)
        string(JSON _qt_catalog GET "${_language_metadata}" ${_index} qtCatalog)
        list(APPEND _shipping_qms "${VULKANA_TRANSLATIONS_DIR}/${_id}.qm")
        if(NOT _id STREQUAL "en")
            list(APPEND _qt_catalogs "${_qt_catalog}")
        endif()
    endif()
endforeach()
# Planned catalogs are editable in the repository, not advertised/installed as
# empty language packs. Their Qt names can differ from our public BCP-47 IDs.
install(FILES ${_shipping_qms} "${PROJECT_SOURCE_DIR}/localization/languages.json"
    DESTINATION "${VULKANA_DATA_DIR}/translations")
# Qt's native dialogs/buttons use its own catalogs, never copied into ours.
get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
execute_process(COMMAND "${_qmake}" -query QT_INSTALL_TRANSLATIONS OUTPUT_VARIABLE _qt_translations OUTPUT_STRIP_TRAILING_WHITESPACE)
foreach(_lang IN LISTS _qt_catalogs)
    if(EXISTS "${_qt_translations}/qtbase_${_lang}.qm")
        install(FILES "${_qt_translations}/qtbase_${_lang}.qm" DESTINATION "${VULKANA_DATA_DIR}/translations")
    elseif(EXISTS "/usr/share/qt6/translations/qtbase_${_lang}.qm")
        install(FILES "/usr/share/qt6/translations/qtbase_${_lang}.qm" DESTINATION "${VULKANA_DATA_DIR}/translations")
    endif()
endforeach()
