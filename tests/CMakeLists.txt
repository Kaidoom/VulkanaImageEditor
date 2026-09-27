include(ShapeTests.cmake)
include(ShapeResizeTests.cmake)
include(LayerSelectionTests.cmake)

add_executable(imageeditor_projective_transform_tests ProjectiveTransformTests.cpp)
target_link_libraries(imageeditor_projective_transform_tests PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_projective_transform_tests)
add_test(NAME imageeditor_projective_transform_tests COMMAND imageeditor_projective_transform_tests)

add_executable(imageeditor_selected_pixel_transform_tests SelectedPixelTransformTests.cpp)
target_link_libraries(imageeditor_selected_pixel_transform_tests PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_selected_pixel_transform_tests)
add_test(NAME imageeditor_selected_pixel_transform_tests COMMAND imageeditor_selected_pixel_transform_tests)
set_tests_properties(imageeditor_selected_pixel_transform_tests PROPERTIES LABELS "core;selection;transform;history" TIMEOUT 120)
add_executable(imageeditor_projective_persistence_tests ProjectivePersistenceTests.cpp)
target_link_libraries(imageeditor_projective_persistence_tests PRIVATE imageeditor_ui)
imageeditor_enable_warnings(imageeditor_projective_persistence_tests)
add_test(NAME imageeditor_projective_persistence_tests COMMAND imageeditor_projective_persistence_tests)
set_tests_properties(imageeditor_projective_persistence_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "project;transform;effects;export" TIMEOUT 60)

add_executable(imageeditor_creative_brush_tests CreativeBrushTests.cpp)
target_link_libraries(imageeditor_creative_brush_tests PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_creative_brush_tests)
add_test(NAME imageeditor_creative_brush_tests COMMAND imageeditor_creative_brush_tests)
set_tests_properties(imageeditor_creative_brush_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 90 LABELS "brush;presets;deterministic")

add_executable(imageeditor_platform_startup_tests PlatformStartupTests.cpp)
target_link_libraries(imageeditor_platform_startup_tests PRIVATE imageeditor_platform)
imageeditor_enable_warnings(imageeditor_platform_startup_tests)
add_test(NAME imageeditor_platform_startup_tests COMMAND imageeditor_platform_startup_tests)
set_tests_properties(imageeditor_platform_startup_tests PROPERTIES LABELS "platform;restart")

add_executable(imageeditor_single_instance_tests SingleInstanceTests.cpp)
target_link_libraries(imageeditor_single_instance_tests PRIVATE imageeditor_platform Qt6::Network)
imageeditor_enable_warnings(imageeditor_single_instance_tests)
add_test(NAME imageeditor_single_instance_tests COMMAND imageeditor_single_instance_tests)
set_tests_properties(imageeditor_single_instance_tests PROPERTIES TIMEOUT 40 LABELS "platform;ipc;restart")

add_executable(imageeditor_overlay_popup_ownership_tests OverlayPopupOwnershipTests.cpp)
target_link_libraries(imageeditor_overlay_popup_ownership_tests PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_overlay_popup_ownership_tests)
add_test(NAME imageeditor_overlay_popup_ownership_tests COMMAND imageeditor_overlay_popup_ownership_tests)
set_tests_properties(imageeditor_overlay_popup_ownership_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30 LABELS "widgets;popup;ownership")

add_executable(imageeditor_shortcut_tests ShortcutTests.cpp)
target_link_libraries(imageeditor_shortcut_tests PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_shortcut_tests)
add_test(NAME imageeditor_shortcut_tests COMMAND imageeditor_shortcut_tests)
set_tests_properties(imageeditor_shortcut_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 45 LABELS "shortcuts;focus;preferences")
if(NOT "$ENV{WAYLAND_DISPLAY}" STREQUAL "")
    add_test(NAME imageeditor_wayland_shortcuts COMMAND imageeditor_shortcut_tests --native)
    set_tests_properties(imageeditor_wayland_shortcuts PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=wayland" RUN_SERIAL TRUE TIMEOUT 60 LABELS "shortcuts;focus;wayland;vulkan")
endif()

add_executable(imageeditor_smart_wand_tests smart_wand_tests.cpp)
target_link_libraries(imageeditor_smart_wand_tests PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_smart_wand_tests)
add_test(NAME imageeditor_smart_wand_tests COMMAND imageeditor_smart_wand_tests)
set_tests_properties(imageeditor_smart_wand_tests PROPERTIES LABELS "core;selection;wand;history;deterministic")

add_executable(imageeditor_quick_selection_tests QuickSelectionTests.cpp)
target_link_libraries(imageeditor_quick_selection_tests PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_quick_selection_tests)
add_test(NAME imageeditor_quick_selection_tests COMMAND imageeditor_quick_selection_tests)
set_tests_properties(imageeditor_quick_selection_tests PROPERTIES TIMEOUT 90 LABELS "core;selection;quick;deterministic")

add_executable(imageeditor_smart_select_art_tests SmartSelectArtTests.cpp)
target_link_libraries(imageeditor_smart_select_art_tests PRIVATE imageeditor_core Qt6::Gui)
target_compile_definitions(imageeditor_smart_select_art_tests PRIVATE IMAGEEDITOR_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
imageeditor_enable_warnings(imageeditor_smart_select_art_tests)
add_test(NAME imageeditor_smart_select_art_tests COMMAND imageeditor_smart_select_art_tests)
set_tests_properties(imageeditor_smart_select_art_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120 LABELS "core;selection;quick;art;deterministic")

add_executable(imageeditor_smart_reference_tests SmartReferenceTests.cpp)
target_link_libraries(imageeditor_smart_reference_tests PRIVATE imageeditor_ui)
imageeditor_enable_warnings(imageeditor_smart_reference_tests)
add_test(NAME imageeditor_smart_reference_tests COMMAND imageeditor_smart_reference_tests)
set_tests_properties(imageeditor_smart_reference_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60 LABELS "selection;reference;typed;crop;adjustments")

add_executable(imageeditor_smart_selection_interaction_tests SmartSelectionInteractionTests.cpp)
target_link_libraries(imageeditor_smart_selection_interaction_tests PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_smart_selection_interaction_tests)
add_test(NAME imageeditor_smart_selection_interaction_tests COMMAND imageeditor_smart_selection_interaction_tests)
set_tests_properties(imageeditor_smart_selection_interaction_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 90 LABELS "selection;smart;widgets;history")
if(NOT "$ENV{WAYLAND_DISPLAY}" STREQUAL "")
    add_test(NAME imageeditor_wayland_smart_selection COMMAND imageeditor_smart_selection_interaction_tests --wayland-validation)
    set_tests_properties(imageeditor_wayland_smart_selection PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=wayland" TIMEOUT 90 RUN_SERIAL TRUE LABELS "selection;smart;wayland;vulkan")
endif()

add_executable(imageeditor_brush_constraint_tests BrushConstraintTests.cpp)
target_link_libraries(imageeditor_brush_constraint_tests PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_brush_constraint_tests)
add_test(NAME imageeditor_brush_constraint_tests COMMAND imageeditor_brush_constraint_tests)
set_tests_properties(imageeditor_brush_constraint_tests PROPERTIES LABELS "core;brush;eraser;history;constraint")
add_executable(imageeditor_brush_constraint_ui_tests BrushConstraintUiTests.cpp)
target_link_libraries(imageeditor_brush_constraint_ui_tests PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_brush_constraint_ui_tests)
add_test(NAME imageeditor_brush_constraint_ui_tests COMMAND imageeditor_brush_constraint_ui_tests)
set_tests_properties(imageeditor_brush_constraint_ui_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30 LABELS "brush;input;widgets")
if(NOT "$ENV{WAYLAND_DISPLAY}" STREQUAL "")
    add_test(NAME imageeditor_wayland_brush_constraint COMMAND imageeditor_brush_constraint_ui_tests --native)
    set_tests_properties(imageeditor_wayland_brush_constraint PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=wayland" TIMEOUT 30 RUN_SERIAL TRUE LABELS "brush;wayland;vulkan")
endif()

add_executable(imageeditor_adjustment_core_tests AdjustmentCoreTests.cpp)
target_link_libraries(imageeditor_adjustment_core_tests PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_adjustment_core_tests)
add_test(NAME imageeditor_adjustment_core_tests COMMAND imageeditor_adjustment_core_tests)
set_tests_properties(imageeditor_adjustment_core_tests PROPERTIES TIMEOUT 60 LABELS "adjustments;core;history;deterministic")

add_executable(imageeditor_adjustment_integration_tests AdjustmentIntegrationTests.cpp)
add_executable(imageeditor_layer_effects_tests LayerEffectsTests.cpp)
add_test(NAME imageeditor_layer_effects_vulkan COMMAND imageeditor_blend_rendering_tests --effects --validation)
set_tests_properties(imageeditor_layer_effects_vulkan PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 180 LABELS "effects;vulkan")
target_link_libraries(imageeditor_layer_effects_tests PRIVATE imageeditor_ui Qt6::Test)
add_test(NAME imageeditor_layer_effects_tests COMMAND imageeditor_layer_effects_tests)
set_tests_properties(imageeditor_layer_effects_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120 LABELS "effects;project;history;sampling")

add_executable(imageeditor_spatial_filter_integration_tests SpatialFilterIntegrationTests.cpp)
target_link_libraries(imageeditor_spatial_filter_integration_tests PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_spatial_filter_integration_tests)
add_test(NAME imageeditor_spatial_filter_integration_tests COMMAND imageeditor_spatial_filter_integration_tests)
set_tests_properties(imageeditor_spatial_filter_integration_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120 LABELS "filters;project;history;sampling")
target_link_libraries(imageeditor_adjustment_integration_tests PRIVATE imageeditor_ui Qt6::Test PkgConfig::MINIZIP)
imageeditor_enable_warnings(imageeditor_adjustment_integration_tests)
add_test(NAME imageeditor_adjustment_integration_tests COMMAND imageeditor_adjustment_integration_tests)
set_tests_properties(imageeditor_adjustment_integration_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60 LABELS "adjustments;project;history")
add_executable(imageeditor_adjustment_ui_tests AdjustmentUiTests.cpp)
add_executable(imageeditor_filters_ui_tests FiltersUiTests.cpp)
target_link_libraries(imageeditor_filters_ui_tests PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_filters_ui_tests)
add_test(NAME imageeditor_filters_ui_tests COMMAND imageeditor_filters_ui_tests)
set_tests_properties(imageeditor_filters_ui_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 90 LABELS "filters;widgets;history;async")
if(NOT "$ENV{WAYLAND_DISPLAY}" STREQUAL "")
    add_test(NAME imageeditor_wayland_filters COMMAND imageeditor_filters_ui_tests --native)
    set_tests_properties(imageeditor_wayland_filters PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=wayland" TIMEOUT 90 RUN_SERIAL TRUE SKIP_RETURN_CODE 77 LABELS "filters;wayland;vulkan;async")
endif()
target_link_libraries(imageeditor_adjustment_ui_tests PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_adjustment_ui_tests)
add_test(NAME imageeditor_adjustment_ui_tests COMMAND imageeditor_adjustment_ui_tests)
set_tests_properties(imageeditor_adjustment_ui_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60 LABELS "adjustments;widgets;history")
if(NOT "$ENV{WAYLAND_DISPLAY}" STREQUAL "")
    add_test(NAME imageeditor_wayland_adjustment_smoke COMMAND imageeditor_adjustment_ui_tests --wayland-validation)
    set_tests_properties(imageeditor_wayland_adjustment_smoke PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=wayland" TIMEOUT 60 RUN_SERIAL TRUE LABELS "adjustments;integration;wayland;vulkan")
endif()

add_executable(imageeditor_blend_math_tests BlendMathTests.cpp)
target_link_libraries(imageeditor_blend_math_tests PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_blend_math_tests)
add_test(NAME imageeditor_blend_math_tests COMMAND imageeditor_blend_math_tests)
add_executable(imageeditor_blend_persistence_tests BlendPersistenceTests.cpp)
target_link_libraries(imageeditor_blend_persistence_tests PRIVATE imageeditor_ui PkgConfig::MINIZIP)
imageeditor_enable_warnings(imageeditor_blend_persistence_tests)
add_test(NAME imageeditor_blend_persistence_tests COMMAND imageeditor_blend_persistence_tests)
set_tests_properties(imageeditor_blend_persistence_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
add_executable(imageeditor_blend_integration_tests BlendIntegrationTests.cpp)
target_link_libraries(imageeditor_blend_integration_tests PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_blend_integration_tests)
add_test(NAME imageeditor_blend_integration_tests COMMAND imageeditor_blend_integration_tests)
set_tests_properties(imageeditor_blend_integration_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
add_executable(imageeditor_blend_rendering_tests BlendRenderingTests.cpp)
target_link_libraries(imageeditor_blend_rendering_tests PRIVATE imageeditor_ui Vulkan::Vulkan)
imageeditor_enable_warnings(imageeditor_blend_rendering_tests)
target_compile_options(imageeditor_blend_rendering_tests PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wno-missing-field-initializers>)
add_test(NAME imageeditor_blend_rendering_tests COMMAND imageeditor_blend_rendering_tests)
set_tests_properties(imageeditor_blend_rendering_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60 RUN_SERIAL TRUE LABELS "vulkan;blend")
add_test(NAME imageeditor_adjustment_rendering_tests COMMAND imageeditor_blend_rendering_tests --adjustments --validation)
add_test(NAME imageeditor_spatial_filter_rendering_tests COMMAND imageeditor_blend_rendering_tests --filters --validation)
set_tests_properties(imageeditor_spatial_filter_rendering_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120 RUN_SERIAL TRUE LABELS "vulkan;filters")
set_tests_properties(imageeditor_adjustment_rendering_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60 RUN_SERIAL TRUE LABELS "vulkan;adjustments")
add_executable(imageeditor_blend_ui_tests BlendUiTests.cpp)
target_link_libraries(imageeditor_blend_ui_tests PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_blend_ui_tests)
add_test(NAME imageeditor_blend_ui_tests COMMAND imageeditor_blend_ui_tests)
set_tests_properties(imageeditor_blend_ui_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)

add_executable(imageeditor_theme_preferences_tests ThemePreferencesTests.cpp)
target_link_libraries(imageeditor_theme_preferences_tests PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_theme_preferences_tests)
add_test(NAME imageeditor_theme_preferences_tests COMMAND imageeditor_theme_preferences_tests)
set_tests_properties(imageeditor_theme_preferences_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30 LABELS "theme;preferences;widgets")

add_executable(imageeditor_update_tests UpdateServiceTests.cpp)
add_executable(imageeditor_feedback_tests FeedbackDialogTests.cpp)
add_executable(imageeditor_document_tabs_tests DocumentTabsTests.cpp)
target_link_libraries(imageeditor_document_tabs_tests PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_document_tabs_tests)
add_test(NAME imageeditor_document_tabs_tests COMMAND imageeditor_document_tabs_tests)
set_tests_properties(imageeditor_document_tabs_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30 LABELS "documents;tabs;widgets")
add_executable(imageeditor_document_transfer_tests DocumentTransferTests.cpp)
target_link_libraries(imageeditor_document_transfer_tests PRIVATE imageeditor_ui)
imageeditor_enable_warnings(imageeditor_document_transfer_tests)
add_test(NAME imageeditor_document_transfer_tests COMMAND imageeditor_document_transfer_tests)
set_tests_properties(imageeditor_document_transfer_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30 LABELS "documents;transfer;history")
add_executable(imageeditor_document_tabs_benchmark DocumentTabsBenchmark.cpp)
target_link_libraries(imageeditor_document_tabs_benchmark PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_document_tabs_benchmark)
# Explicit native validation/profile run; never silently substitutes offscreen.
qt_add_resources(imageeditor_feedback_tests "feedback_test_icon"
    PREFIX "/" BASE "${PROJECT_SOURCE_DIR}/assets"
    FILES "${PROJECT_SOURCE_DIR}/assets/Vulkana512.png")
target_link_libraries(imageeditor_feedback_tests PRIVATE imageeditor_ui Qt6::Test Qt6::Network)
imageeditor_enable_warnings(imageeditor_feedback_tests)
add_test(NAME imageeditor_feedback_tests COMMAND imageeditor_feedback_tests)
set_tests_properties(imageeditor_feedback_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 15 LABELS "feedback;widgets;privacy")

qt_add_resources(imageeditor_update_tests "update_test_icon"
    PREFIX "/" BASE "${PROJECT_SOURCE_DIR}/assets"
    FILES "${PROJECT_SOURCE_DIR}/assets/Vulkana512.png")
target_link_libraries(imageeditor_update_tests PRIVATE imageeditor_ui Qt6::Network Qt6::Test)
target_compile_definitions(imageeditor_update_tests PRIVATE VULKANA_TEST_VERSION="${VULKANA_VERSION}")
imageeditor_enable_warnings(imageeditor_update_tests)
add_test(NAME imageeditor_update_tests COMMAND imageeditor_update_tests)
set_tests_properties(imageeditor_update_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30 LABELS "updates;widgets")

add_executable(imageeditor_widget_polish_tests WidgetPolishTests.cpp)
target_link_libraries(imageeditor_widget_polish_tests PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_widget_polish_tests)
add_test(NAME imageeditor_widget_polish_tests COMMAND imageeditor_widget_polish_tests)
set_tests_properties(imageeditor_widget_polish_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30 LABELS "theme;input;widgets")

add_executable(imageeditor_layer_row_style_tests LayerRowStyleTests.cpp)
target_link_libraries(imageeditor_layer_row_style_tests PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_layer_row_style_tests)
add_test(NAME imageeditor_layer_row_style_tests COMMAND imageeditor_layer_row_style_tests)
set_tests_properties(imageeditor_layer_row_style_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30 LABELS "theme;layers;widgets")

add_executable(imageeditor_tool_icon_tests ToolIconTests.cpp)
target_link_libraries(imageeditor_tool_icon_tests PRIVATE imageeditor_ui)
qt_add_resources(imageeditor_tool_icon_tests "tool_icon_test_resources"
    PREFIX "/" BASE "${PROJECT_SOURCE_DIR}/assets"
    FILES ${PROJECT_SOURCE_DIR}/assets/icons/text.svg)
imageeditor_enable_warnings(imageeditor_tool_icon_tests)
add_test(NAME imageeditor_tool_icon_tests COMMAND imageeditor_tool_icon_tests)
set_tests_properties(imageeditor_tool_icon_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 20 LABELS "theme;icons;widgets;hidpi")

add_executable(imageeditor_theme_preferences_interaction_tests ThemePreferencesInteractionTests.cpp)
target_link_libraries(imageeditor_theme_preferences_interaction_tests PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_theme_preferences_interaction_tests)
qt_add_resources(imageeditor_theme_preferences_interaction_tests "theme_test_icons"
    PREFIX "/" BASE "${PROJECT_SOURCE_DIR}/assets"
    FILES ${PROJECT_SOURCE_DIR}/assets/icons/brush.svg ${PROJECT_SOURCE_DIR}/assets/icons/eraser.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eye-off.svg ${PROJECT_SOURCE_DIR}/assets/icons/eye.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eyedropper.svg ${PROJECT_SOURCE_DIR}/assets/icons/fill.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/lasso.svg ${PROJECT_SOURCE_DIR}/assets/icons/layer-raster.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/marquee.svg ${PROJECT_SOURCE_DIR}/assets/icons/move.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/text.svg)
if(NOT "$ENV{WAYLAND_DISPLAY}" STREQUAL "")
    add_test(NAME imageeditor_wayland_theme_preferences COMMAND imageeditor_theme_preferences_interaction_tests)
    set_tests_properties(imageeditor_wayland_theme_preferences PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=wayland;QT_FORCE_STDERR_LOGGING=1"
        RUN_SERIAL TRUE TIMEOUT 60 LABELS "theme;preferences;integration;wayland;vulkan")
endif()

add_executable(imageeditor_clipboard_image_tests ClipboardImageTests.cpp)
target_link_libraries(imageeditor_clipboard_image_tests PRIVATE imageeditor_ui)
imageeditor_enable_warnings(imageeditor_clipboard_image_tests)
add_test(NAME imageeditor_clipboard_image_tests COMMAND imageeditor_clipboard_image_tests)
set_tests_properties(imageeditor_clipboard_image_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 20 LABELS "clipboard;image;widgets")

add_executable(imageeditor_clipboard_interaction_tests ClipboardInteractionTests.cpp)
target_link_libraries(imageeditor_clipboard_interaction_tests PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_clipboard_interaction_tests)
add_test(NAME imageeditor_clipboard_interaction_tests COMMAND imageeditor_clipboard_interaction_tests)
set_tests_properties(imageeditor_clipboard_interaction_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30 LABELS "clipboard;input;widgets")
# --system-clipboard is an explicit manual/native diagnostic only. CTest never
# overwrites or consumes the user's real Wayland clipboard to make a fixture.

add_executable(imageeditor_pointer_tooltip_tests PointerTooltipTests.cpp)
target_link_libraries(imageeditor_pointer_tooltip_tests PRIVATE imageeditor_render Qt6::Gui)
imageeditor_enable_warnings(imageeditor_pointer_tooltip_tests)
add_test(NAME imageeditor_pointer_tooltip_tests COMMAND imageeditor_pointer_tooltip_tests)
set_tests_properties(imageeditor_pointer_tooltip_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "render;tooltip;cache")

add_executable(imageeditor_pointer_tooltip_interaction_tests PointerTooltipInteractionTests.cpp)
target_link_libraries(imageeditor_pointer_tooltip_interaction_tests PRIVATE imageeditor_ui Qt6::Widgets Qt6::Test)
qt_add_resources(imageeditor_pointer_tooltip_interaction_tests "pointer_tooltip_test_icons"
    PREFIX "/" BASE "${PROJECT_SOURCE_DIR}/assets"
    FILES
        ${PROJECT_SOURCE_DIR}/assets/icons/brush.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eraser.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eye-off.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eye.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eyedropper.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/fill.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/lasso.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/layer-raster.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/marquee.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/move.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/text.svg)
imageeditor_enable_warnings(imageeditor_pointer_tooltip_interaction_tests)
add_test(NAME imageeditor_pointer_tooltip_interaction_tests COMMAND imageeditor_pointer_tooltip_interaction_tests)
set_tests_properties(imageeditor_pointer_tooltip_interaction_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "selection;transform;input;widgets;tooltip")
if(NOT "$ENV{WAYLAND_DISPLAY}" STREQUAL "")
    add_test(NAME imageeditor_wayland_pointer_tooltip_smoke COMMAND imageeditor_pointer_tooltip_interaction_tests)
    set_tests_properties(imageeditor_wayland_pointer_tooltip_smoke PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=wayland;QT_FORCE_STDERR_LOGGING=1;IMAGEEDITOR_TEST_POINTER_TOOLTIP_NATIVE=1"
        TIMEOUT 30 RUN_SERIAL TRUE LABELS "integration;wayland;vulkan;tooltip")
endif()

add_executable(imageeditor_core_tests
    CoreTests.cpp
)
set_target_properties(imageeditor_core_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_link_libraries(imageeditor_core_tests PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_core_tests)

add_test(NAME imageeditor_core_tests COMMAND imageeditor_core_tests)

add_executable(imageeditor_rich_text_tests RichTextTests.cpp)
set_target_properties(imageeditor_rich_text_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_link_libraries(imageeditor_rich_text_tests PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_rich_text_tests)
add_test(NAME imageeditor_rich_text_tests COMMAND imageeditor_rich_text_tests)
set_tests_properties(imageeditor_rich_text_tests PROPERTIES LABELS "core;text;unicode;history;deterministic")

add_executable(imageeditor_selection_tests SelectionTests.cpp)
set_target_properties(imageeditor_selection_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_link_libraries(imageeditor_selection_tests PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_selection_tests)
add_test(NAME imageeditor_selection_tests COMMAND imageeditor_selection_tests)
set_tests_properties(imageeditor_selection_tests PROPERTIES LABELS "core;selection;history;deterministic")

add_executable(imageeditor_selection_manipulation_tests SelectionManipulationTests.cpp)
set_target_properties(imageeditor_selection_manipulation_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_link_libraries(imageeditor_selection_manipulation_tests PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_selection_manipulation_tests)
add_test(NAME imageeditor_selection_manipulation_tests COMMAND imageeditor_selection_manipulation_tests)
set_tests_properties(imageeditor_selection_manipulation_tests PROPERTIES LABELS "core;selection;history;deterministic")

add_executable(imageeditor_selection_transform_tests SelectionTransformTests.cpp)
set_target_properties(imageeditor_selection_transform_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_link_libraries(imageeditor_selection_transform_tests PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_selection_transform_tests)
add_test(NAME imageeditor_selection_transform_tests COMMAND imageeditor_selection_transform_tests)
set_tests_properties(imageeditor_selection_transform_tests PROPERTIES LABELS "core;selection;transform;deterministic")

add_executable(imageeditor_selection_transform_interaction_tests SelectionTransformInteractionTests.cpp)
target_link_libraries(imageeditor_selection_transform_interaction_tests PRIVATE imageeditor_ui Qt6::Widgets Qt6::Test)
qt_add_resources(imageeditor_selection_transform_interaction_tests "selection_transform_test_icons"
    PREFIX "/" BASE "${PROJECT_SOURCE_DIR}/assets"
    FILES
        ${PROJECT_SOURCE_DIR}/assets/icons/brush.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eraser.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eye-off.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eye.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eyedropper.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/fill.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/lasso.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/layer-raster.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/marquee.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/move.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/text.svg)
imageeditor_enable_warnings(imageeditor_selection_transform_interaction_tests)
add_test(NAME imageeditor_selection_transform_interaction_tests COMMAND imageeditor_selection_transform_interaction_tests)
set_tests_properties(imageeditor_selection_transform_interaction_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "selection;transform;input;widgets;history")
if(NOT "$ENV{WAYLAND_DISPLAY}" STREQUAL "")
    add_test(NAME imageeditor_wayland_selected_pixel_transform COMMAND imageeditor_selection_transform_interaction_tests --native)
    set_tests_properties(imageeditor_wayland_selected_pixel_transform PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=wayland" RUN_SERIAL TRUE TIMEOUT 60 LABELS "selection;transform;wayland;vulkan")
endif()
if(NOT "$ENV{DISPLAY}" STREQUAL "")
    add_test(NAME imageeditor_xcb_selected_pixel_transform COMMAND imageeditor_selection_transform_interaction_tests --native)
    set_tests_properties(imageeditor_xcb_selected_pixel_transform PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=xcb" RUN_SERIAL TRUE TIMEOUT 60 LABELS "selection;transform;xcb;vulkan")
endif()

add_executable(imageeditor_selection_interaction_tests SelectionInteractionTests.cpp)
target_link_libraries(imageeditor_selection_interaction_tests PRIVATE imageeditor_ui Qt6::Widgets Qt6::Test)
qt_add_resources(imageeditor_selection_interaction_tests "selection_test_icons"
    PREFIX "/" BASE "${PROJECT_SOURCE_DIR}/assets"
    FILES
        ${PROJECT_SOURCE_DIR}/assets/icons/brush.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eraser.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eye-off.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eye.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eyedropper.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/fill.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/lasso.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/layer-raster.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/marquee.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/move.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/text.svg)
imageeditor_enable_warnings(imageeditor_selection_interaction_tests)
add_test(NAME imageeditor_selection_interaction_tests COMMAND imageeditor_selection_interaction_tests)
set_tests_properties(imageeditor_selection_interaction_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60 LABELS "selection;input;widgets;history;vulkan")

add_executable(imageeditor_layer_transform_tests LayerTransformTests.cpp)
target_link_libraries(imageeditor_layer_transform_tests PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_layer_transform_tests)
add_test(NAME imageeditor_layer_transform_tests COMMAND imageeditor_layer_transform_tests)
set_tests_properties(imageeditor_layer_transform_tests PROPERTIES LABELS "core;transform;deterministic")

add_executable(imageeditor_transformed_brush_safety_tests TransformedBrushSafetyTests.cpp)
target_link_libraries(imageeditor_transformed_brush_safety_tests PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_transformed_brush_safety_tests)
add_test(NAME imageeditor_transformed_brush_safety_tests COMMAND imageeditor_transformed_brush_safety_tests)
set_tests_properties(imageeditor_transformed_brush_safety_tests PROPERTIES LABELS "core;transform;brush;eraser;safety")

add_executable(imageeditor_layer_transform_interaction_tests LayerTransformInteractionTests.cpp)
target_link_libraries(imageeditor_layer_transform_interaction_tests PRIVATE imageeditor_ui Qt6::Widgets Qt6::Test)
qt_add_resources(imageeditor_layer_transform_interaction_tests "transform_test_icons"
    PREFIX "/" BASE "${PROJECT_SOURCE_DIR}/assets"
    FILES
        ${PROJECT_SOURCE_DIR}/assets/icons/brush.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eraser.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eye-off.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eye.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eyedropper.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/fill.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/lasso.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/layer-raster.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/marquee.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/move.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/text.svg
)
imageeditor_enable_warnings(imageeditor_layer_transform_interaction_tests)
add_test(NAME imageeditor_layer_transform_interaction_tests COMMAND imageeditor_layer_transform_interaction_tests)
set_tests_properties(imageeditor_layer_transform_interaction_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "transform;input;widgets;history;vulkan")
if(NOT "$ENV{WAYLAND_DISPLAY}" STREQUAL "")
    add_test(NAME imageeditor_wayland_group_transform_validation COMMAND imageeditor_layer_transform_interaction_tests)
    set_tests_properties(imageeditor_wayland_group_transform_validation PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=wayland;QT_FORCE_STDERR_LOGGING=1;IMAGEEDITOR_TEST_TRANSFORM_PREVIEW_NATIVE=${CMAKE_CURRENT_BINARY_DIR}/group-transform-native.png"
        TIMEOUT 40 RUN_SERIAL TRUE LABELS "integration;wayland;vulkan;transform;layers")
endif()

add_executable(imageeditor_color_sampler_tests ColorSamplerTests.cpp)
set_target_properties(imageeditor_color_sampler_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_link_libraries(imageeditor_color_sampler_tests PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_color_sampler_tests)
add_test(NAME imageeditor_color_sampler_tests COMMAND imageeditor_color_sampler_tests)
set_tests_properties(imageeditor_color_sampler_tests PROPERTIES LABELS "core;color;eyedropper;deterministic")

add_executable(imageeditor_color_selection_tests ColorSelectionTests.cpp)
set_target_properties(imageeditor_color_selection_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_link_libraries(imageeditor_color_selection_tests PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_color_selection_tests)
add_test(NAME imageeditor_color_selection_tests COMMAND imageeditor_color_selection_tests)
set_tests_properties(imageeditor_color_selection_tests PROPERTIES
    TIMEOUT 30 LABELS "core;color;selection;history;deterministic")

add_executable(imageeditor_color_selection_benchmarks ColorSelectionBenchmarks.cpp)
set_target_properties(imageeditor_color_selection_benchmarks PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_link_libraries(imageeditor_color_selection_benchmarks PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_color_selection_benchmarks)

add_executable(imageeditor_color_selection_interaction_tests ColorSelectionInteractionTests.cpp)
target_link_libraries(imageeditor_color_selection_interaction_tests PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_color_selection_interaction_tests)
qt_add_resources(imageeditor_color_selection_interaction_tests "color_selection_test_icons"
    PREFIX "/" BASE "${PROJECT_SOURCE_DIR}/assets"
    FILES
        ${PROJECT_SOURCE_DIR}/assets/icons/brush.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eraser.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eye-off.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eye.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eyedropper.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/fill.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/lasso.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/layer-raster.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/marquee.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/move.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/text.svg)
add_test(NAME imageeditor_color_selection_interaction_tests COMMAND imageeditor_color_selection_interaction_tests)
set_tests_properties(imageeditor_color_selection_interaction_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60 LABELS "selection;color;input;history")
if(NOT "$ENV{WAYLAND_DISPLAY}" STREQUAL "")
    add_test(NAME imageeditor_wayland_color_selection_smoke COMMAND imageeditor_color_selection_interaction_tests --wayland-validation)
    set_tests_properties(imageeditor_wayland_color_selection_smoke PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=wayland" TIMEOUT 60 RUN_SERIAL TRUE LABELS "selection;color;integration;wayland;vulkan")
endif()

add_executable(imageeditor_eyedropper_interaction_tests EyedropperInteractionTests.cpp)
qt_add_resources(imageeditor_eyedropper_interaction_tests "color_test_icons"
    PREFIX "/" BASE "${PROJECT_SOURCE_DIR}/assets"
    FILES
        ${PROJECT_SOURCE_DIR}/assets/icons/brush.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eraser.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eye-off.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eye.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eyedropper.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/fill.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/lasso.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/layer-raster.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/marquee.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/move.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/text.svg
)
target_link_libraries(imageeditor_eyedropper_interaction_tests PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_eyedropper_interaction_tests)
add_test(NAME imageeditor_eyedropper_interaction_tests COMMAND imageeditor_eyedropper_interaction_tests)
set_tests_properties(imageeditor_eyedropper_interaction_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60 LABELS "widgets;input;color;eyedropper")

add_executable(imageeditor_raster_edit_transaction_tests
    RasterEditTransactionTests.cpp
)
set_target_properties(imageeditor_raster_edit_transaction_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)
target_link_libraries(imageeditor_raster_edit_transaction_tests PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_raster_edit_transaction_tests)
add_test(NAME imageeditor_raster_edit_transaction_tests
    COMMAND imageeditor_raster_edit_transaction_tests)
set_tests_properties(imageeditor_raster_edit_transaction_tests PROPERTIES
    LABELS "core;raster;history;m2")

add_executable(imageeditor_basic_pixel_brush_tests
    BasicPixelBrushTests.cpp
)
target_compile_definitions(imageeditor_basic_pixel_brush_tests PRIVATE
    IMAGEEDITOR_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
target_link_libraries(imageeditor_basic_pixel_brush_tests PRIVATE
    imageeditor_core
    Qt6::Gui
)
imageeditor_enable_warnings(imageeditor_basic_pixel_brush_tests)
add_test(NAME imageeditor_basic_pixel_brush_tests
    COMMAND imageeditor_basic_pixel_brush_tests)
set_tests_properties(imageeditor_basic_pixel_brush_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    LABELS "core;raster;brush;golden;m2")

add_executable(imageeditor_eraser_tests
    EraserTests.cpp
)
target_link_libraries(imageeditor_eraser_tests PRIVATE
    imageeditor_core
    Qt6::Gui
)
imageeditor_enable_warnings(imageeditor_eraser_tests)
add_test(NAME imageeditor_eraser_tests COMMAND imageeditor_eraser_tests)
set_tests_properties(imageeditor_eraser_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    LABELS "core;raster;brush;eraser;history;performance;m2"
)

add_executable(imageeditor_advanced_brush_tests
    AdvancedBrushTests.cpp
)
target_compile_definitions(imageeditor_advanced_brush_tests PRIVATE
    IMAGEEDITOR_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
target_link_libraries(imageeditor_advanced_brush_tests PRIVATE
    imageeditor_core
    Qt6::Gui
)
imageeditor_enable_warnings(imageeditor_advanced_brush_tests)
add_test(NAME imageeditor_advanced_brush_tests
    COMMAND imageeditor_advanced_brush_tests)
set_tests_properties(imageeditor_advanced_brush_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    LABELS "core;raster;brush;golden;m2b5")

add_executable(imageeditor_brush_direction_tests
    BrushDirectionTests.cpp
)
set_target_properties(imageeditor_brush_direction_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)
target_link_libraries(imageeditor_brush_direction_tests PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_brush_direction_tests)
add_test(NAME imageeditor_brush_direction_tests
    COMMAND imageeditor_brush_direction_tests)
set_tests_properties(imageeditor_brush_direction_tests PROPERTIES
    LABELS "core;raster;brush;direction;deterministic;m2")

# Opt-in: representative 4K/5K CPU edit timings are too hardware-sensitive for
# pass/fail CTest gating. The documented M2 benchmark command runs this Release
# executable and records its CSV output.
add_executable(imageeditor_m2_benchmarks
    M2Benchmarks.cpp
)
set_target_properties(imageeditor_m2_benchmarks PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)
target_link_libraries(imageeditor_m2_benchmarks PRIVATE
    imageeditor_core imageeditor_render)
imageeditor_enable_warnings(imageeditor_m2_benchmarks)

# Opt-in 4K/5K selection, extraction and masked-edit timings (not CTest gates).
add_executable(imageeditor_selection_benchmarks SelectionBenchmarks.cpp)
set_target_properties(imageeditor_selection_benchmarks PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_link_libraries(imageeditor_selection_benchmarks PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_selection_benchmarks)

add_executable(imageeditor_ui_adapter_tests
    UiAdapterTests.cpp
)
target_link_libraries(imageeditor_ui_adapter_tests PRIVATE imageeditor_ui Qt6::Gui)
imageeditor_enable_warnings(imageeditor_ui_adapter_tests)
add_test(NAME imageeditor_ui_adapter_tests COMMAND imageeditor_ui_adapter_tests)

add_executable(imageeditor_cross_window_pointer_tests
    CrossWindowPointerRouterTests.cpp
)
target_link_libraries(imageeditor_cross_window_pointer_tests PRIVATE imageeditor_ui Qt6::Widgets)
imageeditor_enable_warnings(imageeditor_cross_window_pointer_tests)
add_test(NAME imageeditor_cross_window_pointer_tests COMMAND imageeditor_cross_window_pointer_tests)
set_tests_properties(imageeditor_cross_window_pointer_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    LABELS "input;widgets;native-window"
)

add_executable(imageeditor_properties_layout_tests
    PropertiesPanelLayoutTests.cpp
)
target_link_libraries(imageeditor_properties_layout_tests PRIVATE imageeditor_ui Qt6::Widgets)
imageeditor_enable_warnings(imageeditor_properties_layout_tests)
add_test(NAME imageeditor_properties_layout_tests COMMAND imageeditor_properties_layout_tests)
set_tests_properties(imageeditor_properties_layout_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    LABELS "layout;widgets"
)

add_executable(imageeditor_brush_asset_runtime_tests
    BrushAssetRuntimeTests.cpp
)
target_link_libraries(imageeditor_brush_asset_runtime_tests PRIVATE
    imageeditor_ui Qt6::Widgets Qt6::Test)
imageeditor_enable_warnings(imageeditor_brush_asset_runtime_tests)
add_test(NAME imageeditor_brush_asset_runtime_tests
    COMMAND imageeditor_brush_asset_runtime_tests)
set_tests_properties(imageeditor_brush_asset_runtime_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    LABELS "brush;assets;presets;widgets;m2b6"
)

add_executable(imageeditor_brush_component_golden_tests
    BrushComponentGoldenTests.cpp
)
target_compile_definitions(imageeditor_brush_component_golden_tests PRIVATE
    IMAGEEDITOR_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
target_link_libraries(imageeditor_brush_component_golden_tests PRIVATE
    imageeditor_ui Qt6::Gui)
imageeditor_enable_warnings(imageeditor_brush_component_golden_tests)
add_test(NAME imageeditor_brush_component_golden_tests
    COMMAND imageeditor_brush_component_golden_tests)
set_tests_properties(imageeditor_brush_component_golden_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    LABELS "brush;assets;golden;deterministic;m2b6"
)

add_executable(imageeditor_ui_layout_config_tests
    UiLayoutConfigTests.cpp
)
target_link_libraries(imageeditor_ui_layout_config_tests PRIVATE
    imageeditor_ui Qt6::Widgets)
imageeditor_enable_warnings(imageeditor_ui_layout_config_tests)
add_test(NAME imageeditor_ui_layout_config_tests
    COMMAND imageeditor_ui_layout_config_tests)
set_tests_properties(imageeditor_ui_layout_config_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    LABELS "layout;widgets;configuration"
)

add_executable(imageeditor_canvas_size_dialog_tests
    CanvasSizeDialogTests.cpp
)

add_executable(imageeditor_workspace_dialog_tests WorkspaceDialogTests.cpp)
qt_add_resources(imageeditor_workspace_dialog_tests "workspace_dialog_test_icon"
    PREFIX "/" BASE "${PROJECT_SOURCE_DIR}/assets"
    FILES "${PROJECT_SOURCE_DIR}/assets/Vulkana512.png")
target_link_libraries(imageeditor_workspace_dialog_tests PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_workspace_dialog_tests)
add_test(NAME imageeditor_workspace_dialog_tests COMMAND imageeditor_workspace_dialog_tests)
set_tests_properties(imageeditor_workspace_dialog_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 40 LABELS "document;layout;widgets")
if(NOT "$ENV{WAYLAND_DISPLAY}" STREQUAL "")
    add_test(NAME imageeditor_wayland_workspace_dialog COMMAND imageeditor_workspace_dialog_tests --native)
    set_tests_properties(imageeditor_wayland_workspace_dialog PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=wayland" TIMEOUT 40 RUN_SERIAL TRUE LABELS "document;wayland;vulkan;widgets")
endif()
target_link_libraries(imageeditor_canvas_size_dialog_tests PRIVATE imageeditor_ui Qt6::Widgets Qt6::Test)
imageeditor_enable_warnings(imageeditor_canvas_size_dialog_tests)
add_test(NAME imageeditor_canvas_size_dialog_tests COMMAND imageeditor_canvas_size_dialog_tests)
set_tests_properties(imageeditor_canvas_size_dialog_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    LABELS "document;layout;widgets"
)

add_executable(imageeditor_ui_interaction_tests
    UiInteractionTests.cpp
)
target_link_libraries(imageeditor_ui_interaction_tests PRIVATE
    imageeditor_ui Qt6::Widgets Qt6::Test)
imageeditor_enable_warnings(imageeditor_ui_interaction_tests)
add_test(NAME imageeditor_ui_interaction_tests COMMAND imageeditor_ui_interaction_tests)
set_tests_properties(imageeditor_ui_interaction_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    LABELS "input;widgets"
)

add_executable(imageeditor_overlay_dock_workspace_tests
    OverlayDockWorkspaceTests.cpp
)
target_link_libraries(imageeditor_overlay_dock_workspace_tests PRIVATE
    imageeditor_ui Qt6::Widgets)
imageeditor_enable_warnings(imageeditor_overlay_dock_workspace_tests)
add_test(NAME imageeditor_overlay_dock_workspace_tests
    COMMAND imageeditor_overlay_dock_workspace_tests)
set_tests_properties(imageeditor_overlay_dock_workspace_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    LABELS "layout;widgets;native-window"
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Deliberately not registered with CTest: this is an opt-in physical-input
    # diagnostic for the target KDE Wayland workstation.
    add_executable(imageeditor_wayland_pointer_probe_host
        WaylandPointerProbeHost.cpp
    )
    target_link_libraries(imageeditor_wayland_pointer_probe_host PRIVATE
        imageeditor_ui Qt6::Widgets)
    imageeditor_enable_warnings(imageeditor_wayland_pointer_probe_host)

    # Deliberately not registered with CTest: this drives live native-window
    # geometry on a multi-output Wayland desktop and emits compositor-focused
    # diagnostics for dock-resize artifacts.
    add_executable(imageeditor_wayland_dock_damage_probe
        WaylandDockDamageProbe.cpp
    )
    target_link_libraries(imageeditor_wayland_dock_damage_probe PRIVATE
        imageeditor_ui Qt6::Widgets)
    imageeditor_enable_warnings(imageeditor_wayland_dock_damage_probe)

endif()

add_executable(imageeditor_raster_orientation_tests
    RasterOrientationTests.cpp
)
target_compile_definitions(imageeditor_raster_orientation_tests PRIVATE
    IMAGEEDITOR_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
target_link_libraries(imageeditor_raster_orientation_tests PRIVATE
    imageeditor_core
    imageeditor_render
    imageeditor_ui
    Qt6::Gui
)
imageeditor_enable_warnings(imageeditor_raster_orientation_tests)
add_test(NAME imageeditor_raster_orientation_tests COMMAND imageeditor_raster_orientation_tests)

add_executable(imageeditor_canvas_resize_coalescing_tests
    CanvasResizeCoalescingTests.cpp
)
target_link_libraries(imageeditor_canvas_resize_coalescing_tests PRIVATE
    imageeditor_render
    Qt6::Gui
)
imageeditor_enable_warnings(imageeditor_canvas_resize_coalescing_tests)
add_test(NAME imageeditor_canvas_resize_coalescing_tests
    COMMAND imageeditor_canvas_resize_coalescing_tests)
set_tests_properties(imageeditor_canvas_resize_coalescing_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    LABELS "render;resize;vulkan"
)

add_executable(imageeditor_vulkan_enumeration_tests
    VulkanEnumerationTests.cpp
)
target_include_directories(imageeditor_vulkan_enumeration_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/render/include
)
target_link_libraries(imageeditor_vulkan_enumeration_tests PRIVATE Vulkan::Vulkan)
imageeditor_enable_warnings(imageeditor_vulkan_enumeration_tests)
add_test(NAME imageeditor_vulkan_enumeration_tests
    COMMAND imageeditor_vulkan_enumeration_tests)
set_tests_properties(imageeditor_vulkan_enumeration_tests PROPERTIES
    LABELS "render;vulkan"
)

add_executable(imageeditor_dirty_region_coalescer_tests
    DirtyRegionCoalescerTests.cpp
)
target_link_libraries(imageeditor_dirty_region_coalescer_tests PRIVATE
    imageeditor_render
)
imageeditor_enable_warnings(imageeditor_dirty_region_coalescer_tests)
add_test(NAME imageeditor_dirty_region_coalescer_tests
    COMMAND imageeditor_dirty_region_coalescer_tests)
set_tests_properties(imageeditor_dirty_region_coalescer_tests PROPERTIES
    LABELS "render;raster;performance;m2")

find_program(READELF_EXECUTABLE NAMES readelf REQUIRED)
add_test(NAME imageeditor_no_direct_x11
    COMMAND ${CMAKE_COMMAND}
        -DIMAGEEDITOR_BINARY=$<TARGET_FILE:imageeditor>
        -DIMAGEEDITOR_SOURCE_ROOT=${PROJECT_SOURCE_DIR}/src
        -DREADELF_EXECUTABLE=${READELF_EXECUTABLE}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/NoDirectX11.cmake
)
set_tests_properties(imageeditor_no_direct_x11 PROPERTIES LABELS "architecture;wayland")

if(NOT "$ENV{WAYLAND_DISPLAY}" STREQUAL "")
    add_test(NAME imageeditor_wayland_vulkan_smoke
        COMMAND $<TARGET_FILE:imageeditor> --smoke-test
    )
    set_tests_properties(imageeditor_wayland_vulkan_smoke PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=wayland;QT_FORCE_STDERR_LOGGING=1"
        TIMEOUT 15
        RUN_SERIAL TRUE
        LABELS "integration;wayland;vulkan"
    )
    add_test(NAME imageeditor_wayland_selection_smoke
        COMMAND $<TARGET_FILE:imageeditor_selection_interaction_tests>)
    set_tests_properties(imageeditor_wayland_selection_smoke PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=wayland;QT_FORCE_STDERR_LOGGING=1;IMAGEEDITOR_TEST_SELECTION_NATIVE=1"
        TIMEOUT 20 RUN_SERIAL TRUE LABELS "integration;wayland;vulkan;selection")
endif()

add_executable(imageeditor_fill_tests FillTests.cpp)
target_link_libraries(imageeditor_fill_tests PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_fill_tests)
add_test(NAME imageeditor_fill_tests COMMAND imageeditor_fill_tests)
set_tests_properties(imageeditor_fill_tests PROPERTIES LABELS "core;fill;selection;raster")

add_executable(imageeditor_fill_benchmarks FillBenchmarks.cpp)
target_link_libraries(imageeditor_fill_benchmarks PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_fill_benchmarks)

add_executable(imageeditor_lasso_tests LassoTests.cpp)
target_link_libraries(imageeditor_lasso_tests PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_lasso_tests)
add_test(NAME imageeditor_lasso_tests COMMAND imageeditor_lasso_tests)
set_tests_properties(imageeditor_lasso_tests PROPERTIES LABELS "core;selection;lasso;deterministic;history")
add_executable(imageeditor_lasso_benchmarks LassoBenchmarks.cpp)
target_link_libraries(imageeditor_lasso_benchmarks PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_lasso_benchmarks)

add_executable(imageeditor_lasso_modes_tests LassoModesTests.cpp)
target_link_libraries(imageeditor_lasso_modes_tests PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_lasso_modes_tests)
add_test(NAME imageeditor_lasso_modes_tests COMMAND imageeditor_lasso_modes_tests)
set_tests_properties(imageeditor_lasso_modes_tests PROPERTIES LABELS "core;selection;lasso;magnetic;history;deterministic")
add_executable(imageeditor_live_wire_benchmarks LiveWireBenchmarks.cpp)
target_link_libraries(imageeditor_live_wire_benchmarks PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_live_wire_benchmarks)

add_executable(imageeditor_lasso_modes_interaction_tests LassoModesInteractionTests.cpp)
target_link_libraries(imageeditor_lasso_modes_interaction_tests PRIVATE imageeditor_ui Qt6::Widgets Qt6::Test)
qt_add_resources(imageeditor_lasso_modes_interaction_tests "lasso_modes_test_icons"
    PREFIX "/" BASE "${PROJECT_SOURCE_DIR}/assets"
    FILES
        ${PROJECT_SOURCE_DIR}/assets/icons/brush.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eraser.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eye-off.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eye.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eyedropper.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/fill.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/lasso.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/layer-raster.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/marquee.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/move.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/text.svg)
imageeditor_enable_warnings(imageeditor_lasso_modes_interaction_tests)
add_test(NAME imageeditor_lasso_modes_interaction_tests COMMAND imageeditor_lasso_modes_interaction_tests)
set_tests_properties(imageeditor_lasso_modes_interaction_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "selection;lasso;magnetic;widgets;input;history")
if(NOT "$ENV{WAYLAND_DISPLAY}" STREQUAL "")
    add_test(NAME imageeditor_wayland_lasso_modes_smoke COMMAND imageeditor_lasso_modes_interaction_tests)
    set_tests_properties(imageeditor_wayland_lasso_modes_smoke PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=wayland;QT_FORCE_STDERR_LOGGING=1;IMAGEEDITOR_TEST_LASSO_MODES_NATIVE=1"
        TIMEOUT 40 RUN_SERIAL TRUE LABELS "integration;wayland;vulkan;lasso;magnetic")
endif()

add_executable(imageeditor_selection_toolbar_layout_tests SelectionToolbarLayoutTests.cpp)
target_link_libraries(imageeditor_selection_toolbar_layout_tests PRIVATE imageeditor_ui Qt6::Widgets Qt6::Test)
qt_add_resources(imageeditor_selection_toolbar_layout_tests "selection_toolbar_test_icons"
    PREFIX "/" BASE "${PROJECT_SOURCE_DIR}/assets"
    FILES
        ${PROJECT_SOURCE_DIR}/assets/icons/brush.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eraser.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eye-off.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eye.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eyedropper.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/fill.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/lasso.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/layer-raster.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/marquee.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/move.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/text.svg)
imageeditor_enable_warnings(imageeditor_selection_toolbar_layout_tests)
add_test(NAME imageeditor_selection_toolbar_layout_tests COMMAND imageeditor_selection_toolbar_layout_tests)
set_tests_properties(imageeditor_selection_toolbar_layout_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "selection;lasso;widgets;layout")

add_executable(imageeditor_history_allocation_tests HistoryAllocationTests.cpp)
target_link_libraries(imageeditor_history_allocation_tests PRIVATE imageeditor_core)
set_target_properties(imageeditor_history_allocation_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
imageeditor_enable_warnings(imageeditor_history_allocation_tests)
add_test(NAME imageeditor_history_allocation_tests COMMAND imageeditor_history_allocation_tests)
set_tests_properties(imageeditor_history_allocation_tests PROPERTIES LABELS "core;history;safety")

add_executable(imageeditor_lasso_interaction_tests LassoInteractionTests.cpp)
target_link_libraries(imageeditor_lasso_interaction_tests PRIVATE imageeditor_ui Qt6::Widgets Qt6::Test)
qt_add_resources(imageeditor_lasso_interaction_tests "lasso_test_icons"
    PREFIX "/" BASE "${PROJECT_SOURCE_DIR}/assets"
    FILES
        ${PROJECT_SOURCE_DIR}/assets/icons/brush.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eraser.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eye-off.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eye.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eyedropper.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/fill.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/lasso.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/layer-raster.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/marquee.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/move.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/text.svg)
imageeditor_enable_warnings(imageeditor_lasso_interaction_tests)
add_test(NAME imageeditor_lasso_interaction_tests COMMAND imageeditor_lasso_interaction_tests)
set_tests_properties(imageeditor_lasso_interaction_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "selection;lasso;input;widgets;history")
if(NOT "$ENV{WAYLAND_DISPLAY}" STREQUAL "")
    add_test(NAME imageeditor_wayland_lasso_smoke COMMAND imageeditor_lasso_interaction_tests)
    set_tests_properties(imageeditor_wayland_lasso_smoke PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=wayland;QT_FORCE_STDERR_LOGGING=1;IMAGEEDITOR_TEST_LASSO_NATIVE=1"
        TIMEOUT 30 RUN_SERIAL TRUE LABELS "integration;wayland;vulkan;lasso")
endif()

add_executable(imageeditor_ellipse_tests EllipseSelectionTests.cpp)
target_link_libraries(imageeditor_ellipse_tests PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_ellipse_tests)
add_test(NAME imageeditor_ellipse_tests COMMAND imageeditor_ellipse_tests)
set_tests_properties(imageeditor_ellipse_tests PROPERTIES LABELS "core;selection;ellipse;deterministic;history")
add_executable(imageeditor_ellipse_benchmarks EllipseBenchmarks.cpp)
target_link_libraries(imageeditor_ellipse_benchmarks PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_ellipse_benchmarks)

add_executable(imageeditor_ellipse_interaction_tests EllipseSelectionInteractionTests.cpp)
target_link_libraries(imageeditor_ellipse_interaction_tests PRIVATE imageeditor_ui Qt6::Widgets Qt6::Test)
qt_add_resources(imageeditor_ellipse_interaction_tests "ellipse_test_icons"
    PREFIX "/" BASE "${PROJECT_SOURCE_DIR}/assets"
    FILES
        ${PROJECT_SOURCE_DIR}/assets/icons/brush.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eraser.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eye-off.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eye.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eyedropper.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/fill.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/lasso.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/layer-raster.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/marquee.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/move.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/text.svg)
imageeditor_enable_warnings(imageeditor_ellipse_interaction_tests)
add_test(NAME imageeditor_ellipse_interaction_tests COMMAND imageeditor_ellipse_interaction_tests)
set_tests_properties(imageeditor_ellipse_interaction_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "selection;ellipse;input;widgets;history")
if(NOT "$ENV{WAYLAND_DISPLAY}" STREQUAL "")
    add_test(NAME imageeditor_wayland_ellipse_smoke COMMAND imageeditor_ellipse_interaction_tests)
    set_tests_properties(imageeditor_wayland_ellipse_smoke PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=wayland;QT_FORCE_STDERR_LOGGING=1;IMAGEEDITOR_TEST_ELLIPSE_NATIVE=1"
        TIMEOUT 30 RUN_SERIAL TRUE LABELS "integration;wayland;vulkan;ellipse")
endif()

add_executable(imageeditor_fill_interaction_tests FillInteractionTests.cpp)
target_link_libraries(imageeditor_fill_interaction_tests PRIVATE imageeditor_ui Qt6::Widgets Qt6::Test)
qt_add_resources(imageeditor_fill_interaction_tests "fill_test_icons"
    PREFIX "/" BASE "${PROJECT_SOURCE_DIR}/assets"
    FILES
        ${PROJECT_SOURCE_DIR}/assets/icons/brush.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eraser.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eye-off.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eye.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eyedropper.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/fill.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/lasso.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/layer-raster.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/marquee.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/move.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/text.svg)
imageeditor_enable_warnings(imageeditor_fill_interaction_tests)
add_test(NAME imageeditor_fill_interaction_tests COMMAND imageeditor_fill_interaction_tests)
set_tests_properties(imageeditor_fill_interaction_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "fill;input;widgets;history")
if(NOT "$ENV{WAYLAND_DISPLAY}" STREQUAL "")
    add_test(NAME imageeditor_wayland_fill_smoke COMMAND imageeditor_fill_interaction_tests)
    set_tests_properties(imageeditor_wayland_fill_smoke PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=wayland;QT_FORCE_STDERR_LOGGING=1;IMAGEEDITOR_TEST_FILL_NATIVE=1"
        TIMEOUT 20 RUN_SERIAL TRUE LABELS "integration;wayland;vulkan;fill")
endif()

add_executable(imageeditor_qt_text_layout_tests QtTextLayoutTests.cpp)
add_executable(imageeditor_text_interaction_tests TextInteractionTests.cpp)
target_link_libraries(imageeditor_text_interaction_tests PRIVATE imageeditor_ui Qt6::Test)
qt_add_resources(imageeditor_text_interaction_tests "text_test_icons"
    PREFIX "/" BASE "${PROJECT_SOURCE_DIR}/assets"
    FILES
        ${PROJECT_SOURCE_DIR}/assets/icons/brush.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eraser.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eye-off.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eye.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eyedropper.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/fill.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/lasso.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/layer-raster.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/marquee.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/move.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/text.svg)
imageeditor_enable_warnings(imageeditor_text_interaction_tests)
add_test(NAME imageeditor_text_interaction_tests COMMAND imageeditor_text_interaction_tests)
set_tests_properties(imageeditor_text_interaction_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30 LABELS "text;input;history;widgets")
if(NOT "$ENV{WAYLAND_DISPLAY}" STREQUAL "")
    add_test(NAME imageeditor_wayland_text_smoke COMMAND imageeditor_text_interaction_tests)
    set_tests_properties(imageeditor_wayland_text_smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=wayland;IMAGEEDITOR_TEXT_NATIVE=1" TIMEOUT 40 RUN_SERIAL TRUE LABELS "text;integration;wayland;vulkan")
endif()
target_link_libraries(imageeditor_qt_text_layout_tests PRIVATE imageeditor_ui Qt6::Gui)
target_compile_definitions(imageeditor_qt_text_layout_tests PRIVATE
    IMAGEEDITOR_TEXT_FONT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts")
imageeditor_enable_warnings(imageeditor_qt_text_layout_tests)
add_test(NAME imageeditor_qt_text_layout_tests COMMAND imageeditor_qt_text_layout_tests)
set_tests_properties(imageeditor_qt_text_layout_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 30 LABELS "text;layout;fonts;unicode;deterministic")
add_test(NAME imageeditor_qt_text_layout_hidpi_tests COMMAND imageeditor_qt_text_layout_tests)
set_tests_properties(imageeditor_qt_text_layout_hidpi_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=2;QT_FONT_DPI=144"
    TIMEOUT 30 LABELS "text;layout;fonts;unicode;hidpi;deterministic")

add_executable(imageeditor_text_controller_tests TextControllerTests.cpp)
target_link_libraries(imageeditor_text_controller_tests PRIVATE imageeditor_ui Qt6::Widgets Qt6::Test)
target_compile_definitions(imageeditor_text_controller_tests PRIVATE
    IMAGEEDITOR_TEXT_FONT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts")
imageeditor_enable_warnings(imageeditor_text_controller_tests)
add_test(NAME imageeditor_text_controller_tests COMMAND imageeditor_text_controller_tests)
set_tests_properties(imageeditor_text_controller_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 30 LABELS "text;controller;input;widgets;unicode;history")

add_executable(imageeditor_font_family_picker_tests FontFamilyPickerTests.cpp)
target_link_libraries(imageeditor_font_family_picker_tests PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_font_family_picker_tests)
add_test(NAME imageeditor_font_family_picker_tests COMMAND imageeditor_font_family_picker_tests)
set_tests_properties(imageeditor_font_family_picker_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30 LABELS "text;fonts;widgets;input;deterministic")

add_executable(imageeditor_project_checkpoint_tests ProjectCheckpointTests.cpp)
target_link_libraries(imageeditor_project_checkpoint_tests PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_project_checkpoint_tests)
add_test(NAME imageeditor_project_checkpoint_tests COMMAND imageeditor_project_checkpoint_tests)

add_executable(imageeditor_recent_files_tests RecentFilesTests.cpp)
target_link_libraries(imageeditor_recent_files_tests PRIVATE imageeditor_ui)
imageeditor_enable_warnings(imageeditor_recent_files_tests)
add_test(NAME imageeditor_recent_files_tests COMMAND imageeditor_recent_files_tests)

add_executable(imageeditor_project_file_tests ProjectFileTests.cpp)
target_link_libraries(imageeditor_project_file_tests PRIVATE imageeditor_ui PkgConfig::MINIZIP)
target_compile_definitions(imageeditor_project_file_tests PRIVATE IMAGEEDITOR_PROJECT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/projects")
imageeditor_enable_warnings(imageeditor_project_file_tests)
add_test(NAME imageeditor_project_file_tests COMMAND imageeditor_project_file_tests)
set_tests_properties(imageeditor_project_file_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)

add_executable(imageeditor_project_lifecycle_tests ProjectLifecycleTests.cpp)
target_link_libraries(imageeditor_project_lifecycle_tests PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_project_lifecycle_tests)
add_test(NAME imageeditor_project_lifecycle_tests COMMAND imageeditor_project_lifecycle_tests)
set_tests_properties(imageeditor_project_lifecycle_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60 LABELS "project;lifecycle;widgets")

add_executable(imageeditor_project_persistence_benchmarks ProjectPersistenceBenchmarks.cpp)
target_link_libraries(imageeditor_project_persistence_benchmarks PRIVATE imageeditor_ui)
imageeditor_enable_warnings(imageeditor_project_persistence_benchmarks)

add_executable(imageeditor_project_vulkan_tests ProjectVulkanTests.cpp)
target_link_libraries(imageeditor_project_vulkan_tests PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_project_vulkan_tests)
if(NOT "$ENV{WAYLAND_DISPLAY}" STREQUAL "")
    add_test(NAME imageeditor_wayland_project_smoke COMMAND imageeditor_project_vulkan_tests)
    set_tests_properties(imageeditor_wayland_project_smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=wayland" TIMEOUT 25 RUN_SERIAL TRUE LABELS "project;integration;wayland;vulkan")
endif()
include(${CMAKE_CURRENT_LIST_DIR}/QtShapeRenderTests.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/ShapeProjectTests.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/ShapeInteractionTests.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/FlattenedDocumentTests.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/LayerOrganizationTests.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/LayerHierarchyProjectTests.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/LayerOrganizationInteractionTests.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/LayerMergeTests.cmake)
add_executable(imageeditor_measurement_tests MeasurementTests.cpp)
add_executable(imageeditor_bounds_snapping_tests BoundsSnappingTests.cpp)
target_link_libraries(imageeditor_bounds_snapping_tests PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_bounds_snapping_tests)
add_test(NAME imageeditor_bounds_snapping_tests COMMAND imageeditor_bounds_snapping_tests)
target_link_libraries(imageeditor_measurement_tests PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_measurement_tests)
add_test(NAME imageeditor_measurement_tests COMMAND imageeditor_measurement_tests)

add_executable(imageeditor_ruler_strip_tests RulerStripTests.cpp)
target_link_libraries(imageeditor_ruler_strip_tests PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_ruler_strip_tests)
add_test(NAME imageeditor_ruler_strip_tests COMMAND imageeditor_ruler_strip_tests)
set_tests_properties(imageeditor_ruler_strip_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

add_executable(imageeditor_layer_outline_interaction_tests LayerOutlineInteractionTests.cpp)
target_link_libraries(imageeditor_layer_outline_interaction_tests PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_layer_outline_interaction_tests)
qt_add_resources(imageeditor_layer_outline_interaction_tests "layer_outline_test_icons"
    PREFIX "/" BASE "${PROJECT_SOURCE_DIR}/assets"
    FILES ${PROJECT_SOURCE_DIR}/assets/icons/brush.svg ${PROJECT_SOURCE_DIR}/assets/icons/eraser.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eye-off.svg ${PROJECT_SOURCE_DIR}/assets/icons/eye.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eyedropper.svg ${PROJECT_SOURCE_DIR}/assets/icons/fill.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/lasso.svg ${PROJECT_SOURCE_DIR}/assets/icons/layer-raster.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/marquee.svg ${PROJECT_SOURCE_DIR}/assets/icons/move.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/text.svg)
add_test(NAME imageeditor_layer_outline_interaction_tests COMMAND imageeditor_layer_outline_interaction_tests)
set_tests_properties(imageeditor_layer_outline_interaction_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 45 LABELS "render;layers;input;preferences")
if(NOT "$ENV{WAYLAND_DISPLAY}" STREQUAL "")
    add_test(NAME imageeditor_wayland_layer_outline_smoke COMMAND imageeditor_layer_outline_interaction_tests --wayland-validation)
    set_tests_properties(imageeditor_wayland_layer_outline_smoke PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=wayland" TIMEOUT 45 RUN_SERIAL TRUE LABELS "integration;wayland;vulkan;layers")
endif()
add_executable(imageeditor_measure_interaction_tests MeasureInteractionTests.cpp)
target_link_libraries(imageeditor_measure_interaction_tests PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_measure_interaction_tests)
qt_add_resources(imageeditor_measure_interaction_tests "measure_test_icons"
    PREFIX "/" BASE "${PROJECT_SOURCE_DIR}/assets"
    FILES
        ${PROJECT_SOURCE_DIR}/assets/icons/brush.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eraser.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eye-off.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eye.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/eyedropper.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/fill.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/lasso.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/layer-raster.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/marquee.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/move.svg
        ${PROJECT_SOURCE_DIR}/assets/icons/text.svg)
add_test(NAME imageeditor_measure_interaction_tests COMMAND imageeditor_measure_interaction_tests)
set_tests_properties(imageeditor_measure_interaction_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
if(NOT "$ENV{WAYLAND_DISPLAY}" STREQUAL "")
    add_test(NAME imageeditor_wayland_measure_smoke COMMAND imageeditor_measure_interaction_tests --wayland-validation)
    set_tests_properties(imageeditor_wayland_measure_smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=wayland" TIMEOUT 30 RUN_SERIAL TRUE LABELS "integration;wayland;vulkan;measure")
endif()
add_executable(imageeditor_layer_crop_core_tests LayerCropCoreTests.cpp)
add_executable(imageeditor_layer_chamfer_tests LayerChamferTests.cpp)
target_link_libraries(imageeditor_layer_chamfer_tests PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_layer_chamfer_tests)
add_test(NAME imageeditor_layer_chamfer_tests COMMAND imageeditor_layer_chamfer_tests)
set_tests_properties(imageeditor_layer_chamfer_tests PROPERTIES LABELS "crop;core;deterministic")
target_link_libraries(imageeditor_layer_crop_core_tests PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_layer_crop_core_tests)
add_test(NAME imageeditor_layer_crop_core_tests COMMAND imageeditor_layer_crop_core_tests)
set_tests_properties(imageeditor_layer_crop_core_tests PROPERTIES LABELS "crop;core;deterministic")
add_executable(imageeditor_layer_crop_session_tests LayerCropSessionTests.cpp)
target_link_libraries(imageeditor_layer_crop_session_tests PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_layer_crop_session_tests)
add_test(NAME imageeditor_layer_crop_session_tests COMMAND imageeditor_layer_crop_session_tests)
set_tests_properties(imageeditor_layer_crop_session_tests PROPERTIES LABELS "crop;core;history")
add_executable(imageeditor_layer_crop_persistence_tests LayerCropPersistenceTests.cpp)
target_link_libraries(imageeditor_layer_crop_persistence_tests PRIVATE imageeditor_ui PkgConfig::MINIZIP)
imageeditor_enable_warnings(imageeditor_layer_crop_persistence_tests)
add_test(NAME imageeditor_layer_crop_persistence_tests COMMAND imageeditor_layer_crop_persistence_tests)
set_tests_properties(imageeditor_layer_crop_persistence_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "crop;project")
add_test(NAME imageeditor_layer_crop_rendering_tests COMMAND imageeditor_blend_rendering_tests --crop --validation)
set_tests_properties(imageeditor_layer_crop_rendering_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "crop;vulkan;render" TIMEOUT 90 RUN_SERIAL TRUE)
add_executable(imageeditor_layer_crop_ui_tests LayerCropUiTests.cpp)
target_link_libraries(imageeditor_layer_crop_ui_tests PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_layer_crop_ui_tests)
add_test(NAME imageeditor_layer_crop_ui_tests COMMAND imageeditor_layer_crop_ui_tests)
set_tests_properties(imageeditor_layer_crop_ui_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "crop;input;ui" TIMEOUT 90)
if(NOT "$ENV{WAYLAND_DISPLAY}" STREQUAL "")
    add_test(NAME imageeditor_wayland_layer_crop_smoke COMMAND imageeditor_layer_crop_ui_tests --wayland-validation)
    set_tests_properties(imageeditor_wayland_layer_crop_smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=wayland" LABELS "crop;integration;wayland;vulkan" TIMEOUT 90 RUN_SERIAL TRUE)
endif()

add_executable(imageeditor_image_export_tests ImageExportTests.cpp)
target_link_libraries(imageeditor_image_export_tests PRIVATE imageeditor_ui)
target_compile_definitions(imageeditor_image_export_tests PRIVATE
    IMAGEEDITOR_EXPORT_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts")
imageeditor_enable_warnings(imageeditor_image_export_tests)
add_test(NAME imageeditor_image_export_tests COMMAND imageeditor_image_export_tests)
set_tests_properties(imageeditor_image_export_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 90 LABELS "export;image;codec;deterministic")
include(ExportWorkflowTests.cmake)
include(CloningTests.cmake)
include(LocalBlurTests.cmake)
include(SpatialFilterTests.cmake)
add_test(NAME imageeditor_export_missing_codec_tests COMMAND imageeditor_image_export_tests --missing-codec)
set_tests_properties(imageeditor_export_missing_codec_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 20 LABELS "export;codec")
add_executable(imageeditor_third_party_notices_tests ThirdPartyNoticesTests.cpp)
target_link_libraries(imageeditor_third_party_notices_tests PRIVATE imageeditor_ui Qt6::Test)
target_compile_definitions(imageeditor_third_party_notices_tests PRIVATE
    IMAGEEDITOR_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    IMAGEEDITOR_NOTICES_DIR="${VULKANA_EFFECTIVE_NOTICES_DIRECTORY}"
    IMAGEEDITOR_TEST_DEVELOPMENT_RESOURCES=$<BOOL:${IMAGEEDITOR_DEVELOPMENT_RESOURCES}>)
imageeditor_enable_warnings(imageeditor_third_party_notices_tests)
add_test(NAME imageeditor_third_party_notices_tests COMMAND imageeditor_third_party_notices_tests)
set_tests_properties(imageeditor_third_party_notices_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30 LABELS "licenses;widgets;offline")
