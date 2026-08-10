# Copyright 2025 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# FRIDAY Bridge — Linux toolchain.
#
# Upstream Eclipsa supports macOS and Windows only (README: "MacOS 14.7.1 or
# Windows 11"). This file is the Linux third of the pair that
# third_party/CMakeLists.txt and the plugin targets read through
# ${ECLIPSA_PLATFORM}; its companion is cmake/prebuiltLibs/linux.cmake.
#
# Target host: Ubuntu, gcc 15.2, REAPER as the VST3 host. No AAX and no AU on
# Linux — VST3 and JUCE Standalone only.

#====================================================================
# Include Guard
#====================================================================
if (DEFINED _LINUX_TOOLCHAIN_INCLUDED)
    return()
endif ()
set(_LINUX_TOOLCHAIN_INCLUDED TRUE)

#====================================================================
# Platform Identifier
#====================================================================
set(ECLIPSA_PLATFORM "linux" CACHE STRING "" FORCE)

#====================================================================
# RPATH Configuration
#====================================================================
# The vendored .so files are copied next to the built plugin by
# copy_resources.cmake / deploy_runtime_deps.cmake, so $ORIGIN is what matters
# at run time. The source-tree paths are kept for the build tree, mirroring
# what macos.cmake does with @loader_path.
get_filename_component(_ECLIPSA_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(CMAKE_BUILD_RPATH
        "${_ECLIPSA_ROOT}"
        "${_ECLIPSA_ROOT}/third_party/gpac/lib/linux"
        "${_ECLIPSA_ROOT}/third_party/iamftools/lib/linux"
        "${_ECLIPSA_ROOT}/third_party/obr/lib/linux"
        CACHE STRING ""
)
set(CMAKE_INSTALL_RPATH "$ORIGIN" CACHE STRING "")
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE CACHE BOOL "")
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE CACHE BOOL "")

#====================================================================
# Position-independent code
#====================================================================
# A VST3 is a shared object; every static dependency linked into it must be
# built -fPIC or the link fails with "recompile with -fPIC". On macOS/Windows
# this is the default, on Linux it is not.
set(CMAKE_POSITION_INDEPENDENT_CODE ON CACHE BOOL "" FORCE)

#====================================================================
# Build Settings
#====================================================================
set(IAMF_LIB_NAME "iamf" CACHE STRING "")
set(ECLIPSA_IAMF_LIB_DIR "${CMAKE_BINARY_DIR}/_deps/libiamf-build" CACHE STRING "")
set(ECLIPSA_STATIC_LIB_SUFFIX ".a" CACHE STRING "")

# No AU (Apple-only) and no AAX (not supported on Linux by Avid). The base
# list in the root CMakeLists already carries Standalone, and VST3 is added by
# -DBUILD_VST3=ON.
set(ECLIPSA_PLATFORM_PLUGIN_FORMATS "" CACHE STRING "")

set(ECLIPSA_PLUGIN_PLATFORM_SOURCES
        "${CMAKE_SOURCE_DIR}/common/processors/file_output/FilePermissions_linux.cpp"
        CACHE STRING "")

set(ECLIPSA_PLATFORM_LIBS vendored_obr CACHE STRING "")

# SAF's performance backend. Apple Accelerate does not exist here and Intel MKL
# is proprietary (Intel Simplified Software Licence) — OpenBLAS + LAPACKE is
# BSD-3-Clause and stays inside the FRIDAY licence firewall. See
# CLEAN-ROOM-LOG.md §2.
set(SAF_PERFORMANCE_LIB "SAF_USE_OPEN_BLAS_AND_LAPACKE" CACHE STRING "")

set(LIBIAMF_VENDOR_PATH "${CMAKE_SOURCE_DIR}/third_party/libiamf/lib/linux" CACHE PATH "")

# Linux plugins are plain directories/files, not macOS bundles — no Resources
# subdirectory. copy_resources.cmake treats an empty value as "alongside the
# binary".
set(ECLIPSA_BUNDLE_RESOURCES_SUBDIR "" CACHE STRING "")
