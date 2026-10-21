cmake_minimum_required(VERSION 3.25)

# Release identity lives here, not in a persistent configure cache. A previous
# -D version must not silently survive a checkout/update to a newer release.
unset(VULKANA_VERSION CACHE)
set(VULKANA_VERSION "0.3.6")
set(VULKANA_RELEASE_CHANNEL "Beta")
project(ImageEditor
    VERSION ${VULKANA_VERSION}
    DESCRIPTION "A lightweight, Linux-first Vulkan image editor"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(IMAGEEDITOR_BUILD_TESTS "Build ImageEditor tests" ON)
option(IMAGEEDITOR_DEVELOPMENT_RESOURCES "Allow build/source resource paths in local builds" ON)
option(IMAGEEDITOR_RELOCATABLE_BUNDLE "Never fall back to system-installed application resources" OFF)
# Package identity is independent of private/relocatable runtime libraries.
# AUTO selects the platform's native or portable distribution format.
set(VULKANA_UPDATE_PACKAGE "AUTO" CACHE STRING "Update artifact: AUTO, RPM, APPIMAGE, DEB, WINDOWSPORTABLE, or NONE")
set_property(CACHE VULKANA_UPDATE_PACKAGE PROPERTY STRINGS AUTO RPM APPIMAGE DEB WINDOWSPORTABLE NONE)
if(VULKANA_UPDATE_PACKAGE STREQUAL "AUTO")
    if(WIN32)
        set(VULKANA_EFFECTIVE_UPDATE_PACKAGE "WINDOWSPORTABLE")
    elseif(IMAGEEDITOR_RELOCATABLE_BUNDLE)
        set(VULKANA_EFFECTIVE_UPDATE_PACKAGE "APPIMAGE")
    else()
        set(VULKANA_EFFECTIVE_UPDATE_PACKAGE "RPM")
    endif()
elseif(VULKANA_UPDATE_PACKAGE MATCHES "^(RPM|APPIMAGE|DEB|WINDOWSPORTABLE|NONE)$")
    set(VULKANA_EFFECTIVE_UPDATE_PACKAGE "${VULKANA_UPDATE_PACKAGE}")
else()
    message(FATAL_ERROR "Invalid VULKANA_UPDATE_PACKAGE: ${VULKANA_UPDATE_PACKAGE}")
endif()
if(WIN32 AND NOT VULKANA_EFFECTIVE_UPDATE_PACKAGE MATCHES "^(WINDOWSPORTABLE|NONE)$")
    message(FATAL_ERROR "Windows builds must not advertise Linux update packages.")
endif()
if(NOT WIN32 AND VULKANA_EFFECTIVE_UPDATE_PACKAGE STREQUAL "WINDOWSPORTABLE")
    message(FATAL_ERROR "Windows portable updates require a Windows target.")
endif()
if(MSVC)
    add_compile_options(/utf-8 /permissive- /EHsc /bigobj)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
include(GNUInstallDirs)
if(CMAKE_POSITION_INDEPENDENT_CODE)
    include(CheckPIESupported)
    check_pie_supported(LANGUAGES CXX)
endif()
set(VULKANA_DATA_DIR "${CMAKE_INSTALL_DATADIR}/vulkana-editor")
set(VULKANA_DISTRIBUTION_CMAKE "" CACHE FILEPATH "Optional downstream distribution CMake extension")
set(VULKANA_NOTICES_DIRECTORY "" CACHE PATH "Optional distribution-specific offline notices")
set(VULKANA_EFFECTIVE_NOTICES_DIRECTORY "${PROJECT_BINARY_DIR}/notices")
if(VULKANA_NOTICES_DIRECTORY)
    get_filename_component(VULKANA_EFFECTIVE_NOTICES_DIRECTORY "${VULKANA_NOTICES_DIRECTORY}"
        ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network Pdf)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Qt6 6.8 REQUIRED COMPONENTS DBus)
endif()
find_package(qpdf REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Vulkan 1.2 REQUIRED)
find_program(GLSLC_EXECUTABLE NAMES glslc REQUIRED)

qt_standard_project_setup()
include(cmake/Localization.cmake)

function(imageeditor_enable_warnings target)
    target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
    )
endfunction()

include(cmake/ObjectSelectionBundle.cmake)
include(cmake/BackgroundRemovalBundle.cmake)
add_subdirectory(src/platform)
add_subdirectory(src/core)
add_subdirectory(src/render)
add_subdirectory(src/ui)
add_subdirectory(src/project)
add_subdirectory(src/app)
include(cmake/Notices.cmake)

if(IMAGEEDITOR_BUILD_TESTS)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
    enable_testing()
    add_subdirectory(tests)
    if(NOT WIN32)
        include(tests/ApplicationPathsTests.cmake)
    endif()
endif()

include(cmake/Install.cmake)
if(VULKANA_DISTRIBUTION_CMAKE)
    include("${VULKANA_DISTRIBUTION_CMAKE}")
endif()
