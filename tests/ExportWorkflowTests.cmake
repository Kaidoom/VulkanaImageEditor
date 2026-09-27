add_executable(imageeditor_export_workflow_tests ExportWorkflowTests.cpp)
add_executable(imageeditor_export_benchmarks ExportBenchmarks.cpp)
target_link_libraries(imageeditor_export_benchmarks PRIVATE imageeditor_ui)
imageeditor_enable_warnings(imageeditor_export_benchmarks)
target_link_libraries(imageeditor_export_workflow_tests PRIVATE imageeditor_ui Qt6::Test)
imageeditor_enable_warnings(imageeditor_export_workflow_tests)
add_test(NAME imageeditor_export_workflow_tests COMMAND imageeditor_export_workflow_tests)
set_tests_properties(imageeditor_export_workflow_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 90 LABELS "export;ui;history")
if(NOT "$ENV{WAYLAND_DISPLAY}" STREQUAL "")
    add_test(NAME imageeditor_wayland_export_smoke COMMAND imageeditor_export_workflow_tests --wayland-validation)
    set_tests_properties(imageeditor_wayland_export_smoke PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=wayland" TIMEOUT 90 RUN_SERIAL TRUE LABELS "export;wayland;vulkan")
endif()
