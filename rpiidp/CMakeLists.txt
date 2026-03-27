cmake_minimum_required(VERSION 3.15)

project(rpiidp)

# Prefer FetchContent for consistent static linking
# Fall back to system library if FetchContent unavailable
option(USE_SYSTEM_JSONCPP "Use system jsoncpp instead of FetchContent" OFF)

if(USE_SYSTEM_JSONCPP)
   find_package(PkgConfig REQUIRED)
   pkg_check_modules(JSONCPP REQUIRED jsoncpp)
   message(STATUS "Using system jsoncpp: ${JSONCPP_LIBRARIES}")
else()
   include(FetchContent)
   FetchContent_Declare(
      jsoncpp
      GIT_REPOSITORY https://github.com/open-source-parsers/jsoncpp.git
      GIT_TAG 1.9.6
      EXCLUDE_FROM_ALL
   )
   set(JSONCPP_WITH_TESTS OFF CACHE INTERNAL "")
   set(JSONCPP_WITH_PKGCONFIG_SUPPORT OFF CACHE INTERNAL "")
   set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "")
   set(BUILD_STATIC_LIBS ON CACHE INTERNAL "")
   FetchContent_MakeAvailable(jsoncpp)
   set(JSONCPP_INCLUDE_DIRS ${jsoncpp_SOURCE_DIR}/include)
   set(JSONCPP_LIBRARIES jsoncpp_static)
   message(STATUS "Using FetchContent jsoncpp_static: ${JSONCPP_INCLUDE_DIRS}")
endif()

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
   # C++23 required for std::expected used by RpiFwCrypto wrapper
   set_property(TARGET rpiidp PROPERTY CXX_STANDARD 23)
   set_property(TARGET rpiidp PROPERTY CXX_STANDARD_REQUIRED ON)
   find_package(rpifwcrypto REQUIRED)
   target_include_directories(rpiidp PRIVATE /usr/include/android ${CMAKE_SOURCE_DIR}/fastboot)
   target_include_directories(rpiidp PRIVATE ${CMAKE_SOURCE_DIR}/rpiparted/include)
   target_link_libraries(rpiidp PUBLIC ${RPIFWCRYPTO_LIBRARIES})
   if(CRYPTSETUP_FOUND)
      target_compile_definitions(rpiidp PRIVATE HAVE_LIBCRYPTSETUP)
      target_include_directories(rpiidp PRIVATE ${CRYPTSETUP_INCLUDE_DIRS})
   endif()
else()
   add_subdirectory(../rpiparted rpiparted)
endif()

target_link_libraries(rpiidp PRIVATE ${JSONCPP_LIBRARIES} rpiparted)

target_include_directories(rpiidp PUBLIC 
   ${CMAKE_CURRENT_SOURCE_DIR}/include
   ${JSONCPP_INCLUDE_DIRS}
)

if (IDP_TESTS AND NOT IDP_FASTBOOT)
   set(JSONCPP_INCLUDE_DIR ${JSONCPP_INCLUDE_DIRS} CACHE INTERNAL "")
   add_subdirectory(test)
endif()

# Remove
get_target_property(INCLUDE_DIRS rpiidp INCLUDE_DIRECTORIES)
message(STATUS "rpiidp include dirs: ${INCLUDE_DIRS}")
