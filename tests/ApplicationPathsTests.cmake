add_executable(imageeditor_application_paths_tests
    "${CMAKE_CURRENT_LIST_DIR}/ApplicationPathsTests.cpp")
target_link_libraries(imageeditor_application_paths_tests PRIVATE imageeditor_platform Qt6::Core)
target_compile_definitions(imageeditor_application_paths_tests PRIVATE
    VULKANA_TEST_INSTALLED_DATA_DIR="${CMAKE_INSTALL_FULL_DATADIR}/vulkana-editor")
if(IMAGEEDITOR_RELOCATABLE_BUNDLE)
    target_compile_definitions(imageeditor_application_paths_tests PRIVATE VULKANA_TEST_RELOCATABLE=1)
endif()
if(IMAGEEDITOR_DEVELOPMENT_RESOURCES)
    add_dependencies(imageeditor_application_paths_tests imageeditor_shaders)
    target_compile_definitions(imageeditor_application_paths_tests PRIVATE
        VULKANA_TEST_EXPECT_DEVELOPMENT=1
        VULKANA_TEST_DEVELOPMENT_SHADER_DIR="${PROJECT_BINARY_DIR}/src/render/shaders"
        VULKANA_TEST_DEVELOPMENT_UI_CONFIG="${PROJECT_SOURCE_DIR}/config/ui-layout.ini")
endif()
imageeditor_enable_warnings(imageeditor_application_paths_tests)
add_test(NAME imageeditor_application_paths_tests COMMAND imageeditor_application_paths_tests)
set_tests_properties(imageeditor_application_paths_tests PROPERTIES
    TIMEOUT 30 LABELS "packaging;resources;userdata")

# Exercise the actual path implementation without any development fallbacks,
# even in the ordinary Debug/Release developer configurations.
add_executable(imageeditor_installed_application_paths_tests
    "${CMAKE_CURRENT_LIST_DIR}/ApplicationPathsTests.cpp"
    "${PROJECT_SOURCE_DIR}/src/platform/src/ApplicationPaths.cpp")
target_include_directories(imageeditor_installed_application_paths_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/platform/include")
target_link_libraries(imageeditor_installed_application_paths_tests PRIVATE Qt6::Core)
target_compile_definitions(imageeditor_installed_application_paths_tests PRIVATE
    VULKANA_INSTALLED_DATA_DIR="${CMAKE_INSTALL_FULL_DATADIR}/vulkana-editor"
    VULKANA_TEST_INSTALLED_DATA_DIR="${CMAKE_INSTALL_FULL_DATADIR}/vulkana-editor")
imageeditor_enable_warnings(imageeditor_installed_application_paths_tests)
add_test(NAME imageeditor_installed_application_paths_tests COMMAND imageeditor_installed_application_paths_tests)
set_tests_properties(imageeditor_installed_application_paths_tests PROPERTIES
    TIMEOUT 30 LABELS "packaging;resources;userdata")

add_executable(imageeditor_appimage_paths_tests
    "${CMAKE_CURRENT_LIST_DIR}/ApplicationPathsTests.cpp"
    "${PROJECT_SOURCE_DIR}/src/platform/src/ApplicationPaths.cpp")
target_include_directories(imageeditor_appimage_paths_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/platform/include")
target_link_libraries(imageeditor_appimage_paths_tests PRIVATE Qt6::Core)
target_compile_definitions(imageeditor_appimage_paths_tests PRIVATE
    VULKANA_RELOCATABLE_BUNDLE=1 VULKANA_TEST_RELOCATABLE=1)
imageeditor_enable_warnings(imageeditor_appimage_paths_tests)
add_test(NAME imageeditor_appimage_paths_tests COMMAND imageeditor_appimage_paths_tests)
set_tests_properties(imageeditor_appimage_paths_tests PROPERTIES TIMEOUT 30 LABELS "packaging;resources;userdata")
