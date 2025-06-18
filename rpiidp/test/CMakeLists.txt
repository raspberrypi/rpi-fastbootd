cmake_minimum_required(VERSION 3.15)

project(rpiidp_test)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../rpiparted/include)

add_executable(ptest parser_test.cpp)
add_executable(dtest device_test.cpp)

target_link_libraries(ptest PRIVATE rpiidp jsoncpp_static)
target_link_libraries(dtest PRIVATE rpiidp jsoncpp_static)

target_include_directories(ptest PRIVATE ${JSONCPP_INCLUDE_DIR})
target_include_directories(dtest PRIVATE ${JSONCPP_INCLUDE_DIR})

