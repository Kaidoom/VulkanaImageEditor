# Stage only the two shipped models and the existing shared CPU runtime.
# Every failure case runs in a fresh process so a cached session cannot hide a
# missing or damaged file. Repository assets are never changed.
foreach(_required PROBE BUNDLE RUNTIME_BUNDLE STAGE)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "Missing test argument: ${_required}")
    endif()
endforeach()
get_filename_component(_stage_name "${STAGE}" NAME)
if(NOT _stage_name STREQUAL "bundled-background-removal")
    message(FATAL_ERROR "The test stage must be named bundled-background-removal")
endif()
file(MAKE_DIRECTORY "${STAGE}")
file(REAL_PATH "${STAGE}" _stage)
file(REAL_PATH "${BUNDLE}" _source_models)
file(REAL_PATH "${RUNTIME_BUNDLE}" _source_runtime)
set(_run "${_stage}/relocated-probe")
foreach(_source IN ITEMS "${_source_models}" "${_source_runtime}")
    string(FIND "${_source}/" "${_run}/" _nested)
    string(FIND "${_run}/" "${_source}/" _inside_source)
    if(_nested EQUAL 0 OR _inside_source EQUAL 0)
        message(FATAL_ERROR "The test stage and source bundles must be separate")
    endif()
endforeach()
file(REMOVE_RECURSE "${_run}")
set(_models "${_run}/share/vulkana-editor/background-removal/v1")
set(_runtime "${_run}/share/vulkana-editor/object-selection/mobilesam-v1")
file(MAKE_DIRECTORY "${_run}/bin" "${_run}/unrelated-cwd" "${_models}" "${_runtime}" "${_run}/saved")
file(COPY "${PROBE}" DESTINATION "${_run}/bin")
get_filename_component(_probe_name "${PROBE}" NAME)

set(_model_files
    "u2netp.onnx"
    "birefnet-lite-512/onnx/model_int8.onnx"
    "provenance.json"
    "U2Net-LICENSE.txt"
    "BiRefNet-LICENSE.txt"
    "rembg-LICENSE.txt"
    "BackgroundRemoval-NOTICE.txt")
foreach(_file IN LISTS _model_files)
    get_filename_component(_parent "${_file}" DIRECTORY)
    file(MAKE_DIRECTORY "${_models}/${_parent}")
    file(COPY "${BUNDLE}/${_file}" DESTINATION "${_models}/${_parent}")
endforeach()
if(EXISTS "${RUNTIME_BUNDLE}/onnxruntime.dll")
    set(_runtime_name "onnxruntime.dll")
else()
    set(_runtime_name "libonnxruntime.so")
endif()
foreach(_file IN ITEMS "${_runtime_name}" "vulkana-mobilesam-v1.json"
        "ONNXRuntime-LICENSE.txt" "ONNXRuntime-ThirdPartyNotices.txt")
    file(COPY "${RUNTIME_BUNDLE}/${_file}" DESTINATION "${_runtime}")
endforeach()

function(run_probe _case)
    execute_process(COMMAND "${_run}/bin/${_probe_name}" ${ARGN}
        WORKING_DIRECTORY "${_run}/unrelated-cwd"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err TIMEOUT 90)
    if(NOT "${_result}" STREQUAL "0")
        message(FATAL_ERROR "${_case} failed: ${_result}\n${_out}\n${_err}")
    endif()
    message(STATUS "${_case}: ${_out}")
endfunction()

run_probe("Relocated bundled inference")

set(_ids "u2netp" "birefnet-lite-int8")
set(_paths "u2netp.onnx" "birefnet-lite-512/onnx/model_int8.onnx")
foreach(_index RANGE 0 1)
    list(GET _ids ${_index} _id)
    list(GET _paths ${_index} _path)
    file(RENAME "${_models}/${_path}" "${_run}/saved/${_id}.onnx")
    run_probe("Missing ${_id}" --expect-model-error "${_id}")
    # More than 256 bytes passes the small/truncated-file check and must still
    # fail the production checksum before ORT is allowed to parse the graph.
    string(REPEAT "Invalid bundled graph.\n" 32 _damage)
    file(WRITE "${_models}/${_path}" "${_damage}")
    run_probe("Corrupt ${_id}" --expect-model-checksum-error "${_id}")
    file(REMOVE "${_models}/${_path}")
    file(RENAME "${_run}/saved/${_id}.onnx" "${_models}/${_path}")
endforeach()

file(RENAME "${_runtime}/${_runtime_name}" "${_run}/saved/${_runtime_name}")
run_probe("Missing shared runtime" --expect-runtime-error)
file(WRITE "${_runtime}/${_runtime_name}" "Invalid CPU runtime.\n")
run_probe("Corrupt shared runtime" --expect-runtime-checksum-error)
file(REMOVE "${_runtime}/${_runtime_name}")
file(RENAME "${_run}/saved/${_runtime_name}" "${_runtime}/${_runtime_name}")

file(RENAME "${_runtime}/vulkana-mobilesam-v1.json" "${_run}/saved/runtime-manifest.json")
run_probe("Missing runtime integrity manifest" --expect-runtime-error)
file(RENAME "${_run}/saved/runtime-manifest.json" "${_runtime}/vulkana-mobilesam-v1.json")
message(STATUS "Installed model/runtime lookup, integrity failures and cross-model recovery passed.")
