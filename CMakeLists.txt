# 3.19 floor: nothing below needs 3.20+ features (pkg_get_variable 3.4,
# IMPORTED_TARGET 3.6, CMAKE_CURRENT_FUNCTION_LIST_DIR 3.17 in the included
# firmir cmake modules), so the build also configures on Fedora 33 / Ubuntu
# 21.04-era toolchains.
cmake_minimum_required(VERSION 3.19)
project(firmir-compositor1 CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Install into /usr by default (same convention as the firmir libraries), so
# `sudo cmake --install build` drops the binary into /usr/bin.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT AND NOT WIN32)
    set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "Install prefix" FORCE)
endif()
include(GNUInstallDirs)

# Firmir dependency: prefer the installed libraries via pkg-config
# (firmir-wayland-compositor.pc); fall back to a sibling ../firmir checkout.
# FIRMIR_ROOT points at a firmir source tree to build against directly
# (no installation needed). Empty -> try the installed firmir.
set(FIRMIR_ROOT "" CACHE PATH "firmir source tree to build against")
find_package(PkgConfig REQUIRED)
if(NOT FIRMIR_ROOT)
    pkg_check_modules(FIRMIR_PC QUIET IMPORTED_TARGET firmir-wayland-compositor)
endif()
if(FIRMIR_ROOT)
    # Explicit source-tree build: drop any stale pkg-config results
    # cached by an earlier configuration of this build directory.
    set(FIRMIR_PC_FOUND FALSE)
endif()

if(FIRMIR_PC_FOUND)
    message(STATUS "firmir-compositor1: using firmir via pkg-config "
                   "(${FIRMIR_PC_VERSION})")
    # The UI is written in Firml: compile it with the installed firmlc.
    pkg_get_variable(FIRMIR_PREFIX firmir-gui prefix)
    include(${FIRMIR_PREFIX}/share/firmir/cmake/FirmirFirml.cmake)
    firmir_add_fml(compositor1_codegen SOURCES compositor1.fml
        INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR})
else()
    if(NOT FIRMIR_ROOT)
        get_filename_component(FIRMIR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../firmir"
                               ABSOLUTE)
    endif()
    if(NOT EXISTS "${FIRMIR_ROOT}/CMakeLists.txt")
        message(FATAL_ERROR
            "firmir not found via pkg-config and no sibling checkout at "
            "${FIRMIR_ROOT} — either 'sudo make install' the firmir project "
            "or clone it next to this directory.")
    endif()
    message(STATUS "firmir-compositor1: using firmir from ${FIRMIR_ROOT}")
    set(FIRMIR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(FIRMIR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(FIRMIR_BUILD_DOC_TOOL OFF CACHE BOOL "" FORCE)
    set(FIRMIR_BUILD_TOOL_FIRML ON CACHE BOOL "" FORCE)
    # EXCLUDE_FROM_ALL: build only what firmir-compositor1 needs and keep
    # firmir's own install rules out of `cmake --install` (this project
    # installs just its binary; install the firmir libraries from their
    # own build tree).
    add_subdirectory(${FIRMIR_ROOT} ${CMAKE_BINARY_DIR}/firmir EXCLUDE_FROM_ALL)
    include(${FIRMIR_ROOT}/tools/firml/cmake/FirmlCodegen.cmake)
    firml_add_target(compositor1_codegen SOURCES compositor1.fml
        INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
                     ${FIRMIR_ROOT}/libs/gui/include)
    target_include_directories(compositor1_codegen PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(compositor1_codegen PUBLIC firmir-wayland-compositor)
endif()

add_executable(firmir-compositor1 main.cpp CompositorApp.cpp)
target_include_directories(firmir-compositor1 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(FIRMIR_PC_FOUND)
    target_link_libraries(firmir-compositor1
        PRIVATE compositor1_codegen PkgConfig::FIRMIR_PC)
else()
    target_link_libraries(firmir-compositor1
        PRIVATE compositor1_codegen firmir-wayland-compositor firmir-gui)
endif()

install(TARGETS firmir-compositor1
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
