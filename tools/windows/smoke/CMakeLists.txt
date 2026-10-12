cmake_minimum_required(VERSION 3.28)
project(VulkanaWindowsPrerequisites LANGUAGES CXX)

# Deliberately standalone: validate dependencies and shared core code before
# configuring the full application, without relaxing its bundle verification.
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Use the native Windows x64 MSVC environment from Setup.ps1.")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
get_filename_component(VULKANA_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE CACHE)
# PowerShell supplies backslash paths; generated CMake scripts require forward
# slashes (otherwise, for example, C:\Users becomes an invalid \U escape).
file(TO_CMAKE_PATH "${VULKANA_SOURCE_ROOT}" VULKANA_SOURCE_ROOT)
file(TO_CMAKE_PATH "${ONNX_RUNTIME_ROOT}" ONNX_RUNTIME_ROOT)
enable_testing()

function(imageeditor_enable_warnings target)
    target_compile_options(${target} PRIVATE /W3 /utf-8 /permissive- /EHsc)
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endfunction()

find_package(Qt6 6.10 REQUIRED COMPONENTS Core Gui Widgets Network Pdf Svg Test)
find_package(qpdf CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(minizip-ng CONFIG REQUIRED)
find_package(Vulkan REQUIRED COMPONENTS glslc)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(VULKANA_LRELEASE NAMES lrelease REQUIRED)
file(GLOB catalogs CONFIGURE_DEPENDS "${VULKANA_SOURCE_ROOT}/localization/*.ts")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/translations/en.qm"
    COMMAND "${Python3_EXECUTABLE}" "${VULKANA_SOURCE_ROOT}/tools/localization.py" compile
        --output "${CMAKE_CURRENT_BINARY_DIR}/translations" --lrelease "${VULKANA_LRELEASE}"
    DEPENDS ${catalogs} "${VULKANA_SOURCE_ROOT}/localization/languages.json"
        "${VULKANA_SOURCE_ROOT}/tools/localization.py"
    VERBATIM)
add_custom_target(windows_language_smoke ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/translations/en.qm")

add_executable(windows_dependency_probe Dependencies.cpp)
target_link_libraries(windows_dependency_probe PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets
    Qt6::Network Qt6::Pdf Qt6::Svg qpdf::libqpdf ZLIB::ZLIB WebP::webp
    WebP::libwebpmux MINIZIP::minizip-ng Vulkan::Vulkan)
imageeditor_enable_warnings(windows_dependency_probe)
set(shader "${VULKANA_SOURCE_ROOT}/shaders/composite.comp")
set(spirv "${CMAKE_CURRENT_BINARY_DIR}/composite.comp.spv")
add_custom_command(OUTPUT "${spirv}"
    COMMAND Vulkan::glslc --target-env=vulkan1.2 -O
        -I "${VULKANA_SOURCE_ROOT}/src/core/include" "${shader}" -o "${spirv}"
    DEPENDS "${shader}"
        "${VULKANA_SOURCE_ROOT}/src/core/include/imageeditor/core/detail/BlendMath.inc"
        "${VULKANA_SOURCE_ROOT}/src/core/include/imageeditor/core/detail/AdjustmentMath.inc"
        "${VULKANA_SOURCE_ROOT}/src/core/include/imageeditor/core/detail/LayerEffectMath.inc"
        "${VULKANA_SOURCE_ROOT}/src/core/include/imageeditor/core/detail/ClippingMath.inc"
        "${VULKANA_SOURCE_ROOT}/src/core/include/imageeditor/core/detail/CropMath.inc"
    VERBATIM)
add_custom_target(windows_shader_smoke ALL DEPENDS "${spirv}")
add_test(NAME windows.dependencies COMMAND windows_dependency_probe "${spirv}")
set_tests_properties(windows.dependencies PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)

add_subdirectory("${VULKANA_SOURCE_ROOT}/src/core" core)
foreach(test IN ITEMS CoreTests SelectionGraphCutTests SelectionRefinementTests)
    add_executable(windows_${test} "${VULKANA_SOURCE_ROOT}/tests/${test}.cpp")
    target_link_libraries(windows_${test} PRIVATE imageeditor_core)
    imageeditor_enable_warnings(windows_${test})
    add_test(NAME windows.${test} COMMAND windows_${test})
    set_tests_properties(windows.${test} PROPERTIES TIMEOUT 120)
endforeach()

set(models "${VULKANA_SOURCE_ROOT}/assets/object-selection/mobilesam-v1")
# The same unmodified model inputs as the Linux production bundle. This is not
# an alternate source of model binaries and never downloads or converts them.
file(SHA256 "${models}/encoder.onnx" encoder_hash)
file(SHA256 "${models}/decoder.onnx" decoder_hash)
if(NOT encoder_hash STREQUAL "ab709cf1e7e70db9f09f76691ef761386c5f355c09e4541e6c1d793b1302ed6f"
        OR NOT decoder_hash STREQUAL "f326153ab6436cd6f10cf900343c19ed1537ed86d0e537b9571e99ba9778745d")
    message(FATAL_ERROR "MobileSAM models differ from the tested repository inputs.")
endif()
if(NOT EXISTS "${ONNX_RUNTIME_ROOT}/lib/onnxruntime.dll")
    message(FATAL_ERROR "Run Setup.ps1 to prepare the verified Windows CPU runtime.")
endif()
add_executable(windows_object_runtime_probe "${VULKANA_SOURCE_ROOT}/tests/ObjectSelectionRuntimeProbe.cpp")
imageeditor_enable_warnings(windows_object_runtime_probe)
add_test(NAME windows.mobilesam COMMAND windows_object_runtime_probe "${models}" "${ONNX_RUNTIME_ROOT}/lib/onnxruntime.dll")
set_tests_properties(windows.mobilesam PROPERTIES TIMEOUT 120)
