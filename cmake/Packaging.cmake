# these are cache variables, so they could be overwritten with -D,
set(CPACK_PACKAGE_NAME "rpi-${PROJECT_NAME}"
    CACHE STRING "The resulting package name"
)
# which is useful in case of packing only selected components instead of the whole thing
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Raspberry Pi Fastbootd"
    CACHE STRING "Package description for the package metadata"
)
set(CPACK_PACKAGE_VENDOR "Raspberry Pi Ltd")

# This value will be cached by cmake, a clean re-configure will be required to
# adopt new values
execute_process(
	COMMAND git rev-list --count 92c7a77c18..HEAD
	WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
	OUTPUT_VARIABLE CPACK_DEBIAN_PACKAGE_RELEASE
	OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(CPACK_VERBATIM_VARIABLES YES)

set(CPACK_PACKAGE_INSTALL_DIRECTORY ${CPACK_PACKAGE_NAME})
SET(CPACK_OUTPUT_FILE_PREFIX "${CMAKE_SOURCE_DIR}/dist")

set(CPACK_PACKAGING_INSTALL_PREFIX "/usr/bin")

set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})

set(CPACK_PACKAGE_CONTACT "applications@raspberrypi.com")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Raspberry Pi Signed Boot Team")

set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/fastboot/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/fastboot/README.md")

set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "arm64")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "android-liblog, android-libbase, zlib1g, android-libcutils, android-libsparse, libfdisk1, liburing2, libsystemd0, openssl, coreutils, xxd, sed, awk, raspi-utils-core")

# cryptsetup-bin is needed for the oem commands 'cryptinit', 'cryptopen'
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "cryptsetup-bin")

set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)

set(CPACK_DEB_COMPONENT_INSTALL YES)

include(CPack)
