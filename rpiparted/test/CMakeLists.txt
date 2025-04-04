cmake_minimum_required(VERSION 3.15)

project(rpiparted_test)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBFDISK REQUIRED fdisk)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../include)
link_directories(${CMAKE_CURRENT_SOURCE_DIR}/../build)

add_executable(ptest rpiparted_test.cpp)

target_link_libraries(ptest PRIVATE ${LIBFDISK_LIBRARIES} rpiparted)
