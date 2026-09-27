add_executable(imageeditor_shape_interaction_tests ShapeInteractionTests.cpp)
target_link_libraries(imageeditor_shape_interaction_tests PRIVATE imageeditor_ui Qt6::Test)
qt_add_resources(imageeditor_shape_interaction_tests "shape_test_icons"
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
imageeditor_enable_warnings(imageeditor_shape_interaction_tests)
add_test(NAME imageeditor_shape_interaction_tests COMMAND imageeditor_shape_interaction_tests)
set_tests_properties(imageeditor_shape_interaction_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60 LABELS "shape;input;history;widgets")
add_test(NAME imageeditor_wayland_shape_validation COMMAND imageeditor_shape_interaction_tests)
set_tests_properties(imageeditor_wayland_shape_validation PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=wayland" TIMEOUT 90 RUN_SERIAL TRUE LABELS "shape;wayland;vulkan;manual-desktop")
