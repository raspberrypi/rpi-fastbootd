cmake_minimum_required(VERSION 3.15)

project(fastboot_device_tests)

# Enable testing
enable_testing()

# Fetch Catch2 if not already available
include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.11.0  # Latest stable v3 release (requires C++14)
)

FetchContent_MakeAvailable(Catch2)

# Kernel log (klogctl) tests
add_executable(kernel_log_test kernel_log_test.cpp)

# Link against Catch2 (v3 uses Catch2WithMain for automatic main)
target_link_libraries(kernel_log_test PRIVATE Catch2::Catch2WithMain)

# Set C++ standard (Catch2 v3 requires C++14)
target_compile_features(kernel_log_test PRIVATE cxx_std_14)

# Include directories (if needed for headers)
target_include_directories(kernel_log_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# GPIO (libgpiod) tests
add_executable(gpio_test gpio_test.cpp)

# Link against Catch2
target_link_libraries(gpio_test PRIVATE Catch2::Catch2WithMain)

# Set C++ standard
target_compile_features(gpio_test PRIVATE cxx_std_14)

# Gateway (/proc) parsing tests  
add_executable(gateway_test gateway_test.cpp)

# Link against Catch2
target_link_libraries(gateway_test PRIVATE Catch2::Catch2WithMain)

# Set C++ standard
target_compile_features(gateway_test PRIVATE cxx_std_14)

# Crypto (libcryptsetup) tests
add_executable(crypto_test crypto_test.cpp ../crypto_native.cpp)

# Link against Catch2
target_link_libraries(crypto_test PRIVATE Catch2::Catch2WithMain)

# Set C++ standard
target_compile_features(crypto_test PRIVATE cxx_std_14)

# Verity (dm-verity via libcryptsetup) tests
add_executable(verity_test verity_test.cpp ../crypto_native.cpp)

# Link against Catch2
target_link_libraries(verity_test PRIVATE Catch2::Catch2WithMain)

# Set C++ standard
target_compile_features(verity_test PRIVATE cxx_std_14)

# Link libgpiod if available
if(GPIOD_FOUND)
    target_compile_definitions(gpio_test PRIVATE HAVE_LIBGPIOD)
    target_link_libraries(gpio_test PRIVATE ${GPIOD_LIBRARIES})
    target_include_directories(gpio_test PRIVATE ${GPIOD_INCLUDE_DIRS})
    message(STATUS "GPIO tests will run with libgpiod support")
else()
    message(STATUS "GPIO tests will run in fallback-only mode (libgpiod not available)")
endif()

# Set C++ standard
target_compile_features(gpio_test PRIVATE cxx_std_11)

# Include directories
target_include_directories(gpio_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# Link libcryptsetup if available
if(CRYPTSETUP_FOUND)
    target_compile_definitions(crypto_test PRIVATE HAVE_LIBCRYPTSETUP)
    target_link_libraries(crypto_test PRIVATE ${CRYPTSETUP_LIBRARIES})
    target_include_directories(crypto_test PRIVATE ${CRYPTSETUP_INCLUDE_DIRS})
    message(STATUS "Crypto tests will run with libcryptsetup support")
else()
    message(STATUS "Crypto tests will run in fallback-only mode (libcryptsetup not available)")
endif()

# Include directories for crypto tests
target_include_directories(crypto_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# Link libcryptsetup if available for verity tests
if(CRYPTSETUP_FOUND)
    target_compile_definitions(verity_test PRIVATE HAVE_LIBCRYPTSETUP)
    target_link_libraries(verity_test PRIVATE ${CRYPTSETUP_LIBRARIES})
    target_include_directories(verity_test PRIVATE ${CRYPTSETUP_INCLUDE_DIRS})
    message(STATUS "Verity tests will run with libcryptsetup support")
else()
    message(STATUS "Verity tests will run in fallback-only mode (libcryptsetup not available)")
endif()

# Include directories for verity tests
target_include_directories(verity_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# Firmware crypto (librpifwcrypto) tests via RpiFwCrypto C++ wrapper
# Link the pre-built utility.cpp.o from the fastbootd target to avoid
# re-compiling utility.cpp (which drags in the full transport stack).
set(UTILITY_OBJ $<TARGET_OBJECTS:fastbootd>)
add_executable(fwcrypto_test fwcrypto_test.cpp)

# Ensure fastbootd is built first so utility.cpp.o exists
add_dependencies(fwcrypto_test fastbootd)

target_link_directories(fwcrypto_test PRIVATE "/usr/lib/aarch64-linux-gnu/android")

# utility.cpp.o must appear before the libraries it depends on
target_link_options(fwcrypto_test PRIVATE
    ${CMAKE_BINARY_DIR}/fastboot/CMakeFiles/fastbootd.dir/device/utility.cpp.o
)
target_link_libraries(fwcrypto_test PRIVATE
    Catch2::Catch2WithMain
    ${RPIFWCRYPTO_LIBRARIES}
    OpenSSL::Crypto
    base
    log
)

set_property(TARGET fwcrypto_test PROPERTY CXX_STANDARD 23)
set_property(TARGET fwcrypto_test PROPERTY CXX_STANDARD_REQUIRED ON)

target_include_directories(fwcrypto_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    /usr/include/android
    ${RPIFWCRYPTO_INCLUDE_DIRS}
)

# Add test to CTest (try catch_discover_tests if available, else manual)
if(EXISTS "${catch2_SOURCE_DIR}/contrib/Catch.cmake")
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/contrib)
    include(Catch)
    catch_discover_tests(kernel_log_test)
else()
    message(STATUS "Catch test discovery not available, adding tests manually")
endif()

# Alternative: Manual test addition
add_test(NAME kernel_log_all_tests COMMAND kernel_log_test)
add_test(NAME gpio_all_tests COMMAND gpio_test)
add_test(NAME gateway_all_tests COMMAND gateway_test)
add_test(NAME crypto_all_tests COMMAND crypto_test)
add_test(NAME verity_all_tests COMMAND verity_test)

# Add specific test suites
add_test(NAME kernel_log_basic COMMAND kernel_log_test "[kernel_log]")
add_test(NAME kernel_log_errors COMMAND kernel_log_test "[errors]")
add_test(NAME kernel_log_performance COMMAND kernel_log_test "[performance]")

add_test(NAME gpio_basic COMMAND gpio_test "[gpio]")
add_test(NAME gpio_parsing COMMAND gpio_test "[parsing]")
add_test(NAME gpio_errors COMMAND gpio_test "[errors]")
add_test(NAME gpio_performance COMMAND gpio_test "[performance]")

add_test(NAME gateway_basic COMMAND gateway_test "[gateway]")
add_test(NAME gateway_ipv4 COMMAND gateway_test "[ipv4]")
add_test(NAME gateway_ipv6 COMMAND gateway_test "[ipv6]")
add_test(NAME gateway_errors COMMAND gateway_test "[errors]")
add_test(NAME gateway_performance COMMAND gateway_test "[performance]")

add_test(NAME crypto_luks COMMAND crypto_test "[crypto][luks]")
add_test(NAME crypto_performance COMMAND crypto_test "[crypto][performance]")

add_test(NAME verity_native COMMAND verity_test "[verity][native]")
add_test(NAME verity_activate COMMAND verity_test "[verity][activate]")
add_test(NAME verity_integration COMMAND verity_test "[verity][integration]")

add_test(NAME fwcrypto_all_tests COMMAND fwcrypto_test)
add_test(NAME fwcrypto_status COMMAND fwcrypto_test "[status]")
add_test(NAME fwcrypto_hmac COMMAND fwcrypto_test "[hmac]")
add_test(NAME fwcrypto_pubkey COMMAND fwcrypto_test "[pubkey]")

# Installation (optional)
install(TARGETS kernel_log_test gpio_test gateway_test crypto_test verity_test fwcrypto_test
        RUNTIME DESTINATION tests
)

# Print status
message(STATUS "Fastboot device tests configured")
message(STATUS "  - kernel_log_test: klogctl() syscall")
message(STATUS "  - gpio_test: libgpiod library")
message(STATUS "  - gateway_test: /proc gateway parsing")
message(STATUS "  - crypto_test: libcryptsetup LUKS operations")
message(STATUS "  - verity_test: dm-verity hash tree generation")
message(STATUS "  - fwcrypto_test: librpifwcrypto firmware crypto operations")
message(STATUS "  - Run tests with: ctest --test-dir build/fastboot/device/test")
message(STATUS "  - Or directly: build/fastboot/device/test/<test_name>")
message(STATUS "  - For verbose output: <test> -s")
message(STATUS "  - For benchmarks: kernel_log_test [benchmark]")
message(STATUS "")
message(STATUS "NOTE: Some tests require specific system configuration")
message(STATUS "      - kernel_log_test needs root for klogctl() access")
message(STATUS "      - gpio_test needs GPIO hardware access")
message(STATUS "      - gateway_test needs network configuration")
message(STATUS "      - crypto_test needs root for loopback devices")
message(STATUS "      - verity_test needs root for loopback devices")
message(STATUS "      - fwcrypto_test needs /dev/vcio (firmware mailbox)")
message(STATUS "      Run as root for full test coverage: sudo ctest")

