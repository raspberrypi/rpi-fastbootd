set(COMMON_CPPFLAGS
    -D_GNU_SOURCE
)

set(LIB_SOURCES
    backed_block.cpp
    output_file.cpp
    sparse.cpp
    sparse_crc32.cpp
    sparse_err.cpp
    sparse_read.cpp
)

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

add_library(libsparse STATIC ${LIB_SOURCES})
target_compile_definitions(libsparse PRIVATE ${COMMON_CPPFLAGS})
target_compile_options(libsparse PRIVATE
    ${FDISK_CFLAGS_OTHER}
    ${URING_CFLAGS_OTHER}
    ${SYSTEMD_CFLAGS_OTHER}
)
target_include_directories(libsparse PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}
    /usr/include/android
    ${ZLIB_INCUDE_DIRS}
)

target_include_directories(libsparse PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/include
    )

target_link_directories(libsparse PRIVATE "/usr/lib/aarch64-linux-gnu/android")
target_link_libraries(libsparse PRIVATE 
    ${ZLIB_LIBRARIES}
    base
)

# Define install, packaging, signing tasks
install(TARGETS libsparse
        BUNDLE DESTINATION .
        RUNTIME DESTINATION /usr/lib 
)

