add_library(imageeditor_ui STATIC
    src/Localization.cpp
    include/imageeditor/ui/Localization.hpp
    ${PROJECT_BINARY_DIR}/qrc_localization.cpp
    src/AboutDialog.cpp
    src/FeedbackDialog.cpp
    src/FeedbackService.cpp
    src/ThirdPartyNoticesDialog.cpp
    src/UpdateService.cpp
    src/AbsolutePositionSlider.cpp
    src/AdjustmentCodec.cpp
    src/SpatialFilterCodec.cpp
    src/LayerEffectCodec.cpp
    src/AdjustmentCurveEditor.cpp
    src/AdjustmentHistograms.cpp
    src/AdjustmentsPanel.cpp
    src/FiltersPanel.cpp
    src/EffectsPanel.cpp
    src/EffectsPanelBevel.cpp
    src/MainWindowEffects.cpp
    src/MainWindowFilters.cpp
    src/BrushAssetLibrary.cpp
    src/BrushComponentPicker.cpp
    src/BrushOptionsPage.cpp
    src/CloningOptionsPage.cpp
    src/MainWindowCloning.cpp
    src/MainWindowSpotHeal.cpp
    src/LocalBlurOptionsPage.cpp
    src/MainWindowBlur.cpp
    src/BrushPresetGrid.cpp
    src/BrushPresetStore.cpp
    src/CompactValueControl.cpp
    src/CrossWindowPointerRouter.cpp
    src/ColorPanel.cpp
    src/SwatchesPanel.cpp
    src/NavigatorPanel.cpp
    src/ColorPicker.cpp
    src/ColorDialog.cpp
    include/imageeditor/ui/ColorDialog.hpp
    src/ColorSelector.cpp
    src/ClipboardImage.cpp
    src/LayerListModel.cpp
    src/LayerListView.cpp
    src/MainWindow.cpp
    src/MemoryStatusLabel.cpp
    src/MainWindowCrop.cpp
    src/CropOptionsPage.cpp
    src/MainWindowAdjustments.cpp
    src/MainWindowMeasure.cpp
    src/MainWindowLayerOrganization.cpp
    src/MainWindowLayerMasks.cpp
    src/MainWindowFiles.cpp
    src/MainWindowUpdates.cpp
    src/MainWindowDocuments.cpp
    src/ImageExport.cpp
    src/PdfExport.cpp
    src/PdfExportPanel.cpp
    src/FileDialogLocations.cpp
    src/ExportDialog.cpp
    src/MainWindowExport.cpp
    src/MainWindowClipboard.cpp
    src/ScreenCapture.cpp
    include/imageeditor/ui/ScreenCapture.hpp
    src/ScreenshotPreview.cpp
    src/ScreenshotDialog.cpp
    include/imageeditor/ui/ScreenshotDialog.hpp
    src/MainWindowScreenshot.cpp
    src/ProjectFile.cpp
    src/RecentFiles.cpp
    src/RulerStrip.cpp
    src/MainWindowTransform.cpp
    src/MainWindowShape.cpp
    src/ShapeOptionsPage.cpp
    src/QtShapeRenderService.cpp
    src/FlattenedDocument.cpp
    src/NativeRasterBake.cpp
    src/PixelPreview.cpp
    src/MainWindowSelection.cpp
    src/MainWindowRefinement.cpp
    src/MainWindowColorSelection.cpp
    src/MainWindowSmartSelection.cpp
    src/ObjectSelection.cpp
    src/MainWindowLasso.cpp
    src/MainWindowSelectionTransform.cpp
    src/MainWindowFill.cpp
    src/ToolIcons.cpp
    src/NewDocumentDialog.cpp
    src/OverlayDockWorkspace.cpp
    src/OverlayDockWorkspaceTabs.cpp
    src/PopupOwnership.cpp
    src/PropertiesPanel.cpp
    src/QtRasterImageLoader.cpp
    src/PdfImport.cpp
    src/PdfImportDialog.cpp
    src/MainWindowPdf.cpp
    src/PsdReader.cpp
    src/PsdWriter.cpp
    src/PsdNativeRecords.cpp
    src/PsdExport.cpp
    src/PsdExportPanel.cpp
    src/PsdImport.cpp
    src/PsdImportDialog.cpp
    src/MainWindowPsd.cpp
    src/QtTextLayoutService.cpp
    src/TextController.cpp
    src/FontFamilyPicker.cpp
    src/TextClipboard.cpp
    src/Theme.cpp
    src/ThemeSettings.cpp
    src/PreferencesDialog.cpp
    src/ProjectIntegrationDialog.cpp
    src/EditorShortcuts.cpp
    src/ShortcutsPanel.cpp
    src/MainWindowShortcuts.cpp
    src/ToolOptionsBar.cpp
    src/ToolOptionsButton.cpp
    src/TransformOptionsPage.cpp
    src/UiLayoutConfig.cpp
    src/WorkspacePanel.cpp
    src/WorkspaceDialog.cpp
    ${PROJECT_SOURCE_DIR}/assets/brush/brush_assets.qrc
    ${PROJECT_SOURCE_DIR}/assets/editor_theme.qrc
)

set_target_properties(imageeditor_ui PROPERTIES AUTORCC ON)
add_custom_command(OUTPUT "${PROJECT_BINARY_DIR}/qrc_localization.cpp"
    COMMAND Qt6::rcc -name localization "${PROJECT_BINARY_DIR}/localization.qrc" -o "${PROJECT_BINARY_DIR}/qrc_localization.cpp"
    DEPENDS ${_qm_files} "${VULKANA_TRANSLATIONS_DIR}/history-ids.json" "${PROJECT_BINARY_DIR}/localization.qrc" "${PROJECT_SOURCE_DIR}/localization/languages.json" VERBATIM)
add_dependencies(imageeditor_ui vulkana_language_packs)
if(IMAGEEDITOR_DEVELOPMENT_RESOURCES)
    target_compile_definitions(imageeditor_ui PRIVATE VULKANA_DEVELOPMENT_TRANSLATIONS="${VULKANA_TRANSLATIONS_DIR}")
endif()

target_include_directories(imageeditor_ui
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(imageeditor_ui
    PUBLIC
        imageeditor_core
        imageeditor_render
        Qt6::Widgets
)

imageeditor_enable_warnings(imageeditor_ui)
if(IMAGEEDITOR_DEVELOPMENT_RESOURCES)
    target_compile_definitions(imageeditor_ui PRIVATE
        VULKANA_DEVELOPMENT_NOTICES="${VULKANA_EFFECTIVE_NOTICES_DIRECTORY}/notices.json")
endif()
target_compile_definitions(imageeditor_ui PRIVATE
    VULKANA_UPDATE_PACKAGE_${VULKANA_EFFECTIVE_UPDATE_PACKAGE}=1)

if(WIN32)
    target_link_libraries(imageeditor_ui PRIVATE user32 gdi32 dwmapi)
    find_package(minizip-ng 4.0 CONFIG REQUIRED GLOBAL)
    find_package(WebP CONFIG REQUIRED GLOBAL)
    target_link_libraries(imageeditor_ui PRIVATE MINIZIP::minizip-ng WebP::webp WebP::libwebpmux)
    set(MINIZIP_VERSION "${minizip-ng_VERSION}" PARENT_SCOPE)
    set(WEBP_libwebp_VERSION "${WebP_VERSION}" PARENT_SCOPE)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MINIZIP REQUIRED IMPORTED_TARGET GLOBAL minizip>=4.0)
    target_link_libraries(imageeditor_ui PRIVATE PkgConfig::MINIZIP)
    pkg_check_modules(WEBP REQUIRED IMPORTED_TARGET libwebp>=1.2 libwebpmux>=1.2)
    target_link_libraries(imageeditor_ui PRIVATE PkgConfig::WEBP)
endif()
target_link_libraries(imageeditor_ui PRIVATE Qt6::Network Qt6::Pdf qpdf::libqpdf)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(imageeditor_ui PRIVATE Qt6::DBus)
endif()
target_link_libraries(imageeditor_ui PRIVATE ZLIB::ZLIB)
target_link_libraries(imageeditor_ui PRIVATE imageeditor_project_preview)
