# Explicit developer target only: no runtime dependency, application link,
# installed shader or automatic benchmark work.
set(probe_shader "${CMAKE_CURRENT_BINARY_DIR}/spot-heal-score.comp.spv")
add_custom_command(OUTPUT "${probe_shader}"
    COMMAND "${GLSLC_EXECUTABLE}" --target-env=vulkan1.2 -O
        "${CMAKE_CURRENT_SOURCE_DIR}/score.comp" -o "${probe_shader}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/score.comp" VERBATIM)
add_executable(imageeditor_spot_heal_gpu_probe EXCLUDE_FROM_ALL
    SpotHealGpuProbe.cpp "${probe_shader}")
set_target_properties(imageeditor_spot_heal_gpu_probe PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_link_libraries(imageeditor_spot_heal_gpu_probe PRIVATE Vulkan::Vulkan)
target_compile_definitions(imageeditor_spot_heal_gpu_probe PRIVATE
    SPOT_HEAL_PROBE_SHADER="${probe_shader}")
target_compile_options(imageeditor_spot_heal_gpu_probe PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-ffp-contract=off;-Wno-missing-field-initializers>)
imageeditor_enable_warnings(imageeditor_spot_heal_gpu_probe)
