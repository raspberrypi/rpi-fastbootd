cmake_minimum_required(VERSION 3.15)


project(asyncio VERSION 14.0.0)
add_subdirectory(libasyncio)
project(fastbootd VERSION 14.0.0 LANGUAGES C CXX)
add_subdirectory(fastboot)

# CPack directives
set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
include(Packaging)