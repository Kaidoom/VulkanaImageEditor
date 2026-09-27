add_executable(imageeditor_layer_organization_interaction_tests LayerOrganizationInteractionTests.cpp)
target_link_libraries(imageeditor_layer_organization_interaction_tests PRIVATE imageeditor_ui Qt6::Widgets Qt6::Test)
qt_add_resources(imageeditor_layer_organization_interaction_tests "organization_test_icons"
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
imageeditor_enable_warnings(imageeditor_layer_organization_interaction_tests)
add_test(NAME imageeditor_layer_organization_interaction_tests COMMAND imageeditor_layer_organization_interaction_tests)
set_tests_properties(imageeditor_layer_organization_interaction_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 45 LABELS "widgets;layers;hierarchy;selection;history;transform")
if(NOT "$ENV{WAYLAND_DISPLAY}" STREQUAL "")
    add_test(NAME imageeditor_wayland_layer_organization_validation COMMAND imageeditor_layer_organization_interaction_tests)
    set_tests_properties(imageeditor_wayland_layer_organization_validation PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=wayland;QT_FORCE_STDERR_LOGGING=1;IMAGEEDITOR_ORGANIZATION_NATIVE=1"
        RUN_SERIAL TRUE TIMEOUT 45 LABELS "integration;wayland;vulkan;layers;hierarchy;transform")
endif()
