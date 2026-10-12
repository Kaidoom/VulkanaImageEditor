# Native Windows coverage. Linux-specific resource-limit/desktop harnesses stay
# in the full suite; shared behavior is tested from its original test sources.
function(vulkana_windows_test name source)
    add_executable(${name} ${source} "${PROJECT_SOURCE_DIR}/res/windows/Vulkana.manifest")
    target_link_libraries(${name} PRIVATE imageeditor_ui Qt6::Test)
    imageeditor_enable_warnings(${name})
    file(GLOB icons "${PROJECT_SOURCE_DIR}/assets/icons/*.svg")
    qt_add_resources(${name} "${name}_icons" PREFIX "/" BASE "${PROJECT_SOURCE_DIR}/assets"
        FILES ${icons} "${PROJECT_SOURCE_DIR}/assets/Vulkana512.png")
    add_test(NAME ${name} COMMAND ${name})
    # Qt's offscreen plugin has no native Windows font database. Use the native
    # QPA even for noninteractive tests so shaping/metrics use real system fonts.
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_FORCE_STDERR_LOGGING=1" TIMEOUT 120)
endfunction()
vulkana_windows_test(imageeditor_windows_platform_tests WindowsPlatformTests.cpp)
vulkana_windows_test(imageeditor_core_tests CoreTests.cpp)
vulkana_windows_test(imageeditor_project_file_tests ProjectFileTests.cpp)
target_link_libraries(imageeditor_project_file_tests PRIVATE MINIZIP::minizip-ng)
target_compile_definitions(imageeditor_project_file_tests PRIVATE IMAGEEDITOR_PROJECT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/projects")
vulkana_windows_test(imageeditor_project_lifecycle_tests ProjectLifecycleTests.cpp)
vulkana_windows_test(imageeditor_document_tabs_tests DocumentTabsTests.cpp)
vulkana_windows_test(imageeditor_document_transfer_tests DocumentTransferTests.cpp)
vulkana_windows_test(imageeditor_layer_mask_tests LayerMaskTests.cpp)
vulkana_windows_test(imageeditor_adjustment_layer_tests AdjustmentLayerTests.cpp)
vulkana_windows_test(imageeditor_object_selection_tests ObjectSelectionTests.cpp)
target_compile_definitions(imageeditor_object_selection_tests PRIVATE VULKANA_TEST_OBJECT_SELECTION_BUNDLE="${VULKANA_OBJECT_SELECTION_BUNDLE}")
vulkana_windows_test(imageeditor_navigator_tests NavigatorTests.cpp)
vulkana_windows_test(imageeditor_localization_tests LocalizationTests.cpp)
target_compile_definitions(imageeditor_localization_tests PRIVATE VULKANA_TEST_TRANSLATIONS="${VULKANA_TRANSLATIONS_DIR}")
vulkana_windows_test(imageeditor_blend_math_tests BlendMathTests.cpp)
vulkana_windows_test(imageeditor_blend_persistence_tests BlendPersistenceTests.cpp)
target_link_libraries(imageeditor_blend_persistence_tests PRIVATE MINIZIP::minizip-ng)
vulkana_windows_test(imageeditor_psd_export_tests PsdExportTests.cpp)
vulkana_windows_test(imageeditor_pdf_export_tests PdfExportTests.cpp)
target_link_libraries(imageeditor_pdf_export_tests PRIVATE Qt6::Pdf qpdf::libqpdf)
set_tests_properties(imageeditor_pdf_export_tests PROPERTIES TIMEOUT 180)
add_test(NAME imageeditor_localization_catalogs COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/localization.py" validate)

# Interactive desktop only: run explicitly next to the staged app's DLLs/data.
# A session-0/offscreen run cannot inspect Windows' actual alpha composition.
add_executable(imageeditor_windows_overlay_tests WindowsOverlayTests.cpp
    "${PROJECT_SOURCE_DIR}/res/windows/Vulkana.manifest")
target_link_libraries(imageeditor_windows_overlay_tests PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_windows_overlay_tests)
