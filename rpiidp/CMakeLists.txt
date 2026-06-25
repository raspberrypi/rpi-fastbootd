cmake_minimum_required(VERSION 3.15)

project(rpiidp)

# jsoncpp comes from the Debian libjsoncpp-dev package. dh_shlibdeps
# picks up the runtime dep automatically; declared in debian/control as
# Build-Depends: libjsoncpp-dev.
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)
set(JSONCPP_LIBRARIES "${JSONCPP_LIBRARIES}" CACHE INTERNAL "")
set(JSONCPP_INCLUDE_DIRS "${JSONCPP_INCLUDE_DIRS}" CACHE INTERNAL "")
message(STATUS "Using system jsoncpp: ${JSONCPP_LIBRARIES}")

include_directories(${CMAKE_SOURCE_DIR}/include)

find_package(PkgConfig REQUIRED)
pkg_check_modules(UDEV REQUIRED libudev)

# libblockdeviceid (C ABI) provides bdi_get_id() for block-device identity.
# Shared with the block-device-id CLI used by the boot-time cryptroot
# unlocker, keeping IDP key derivation and unlock in lock-step.
find_path(BLOCKDEVICEID_INCLUDE_DIR block_device_id.h REQUIRED)
find_library(BLOCKDEVICEID_LIBRARY NAMES blockdeviceid REQUIRED)

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

target_include_directories(rpiidp PRIVATE ${UDEV_INCLUDE_DIRS} ${BLOCKDEVICEID_INCLUDE_DIR})
target_link_libraries(rpiidp PRIVATE ${JSONCPP_LIBRARIES} rpiparted ${UDEV_LIBRARIES} ${BLOCKDEVICEID_LIBRARY})

target_include_directories(rpiidp PUBLIC 
   ${CMAKE_CURRENT_SOURCE_DIR}/include
   ${JSONCPP_INCLUDE_DIRS}
)

if (IDP_TESTS)
   set(JSONCPP_INCLUDE_DIR ${JSONCPP_INCLUDE_DIRS} CACHE INTERNAL "")
   add_subdirectory(test)
endif()

# Remove
get_target_property(INCLUDE_DIRS rpiidp INCLUDE_DIRECTORIES)
message(STATUS "rpiidp include dirs: ${INCLUDE_DIRS}")
