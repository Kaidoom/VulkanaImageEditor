add_library(imageeditor_ui STATIC
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
    src/ColorSelector.cpp
    src/ClipboardImage.cpp
    src/LayerListModel.cpp
    src/LayerListView.cpp
    src/MainWindow.cpp
    src/MainWindowCrop.cpp
    src/CropOptionsPage.cpp
    src/MainWindowAdjustments.cpp
    src/MainWindowMeasure.cpp
    src/MainWindowLayerOrganization.cpp
    src/MainWindowFiles.cpp
    src/MainWindowUpdates.cpp
    src/MainWindowDocuments.cpp
    src/ImageExport.cpp
    src/FileDialogLocations.cpp
    src/ExportDialog.cpp
    src/MainWindowExport.cpp
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
    src/MainWindowColorSelection.cpp
    src/MainWindowSmartSelection.cpp
    src/MainWindowLasso.cpp
    src/MainWindowSelectionTransform.cpp
    src/MainWindowFill.cpp
    src/ToolIcons.cpp
    src/NewDocumentDialog.cpp
    src/OverlayDockWorkspace.cpp
    src/PopupOwnership.cpp
    src/PropertiesPanel.cpp
    src/QtRasterImageLoader.cpp
    src/PdfImport.cpp
    src/PdfImportDialog.cpp
    src/MainWindowPdf.cpp
    src/QtTextLayoutService.cpp
    src/TextController.cpp
    src/FontFamilyPicker.cpp
    src/TextClipboard.cpp
    src/Theme.cpp
    src/ThemeSettings.cpp
    src/PreferencesDialog.cpp
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
if(IMAGEEDITOR_RELOCATABLE_BUNDLE)
    target_compile_definitions(imageeditor_ui PRIVATE VULKANA_APPIMAGE_BUILD=1)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(MINIZIP REQUIRED IMPORTED_TARGET GLOBAL minizip>=4.0)
target_link_libraries(imageeditor_ui PRIVATE PkgConfig::MINIZIP)
pkg_check_modules(WEBP REQUIRED IMPORTED_TARGET libwebp>=1.2 libwebpmux>=1.2)
target_link_libraries(imageeditor_ui PRIVATE PkgConfig::WEBP)
target_link_libraries(imageeditor_ui PRIVATE Qt6::Network Qt6::Pdf qpdf::libqpdf)
