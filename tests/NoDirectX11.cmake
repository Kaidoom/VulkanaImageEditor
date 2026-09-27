if(NOT EXISTS "${IMAGEEDITOR_BINARY}")
    message(FATAL_ERROR "ImageEditor executable does not exist: ${IMAGEEDITOR_BINARY}")
endif()

execute_process(
    COMMAND "${READELF_EXECUTABLE}" -d "${IMAGEEDITOR_BINARY}"
    RESULT_VARIABLE readelf_result
    OUTPUT_VARIABLE dynamic_section
    ERROR_VARIABLE readelf_error
)
if(NOT readelf_result EQUAL 0)
    message(FATAL_ERROR "readelf failed: ${readelf_error}")
endif()
if(dynamic_section MATCHES "Shared library: \\[lib(X11|Xau|Xdmcp|xcb|GLX)[^]]*\\]")
    message(FATAL_ERROR "ImageEditor has a direct X11/XCB/GLX dependency:\n${dynamic_section}")
endif()

file(GLOB_RECURSE application_sources
    "${IMAGEEDITOR_SOURCE_ROOT}/*.cpp"
    "${IMAGEEDITOR_SOURCE_ROOT}/*.hpp"
    "${IMAGEEDITOR_SOURCE_ROOT}/CMakeLists.txt"
)
foreach(source_file IN LISTS application_sources)
    file(READ "${source_file}" source_text)
    if(source_text MATCHES "#[ \t]*include[ \t]*[<\"](X11|xcb)/"
        OR source_text MATCHES "xcb_(connection|window|visual|screen)_t")
        message(FATAL_ERROR "Direct X11/XCB application code found in ${source_file}")
    endif()
endforeach()
