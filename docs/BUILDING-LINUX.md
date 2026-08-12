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

## 1. Dependencies — the local sysroot (canonical)

The build does **not** require root. The `-dev` packages and their full
dependency closure are downloaded with `apt-get download` and unpacked with
`dpkg -x` into a local sysroot, which is then exposed to the compiler, the
linker, `pkg-config` and CMake through environment variables. This is the
supported, reproducible path on this bench; the matching runtime libraries are
already present on any desktop Ubuntu, so only headers and dev symlinks are
being supplied.

```
# one-time
mkdir -p /data/build/aptdev/debs && cd /data/build/aptdev/debs
apt-get install --print-uris -y \
  libx11-dev libxext-dev libxinerama-dev libxrandr-dev libxcursor-dev \
  libxcomposite-dev libxrender-dev libfreetype-dev libfontconfig1-dev \
  libgl1-mesa-dev libglu1-mesa-dev mesa-common-dev \
  libogg-dev libopenblas-dev liblapacke-dev \
  | grep -oP "^'\K[^']+" > urls.txt
xargs -n1 -P4 curl -sfLO < urls.txt
apt-get download libtmglib3 liblapack3 libblas3 libgfortran5
for d in *.deb; do dpkg -x "$d" ../sysroot/; done
```

Two fix-ups the unpacked tree needs, because `dpkg -x` runs no maintainer
scripts and no `update-alternatives`:

```
cd /data/build/aptdev/sysroot/usr/lib/x86_64-linux-gnu
# 1. dev symlinks (libX11.so -> libX11.so.6.4.0) dangle inside the sysroot;
#    repoint them at the real system libraries
for l in $(find . -maxdepth 1 -type l -name '*.so*'); do
  t=$(readlink "$l"); case "$t" in /*) continue;; esac
  [ -e "$t" ] || for s in /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu; do
      [ -e "$s/$t" ] && ln -sf "$s/$t" "$t" && break; done
done
# 2. BLAS/LAPACK alternatives symlinks
ln -sf lapack/liblapack.so.3 liblapack.so.3
ln -sf blas/libblas.so.3     libblas.so.3
```

Then source `docs/evidence/b1-linux/bench-sysroot-env.sh`, which sets
`PKG_CONFIG_PATH`, `PKG_CONFIG_SYSROOT_DIR`, `CPATH` (including
`…/freetype2` and `…/openblas-pthread` for `cblas.h`), `LIBRARY_PATH`,
`CMAKE_PREFIX_PATH` and `LD_LIBRARY_PATH`.

`libasound2-dev` (ALSA) is expected from the system; it was already present.

### If you prefer a system install instead

```
sudo apt install -y \
  libx11-dev libxext-dev libxinerama-dev libxrandr-dev libxcursor-dev \
  libxcomposite-dev libxrender-dev \
  libfreetype-dev libfontconfig1-dev \
  libgl1-mesa-dev libglu1-mesa-dev mesa-common-dev \
  libogg-dev libopenblas-dev liblapacke-dev
```

This is optional and not blocking. With it you can drop the env file and the
explicit `-DOPENBLAS_LIBRARY` / `-DLAPACKE_LIBRARY` flags, because SAF's own
`find_library` and the alternatives symlinks resolve on their own.

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
. docs/evidence/b1-linux/bench-sysroot-env.sh
L=/data/build/aptdev/sysroot/usr/lib/x86_64-linux-gnu
cmake -S . -B /data/build/bridge-build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_VST3=ON \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux.cmake \
      -DOPENBLAS_LIBRARY=$L/openblas-pthread/libopenblas.so \
      -DLAPACKE_LIBRARY="$L/liblapacke.so;$L/libtmglib.so.3;$L/liblapack.so.3;$L/libblas.so.3"
cmake --build /data/build/bridge-build -j"$(nproc)"
```

The two library flags are needed only on the sysroot path. `LAPACKE_LIBRARY`
must list the transitive libraries as well: `ld` does not resolve a shared
library's `DT_NEEDED` symbols unless those libraries also appear on the link
line, and Debian's `liblapacke.so` pulls `libtmglib`, `liblapack` and `libblas`.

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

REAPER must be launched with the sysroot env sourced until the optional system
install is done — otherwise the plugins fail to load with
`liblapacke.so.3 => not found`.

## 6. The B1-Linux audio gate

**Definition (corrected 2026-08-10).** The earlier wording — "audio passes
through the renderer plugin on a test track" — does not match this software and
can never pass: the renderer sets `JucePlugin_IsSynth=1`, has no audio input
bus, and is not an insert effect. It is the monitoring/master-position endpoint
that renders Audio Elements published by `AudioElementPlugin` instances over
ZeroMQ (`tcp://localhost:5555`; renderer binds, panners connect — so both live
in one REAPER instance, and no companion process is needed).

The gate is:

1. A source track with a real test signal (997 Hz, −18 dBFS) and the **Eclipsa
   Audio Element Plugin** as FX.
2. The **Eclipsa Audio Renderer** on a separate monitoring-position track,
   created *first* so it binds the port before any panner connects.
3. The source routed into the renderer track, with `B_MAINSEND=0` on the source
   so nothing dry can reach the master.
4. A rendered master that is **non-silent with plausible spatial content**,
   checked against a negative control that removes only the renderer.

Run it:

```
. docs/evidence/b1-linux/bench-sysroot-env.sh
cd /data/build/b1-gate
reaper -nosplash -new docs/evidence/b1-linux/b1_gate.lua     # run SYNCHRONOUSLY
```

Do **not** launch REAPER backgrounded from a compound shell command — the
script then races and may not run at all. The harness quits via
`Main_SaveProjectEx` so the "save changes?" modal never blocks it.

**Every harness must delete the artefacts it is about to write, before it
writes them.** Two separate REAPER behaviours make a stale file look like a
plugin bug: a render whose output file already exists is *silently skipped*
(this is what made the first B1 control flaky), and `Main_SaveProjectEx` over
an existing `.rpp` raises an invisible overwrite modal that hangs a headless
run until it is killed.

### Audio device (required for any export gate)

An export only finalises when the host returns to realtime — see
`BRIDGE-B2-PLAN.md` §11 — so REAPER needs a working audio device even
headless. Neither JACK nor hardware is required; REAPER's own Dummy Audio
driver is enough and is what this bench uses. Set it in `~/.config/REAPER/reaper.ini`:

```ini
[reaper]
linux_audio_mode=2        ; 0,1,4 = no device · 2 = Dummy Audio · 3 = PulseAudio
linux_audio_srate=48000
linux_audio_bsize=1024
```

Confirm from a script with `reaper.Audio_IsRunning()` and
`reaper.GetAudioDeviceInfo("MODE")` before trusting any export result.

Both plugins need authored state before any audio flows (the panner's
`firstOutputChannel` is −1 until an Audio Element is assigned, which is normally
a GUI action). `docs/evidence/b1-linux/rewrite_chunk.py` writes that state
directly into the VST3 chunks. See `docs/B1-LINUX-EVIDENCE.md` §4.

**Known defect:** REAPER segfaults on exit whenever the Renderer plugin has been
instantiated, always after the render completes. Renders are unaffected.

## Licence note

This tree builds under **JUCE 7.0.12**, whose licence is the JUCE 7 EULA *or*
GPLv3 (not AGPL — that is JUCE 8). The owner's 2026-08-10 decision is to
develop under GPLv3 with **no distribution**; the commercial-vs-open question
is deferred to first external release. Read `CLEAN-ROOM-LOG.md` before shipping
anything built from this tree.
