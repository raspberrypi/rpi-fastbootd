cmake_minimum_required(VERSION 3.15)

project(rpiidp)

include(FetchContent)

FetchContent_Declare(
   jsoncpp
   GIT_REPOSITORY https://github.com/open-source-parsers/jsoncpp.git
   GIT_TAG 1.9.6
)

set(JSONCPP_WITH_TESTS OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(jsoncpp)

include_directories(${CMAKE_SOURCE_DIR}/include)

option(IDP_FASTBOOT "Build IDP with fastboot integration" ON)
option(IDP_TESTS "Build IDP tests" ON)

add_library(rpiidp STATIC
   src/parser.cpp
   src/version.cpp
   src/nav.cpp
   src/device.cpp
   src/crypt.cpp
   src/utility.cpp
)

if(IDP_FASTBOOT)
   target_compile_definitions(rpiidp PRIVATE WITH_FASTBOOT)
   target_include_directories(rpiidp PRIVATE /usr/include/android ${CMAKE_SOURCE_DIR}/fastboot)
   target_include_directories(rpiidp PRIVATE ${CMAKE_SOURCE_DIR}/rpiparted/include)
   target_include_directories(rpiidp PUBLIC ${jsoncpp_SOURCE_DIR}/include)
else()
   add_subdirectory(../rpiparted rpiparted)
endif()

target_link_libraries(rpiidp PRIVATE jsoncpp_static rpiparted)

target_include_directories(rpiidp PUBLIC 
   ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(rpiidp PRIVATE
   ${jsoncpp_SOURCE_DIR}/include
)

if (IDP_TESTS)
   set(JSONCPP_INCLUDE_DIR ${jsoncpp_SOURCE_DIR}/include CACHE INTERNAL "")
   add_subdirectory(test)
endif()

# Remove
get_target_property(INCLUDE_DIRS rpiidp INCLUDE_DIRECTORIES)
message(STATUS "rpiidp include dirs: ${INCLUDE_DIRS}")
