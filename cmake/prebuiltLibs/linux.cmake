# Copyright 2025 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.

# FRIDAY Bridge — Linux vendored libraries.
#
# Upstream ships prebuilt gpac / iamf-tools / obr / libear binaries for macOS
# (.dylib/.a) and Windows (.dll/.lib) only; no .so exists. These are built from
# source by scripts/linux/build_vendored.sh into third_party/*/lib/linux/,
# which is where this file expects them. That script keeps its own build trees
# under /data/build (this bench's root filesystem is small).
#
# gpac stays SHARED on purpose: it is LGPL-2.1 and the FRIDAY licence policy
# only permits LGPL when dynamically linked. Do not convert it to a static
# archive. See CLEAN-ROOM-LOG.md §2 and §3.1.

set(_ECLIPSA_LINUX_LIBDIR "lib/linux")

#====================================================================
# Missing-artefact diagnostic
#====================================================================
function(_eclipsa_require_vendored_lib _path _what)
    if (NOT EXISTS "${_path}")
        message(FATAL_ERROR
                "FRIDAY Bridge: vendored ${_what} not found at\n  ${_path}\n"
                "Build the Linux vendored libraries first:\n"
                "  ./scripts/linux/build_vendored.sh\n"
                "(see CLEAN-ROOM-LOG.md §4 — upstream ships no Linux binaries)")
    endif ()
endfunction()

#====================================================================
# Prebuilt Libraries
#====================================================================

# GPAC — LGPL-2.1, MUST remain shared.
set(_GPAC_SO "${CMAKE_SOURCE_DIR}/third_party/gpac/${_ECLIPSA_LINUX_LIBDIR}/libgpac.so")
_eclipsa_require_vendored_lib("${_GPAC_SO}" "gpac")
add_library(gpac_impl SHARED IMPORTED GLOBAL)
set_target_properties(gpac_impl PROPERTIES
        IMPORTED_LOCATION "${_GPAC_SO}"
        IMPORTED_NO_SONAME TRUE
)
add_library(vendored_gpac ALIAS gpac_impl)

# IAMF Tools
set(_IAMFTOOLS_SO "${CMAKE_SOURCE_DIR}/third_party/iamftools/${_ECLIPSA_LINUX_LIBDIR}/libiamf_tools.so")
_eclipsa_require_vendored_lib("${_IAMFTOOLS_SO}" "iamf-tools")
add_library(iamftools_impl SHARED IMPORTED GLOBAL)
set_target_properties(iamftools_impl PROPERTIES
        IMPORTED_LOCATION "${_IAMFTOOLS_SO}"
        IMPORTED_NO_SONAME TRUE
)
add_library(vendored_iamf_tools ALIAS iamftools_impl)

# OBR
set(_OBR_SO "${CMAKE_SOURCE_DIR}/third_party/obr/${_ECLIPSA_LINUX_LIBDIR}/libobr.so")
_eclipsa_require_vendored_lib("${_OBR_SO}" "obr")
add_library(obr_impl SHARED IMPORTED GLOBAL)
set_target_properties(obr_impl PROPERTIES
        IMPORTED_LOCATION "${_OBR_SO}"
        IMPORTED_NO_SONAME TRUE
)
add_library(vendored_obr ALIAS obr_impl)

# Libear — Apache-2.0, static is fine.
set(_LIBEAR_A "${CMAKE_SOURCE_DIR}/third_party/libear/${_ECLIPSA_LINUX_LIBDIR}/libear.a")
_eclipsa_require_vendored_lib("${_LIBEAR_A}" "libear")
add_library(libear_impl STATIC IMPORTED GLOBAL)
set_target_properties(libear_impl PROPERTIES
        IMPORTED_LOCATION "${_LIBEAR_A}"
)

#====================================================================
# Vendored Libraries
#====================================================================
set(ECLIPSA_VENDORED_LIBS
        vendored_gpac
        vendored_iamf_tools
        vendored_obr
        libzmq
        CACHE STRING ""
)
