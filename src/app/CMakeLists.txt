qt_add_executable(imageeditor
    main.cpp
)

qt_add_resources(imageeditor "app_icons"
    PREFIX "/"
    BASE "${PROJECT_SOURCE_DIR}/assets"
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
        ${PROJECT_SOURCE_DIR}/assets/Vulkana512.png
)

target_link_libraries(imageeditor
    PRIVATE
        imageeditor_ui
        Qt6::Widgets
)

if(UNIX AND NOT APPLE)
    # Some Qt distributions expose optional GLX/OpenGL libraries through the
    # QtGui imported target. ImageEditor does not use them: keep the native
    # Wayland executable free of those unused direct dependencies.
    target_link_options(imageeditor PRIVATE "LINKER:--as-needed")
endif()

target_compile_definitions(imageeditor PRIVATE
    $<$<CONFIG:Debug>:IMAGEEDITOR_ENABLE_VALIDATION=1>
)
if(IMAGEEDITOR_RELOCATABLE_BUNDLE)
    target_compile_definitions(imageeditor PRIVATE VULKANA_APPIMAGE_BUILD=1)
endif()

install(TARGETS imageeditor RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

imageeditor_enable_warnings(imageeditor)
