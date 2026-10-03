set(IMAGEEDITOR_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${IMAGEEDITOR_SHADER_OUTPUT_DIR}")

set(IMAGEEDITOR_SHADER_SOURCES
    ${PROJECT_SOURCE_DIR}/shaders/fullscreen.vert
    ${PROJECT_SOURCE_DIR}/shaders/checkerboard.frag
    ${PROJECT_SOURCE_DIR}/shaders/layer.vert
    ${PROJECT_SOURCE_DIR}/shaders/layer.frag
    ${PROJECT_SOURCE_DIR}/shaders/composite.comp
    ${PROJECT_SOURCE_DIR}/shaders/overlay.frag
    ${PROJECT_SOURCE_DIR}/shaders/transform.frag
    ${PROJECT_SOURCE_DIR}/shaders/measure.frag
    ${PROJECT_SOURCE_DIR}/shaders/selection.vert
    ${PROJECT_SOURCE_DIR}/shaders/selection.frag
    ${PROJECT_SOURCE_DIR}/shaders/tooltip.vert
    ${PROJECT_SOURCE_DIR}/shaders/tooltip.frag
    ${PROJECT_SOURCE_DIR}/shaders/textquad.vert
    ${PROJECT_SOURCE_DIR}/shaders/textquad.frag
)

set(IMAGEEDITOR_SHADER_OUTPUTS)
foreach(shader IN LISTS IMAGEEDITOR_SHADER_SOURCES)
    get_filename_component(shader_name "${shader}" NAME)
    set(shader_output "${IMAGEEDITOR_SHADER_OUTPUT_DIR}/${shader_name}.spv")
    add_custom_command(
        OUTPUT "${shader_output}"
        COMMAND "${GLSLC_EXECUTABLE}" --target-env=vulkan1.2 -O -I "${PROJECT_SOURCE_DIR}/src/core/include" "${shader}" -o "${shader_output}"
        DEPENDS "${shader}" "${PROJECT_SOURCE_DIR}/src/core/include/imageeditor/core/detail/BlendMath.inc"
            "${PROJECT_SOURCE_DIR}/src/core/include/imageeditor/core/detail/AdjustmentMath.inc"
            "${PROJECT_SOURCE_DIR}/src/core/include/imageeditor/core/detail/LayerEffectMath.inc"
            "${PROJECT_SOURCE_DIR}/src/core/include/imageeditor/core/detail/ClippingMath.inc"
            "${PROJECT_SOURCE_DIR}/src/core/include/imageeditor/core/detail/CropMath.inc"
        COMMENT "Compiling Vulkan shader ${shader_name}"
        VERBATIM
    )
    list(APPEND IMAGEEDITOR_SHADER_OUTPUTS "${shader_output}")
endforeach()

add_custom_target(imageeditor_shaders DEPENDS ${IMAGEEDITOR_SHADER_OUTPUTS})

add_library(imageeditor_render STATIC
    src/CanvasCoordinateMapping.cpp
    src/CanvasWindow.cpp
    src/CanvasMeasure.cpp
    src/CanvasSelection.cpp
    src/CanvasLayerOutlines.cpp
    src/PointerTooltip.cpp
    src/VulkanPointerTooltip.cpp
    src/DirtyRegionCoalescer.cpp
    src/RasterUploadPlan.cpp
    src/VulkanCanvasBackend.cpp
    src/VulkanCanvasRenderer.cpp
    src/VulkanComposition.cpp
    src/VulkanAdjustments.cpp
    src/VulkanSelection.cpp
)

add_dependencies(imageeditor_render imageeditor_shaders)

target_include_directories(imageeditor_render
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

install(FILES ${IMAGEEDITOR_SHADER_OUTPUTS} DESTINATION "${VULKANA_DATA_DIR}/shaders")

target_link_libraries(imageeditor_render
    PUBLIC
        imageeditor_core
        Qt6::Gui
        imageeditor_platform
    PRIVATE
        Vulkan::Vulkan
)

imageeditor_enable_warnings(imageeditor_render)
target_compile_options(imageeditor_render PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wno-missing-field-initializers>
)
