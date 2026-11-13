if(NOT VULKANA_DIFFUSION_BUNDLE)
    return()
endif()
# Augment source-build or downstream notices in a generated directory; never
# modify an external inventory provided by a distributor.
set(_diffusion_combined_notices "${PROJECT_BINARY_DIR}/diffusion-notices")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/diffusion/prepare_bundle.py"
    --verify "${VULKANA_DIFFUSION_BUNDLE}"
    --merge-notices "${VULKANA_EFFECTIVE_NOTICES_DIRECTORY}"
    --output "${_diffusion_combined_notices}"
    RESULT_VARIABLE _diffusion_notice_result OUTPUT_VARIABLE _diffusion_notice_output ERROR_VARIABLE _diffusion_notice_error)
if(NOT _diffusion_notice_result EQUAL 0)
    message(FATAL_ERROR "Cannot merge diffusion worker notices: ${_diffusion_notice_output}${_diffusion_notice_error}")
endif()
set(VULKANA_EFFECTIVE_NOTICES_DIRECTORY "${_diffusion_combined_notices}")
