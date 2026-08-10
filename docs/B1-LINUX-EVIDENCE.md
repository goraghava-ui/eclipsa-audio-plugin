# B1-Linux — milestone evidence

**Date:** 2026-08-10 · **Bench:** Dell Precision 3630, Ubuntu (resolute),
gcc 15.2.0, cmake 4.2.3, ninja 1.13.2 · **Branch:** `linux-port`

First successful build of the Eclipsa plugins on Linux. Upstream supports
macOS and Windows only, so nothing here has a precedent to compare against.

## Scorecard

| B1 criterion | Result |
|---|---|
| VST3 builds | ✅ **PASS** — both plugins compile and link |
| Installs to `~/.vst3` | ✅ **PASS** |
| REAPER plugin scan is clean | ✅ **PASS** — both scanned, both instantiate |
| Audio passes through the renderer plugin | ⚠️ **NOT PROVEN** — see §4 |

## 1. Build

```
cmake -S . -B /data/build/bridge-build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DBUILD_VST3=ON \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux.cmake
cmake --build /data/build/bridge-build -j$(nproc)      # exit 0
```

Artefacts:

```
rendererplugin/RendererPlugin_artefacts/Release/VST3/
    Eclipsa Audio Renderer.vst3/Contents/x86_64-linux/Eclipsa Audio Renderer.so       16,674,968 B
audioelementplugin/AudioElementPlugin_artefacts/Release/VST3/
    Eclipsa Audio Element Plugin.vst3/Contents/x86_64-linux/…so                        11,979,064 B
```

Four source-level portability defects were found and fixed — none had ever
surfaced because they are all invisible on macOS and Windows:

| Defect | Where | Why it only breaks on Linux |
|---|---|---|
| `#include <ADMRenderer.h>` but the file is `AdmRenderer.h` | `common/substream_rdr/surround_panner/MonoToSpeakerPanner.h` | APFS/NTFS are case-insensitive; ext4 is not |
| `#include "Ebu128LoudnessMeter.h"` but the file is `EBU128LoudnessMeter.h` | `third_party/LUFSMeter/src/EBU128LoudnessMeter.cpp` | same |
| `const std::set<juce::Uuid> const f() const` — duplicate `const` | `rendererplugin/src/screens/mix_tabs/PresentationEditorTab.h` | clang/MSVC warn, gcc rejects |
| libspatialaudio's shared target is `spatialaudio-shared`, not `spatialaudio` | `cmake/libspatialaudio.cmake` | only reached because this fork stopped linking the static one |

One CMake trap worth recording: forcing `BUILD_SHARED_LIBS` as a **cache**
entry does nothing here. `third_party/CMakeLists.txt:15` sets it as a plain
variable, and a normal variable shadows the cache one for everything below —
including inside `FetchContent_MakeAvailable`'s `add_subdirectory`. The
libspatialaudio LGPL fix only works because it sets the *normal* variable.

## 2. Install

`JUCE_COPY_PLUGIN_AFTER_BUILD` is ON, so the build installs them itself:

```
~/.vst3/Eclipsa Audio Renderer.vst3/
~/.vst3/Eclipsa Audio Element Plugin.vst3/
```

## 3. REAPER scan — clean

REAPER 7.78 (`reaper778_linux_x86_64`, run from the extracted tree, not
installed to `/opt`). After launch, `~/.config/REAPER/reaper-vstplugins64.ini`
gained exactly two entries and no failure markers:

```
Eclipsa_Audio_Element_Plugin.vst3=8039CB61D028DD01,991870327{ABCDEF019182FAEB45636C7045636165,Eclipsa Audio Element Plugin (Eclipsa Project)
Eclipsa_Audio_Renderer.vst3=00162786D028DD01,1274971279{ABCDEF019182FAEB45636C7045637264,Eclipsa Audio Renderer (Eclipsa Project)
```

A cache line with a resolved VST3 class UID and product name means REAPER
loaded the binary, instantiated the factory and read its metadata — the scan
did not merely find the files.

Instantiation on a real track was confirmed separately via ReaScript:

```
TrackFX_AddByName("Eclipsa Audio Renderer") -> index 0
fx name: VST3: Eclipsa Audio Renderer (Eclipsa Project)
fx enabled: true
```

## 4. Audio pass-through — NOT PROVEN

Headless render harness (`reaper -nosplash -new <script>.lua`, render via
`Main_OnCommand(41824)`), source = 12 ch / 24 bit / 48 kHz, 997 Hz at
−18 dBFS in channels 1–2 only, ten channels silent — the same signal the KALA
Phase 0 conformance test uses.

| Render | Output |
|---|---|
| Renderer plugin on the track | 12 ch, 2.000 s, **all channels silent** |
| Audio Element (panner) on the track | 12 ch, 2.000 s, **all channels silent** |
| **No FX (control)** | **no file produced — harness unvalidated** |

**The silence is not attributable to the plugins.** The control run — the one
that would prove the harness passes audio at all — did not render a file, and
did not reproduce across attempts. Without a working baseline, "plugin outputs
silence" and "harness never fed it audio" are indistinguishable.

There is also a design reason to expect silence from the *renderer* in this
configuration: `rendererplugin/CMakeLists.txt:61` sets
**`JucePlugin_IsSynth=1`**, so JUCE gives it no audio input bus. Eclipsa's
renderer is not an insert effect — it receives audio from AudioElementPlugin
instances over the project's internal transport (ZeroMQ), not from the track it
sits on. "Audio passes through the renderer on a test track" may therefore be
the wrong shape of test for this architecture.

**Next session, in order:**
1. Fix the control: get a no-FX render to produce a non-silent file. Until that
   works nothing else here means anything.
2. If the control passes and the panner still outputs silence, that is a real
   defect — debug the panner's bus layout on Linux.
3. Re-frame the renderer test to match the architecture: panner on an audio
   track, renderer on a separate track, then check the renderer's output.

## 5. Bench caveat — the sysroot

The system packages in `docs/BUILDING-LINUX.md` §1 were **not installed** on
this bench (dpkg shows no such transaction). To make progress without root, the
`-dev` packages and their dependency closure were downloaded with
`apt-get download` and unpacked with `dpkg -x` into
`/data/build/aptdev/sysroot`, then exposed via `CPATH`, `LIBRARY_PATH`,
`PKG_CONFIG_PATH`, `PKG_CONFIG_SYSROOT_DIR`, `CMAKE_PREFIX_PATH` and
`LD_LIBRARY_PATH` (`/data/build/aptdev/env.sh`).

Consequences to undo once the real `apt install` runs:

- `OPENBLAS_LIBRARY` and `LAPACKE_LIBRARY` had to be passed explicitly, because
  the unpacked tree has no `update-alternatives` symlinks. With a real install,
  SAF's own `find_library` finds them.
- `LAPACKE_LIBRARY` needed the transitive list
  `liblapacke.so;libtmglib.so.3;liblapack.so.3;libblas.so.3` — `ld` does not
  resolve a shared library's `DT_NEEDED` symbols unless those libraries are
  also on the link line.
- The built plugins still report `liblapacke.so.3 => not found` without
  `LD_LIBRARY_PATH`, so **REAPER must be launched with
  `/data/build/aptdev/env.sh` sourced** until the packages are installed
  properly. `liblapacke3` arrives as a dependency of `liblapacke-dev`, so the
  documented apt line already covers it.
