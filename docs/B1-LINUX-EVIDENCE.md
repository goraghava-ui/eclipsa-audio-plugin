# B1-Linux — milestone evidence

**Date:** 2026-08-10 · **Bench:** Dell Precision 3630, Ubuntu (resolute),
gcc 15.2.0, cmake 4.2.3, ninja 1.13.2 · **Branch:** `linux-port`

First successful build of the Eclipsa plugins on Linux. Upstream supports
macOS and Windows only, so nothing here has a precedent to compare against.

## Scorecard — **B1-Linux PASSED**

| B1 criterion | Result |
|---|---|
| VST3 builds | ✅ **PASS** — both plugins compile and link |
| Installs to `~/.vst3` | ✅ **PASS** |
| REAPER plugin scan is clean | ✅ **PASS** — both scanned, both instantiate |
| Audio flows panner → renderer, non-silent output | ✅ **PASS** — see §4 |

### The audio gate, corrected

The original wording — *"audio passes through the renderer plugin on a test
track"* — does not describe this software. `rendererplugin/CMakeLists.txt:61`
sets `JucePlugin_IsSynth=1`, and the renderer is not an insert effect: it is the
monitoring/master-position endpoint that renders **Audio Elements** published by
`AudioElementPlugin` instances. A renderer inserted on an audio track will
output silence no matter how healthy the build is, so the old wording could
never pass and would have failed a working plugin.

**Gate as now defined:** a REAPER project with a source track carrying a real
test signal + the Audio Element Plugin, the Renderer on a separate
monitoring-position track, the source routed into it, and a rendered master
that is **non-silent with plausible spatial content** — verified against a
negative control that removes only the renderer.

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

## 4. Audio gate — PASSED

Headless harness: `reaper -nosplash -new docs/evidence/b1-linux/b1_gate.lua`,
render via `Main_OnCommand(41824)`. Source signal = 12 ch / 24 bit / 48 kHz,
997 Hz at −18 dBFS in channels 1–2 only, ten channels digitally silent — the
same signal the KALA Phase 0 conformance test uses.

### Harness validation first

The harness itself was proved to carry audio *before* any plugin claim, as a
no-FX control. It reproduces the input exactly:

| ch | peak dBFS | rms dBFS |
|---|---|---|
| 1 | −18.00 | −21.01 |
| 2 | −18.00 | −21.01 |

(An earlier run of this control produced no file at all. That was an artefact of
launching REAPER backgrounded from a compound shell command — a race, not a
REAPER or plugin problem. Run the harness **synchronously**.)

### The gate run

Topology: renderer track created **first** so it binds `tcp://localhost:5555`
before any panner connects; source track routed into it by a 12-channel send
with `B_MAINSEND=0`, so the dry source physically cannot reach the master. Any
audio in the render therefore came out of the renderer.

| Render | Result |
|---|---|
| **gate.wav** — panner + renderer | ✅ **NON-SILENT** |
| **negctl.wav** — identical project, renderer removed | ✅ silent, as required |

`gate.wav`, 12 ch / 24 bit / 48 kHz / 2.000 s:

| ch | speaker | peak dBFS | rms dBFS |
|---|---|---|---|
| 3 | C | −30.74 | −33.77 |
| 5 | Ls | −27.75 | −30.77 |
| 6 | Rs | −27.75 | −30.77 |
| 7 | Lrs | −26.24 | −29.26 |
| 8 | Rrs | −26.24 | −29.26 |
| 1, 2, 4, 9–12 | L, R, LFE, heights | silent | silent |

**Plausibility.** The panner's default object position is centre
(X = Y = Z = 0), and the output is exactly what a centre-placed object should
produce: energy in C plus a symmetric spread across the four surrounds, L/R
pair matched to within 0.00 dB, no LFE and no height content. Summed output
power is ≈ −23.5 dBFS against a −18.0 dBFS input — about 5.5 dB of panning and
gain-normalisation loss, the right order for a VBAP spread. This is a rendered
spatial image, not a pass-through and not noise.

```
sha256  e82e86e8f1e6f0ccbe5d657a462731352f3f1fd2f8071e0ac57b1f697747992d  gate.wav
sha256  23e51d7556ca41170cab25d37c0daf9ff70255a4c560465e7803cd3983a39b0c  negctl.wav
sha256  0aba237ede114cb5b5ecc132b6782476936e40c0ca7e751da9c3449b66f3bf88  ctrl3.wav
sha256  94d9b5d7133002f857f120581b93fd97885eed4041f18517f395197c3839402f  test-714.wav
```

### What made it work: authored plugin state

Out of the box the panner publishes nothing. `AudioElementPluginProcessor`
gates all its work on `firstOutputChannel >= 0`, which is initialised to −1 and
only set from the **Audio Element Spatial Layout** repository — i.e. from the
GUI act of creating an Audio Element in the renderer and assigning the panner to
it. Headlessly that assignment has to be authored.

Both plugins store state as **plain XML inside the VST3 chunk**, so it can be
written directly (`docs/evidence/b1-linux/rewrite_chunk.py`). Chunk framing is
`<u32 payload_len><u32 1>"VC2!"<u32 xml_len><xml><tail>`; rewriting the XML
means recomputing the first and third fields.

Renderer — add the Audio Element and set the room layout:

```xml
<room_setup speaker_layout="7.1.4" …/>
<audio_elements>
  <audio_element id="a1b2c3d4e5f6470880b1c2d3e4f50011" name="AE1"
                 description="" channel_config="7" first_channel="0"/>
</audio_elements>
```

Panner — point at the same element and enable panning:

```xml
<audio_element_spatial_layout_repository_state …
    audio_element_id="a1b2c3d4e5f6470880b1c2d3e4f50011"
    first_channel="0" layout="7" layout_selected="1" panning_enabled="1"/>
```

`channel_config`/`layout` `7` is `k7Point1Point4`
(`common/substream_rdr/substream_rdr_utils/Speakers.h:140`). The two ids must
match — that is the whole binding between panner and renderer.

Note `PannerMute` is *not* a mute: `ParameterMetaData.h:30` maps `unmuteId` to
the string `"PannerMute"`, so its default `1.0 (On)` means **unmuted**.

### Open defect found while doing this

**REAPER segfaults on exit whenever the Renderer plugin was instantiated**
(`SIGSEGV`, sometimes `SIGABRT`, always *after* the render completes and the
project is saved). The negative control, which differs only in not loading the
renderer, exits cleanly with status 0. Renders are unaffected, but this is a
real Linux-side shutdown bug in the renderer — most likely its ZeroMQ SUB
socket teardown. Filed here rather than fixed; it does not block B1.

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
