# One install manifest for local staging and CPack. The reviewed CPU inference
# bundle is installed unchanged; development/test runtimes are never included.
install(DIRECTORY "${VULKANA_OBJECT_SELECTION_BUNDLE}/"
    DESTINATION "${VULKANA_DATA_DIR}/object-selection/mobilesam-v1"
    FILES_MATCHING PATTERN "*.onnx" PATTERN "*.so" PATTERN "*.json" PATTERN "*.txt")
install(FILES config/ui-layout.ini DESTINATION "${VULKANA_DATA_DIR}")
# Only this optional public/extractable client configuration is distributed.
# Never install the private directory or separate signing files as a whole.
install(FILES private/services.json DESTINATION "${VULKANA_DATA_DIR}" OPTIONAL)
install(FILES assets/brush/registry-v1.json DESTINATION "${VULKANA_DATA_DIR}/brush"
    RENAME registry.json)
install(DIRECTORY assets/brush/packaged/v1/ DESTINATION "${VULKANA_DATA_DIR}/brush")
install(DIRECTORY assets/brush-presets/ DESTINATION "${VULKANA_DATA_DIR}/brush-presets"
    FILES_MATCHING PATTERN "*.iebrush")
install(DIRECTORY assets/icons/ DESTINATION "${VULKANA_DATA_DIR}/icons"
    FILES_MATCHING PATTERN "*.svg")
install(FILES assets/Vulkana512.png
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps" RENAME vulkana-editor.png)
install(FILES assets/Vulkana512.png
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/mimetypes" RENAME application-x-vulkana.png)
install(FILES res/linux/vulkana-editor.desktop DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
install(FILES res/linux/vulkana-editor.xml DESTINATION "${CMAKE_INSTALL_DATADIR}/mime/packages")
install(FILES LICENSE THIRD_PARTY.md assets/brush/ASSET_PROVENANCE.md
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/vulkana-editor")
file(READ "${VULKANA_EFFECTIVE_NOTICES_DIRECTORY}/notices.json" _vulkana_notice_json)
string(JSON _vulkana_notice_version GET "${_vulkana_notice_json}" version)
string(JSON _vulkana_notice_channel GET "${_vulkana_notice_json}" release_channel)
if(NOT _vulkana_notice_version STREQUAL VULKANA_VERSION OR NOT _vulkana_notice_channel STREQUAL VULKANA_RELEASE_CHANNEL)
    message(FATAL_ERROR "Regenerate the offline notices: their version/channel does not match the build metadata.")
endif()
install(FILES
    "${VULKANA_EFFECTIVE_NOTICES_DIRECTORY}/notices.json"
    "${VULKANA_EFFECTIVE_NOTICES_DIRECTORY}/THIRD-PARTY-NOTICES.txt"
    DESTINATION "${VULKANA_DATA_DIR}/notices")
install(FILES docs/VULKANA_FORMAT.md DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(FILES docs/PSD_IMPORT.md DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(FILES THIRD_PARTY.md docs/HEALING_ALGORITHM.md
    DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(FILES docs/spatial-filter-kernels.md docs/SPATIAL_FILTER_PIPELINE.md docs/LOCAL_BLUR.md
    DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(FILES docs/SPOT_HEAL_ALGORITHM.md
    DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(FILES res/linux/imageeditor.1 DESTINATION "${CMAKE_INSTALL_MANDIR}/man1")
