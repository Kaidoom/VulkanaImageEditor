add_library(imageeditor_platform STATIC src/ApplicationPaths.cpp src/PlatformStartup.cpp src/SingleInstance.cpp src/ServiceConfig.cpp src/AvailableMemory.cpp)
configure_file(include/imageeditor/platform/BuildInfo.hpp.in
    "${CMAKE_CURRENT_BINARY_DIR}/include/imageeditor/platform/BuildInfo.hpp" @ONLY)
target_include_directories(imageeditor_platform PUBLIC include "${CMAKE_CURRENT_BINARY_DIR}/include")
target_link_libraries(imageeditor_platform PUBLIC Qt6::Core)
target_link_libraries(imageeditor_platform PRIVATE Qt6::Network)
target_compile_definitions(imageeditor_platform PRIVATE
    VULKANA_INSTALLED_DATA_DIR="${CMAKE_INSTALL_FULL_DATADIR}/vulkana-editor")
if(IMAGEEDITOR_RELOCATABLE_BUNDLE)
    if(IMAGEEDITOR_DEVELOPMENT_RESOURCES)
        message(FATAL_ERROR "Relocatable bundles must disable development resource lookup")
    endif()
    target_compile_definitions(imageeditor_platform PRIVATE VULKANA_RELOCATABLE_BUNDLE=1)
endif()
if(IMAGEEDITOR_DEVELOPMENT_RESOURCES)
    target_compile_definitions(imageeditor_platform PRIVATE
        VULKANA_DEVELOPMENT_SHADER_DIR="${PROJECT_BINARY_DIR}/src/render/shaders"
        VULKANA_DEVELOPMENT_UI_CONFIG="${PROJECT_SOURCE_DIR}/config/ui-layout.ini"
        VULKANA_DEVELOPMENT_OBJECT_SELECTION="${VULKANA_OBJECT_SELECTION_BUNDLE}"
        VULKANA_DEVELOPMENT_SERVICE_CONFIG="${PROJECT_SOURCE_DIR}/private/services.json")
endif()
imageeditor_enable_warnings(imageeditor_platform)
