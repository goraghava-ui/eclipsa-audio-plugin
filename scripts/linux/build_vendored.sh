#!/usr/bin/env bash
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

# FRIDAY Bridge — build the four vendored dependencies for Linux.
#
# Upstream Eclipsa ships these as prebuilt binaries for macOS (.dylib/.a) and
# Windows (.dll/.lib) only. There is no .so for any of them, which is one half
# of why a Linux build was impossible (the other half was the missing
# toolchain/prebuiltLibs pair — see cmake/toolchains/linux.cmake).
#
# Each library is pinned to the SAME commit the vendored macOS/Windows binaries
# were built from, as recorded in the third_party/*/README.md files, so the
# Linux artefacts are the same code as the other platforms'.
#
# Output: third_party/{gpac,iamftools,obr,libear}/lib/linux/
# Work trees: $BUILD_ROOT (default /data/build/bridge-deps) — deliberately NOT
# under the repo and NOT on this bench's small root filesystem.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_ROOT="${BUILD_ROOT:-/data/build/bridge-deps}"
BAZEL_ROOT="${BAZEL_ROOT:-/data/build/bazel}"
JOBS="$(nproc)"

# Pinned commits — from third_party/<dep>/README.md.
OBR_COMMIT=212982959ef5fe0de36f666c0450e11c711c58e8
IAMFTOOLS_COMMIT=7542365c18d02ea4857c492963c50788cf20158e
GPAC_COMMIT=4f33ccde09bce9e6d56a56304250fa49893f5019
# libear: third_party/libear/README.txt says "Version: 0.9.0, Built:
# 2024-05-02". ebu/libear publishes no tags; master's CMakeLists declares
# 0.9.0 and this is the closest commit before that build date.
LIBEAR_COMMIT=2db69f8

log() { printf '\n=== %s ===\n' "$*"; }

need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "missing tool: $1${2:+ — $2}" >&2
        exit 1
    }
}

need git
need cmake
need ninja
need bazelisk "install from https://github.com/bazelbuild/bazelisk/releases (no sudo needed: drop the binary in ~/.local/bin)"

mkdir -p "$BUILD_ROOT" "$BAZEL_ROOT"

# clone_at <url> <dir> <commit> [--recurse]
clone_at() {
    local url=$1 dir=$2 commit=$3 recurse=${4:-}
    if [ ! -d "$BUILD_ROOT/$dir/.git" ]; then
        log "cloning $dir"
        git clone -q ${recurse:+--recurse-submodules} "$url" "$BUILD_ROOT/$dir"
    fi
    git -C "$BUILD_ROOT/$dir" checkout -q "$commit"
    [ -n "$recurse" ] && git -C "$BUILD_ROOT/$dir" submodule update --init --recursive -q
    return 0
}

#--------------------------------------------------------------------
# 1. obr — Open Binaural Renderer (BSD-3-Clause)
#--------------------------------------------------------------------
# obr has a CMakeLists (so no Bazel needed, unlike what its README assumes),
# but it hardcodes `add_library(obr STATIC)`. Eclipsa imports it as a SHARED
# library on every platform, so flip it. This edits only the throwaway build
# tree, never our repo.
log "obr"
clone_at https://github.com/google/obr obr "$OBR_COMMIT"
sed -i 's/^add_library(obr STATIC)$/add_library(obr SHARED)/' "$BUILD_ROOT/obr/CMakeLists.txt"
cmake -S "$BUILD_ROOT/obr" -B "$BUILD_ROOT/obr-build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON
cmake --build "$BUILD_ROOT/obr-build" -j "$JOBS"
install -D -m 0755 "$BUILD_ROOT/obr-build/libobr.so" \
    "$REPO_ROOT/third_party/obr/lib/linux/libobr.so"

#--------------------------------------------------------------------
# 2. gpac — LGPL-2.1, MUST stay a shared library
#--------------------------------------------------------------------
# --disable-player looks like an obvious size win but breaks the link:
# filters/resample_audio.c calls gf_mixer_set_config, which lives in the
# compositor's audio mixer. Leave the player in.
log "gpac"
clone_at https://github.com/gpac/gpac gpac "$GPAC_COMMIT" --recurse
(
    cd "$BUILD_ROOT/gpac"
    ./configure --prefix="$BUILD_ROOT/gpac-install" --enable-pic --disable-x11 --disable-ssl
    make -j "$JOBS" lib
)
GPAC_SO=$(ls "$BUILD_ROOT/gpac/bin/gcc/"libgpac.so.*.*.* | head -1)
GPAC_SONAME=$(basename "$GPAC_SO")
GPAC_MAJOR=${GPAC_SONAME%.*.*}
install -D -m 0755 "$GPAC_SO" "$REPO_ROOT/third_party/gpac/lib/linux/$GPAC_SONAME"
ln -sf "$GPAC_SONAME" "$REPO_ROOT/third_party/gpac/lib/linux/$GPAC_MAJOR"
ln -sf "$GPAC_SONAME" "$REPO_ROOT/third_party/gpac/lib/linux/libgpac.so"

#--------------------------------------------------------------------
# 3. iamf-tools (BSD-3-Clause + AOM Patent 1.0) — Bazel only
#--------------------------------------------------------------------
# Upstream publishes no shared-library target. We append a cc_binary with
# linkshared=True over the two public API targets Eclipsa consumes
# (iamf_encoder_factory / iamf_decoder_factory) — the same pattern the Eclipsa
# third_party/obr README documents for obr.
#
# Two settings here are load-bearing, and getting either wrong produces a .so
# that builds successfully and is useless:
#   linkstatic = True  — with False, Bazel emits a ~35 KB stub that DT_NEEDEDs
#                        one mangled .so per internal target
#                        (libiamf_Sapi_Sdecoder_Slibiamf_Udecoder.so, ...).
#   alwayslink = True  — on the two factory cc_library targets. Without it the
#                        linker drops every object, because a shared library
#                        has no entry point referencing them: the result is a
#                        ~15 KB .so exporting nothing.
# A correct build is ~13 MB and exports iamf_tools::api::Iamf{Decoder,Encoder}
# Factory::* with only libc/libstdc++/libm/libgcc_s as NEEDED.
#
# --output_user_root keeps Bazel's cache off the root filesystem; its default
# (~/.cache/bazel) would fill a small / very quickly.
log "iamf-tools"
clone_at https://github.com/AOMediaCodec/iamf-tools iamf-tools "$IAMFTOOLS_COMMIT"
IAMF_BUILD_FILE="$BUILD_ROOT/iamf-tools/iamf/include/iamf_tools/BUILD"
if ! grep -q 'name = "libiamf_tools.so"' "$IAMF_BUILD_FILE"; then
    python3 - "$IAMF_BUILD_FILE" <<'PY'
import re, sys
path = sys.argv[1]
src = open(path).read()
for name in ("iamf_decoder_factory", "iamf_encoder_factory"):
    m = re.search(r'(name = "%s",\n    srcs = \["%s\.cc"\],\n)' % (name, name), src)
    if m and "alwayslink" not in src[m.start():m.start() + 600]:
        src = src[:m.end()] + "    alwayslink = True,\n" + src[m.end():]
src += '''
# FRIDAY Bridge — shared library for the Eclipsa/Bridge CMake build.
cc_binary(
    name = "libiamf_tools.so",
    linkshared = True,
    linkstatic = True,
    visibility = ["//visibility:public"],
    deps = [
        ":iamf_decoder_factory",
        ":iamf_encoder_factory",
    ],
)
'''
open(path, "w").write(src)
PY
fi
(
    cd "$BUILD_ROOT/iamf-tools"
    bazelisk --output_user_root="$BAZEL_ROOT" build -c opt \
        //iamf/include/iamf_tools:libiamf_tools.so
)
install -D -m 0755 \
    "$BUILD_ROOT/iamf-tools/bazel-bin/iamf/include/iamf_tools/libiamf_tools.so" \
    "$REPO_ROOT/third_party/iamftools/lib/linux/libiamf_tools.so"

#--------------------------------------------------------------------
# 4. libear (Apache-2.0) — static is fine
#--------------------------------------------------------------------
# Needs Boost headers (>= 1.57). Eigen and xsimd come from libear's own
# submodules, so --recurse-submodules is required.
log "libear"
clone_at https://github.com/ebu/libear libear "$LIBEAR_COMMIT" --recurse

# libear's find_package(Boost 1.57 REQUIRED) is headers-only (optional,
# variant, math, algorithm/clamp, make_unique). Rather than add a system
# package, fetch the same Boost version Eclipsa already uses. Note it must be
# the CLASSIC source tarball: the boost-*-cmake.tar.gz release has a modular
# layout (libs/*/include/boost/...) that CMake's legacy FindBoost cannot
# resolve, and libear calls find_package in MODULE mode.
BOOST_VER=1_86_0
BOOST_DIR="$BUILD_ROOT/boost_${BOOST_VER}"
if [ ! -d "$BOOST_DIR/boost" ]; then
    log "fetching Boost ${BOOST_VER} headers"
    curl -fL -o "$BUILD_ROOT/boost_${BOOST_VER}.tar.gz" \
        "https://archives.boost.io/release/1.86.0/source/boost_${BOOST_VER}.tar.gz"
    tar xzf "$BUILD_ROOT/boost_${BOOST_VER}.tar.gz" -C "$BUILD_ROOT"
fi

cmake -S "$BUILD_ROOT/libear" -B "$BUILD_ROOT/libear-build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=OFF -DEAR_UNIT_TESTS=OFF -DEAR_EXAMPLES=OFF \
    -DBOOST_ROOT="$BOOST_DIR" -DBoost_NO_SYSTEM_PATHS=ON
cmake --build "$BUILD_ROOT/libear-build" -j "$JOBS"
install -D -m 0644 "$(find "$BUILD_ROOT/libear-build" -name 'libear.a' | head -1)" \
    "$REPO_ROOT/third_party/libear/lib/linux/libear.a"

log "done"
ls -la "$REPO_ROOT"/third_party/{gpac,iamftools,obr,libear}/lib/linux/
