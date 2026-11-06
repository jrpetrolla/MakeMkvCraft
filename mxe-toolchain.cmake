# CMake toolchain file for MXE x86_64-w64-mingw32.shared
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MXE_ROOT "$ENV{MXE_ROOT}" CACHE PATH "MXE root directory")
set(MXE_TARGET "x86_64-w64-mingw32.shared" CACHE STRING "MXE target triplet")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES MXE_ROOT MXE_TARGET)

if(NOT MXE_ROOT)
    message(FATAL_ERROR "Set MXE_ROOT with -DMXE_ROOT=/path/to/mxe or the MXE_ROOT environment variable")
endif()
set(MXE_PREFIX "${MXE_ROOT}/usr/${MXE_TARGET}")

set(CMAKE_C_COMPILER   "${MXE_ROOT}/usr/bin/${MXE_TARGET}-gcc")
set(CMAKE_CXX_COMPILER "${MXE_ROOT}/usr/bin/${MXE_TARGET}-g++")
set(CMAKE_RC_COMPILER  "${MXE_ROOT}/usr/bin/${MXE_TARGET}-windres")

set(CMAKE_FIND_ROOT_PATH "${MXE_PREFIX}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Qt5 path
set(Qt5_DIR "${MXE_PREFIX}/qt5/lib/cmake/Qt5")

# pkg-config for cross-compile
set(ENV{PKG_CONFIG_PATH} "${MXE_PREFIX}/lib/pkgconfig")
set(ENV{PKG_CONFIG_LIBDIR} "${MXE_PREFIX}/lib/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${MXE_PREFIX}")
