cmake_minimum_required(VERSION 3.18)

# The code does not depend on the CPU: the module is built per architecture (-DDMOD_TOOLS_NAME=arch/<name>, as the release workflow does).
# DMOD_CPU_FAMILY only decides which package of dmgpio dmod_link_modules() takes the headers from (its packages are named by family,
# there is none for x86_64): they are the same for every architecture.
set(DMOD_CPU_FAMILY "stm32f7" CACHE STRING "CPU family of the packages the headers are taken from")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT DEFINED DMOD_MODULE_VERSION)
    set(DMOD_MODULE_VERSION "0.1" CACHE STRING "DMOD module version")
endif()

set(DMOD_MODE "DMOD_MODULE" CACHE STRING "DMOD build mode")
set(DMOD_BUILD_TESTS OFF CACHE BOOL "Build tests")
set(DMOD_BUILD_EXAMPLES OFF CACHE BOOL "Build examples")
set(DMOD_BUILD_TOOLS OFF CACHE BOOL "Build tools")
set(DMOD_BUILD_TEMPLATES OFF CACHE BOOL "Build templates")
option(DMOD_USE_INSTALLED_SDK "Use an installed dmod-dev SDK when available" ON)

# Explicit source paths take precedence over an installed SDK. Resolve DMOD
# before project() so DMOD_TOOLS_NAME can select the compiler in either flow.
set(_dmod_installed_sdk FALSE)
if(DMOD_USE_INSTALLED_SDK AND DMOD_MODE STREQUAL "DMOD_MODULE"
        AND NOT DMOD_DIR AND NOT FETCHCONTENT_SOURCE_DIR_DMOD)
    find_package(dmod CONFIG QUIET)
    if(dmod_FOUND)
        set(_dmod_installed_sdk TRUE)
    endif()
endif()

if(NOT _dmod_installed_sdk)
    if(DEFINED DMOD_DIR AND NOT "${DMOD_DIR}" STREQUAL "")
        set(FETCHCONTENT_SOURCE_DIR_DMOD "${DMOD_DIR}" CACHE PATH "" FORCE)
    endif()
    include(FetchContent)
    FetchContent_Declare(
        dmod
        GIT_REPOSITORY https://github.com/choco-technologies/dmod.git
        GIT_TAG        develop
    )
    FetchContent_MakeAvailable(dmod)
endif()

project(dmuc8253 VERSION ${DMOD_MODULE_VERSION}
    DESCRIPTION "dmuc8253 DMOD library module"
    LANGUAGES C CXX)

if(NOT _dmod_installed_sdk)
    set(DMOD_DIR "${dmod_SOURCE_DIR}" CACHE PATH "DMOD source directory")
    set(DMOD_SCRIPTS_DIR "${DMOD_DIR}/scripts" CACHE PATH "DMOD scripts directory")
    include("${DMOD_DIR}/paths.cmake")
    dmod_setup_external_module()
endif()

# Name of the module
set(DMOD_MODULE_NAME        dmuc8253)

# Author (should be string)
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")

# Stack size for the module (should be integer)
set(DMOD_STACK_SIZE         1024)

# Path to the .dmr file - enables release package creation in build/packages/
set(DMOD_DMR_PATH           ${CMAKE_CURRENT_SOURCE_DIR}/dmuc8253.dmr)

# List of MAL interfaces implemented by this module (leave empty if none)
set(DMOD_MAL_IMPLS          )

# List of DIF interfaces implemented by this module (leave empty if none)
set(DMOD_DIF_IMPLS          )

#
#   dmod_add_library - create a library module
#   it has the same signature as add_library
#   and can be used in the same way after the creation
#   (for example, to link libraries)
#
dmod_add_library(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    # List of source files - can include C and C++ files
    src/dmuc8253.c
    src/dmuc8253_splash.c
)

target_include_directories(${DMOD_MODULE_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(${DMOD_MODULE_NAME}_if INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# dmdrvi: the driver interface (the friends of dmdevfs and the generic graphics commands), dmini: the configuration, dmosi: the lock
# and the delays, dmgpio: the commands of the data/command, reset and BUSY lines. No versions: the latest ones are taken.
dmod_link_modules(${DMOD_MODULE_NAME}
    dmdrvi
    dmini
    dmosi
    dmgpio
)
enable_testing()
add_subdirectory(tests)
