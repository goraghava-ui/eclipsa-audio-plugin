# Building FRIDAY Bridge on Linux

Upstream Eclipsa supports **macOS 14.7.1 or Windows 11** only (see the root
`README.md`). This fork adds Linux, because PRD-v2 V2-01 is REAPER-first and
the FRIDAY bench is a Linux box.

What was missing upstream, and what this fork adds:

| Gap | Fix |
|---|---|
| `cmake/toolchains/` had only macos/windows | `cmake/toolchains/linux.cmake` |
| `cmake/prebuiltLibs/` had only macos/windows, and `third_party/CMakeLists.txt` includes `prebuiltLibs/${ECLIPSA_PLATFORM}.cmake` — so a Linux configure died on that line | `cmake/prebuiltLibs/linux.cmake` |
| gpac / iamf-tools / obr / libear ship as `.dylib`/`.dll` only — no `.so` exists | `scripts/linux/build_vendored.sh`, artefacts committed under `third_party/*/lib/linux/` |
| `FilePermissions` had mac + windows implementations only | `FilePermissions_linux.cpp` (no-op, same as Windows) |
| `third_party/libiamf/CMakeLists.txt` branched on APPLE/WIN32 only | Linux branch: fdk-aac removed (licence firewall), libogg resolved from the system |
| libspatialaudio linked statically — LGPL-2.1 blocker | built and linked **shared** (`cmake/libspatialaudio.cmake`) |
| Nothing stopped SAF's GPLv2 modules being switched on | assert in `third_party/CMakeLists.txt` |

## 1. System packages

```
sudo apt install -y \
  libx11-dev libxext-dev libxinerama-dev libxrandr-dev libxcursor-dev \
  libxcomposite-dev libxrender-dev \
  libfreetype-dev libfontconfig1-dev \
  libgl1-mesa-dev libglu1-mesa-dev mesa-common-dev \
  libogg-dev libopenblas-dev liblapacke-dev
```

Why each group:

- **X11 + freetype + fontconfig + GL** — JUCE's Linux GUI backend. Without them
  the configure step fails while building `juceaide` with
  `fatal error: ft2build.h: No such file or directory` and
  `fatal error: X11/Xlib.h: No such file or directory`. `juceaide` is a host
  tool JUCE builds *during configure*, so this fails before any plugin source
  is compiled.
- **libogg-dev** — libiamf links `ogg` on non-Windows. Upstream vendors
  `libogg.a` for macOS only; on Linux it comes from the system.
- **libopenblas-dev / liblapacke-dev** — SAF's performance backend.
  `SAF_PERFORMANCE_LIB` is `SAF_USE_OPEN_BLAS_AND_LAPACKE` here: Apple
  Accelerate does not exist on Linux and Intel MKL is proprietary (Intel
  Simplified Software Licence), while OpenBLAS is BSD-3-Clause and stays inside
  the FRIDAY licence firewall — see `CLEAN-ROOM-LOG.md` §2.

`libasound2-dev` (ALSA) is also required; it was already present on this bench.

JUCE's web browser and cURL modules are already disabled by
`cmake/eclipsa_plugin_defaults.cmake` (`JUCE_WEB_BROWSER=0`, `JUCE_USE_CURL=0`),
so `libwebkit2gtk` and `libcurl` are **not** needed.

## 2. Submodules

`gh repo clone` does not recurse. Shallow is enough:

```
git submodule update --init --depth 1 \
    third_party/JUCE third_party/Spatial_Audio_Framework
```

## 3. Vendored libraries

Already committed under `third_party/*/lib/linux/`. To rebuild them from
source (pinned to the same commits the macOS/Windows binaries came from):

```
./scripts/linux/build_vendored.sh
```

It needs `bazelisk` on PATH for iamf-tools — a single static binary, no sudo:
drop the release from <https://github.com/bazelbuild/bazelisk/releases> into
`~/.local/bin`. Work trees go to `/data/build/bridge-deps` and Bazel's cache to
`/data/build/bazel`; both are overridable with `BUILD_ROOT` / `BAZEL_ROOT`.
**Do not let Bazel use its default `~/.cache/bazel` on this bench** — the root
filesystem has ~2.5 GB free and Bazel will exhaust it.

## 4. Configure and build

```
cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_VST3=ON \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux.cmake
cmake --build build -j"$(nproc)"
```

No AAX and no AU on Linux. `BUILD_AAX=ON` will fail looking for the AAX SDK;
AU is macOS-only. VST3 plus JUCE's Standalone format is the whole surface here.

## 5. Install for REAPER

```
mkdir -p ~/.vst3
cp -r build/rendererplugin/RendererPlugin_artefacts/Release/VST3/*.vst3 ~/.vst3/
cp -r build/audioelementplugin/AudioElementPlugin_artefacts/Release/VST3/*.vst3 ~/.vst3/
```

Then in REAPER: Options → Preferences → Plug-ins → VST → *Re-scan*. `~/.vst3`
is on REAPER's default VST3 search path.

## Licence note

This tree builds under **JUCE 7.0.12**, whose licence is the JUCE 7 EULA *or*
GPLv3 (not AGPL — that is JUCE 8). The owner's 2026-08-10 decision is to
develop under GPLv3 with **no distribution**; the commercial-vs-open question
is deferred to first external release. Read `CLEAN-ROOM-LOG.md` before shipping
anything built from this tree.
