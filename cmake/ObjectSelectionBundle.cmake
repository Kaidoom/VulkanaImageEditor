# Shipping inference inputs are immutable, reviewed artifacts. No configure-time
# downloads, Python tools, model export, stripping or runtime substitution.
set(VULKANA_OBJECT_SELECTION_BUNDLE "${PROJECT_SOURCE_DIR}/assets/object-selection/mobilesam-v1")
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    message(FATAL_ERROR "The tested bundled Object Selection runtime currently supports Linux x86-64 only.")
endif()
set(_object_files encoder.onnx decoder.onnx libonnxruntime.so)
set(_object_hashes
    ab709cf1e7e70db9f09f76691ef761386c5f355c09e4541e6c1d793b1302ed6f
    f326153ab6436cd6f10cf900343c19ed1537ed86d0e537b9571e99ba9778745d
    c902c70b3003c0e99fada202f37478c515ae9bba7944c2b2abd0017bae0c82ed)
foreach(_index RANGE 2)
    list(GET _object_files ${_index} _file)
    list(GET _object_hashes ${_index} _expected)
    if(NOT EXISTS "${VULKANA_OBJECT_SELECTION_BUNDLE}/${_file}")
        message(FATAL_ERROR "Missing bundled Object Selection file: ${_file}. Restore the repository assets.")
    endif()
    file(SHA256 "${VULKANA_OBJECT_SELECTION_BUNDLE}/${_file}" _actual)
    if(NOT _actual STREQUAL _expected)
        message(FATAL_ERROR "Bundled Object Selection ${_file} differs from the quality-tested input.")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${VULKANA_OBJECT_SELECTION_BUNDLE}/${_file}")
endforeach()
