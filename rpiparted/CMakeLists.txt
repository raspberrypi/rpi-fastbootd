cmake_minimum_required(VERSION 3.15)

project(rpiparted)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBFDISK REQUIRED fdisk)

include_directories(${LIBFDISK_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/include)

add_library(rpiparted STATIC
   src/rpiparted.cpp
)

target_compile_options(rpiparted PRIVATE ${FDISK_CFLAGS_OTHER})
target_link_libraries(rpiparted ${LIBFDISK_LIBRARIES})
target_include_directories(rpiparted PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

option(BUILD_TESTS "Build partition tests" OFF)
if (BUILD_TESTS)
   add_subdirectory(test)
endif()
