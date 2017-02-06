# vim:ts=4:sw=4:et

cmake_minimum_required(VERSION 2.6)

# TODO: get rid of this once we bump the cmake version to ≥ 2.8.2
string(REGEX MATCH "clang" CMAKE_COMPILER_IS_CLANG "${CMAKE_C_COMPILER}")

if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_CLANG)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wconversion")
    set(CMAKE_C_FLAGS_ASAN "-fsanitize=address -fno-omit-frame-pointer -g3 -O1")
    set(CMAKE_C_FLAGS_RELEASE "-O2")
    set(CMAKE_C_FLAGS_DEBUG "-g3 -O0")
endif()

set(CMAKE_BUILD_TYPE ${CMAKE_BUILD_TYPE}
    CACHE STRING "Choose the type of build: None Debug Release ASan"
    FORCE)

# Required by irssi/src/common.h.
add_definitions("-DUOFF_T_LONG")

find_package(PkgConfig REQUIRED)

pkg_check_modules(DEPS REQUIRED glib-2.0 yajl gio-2.0 libcurl)

include_directories(${DEPS_INCLUDE_DIRS})
link_directories(${DEPS_LIBRARY_DIRS})
add_definitions(${DEPS_CFLAGS_OTHER})

set(IRSSI_PATH "/usr/include/irssi" CACHE PATH "path to irssi include files")
find_path(irssi_INCLUDE_DIR
    NAMES irssi-config.h src/common.h
    PATHS ${IRSSI_PATH} /usr/local/include/irssi
    )

if(NOT irssi_INCLUDE_DIR)
    message(SEND_ERROR "Could not auto find the irssi include files, please run:\n# cmake -DIRSSI_PATH:PATH=/path/to/irssi/includes .")
    return()
endif()

include_directories(src/core)
include_directories(src/core/robustsession)
include_directories(src/fe-common)

include_directories(
    ${irssi_INCLUDE_DIR}
    ${irssi_INCLUDE_DIR}/src
    ${irssi_INCLUDE_DIR}/src/fe-common/core
    ${irssi_INCLUDE_DIR}/src/core
    ${irssi_INCLUDE_DIR}/src/irc/core
)

add_definitions("-std=gnu99")
# TODO: switch to this method for specifying -std once cmake 3.1+ is sufficiently wide-spread
# set(CMAKE_C_STANDARD 99)
# set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(src/core)
add_subdirectory(src/fe-common)
