# Optional prebuilt worker payload. Ordinary editor builds do not need CUDA or
# the inference engine. Only a verified, explicitly supplied bundle is staged.
set(VULKANA_DIFFUSION_BUNDLE "" CACHE PATH "Verified optional diffusion worker bundle prefix")
if(NOT VULKANA_DIFFUSION_BUNDLE)
    return()
endif()
get_filename_component(VULKANA_DIFFUSION_BUNDLE "${VULKANA_DIFFUSION_BUNDLE}" ABSOLUTE
    BASE_DIR "${PROJECT_SOURCE_DIR}")
set(_diffusion_validator "${PROJECT_SOURCE_DIR}/tools/diffusion/prepare_bundle.py")
set(_diffusion_manifest "${VULKANA_DIFFUSION_BUNDLE}/share/vulkana-editor/diffusion/cuda/manifest.json")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${_diffusion_validator}"
    --verify "${VULKANA_DIFFUSION_BUNDLE}"
    RESULT_VARIABLE _diffusion_result OUTPUT_VARIABLE _diffusion_output ERROR_VARIABLE _diffusion_error)
if(NOT _diffusion_result EQUAL 0)
    message(FATAL_ERROR "Invalid diffusion worker bundle: ${_diffusion_output}${_diffusion_error}")
endif()
file(READ "${_diffusion_manifest}" _diffusion_json)
file(SHA256 "${_diffusion_manifest}" _diffusion_manifest_sha256)
string(JSON _diffusion_platform GET "${_diffusion_json}" platform)
if((WIN32 AND NOT _diffusion_platform STREQUAL "windows")
   OR (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT _diffusion_platform STREQUAL "linux")
   OR (NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    message(FATAL_ERROR "Diffusion worker bundle does not match this target platform")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_diffusion_manifest}" "${_diffusion_validator}" "${PROJECT_SOURCE_DIR}/src/diffusion/engine-lock.json")
# CPack and cmake --install recheck the supplied bytes immediately before copy.
configure_file("${CMAKE_CURRENT_LIST_DIR}/VerifyDiffusionBundle.cmake.in"
    "${PROJECT_BINARY_DIR}/VerifyDiffusionBundle.cmake" @ONLY)
install(SCRIPT "${PROJECT_BINARY_DIR}/VerifyDiffusionBundle.cmake")
string(JSON _diffusion_file_count LENGTH "${_diffusion_json}" files)
math(EXPR _diffusion_file_last "${_diffusion_file_count} - 1")
foreach(_i RANGE ${_diffusion_file_last})
    string(JSON _diffusion_file GET "${_diffusion_json}" files ${_i} path)
    get_filename_component(_diffusion_directory "${_diffusion_file}" DIRECTORY)
    if(_diffusion_file MATCHES "^bin/vulkana-diffusion-worker-cuda(\\.exe)?$")
        install(PROGRAMS "${VULKANA_DIFFUSION_BUNDLE}/${_diffusion_file}" DESTINATION "${_diffusion_directory}")
    else()
        install(FILES "${VULKANA_DIFFUSION_BUNDLE}/${_diffusion_file}" DESTINATION "${_diffusion_directory}")
    endif()
endforeach()
install(FILES "${_diffusion_manifest}" DESTINATION "share/vulkana-editor/diffusion/cuda")
message(STATUS "Optional diffusion worker: ${_diffusion_platform} CUDA, verified release bundle")
