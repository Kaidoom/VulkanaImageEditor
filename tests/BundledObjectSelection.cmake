# Exercise default lookup with development paths compiled out, relocated to a
# clean prefix. The probe intentionally sets the obsolete override to nonsense.
file(MAKE_DIRECTORY "${STAGE}/bin" "${STAGE}/unrelated-cwd" "${STAGE}/share/vulkana-editor/object-selection")
file(COPY "${PROBE}" DESTINATION "${STAGE}/bin")
file(COPY "${BUNDLE}" DESTINATION "${STAGE}/share/vulkana-editor/object-selection")
get_filename_component(_probe_name "${PROBE}" NAME)
execute_process(COMMAND "${STAGE}/bin/${_probe_name}" WORKING_DIRECTORY "${STAGE}/unrelated-cwd"
    RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err TIMEOUT 45)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "Relocated bundled inference failed: ${_result}\n${_out}\n${_err}")
endif()
message(STATUS "${_out}")
