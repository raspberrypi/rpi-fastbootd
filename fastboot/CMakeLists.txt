set(FASTBOOTD_INTERFACE_DESCRIPTOR "fastbootd-provisioner" CACHE STRING "Interface descriptor for fastbootd")
set(COMMON_CPPFLAGS
    -D_GNU_SOURCE
    -DSTR_INTERFACE_="${FASTBOOTD_INTERFACE_DESCRIPTOR}"
)

set(SOURCES
    device/commands.cpp
    device/fastboot_device.cpp
    device/flashing.cpp
    device/main.cpp
    device/partition_lock_manager.cpp
    device/usb.cpp
    device/usb_iouring.cpp
    device/usb_client.cpp
    device/tcp_client.cpp
    device/utility.cpp
    device/variables.cpp
    device/crypto_native.cpp
    socket.cpp
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FDISK REQUIRED fdisk)
pkg_check_modules(URING REQUIRED liburing)
pkg_check_modules(SYSTEMD REQUIRED libsystemd)
# librpi-verity-sign (signing + blob-format C API) is vendored as a git
# submodule under third_party/rpi-verity-verifier and statically linked
# into fastbootd. This avoids a runtime dep on librpi-verity-sign0 and
# keeps the build hermetic — no apt source for the verifier package
# required at build time.
#
# BUILD_SHARED_LIBS=OFF tells the verifier's CMakeLists to produce a
# static archive; RPI_VERITY_INSTALL=OFF suppresses its install rules
# so they don't leak into our cmake_install. EXCLUDE_FROM_ALL keeps the
# verifier's own binaries (rpi-verity-verify, image-check) out of our
# default build.
set(_rpi_verity_dir "${CMAKE_SOURCE_DIR}/third_party/rpi-verity-verifier")
if(NOT EXISTS "${_rpi_verity_dir}/CMakeLists.txt")
    message(FATAL_ERROR
        "rpi-verity-verifier submodule missing at ${_rpi_verity_dir}. "
        "Run: git submodule update --init --recursive")
endif()
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(RPI_VERITY_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${_rpi_verity_dir}" rpi-verity-verifier EXCLUDE_FROM_ALL)
find_package(rpifwcrypto REQUIRED)

# GPIO control via libgpiod (optional - falls back to command if not available)
pkg_check_modules(GPIOD libgpiod>=2.0)

# Crypto operations via libcryptsetup (optional - falls back to command if not available)
option(SKIP_CRYPTSETUP "Skip libcryptsetup for Apache 2.0 licensing" OFF)
if(NOT SKIP_CRYPTSETUP)
    pkg_check_modules(CRYPTSETUP libcryptsetup)
endif()

find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)

include(CheckIncludeFileCXX)

macro(check_header_fatal header_file needed_deb)
    set(CMAKE_REQUIRED_INCLUDES "/usr/include/android")
    check_include_file_cxx(${header_file} have_file)
    if(NOT have_file)
        unset(have_file CACHE)
        message(FATAL_ERROR "${header_file} not found, install ${needed_deb}")
    endif()
    unset(have_file CACHE)
endmacro()

check_header_fatal(android-base/logging.h android-libbase-dev)
check_header_fatal(cutils/sockets.h android-libcutils-dev)

add_executable(fastbootd ${SOURCES})

# Set C++23 standard for std::expected support
set_property(TARGET fastbootd PROPERTY CXX_STANDARD 23)
set_property(TARGET fastbootd PROPERTY CXX_STANDARD_REQUIRED ON)

# Get package version from debian/changelog if available
find_program(DPKG_PARSECHANGELOG dpkg-parsechangelog)
if(DPKG_PARSECHANGELOG)
    execute_process(
        COMMAND dpkg-parsechangelog -l ${CMAKE_SOURCE_DIR}/debian/changelog -S Version
        OUTPUT_VARIABLE FASTBOOTD_PACKAGE_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _dpkg_result
    )
    if(NOT _dpkg_result EQUAL 0)
        set(FASTBOOTD_PACKAGE_VERSION "${PROJECT_VERSION}")
    endif()
else()
    set(FASTBOOTD_PACKAGE_VERSION "${PROJECT_VERSION}")
endif()
message(STATUS "fastbootd package version: ${FASTBOOTD_PACKAGE_VERSION}")
target_compile_definitions(fastbootd PRIVATE FASTBOOTD_VERSION="${FASTBOOTD_PACKAGE_VERSION}")

# Define HAVE_LIBGPIOD if libgpiod is available
if(GPIOD_FOUND)
    message(STATUS "libgpiod found - enabling native GPIO control")
    target_compile_definitions(fastbootd PRIVATE HAVE_LIBGPIOD)
else()
    message(STATUS "libgpiod not found - will use gpioset command fallback")
endif()

# Define HAVE_LIBCRYPTSETUP if libcryptsetup is available
if(SKIP_CRYPTSETUP)
    message(STATUS "libcryptsetup SKIPPED - using cryptsetup command fallback (Apache 2.0 license)")
    set(FASTBOOTD_LICENSE "Apache-2.0")
elseif(CRYPTSETUP_FOUND)
    message(STATUS "libcryptsetup found - enabling native LUKS operations (GPLv2+/GPLv3 license)")
    target_compile_definitions(fastbootd PRIVATE HAVE_LIBCRYPTSETUP)
    set(FASTBOOTD_LICENSE "GPL-2+")
else()
    message(STATUS "libcryptsetup not found - will use cryptsetup command fallback (Apache 2.0 license)")
    set(FASTBOOTD_LICENSE "Apache-2.0")
endif()

# Write license information to a file
file(WRITE "${CMAKE_BINARY_DIR}/LICENSE_INFO" "${FASTBOOTD_LICENSE}\n")

target_compile_definitions(fastbootd PRIVATE ${COMMON_CPPFLAGS})
target_compile_options(fastbootd PRIVATE
    ${FDISK_CFLAGS_OTHER}
    ${URING_CFLAGS_OTHER}
    ${SYSTEMD_CFLAGS_OTHER}
    ${GPIOD_CFLAGS_OTHER}
    ${CRYPTSETUP_CFLAGS_OTHER}
)
target_include_directories(fastbootd PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../libasyncio/include
    /usr/include/android
    ${FDISK_INCLUDE_DIRS}
    ${URING_INCLUDE_DIRS}
    ${ZLIB_INCUDE_DIRS}
    ${SYSTEMD_INCLUDE_DIRS}
    ${GPIOD_INCLUDE_DIRS}
    ${CRYPTSETUP_INCLUDE_DIRS}
    ${RPIFWCRYPTO_INCLUDE_DIRS}
    ${CMAKE_CURRENT_LIST_DIR}/../rpiparted/include
)

target_link_directories(fastbootd PRIVATE "/usr/lib/aarch64-linux-gnu/android")
target_link_libraries(fastbootd PRIVATE
    libsparse
    ${ZLIB_LIBRARIES}
    ${FDISK_LIBRARIES}
    ${URING_LIBRARIES}
    ${SYSTEMD_LIBRARIES}
    ${GPIOD_LIBRARIES}
    ${CRYPTSETUP_LIBRARIES}
    ${RPIFWCRYPTO_LIBRARIES}
    OpenSSL::Crypto
    asyncio
    cutils
    log
    base
    rpiparted
    rpiidp
    ${JSONCPP_LIBRARIES}
    rpi_verity_sign
)

# Define install, packaging, signing tasks
install(TARGETS fastbootd
        BUNDLE DESTINATION .
        RUNTIME DESTINATION /usr/bin 
)

install(DIRECTORY systemd/system
	DESTINATION /usr/lib/systemd
)

install(DIRECTORY systemd/modules-load.d
	DESTINATION /usr/lib
)

# Install internal systemd helper scripts to /usr/lib/rpi-fastbootd/bin
install(PROGRAMS 
	systemd/bin/run_fastboot
	systemd/bin/fastboot_udc_attach
	systemd/bin/initialise_configfs
	DESTINATION /usr/lib/rpi-fastbootd/bin
)

# Add test subdirectory
if(BUILD_TESTING)
    add_subdirectory(device/test)
endif()

