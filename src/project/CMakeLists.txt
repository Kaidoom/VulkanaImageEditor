add_library(imageeditor_project_preview STATIC src/ProjectPreview.cpp)
target_include_directories(imageeditor_project_preview PUBLIC include)
target_link_libraries(imageeditor_project_preview PUBLIC Qt6::Core Qt6::Gui)
if(WIN32)
    target_link_libraries(imageeditor_project_preview PUBLIC MINIZIP::minizip-ng)
else()
    target_link_libraries(imageeditor_project_preview PUBLIC PkgConfig::MINIZIP)
endif()

if(WIN32)
    # The shell-facing shim has only OS imports, so COM can load it from an
    # arbitrary portable directory without a development/runtime PATH.
    add_library(vulkana-thumbnail-provider SHARED windows/ThumbnailProvider.cpp windows/ThumbnailProvider.def)
    set_target_properties(vulkana-thumbnail-provider PROPERTIES
        AUTOMOC OFF AUTOUIC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_libraries(vulkana-thumbnail-provider PRIVATE ole32 uuid)
    add_library(vulkana-thumbnail-codec SHARED windows/ThumbnailCodec.cpp)
    target_link_libraries(vulkana-thumbnail-codec PRIVATE imageeditor_project_preview gdi32 ole32)
    install(TARGETS vulkana-thumbnail-provider vulkana-thumbnail-codec RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
imageeditor_enable_warnings(imageeditor_project_preview)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(vulkana-thumbnailer src/Thumbnailer.cpp)
    target_link_libraries(vulkana-thumbnailer PRIVATE imageeditor_project_preview)
    imageeditor_enable_warnings(vulkana-thumbnailer)
    install(TARGETS vulkana-thumbnailer RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
