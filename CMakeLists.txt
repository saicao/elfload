cmake_minimum_required(VERSION 3.10) 
project(elfloader)
if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "ReleaseWithDebInfo")
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(TARGET_PLATFROM CACHE STRING "Target platform")
if (TARGET_PLATFROM STREQUAL "ios")
  set(CMAKE_SYSTEM_NAME "iOS")
  set(CMAKE_SYSTEM_VERSION "8.0")
  set(CMAKE_OSX_SYSROOT "/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk")
  set(CMAKE_OSX_ARCHITECTURES "arm64")
  
elseif(TARGET_PLATFROM STREQUAL "macos")
  set(CMAKE_SYSTEM_NAME "macOS")
  set(CMAKE_SYSTEM_VERSION "10.14")
  set(CMAKE_OSX_SYSROOT "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk")
  set(CMAKE_OSX_ARCHITECTURES "arm64")
else()
  message(FATAL_ERROR "Unknown target platform")
endif()



if (CMAKE_COMPILER_IS_GNUCC)
  add_compile_options(-Wall -Wextra)
endif (CMAKE_COMPILER_IS_GNUCC)
if (TARGET_PLATFROM STREQUAL "ios")
  # "-target arm64e-apple-ios8.0"
  add_compile_options("-target" "arm64e-apple-ios8.0")
endif()

add_library("elfload"
  "elf.h"
  "elfarch.h"
  "elfload.h"
  "elfload.c"
  "elfreloc_aarch64.c"
  "elfreloc_amd64.c"
  "elfreloc_i386.c")


add_executable("elfloader" "elfloader.c")
target_link_options("elfloader" PRIVATE "-DDEBUG")
target_link_libraries("elfloader" "elfload")

# add_executable("sample" "sample.c")

if (CMAKE_COMPILER_IS_GNUCC)
	set_target_properties("sample" PROPERTIES COMPILE_OPTIONS "-nostartfiles")
	set_target_properties("sample" PROPERTIES LINK_FLAGS "-nostdlib")
endif (CMAKE_COMPILER_IS_GNUCC)
