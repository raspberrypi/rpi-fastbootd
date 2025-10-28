cmake_minimum_required(VERSION 3.15)

project(rpiidp_test)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../rpiparted/include)

add_executable(rpiidp_ptest parser_test.cpp)
add_executable(rpiidp_dtest device_test.cpp)

target_link_libraries(rpiidp_ptest PRIVATE rpiidp jsoncpp_static)
target_link_libraries(rpiidp_dtest PRIVATE rpiidp jsoncpp_static)

target_include_directories(rpiidp_ptest PRIVATE ${JSONCPP_INCLUDE_DIR})
target_include_directories(rpiidp_dtest PRIVATE ${JSONCPP_INCLUDE_DIR})

