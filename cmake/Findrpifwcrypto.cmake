find_path(RPIFWCRYPTO_INCLUDE_DIR
    NAMES rpifwcrypto.h
)

find_library(RPIFWCRYPTO_LIBRARY
    NAMES rpifwcrypto
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(rpifwcrypto
    REQUIRED_VARS RPIFWCRYPTO_LIBRARY RPIFWCRYPTO_INCLUDE_DIR
    FAIL_MESSAGE "Could not find librpifwcrypto"
)

if(RPIFWCRYPTO_FOUND)
    set(RPIFWCRYPTO_LIBRARIES ${RPIFWCRYPTO_LIBRARY})
    set(RPIFWCRYPTO_INCLUDE_DIRS ${RPIFWCRYPTO_INCLUDE_DIR})
endif()

mark_as_advanced(RPIFWCRYPTO_INCLUDE_DIR RPIFWCRYPTO_LIBRARY)
