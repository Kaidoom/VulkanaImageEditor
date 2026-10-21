# Native Windows coverage. Linux-specific resource-limit/desktop harnesses stay
# in the full suite; shared behavior is tested from its original test sources.
# Relocatable tests use the same executable-relative shader lookup as the app.
# Populate their build-tree runtime assets before tests, without an install or
# package step (and without falling back to another installed application).
add_custom_target(imageeditor_windows_test_assets
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${PROJECT_BINARY_DIR}/src/render/shaders"
        "$<TARGET_FILE_DIR:imageeditor_blend_rendering_tests>/../share/vulkana-editor/shaders"
    DEPENDS imageeditor_shaders VERBATIM)
set_tests_properties(imageeditor_layer_drag_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_FORCE_STDERR_LOGGING=1" RUN_SERIAL TRUE)
add_executable(imageeditor_windows_thumbnail_tests WindowsThumbnailTests.cpp)
set_target_properties(imageeditor_windows_thumbnail_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_link_libraries(imageeditor_windows_thumbnail_tests PRIVATE ole32 uuid shell32 shlwapi gdi32)
# The Shell loader deliberately ignores PATH. Exercise the same sibling-DLL
# layout in a clean build tree as in an installed portable application.
add_custom_target(imageeditor_windows_thumbnail_test_runtime
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        $<TARGET_RUNTIME_DLLS:vulkana-thumbnail-codec>
        "$<TARGET_FILE_DIR:vulkana-thumbnail-codec>"
    DEPENDS vulkana-thumbnail-codec vulkana-thumbnail-provider
    COMMAND_EXPAND_LISTS VERBATIM)
add_dependencies(imageeditor_windows_thumbnail_tests imageeditor_windows_thumbnail_test_runtime)
add_executable(imageeditor_windows_project_integration_tests WindowsProjectIntegrationTests.cpp)
target_link_libraries(imageeditor_windows_project_integration_tests PRIVATE imageeditor_platform)
add_test(NAME imageeditor_windows_project_integration_tests COMMAND imageeditor_windows_project_integration_tests)
find_program(VULKANA_WINDOWS_POWERSHELL NAMES powershell.exe REQUIRED)
add_test(NAME imageeditor_windows_build_scripts COMMAND "${VULKANA_WINDOWS_POWERSHELL}"
    -NoProfile -ExecutionPolicy RemoteSigned -File "${CMAKE_CURRENT_SOURCE_DIR}/WindowsBuildScriptTests.ps1")
set_tests_properties(imageeditor_windows_build_scripts PROPERTIES TIMEOUT 30 LABELS "platform;build"
    # A native CTest process can inherit PowerShell 7's module search path when
    # invoking Windows PowerShell. Let each shell initialize its own modules.
    ENVIRONMENT_MODIFICATION "PSModulePath=unset:")
find_program(VULKANA_POWERSHELL_CORE NAMES pwsh.exe)
if(VULKANA_POWERSHELL_CORE)
    add_test(NAME imageeditor_windows_build_scripts_core COMMAND "${VULKANA_POWERSHELL_CORE}"
        -NoProfile -ExecutionPolicy RemoteSigned -File "${CMAKE_CURRENT_SOURCE_DIR}/WindowsBuildScriptTests.ps1")
    set_tests_properties(imageeditor_windows_build_scripts_core PROPERTIES TIMEOUT 30 LABELS "platform;build"
        ENVIRONMENT_MODIFICATION "PSModulePath=unset:")
endif()

function(vulkana_windows_test name source)
    add_executable(${name} ${source} "${PROJECT_SOURCE_DIR}/res/windows/Vulkana.manifest")
    add_dependencies(${name} imageeditor_windows_test_assets)
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
target_link_libraries(imageeditor_project_file_tests PRIVATE MINIZIP::minizip-ng imageeditor_project_preview)
target_compile_definitions(imageeditor_project_file_tests PRIVATE IMAGEEDITOR_PROJECT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/projects")
add_test(NAME imageeditor_windows_thumbnail_provider COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/WindowsThumbnailTests.py"
    "$<TARGET_FILE:imageeditor_project_file_tests>" "$<TARGET_FILE:imageeditor_windows_thumbnail_tests>"
    "$<TARGET_FILE:vulkana-thumbnail-provider>")
set_tests_properties(imageeditor_windows_thumbnail_provider PROPERTIES TIMEOUT 90 LABELS "project;thumbnail;windows")
vulkana_windows_test(imageeditor_project_lifecycle_tests ProjectLifecycleTests.cpp)
vulkana_windows_test(imageeditor_document_tabs_tests DocumentTabsTests.cpp)
add_test(NAME imageeditor_windows_document_layer_drag COMMAND imageeditor_document_tabs_tests --layer-drag)
set_tests_properties(imageeditor_windows_document_layer_drag PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_FORCE_STDERR_LOGGING=1" TIMEOUT 60 RUN_SERIAL TRUE LABELS "layers;input;tabs;windows")
vulkana_windows_test(imageeditor_document_transfer_tests DocumentTransferTests.cpp)
vulkana_windows_test(imageeditor_layer_mask_tests LayerMaskTests.cpp)
vulkana_windows_test(imageeditor_adjustment_layer_tests AdjustmentLayerTests.cpp)
vulkana_windows_test(imageeditor_object_selection_tests ObjectSelectionTests.cpp)
target_compile_definitions(imageeditor_object_selection_tests PRIVATE VULKANA_TEST_OBJECT_SELECTION_BUNDLE="${VULKANA_OBJECT_SELECTION_BUNDLE}")
vulkana_windows_test(imageeditor_navigator_tests NavigatorTests.cpp)
vulkana_windows_test(imageeditor_gradient_editor_tests GradientEditorTests.cpp)
vulkana_windows_test(imageeditor_color_panel_tests ColorPanelTests.cpp)
vulkana_windows_test(imageeditor_properties_inspector_tests PropertiesInspectorTests.cpp)
vulkana_windows_test(imageeditor_quick_canvas_resize_tests QuickCanvasResizeTests.cpp)
vulkana_windows_test(imageeditor_text_controller_tests TextControllerTests.cpp)
target_compile_definitions(imageeditor_text_controller_tests PRIVATE
    IMAGEEDITOR_TEXT_FONT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts")
vulkana_windows_test(imageeditor_theme_preferences_tests ThemePreferencesTests.cpp)
vulkana_windows_test(imageeditor_localization_tests LocalizationTests.cpp)
target_compile_definitions(imageeditor_localization_tests PRIVATE VULKANA_TEST_TRANSLATIONS="${VULKANA_TRANSLATIONS_DIR}")
vulkana_windows_test(imageeditor_update_tests UpdateServiceTests.cpp)
vulkana_windows_test(imageeditor_menu_clipboard_tests MenuClipboardTests.cpp)
vulkana_windows_test(imageeditor_clipboard_image_tests ClipboardImageTests.cpp)
vulkana_windows_test(imageeditor_screenshot_tests ScreenshotTests.cpp)
vulkana_windows_test(imageeditor_performance_tests PerformanceTests.cpp)
target_link_libraries(imageeditor_update_tests PRIVATE Qt6::Network)
target_compile_definitions(imageeditor_update_tests PRIVATE
    VULKANA_TEST_VERSION="${VULKANA_VERSION}"
    VULKANA_TEST_TRANSLATIONS="${VULKANA_TRANSLATIONS_DIR}")
vulkana_windows_test(imageeditor_blend_math_tests BlendMathTests.cpp)
vulkana_windows_test(imageeditor_blend_persistence_tests BlendPersistenceTests.cpp)
target_link_libraries(imageeditor_blend_persistence_tests PRIVATE MINIZIP::minizip-ng)
vulkana_windows_test(imageeditor_fill_opacity_tests FillOpacityTests.cpp)
vulkana_windows_test(imageeditor_psd_import_tests PsdImportTests.cpp)
target_link_libraries(imageeditor_psd_import_tests PRIVATE ZLIB::ZLIB)
vulkana_windows_test(imageeditor_psd_import_interaction_tests PsdImportInteractionTests.cpp)
vulkana_windows_test(imageeditor_psd_export_tests PsdExportTests.cpp)
vulkana_windows_test(imageeditor_blend_rendering_tests BlendRenderingTests.cpp)
target_link_libraries(imageeditor_blend_rendering_tests PRIVATE Vulkan::Vulkan)
foreach(mode fill-opacity effects encoded-compositing)
    string(REPLACE "-" "_" name "${mode}")
    add_test(NAME imageeditor_${name}_vulkan COMMAND imageeditor_blend_rendering_tests --${mode} --validation)
    set_tests_properties(imageeditor_${name}_vulkan PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_FORCE_STDERR_LOGGING=1" TIMEOUT 180 RUN_SERIAL TRUE LABELS "vulkan;blend;effects")
endforeach()
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
file(GLOB _windows_overlay_icons "${PROJECT_SOURCE_DIR}/assets/icons/*.svg")
qt_add_resources(imageeditor_windows_overlay_tests "windows_overlay_icons" PREFIX "/" BASE "${PROJECT_SOURCE_DIR}/assets"
    FILES ${_windows_overlay_icons} "${PROJECT_SOURCE_DIR}/assets/Vulkana512.png")

add_executable(imageeditor_windows_screenshot_tests WindowsScreenshotTests.cpp
    "${PROJECT_SOURCE_DIR}/res/windows/Vulkana.manifest")
target_link_libraries(imageeditor_windows_screenshot_tests PRIVATE imageeditor_ui Qt6::Test user32)
imageeditor_enable_warnings(imageeditor_windows_screenshot_tests)
