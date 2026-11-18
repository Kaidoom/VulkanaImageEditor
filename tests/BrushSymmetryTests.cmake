add_executable(imageeditor_brush_symmetry_state_tests BrushSymmetryStateTests.cpp)
target_link_libraries(imageeditor_brush_symmetry_state_tests PRIVATE imageeditor_ui imageeditor_project_preview)
imageeditor_enable_warnings(imageeditor_brush_symmetry_state_tests)
add_test(NAME imageeditor_brush_symmetry_state_tests COMMAND imageeditor_brush_symmetry_state_tests)
set_tests_properties(imageeditor_brush_symmetry_state_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60 LABELS "brush;symmetry;history;project")

add_executable(imageeditor_brush_symmetry_stroke_tests BrushSymmetryStrokeTests.cpp)
target_link_libraries(imageeditor_brush_symmetry_stroke_tests PRIVATE imageeditor_core)
imageeditor_enable_warnings(imageeditor_brush_symmetry_stroke_tests)
add_test(NAME imageeditor_brush_symmetry_stroke_tests COMMAND imageeditor_brush_symmetry_stroke_tests)
set_tests_properties(imageeditor_brush_symmetry_stroke_tests PROPERTIES TIMEOUT 60 LABELS "brush;symmetry;painting;history")

add_executable(imageeditor_brush_symmetry_ui_tests BrushSymmetryUiTests.cpp)
target_link_libraries(imageeditor_brush_symmetry_ui_tests PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_brush_symmetry_ui_tests)
file(GLOB _brush_symmetry_icons "${PROJECT_SOURCE_DIR}/assets/icons/*.svg")
qt_add_resources(imageeditor_brush_symmetry_ui_tests "brush_symmetry_icons" PREFIX "/" BASE "${PROJECT_SOURCE_DIR}/assets" FILES ${_brush_symmetry_icons})
add_test(NAME imageeditor_brush_symmetry_ui_tests COMMAND imageeditor_brush_symmetry_ui_tests)
set_tests_properties(imageeditor_brush_symmetry_ui_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 90 LABELS "brush;symmetry;input;widgets")
if(NOT WIN32 AND NOT "$ENV{WAYLAND_DISPLAY}" STREQUAL "")
    add_test(NAME imageeditor_wayland_brush_symmetry_ui_tests COMMAND imageeditor_brush_symmetry_ui_tests)
    set_tests_properties(imageeditor_wayland_brush_symmetry_ui_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=wayland" TIMEOUT 90 RUN_SERIAL TRUE LABELS "brush;symmetry;input;native;vulkan")
endif()

add_test(NAME imageeditor_brush_symmetry_vulkan COMMAND imageeditor_blend_rendering_tests --brush-symmetry --validation)
set_tests_properties(imageeditor_brush_symmetry_vulkan PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 90 LABELS "brush;symmetry;vulkan")
