cmake_minimum_required(VERSION 3.15)

project(rpiidp_test)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../rpiparted/include)

add_executable(rpiidp_ptest parser_test.cpp)
add_executable(rpiidp_dtest device_test.cpp)

target_link_libraries(rpiidp_ptest PRIVATE rpiidp ${JSONCPP_LIBRARIES})
target_link_libraries(rpiidp_dtest PRIVATE rpiidp ${JSONCPP_LIBRARIES})

target_include_directories(rpiidp_ptest PRIVATE ${JSONCPP_INCLUDE_DIR})
target_include_directories(rpiidp_dtest PRIVATE ${JSONCPP_INCLUDE_DIR})

# When built with IDP_FASTBOOT, rpiidp pulls in rpifwcrypto (via device.cpp)
# and the native libcryptsetup wrapper (via crypt.cpp).  Both test executables
# link the full rpiidp static library, so both need these dependencies
# resolved — even rpiidp_ptest, which only exercises the parser, because the
# linker sees the unresolved symbols from device.cpp.o and crypt.cpp.o inside
# librpiidp.a.
#
# utility.cpp cannot be recompiled in isolation (it includes
# fastboot_device.h).  Link the pre-built .o from the fastbootd target
# instead — the same approach used by fwcrypto_test.
if(IDP_FASTBOOT)
   find_package(OpenSSL REQUIRED)

   foreach(tgt rpiidp_ptest rpiidp_dtest)
      target_sources(${tgt} PRIVATE
         ${CMAKE_SOURCE_DIR}/fastboot/device/crypto_native.cpp)

      add_dependencies(${tgt} fastbootd)

      target_link_options(${tgt} PRIVATE
         ${CMAKE_BINARY_DIR}/fastboot/CMakeFiles/fastbootd.dir/device/utility.cpp.o)

      target_link_directories(${tgt} PRIVATE "/usr/lib/aarch64-linux-gnu/android")

      target_link_libraries(${tgt} PRIVATE
         ${RPIFWCRYPTO_LIBRARIES}
         OpenSSL::Crypto
         base
         log)

      target_include_directories(${tgt} PRIVATE
         ${CMAKE_SOURCE_DIR}/fastboot/device
         /usr/include/android
         ${RPIFWCRYPTO_INCLUDE_DIRS})

      if(CRYPTSETUP_FOUND)
         target_compile_definitions(${tgt} PRIVATE HAVE_LIBCRYPTSETUP)
         target_link_libraries(${tgt} PRIVATE ${CRYPTSETUP_LIBRARIES})
         target_include_directories(${tgt} PRIVATE ${CRYPTSETUP_INCLUDE_DIRS})
      endif()
   endforeach()

   set_property(TARGET rpiidp_dtest PROPERTY CXX_STANDARD 17)
   set_property(TARGET rpiidp_dtest PROPERTY CXX_STANDARD_REQUIRED ON)
endif()
