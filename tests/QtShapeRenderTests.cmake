add_executable(imageeditor_qt_shape_render_tests QtShapeRenderTests.cpp)
target_link_libraries(imageeditor_qt_shape_render_tests PRIVATE imageeditor_ui Qt6::Gui)
imageeditor_enable_warnings(imageeditor_qt_shape_render_tests)
add_test(NAME imageeditor_qt_shape_render_tests COMMAND imageeditor_qt_shape_render_tests)
set_tests_properties(imageeditor_qt_shape_render_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 45 LABELS "shape;render;sampling;deterministic")
add_test(NAME imageeditor_qt_shape_render_hidpi_tests COMMAND imageeditor_qt_shape_render_tests)
set_tests_properties(imageeditor_qt_shape_render_hidpi_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=2;QT_FONT_DPI=144"
    TIMEOUT 45 LABELS "shape;render;hidpi;deterministic")
