cmake_minimum_required(VERSION 3.15)

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
