# Reviewed, offline CPU model artifacts; never download or convert during a build.
set(VULKANA_BACKGROUND_REMOVAL_BUNDLE "${PROJECT_SOURCE_DIR}/assets/background-removal/v1")
set(VULKANA_BACKGROUND_REMOVAL_FILES
    u2netp.onnx birefnet-lite-512/onnx/model_int8.onnx
    provenance.json BackgroundRemoval-NOTICE.txt U2Net-LICENSE.txt
    BiRefNet-LICENSE.txt rembg-LICENSE.txt)
set(_background_hashes
    309c8469258dda742793dce0ebea8e6dd393174f89934733ecc8b14c76f4ddd8
    aec1e9eca3ec401f82a32911d9e8369a24bae8ae490168308f768ddd5a936da2)
foreach(_file IN LISTS VULKANA_BACKGROUND_REMOVAL_FILES)
    if(NOT EXISTS "${VULKANA_BACKGROUND_REMOVAL_BUNDLE}/${_file}")
        message(FATAL_ERROR "Missing bundled Remove Background file: ${_file}. Restore the repository assets.")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${VULKANA_BACKGROUND_REMOVAL_BUNDLE}/${_file}")
endforeach()
file(READ "${VULKANA_BACKGROUND_REMOVAL_BUNDLE}/provenance.json" _background_provenance)
foreach(_index RANGE 1)
    list(GET VULKANA_BACKGROUND_REMOVAL_FILES ${_index} _file)
    list(GET _background_hashes ${_index} _expected)
    file(SHA256 "${VULKANA_BACKGROUND_REMOVAL_BUNDLE}/${_file}" _actual)
    string(JSON _recorded GET "${_background_provenance}" models ${_index} sha256)
    string(JSON _recorded_file GET "${_background_provenance}" models ${_index} file)
    if(NOT _actual STREQUAL _expected OR NOT _recorded STREQUAL _expected OR NOT _recorded_file STREQUAL _file)
        message(FATAL_ERROR "Bundled Remove Background ${_file} differs from its reviewed provenance.")
    endif()
endforeach()
