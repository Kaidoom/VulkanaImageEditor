if(WIN32)
    set(_background_test_qpa windows)
else()
    set(_background_test_qpa offscreen)
endif()

add_executable(imageeditor_background_removal_tests BackgroundRemovalTests.cpp)
target_link_libraries(imageeditor_background_removal_tests PRIVATE imageeditor_ui)
imageeditor_enable_warnings(imageeditor_background_removal_tests)
add_test(NAME imageeditor_background_removal_tests COMMAND imageeditor_background_removal_tests)
set_tests_properties(imageeditor_background_removal_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=${_background_test_qpa}" TIMEOUT 30 LABELS "background-removal;onnx")

add_executable(imageeditor_background_removal_mask_tests BackgroundRemovalMaskTests.cpp)
target_link_libraries(imageeditor_background_removal_mask_tests PRIVATE imageeditor_ui)
imageeditor_enable_warnings(imageeditor_background_removal_mask_tests)
add_test(NAME imageeditor_background_removal_mask_tests COMMAND imageeditor_background_removal_mask_tests)
set_tests_properties(imageeditor_background_removal_mask_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=${_background_test_qpa}" TIMEOUT 30 LABELS "background-removal;mask;history")

add_executable(imageeditor_background_removal_panel_tests BackgroundRemovalPanelTests.cpp)
target_link_libraries(imageeditor_background_removal_panel_tests PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_background_removal_panel_tests)
add_test(NAME imageeditor_background_removal_panel_tests COMMAND imageeditor_background_removal_panel_tests)
set_tests_properties(imageeditor_background_removal_panel_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=${_background_test_qpa}" TIMEOUT 30 LABELS "background-removal;ui")

add_executable(imageeditor_background_removal_interaction_tests BackgroundRemovalInteractionTests.cpp)
target_link_libraries(imageeditor_background_removal_interaction_tests PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_background_removal_interaction_tests)
add_test(NAME imageeditor_background_removal_interaction_tests COMMAND imageeditor_background_removal_interaction_tests)
set_tests_properties(imageeditor_background_removal_interaction_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=${_background_test_qpa}" TIMEOUT 180 LABELS "background-removal;ui;mask;history")

# Manual benchmark; synthetic or trusted model roots are explicit arguments.
add_executable(imageeditor_background_removal_probe BackgroundRemovalProbe.cpp)
target_link_libraries(imageeditor_background_removal_probe PRIVATE imageeditor_ui)
imageeditor_enable_warnings(imageeditor_background_removal_probe)

add_executable(imageeditor_bundled_background_removal_tests
    BundledBackgroundRemovalProbe.cpp "${PROJECT_SOURCE_DIR}/src/platform/src/ApplicationPaths.cpp")
target_link_libraries(imageeditor_bundled_background_removal_tests PRIVATE imageeditor_ui)
target_compile_definitions(imageeditor_bundled_background_removal_tests PRIVATE VULKANA_RELOCATABLE_BUNDLE=1)
imageeditor_enable_warnings(imageeditor_bundled_background_removal_tests)
add_test(NAME imageeditor_bundled_background_removal_tests COMMAND "${CMAKE_COMMAND}"
    "-DPROBE=$<TARGET_FILE:imageeditor_bundled_background_removal_tests>"
    "-DBUNDLE=${VULKANA_BACKGROUND_REMOVAL_BUNDLE}"
    "-DRUNTIME_BUNDLE=${VULKANA_OBJECT_SELECTION_BUNDLE}"
    "-DSTAGE=${CMAKE_CURRENT_BINARY_DIR}/bundled-background-removal"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/BundledBackgroundRemoval.cmake")
set_tests_properties(imageeditor_bundled_background_removal_tests PROPERTIES
    TIMEOUT 240 LABELS "packaging;background-removal;offline")
