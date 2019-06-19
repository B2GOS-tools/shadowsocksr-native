# -*- coding: utf-8 -*-
# ----------------------------------------------------------------------
# Copyright © 2011-2015, RedJack, LLC.
# All rights reserved.
#
# Please see the COPYING file in this distribution for license details.
# ----------------------------------------------------------------------

set(CMAKE_LEGACY_CYGWIN_WIN32 0)

cmake_minimum_required(VERSION 3.0)
set(PROJECT_NAME shadowsocksr-native)
set(RELEASE_DATE 2015-09-03)


####################################################################
set(CMAKE_SYSTEM_NAME AndroidORB2gOS)

set(CMAKE_SYSTEM_VERSION 21) # API level

set(CMAKE_ANDROID_ARCH_ABI armeabi-v7a)

set(CMAKE_ANDROID_NDK /path/to/android-ndk)

set(CMAKE_ANDROID_STL_TYPE gnustl_static)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu99")

# Android 5.0 以上需要在此处设置 PIE
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIE")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fPIE -pie")
# 配置使用 NDK Standalone Toolchain 编译
#set(NDK_STANDALONE_TOOLCHAIN ~/app/ndk_toolchain)
#set(CMAKE_SYSTEM_NAME AndroidARM)
#set(CMAKE_SYSTEM_VERSION 3)
#set(CMAKE_C_COMPILER ${NDK_STANDALONE_TOOLCHAIN}/arm-linux-androideabi/bin/gcc)
#set(CMAKE_CXX_COMPILER ${NDK_STANDALONE_TOOLCHAIN}/arm-linux-androideabi/bin/g++)
#set(CMAKE_FIND_ROOT_PATH ${NDK_STANDALONE_TOOLCHAIN})

# 使用 NDK 提供的头文件
add_definitions("--sysroot=${CMAKE_ANDROID_NDK}/sysroot")
####################################################################

project(${PROJECT_NAME} VERSION 0.6.0 LANGUAGES C)

set(with_crypto_library "mbedtls" CACHE STRING "build with the given crypto library, TYPE=mbedtls")

add_subdirectory(depends/mbedtls)
add_subdirectory(depends/zlib/staticbuild)
add_subdirectory(depends/pcre/staticbuild)

include ( cmake/dist.cmake )
include ( configure )

configure_file ( ${CMAKE_CURRENT_SOURCE_DIR}/config.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config.h )
ADD_DEFINITIONS(-DHAVE_CONFIG_H)

add_subdirectory(depends/libsodium)
add_subdirectory(libcork)
add_subdirectory(libipset)
add_subdirectory(depends/libuv)
add_subdirectory(depends/json-c/staticbuild)
add_subdirectory(depends/uv-mbed/win32)

include_directories( ${CMAKE_CURRENT_BINARY_DIR} )

add_subdirectory(libudns)
add_subdirectory(src)
