cmake_minimum_required(VERSION 3.15)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    include(cmake/DebianHardening.cmake)
    include(cmake/RaspberryPiHardening.cmake)
endif()

project(libsparse VERSION 14.0.0 LANGUAGES C CXX)
add_subdirectory(libsparse)
project(asyncio VERSION 14.0.0)
add_subdirectory(libasyncio)
project(fastbootd VERSION 14.0.0 LANGUAGES C CXX)
add_subdirectory(fastboot)
add_subdirectory(rpiparted)
add_subdirectory(rpiidp)

# Enable CTest for testing support
include(CTest)
enable_testing()
