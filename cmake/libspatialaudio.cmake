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

# Note: We're using a custom version of libspatialaudio for now
# since there is a bug in the original version. If the bug is fixed
# we should move back to the original
message(STATUS "Fetching LibSpatialAudio")
include(FetchContent)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

FetchContent_Declare(
  libspatialaudio
  GIT_REPOSITORY "https://github.com/WithACX/libspatialaudio.git"
  SOURCE_DIR ${CMAKE_BINARY_DIR}/_deps/libspatialaudio-src
)

# FRIDAY Bridge — libspatialaudio is LGPL-2.1-or-later.
#
# Upstream links `spatialaudio-static` under a tree-wide
# `set(BUILD_SHARED_LIBS OFF)`, which absorbs LGPL object code into both VST3
# binaries. LGPL-2.1 §6 then obliges us to let a user relink the plugin against
# a modified libspatialaudio — incompatible with shipping a closed binary, and
# forbidden outright by the KALA LICENSE-POLICY.md.
#
# So: build it SHARED and link the .so. `BUILD_SHARED_LIBS` has to be forced
# back ON around this one FetchContent, because third_party/CMakeLists.txt set
# it OFF for the whole tree and FetchContent_MakeAvailable inherits parent
# scope (its CMAKE_ARGS field is an ExternalProject option and is ignored —
# see CLEAN-ROOM-LOG.md §7.2).
#
# This is the B1 fix. B2 removes the dependency entirely by routing
# substream_rdr through KALA's own clean-room VBAP/AllRAD over the kala-cabi
# C ABI — see CLEAN-ROOM-LOG.md §3.1 option (b).
#
# Note the target names: libspatialaudio does NOT honour BUILD_SHARED_LIBS the
# usual way. It defines BOTH `spatialaudio-static` and `spatialaudio-shared`,
# each behind its own option (BUILD_STATIC_LIBS / BUILD_SHARED_LIBS, both
# default ON). So the shared target is `spatialaudio-shared`, and simply
# flipping BUILD_SHARED_LIBS is not enough — the link line below has to name it.
# It must be the NORMAL variable, not the cache entry: third_party/CMakeLists.txt
# line 15 does a plain `set(BUILD_SHARED_LIBS OFF)`, and a normal variable
# shadows the cache one for everything below it — including inside the
# add_subdirectory that FetchContent_MakeAvailable performs. Forcing only the
# cache entry silently has no effect. CMP0077 is NEW (set in the root
# CMakeLists), so the subproject's option() honours the normal variable.
set(_ECLIPSA_SAVED_BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS ON)
set(BUILD_STATIC_LIBS OFF)
FetchContent_MakeAvailable(libspatialaudio)
set(BUILD_SHARED_LIBS ${_ECLIPSA_SAVED_BUILD_SHARED_LIBS})
set(spatialaudio_SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/libspatialaudio-src")
set(spatialaudio_BUILD_DIR "${CMAKE_BINARY_DIR}/_deps/libspatialaudio-build")

add_library(libspatialaudio INTERFACE)

# Platform-specific compiler options
if(WIN32)
    target_compile_options(libspatialaudio INTERFACE /w)
elseif(APPLE)
    target_compile_options(libspatialaudio INTERFACE "-w")
else()
    # Linux and other Unix-like systems
    target_compile_options(libspatialaudio INTERFACE "-w")
endif()

# Link to the CMake target - CMake handles platform-specific library names.
# LGPL-2.1: link the SHARED target. Never `spatialaudio-static` — see the note
# above and CLEAN-ROOM-LOG.md §3.1.
if (TARGET spatialaudio-shared)
    # Windows: the sources carry no __declspec exports (the Linux .so never
    # needed them), so MSVC emits the DLL without an import .lib and the
    # plugin link dies at LNK1181. Export everything via a generated .def —
    # the LGPL shared-linking rationale above is platform-independent.
    if (WIN32)
        set_target_properties(spatialaudio-shared PROPERTIES
                WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
    endif ()
    target_link_libraries(libspatialaudio INTERFACE spatialaudio-shared)
elseif (TARGET spatialaudio)
    target_link_libraries(libspatialaudio INTERFACE spatialaudio)
elseif (TARGET spatialaudio-static)
    message(FATAL_ERROR
            "FRIDAY Bridge: libspatialaudio produced only a static target. "
            "Linking it would statically absorb LGPL-2.1 code into the VST3, "
            "which the licence policy forbids (CLEAN-ROOM-LOG.md §3.1). "
            "Check that BUILD_SHARED_LIBS was ON for this FetchContent.")
else ()
    message(FATAL_ERROR "FRIDAY Bridge: no libspatialaudio target was created.")
endif ()
