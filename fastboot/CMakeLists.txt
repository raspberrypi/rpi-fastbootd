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

# GPIO control via libgpiod (optional - falls back to command if not available)
pkg_check_modules(GPIOD libgpiod)

# Crypto operations via libcryptsetup (optional - falls back to command if not available)
pkg_check_modules(CRYPTSETUP libcryptsetup)

find_package(ZLIB REQUIRED)

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

# Define HAVE_LIBGPIOD if libgpiod is available
if(GPIOD_FOUND)
    message(STATUS "libgpiod found - enabling native GPIO control")
    target_compile_definitions(fastbootd PRIVATE HAVE_LIBGPIOD)
else()
    message(STATUS "libgpiod not found - will use gpioset command fallback")
endif()

# Define HAVE_LIBCRYPTSETUP if libcryptsetup is available
if(CRYPTSETUP_FOUND)
    message(STATUS "libcryptsetup found - enabling native LUKS operations")
    target_compile_definitions(fastbootd PRIVATE HAVE_LIBCRYPTSETUP)
else()
    message(STATUS "libcryptsetup not found - will use cryptsetup command fallback")
endif()

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
    asyncio
    cutils
    log
    base
    rpiparted
    rpiidp
    jsoncpp_static
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

install(DIRECTORY systemd/bin
	DESTINATION /usr/local/
	PATTERN "*"
	PERMISSIONS
		OWNER_READ OWNER_WRITE OWNER_EXECUTE
		GROUP_READ GROUP_EXECUTE
		WORLD_READ WORLD_EXECUTE
)

# Add test subdirectory
if(BUILD_TESTING)
    add_subdirectory(device/test)
endif()

