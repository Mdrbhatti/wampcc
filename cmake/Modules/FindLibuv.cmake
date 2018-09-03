# Locate libuv library
# This module defines
#  LIBUV_FOUND, if false, do not try to link to libuv
#  LIBUV_LIBRARIES
#  LIBUV_INCLUDE_DIRS, where to find uv.h
# NOTE: LIBUV_ROOT should be set on windows, as library install/include paths differ from linux

FIND_PATH(LIBUV_INCLUDE_DIRS NAMES uv.h HINTS $ENV{LIBUV_ROOT}/include)
FIND_LIBRARY(LIBUV_LIBRARIES NAMES uv libuv HINTS $ENV{LIBUV_ROOT}/Debug/lib $ENV{LIBUV_ROOT}/lib)

if(WIN32)
  list(APPEND LIBUV_LIBRARIES iphlpapi)
  list(APPEND LIBUV_LIBRARIES psapi)
  list(APPEND LIBUV_LIBRARIES userenv)
  list(APPEND LIBUV_LIBRARIES ws2_32)
endif()

INCLUDE(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(LIBUV DEFAULT_MSG LIBUV_LIBRARIES LIBUV_INCLUDE_DIRS)
 