# FRIDAY Bridge — B2 plan: swap the export stage to KALA

**Branch:** `b2-kala-export` (off `linux-port`) · **Date:** 2026-08-10
**Goal:** PRD-v2 gate GA — Bridge IAMF == Studio IAMF for the same pan.
**Principle (V2-01):** all DSP stays in KALA; plugins are transport + UI.
Eclipsa's own render path (obr / libspatialaudio) stays untouched so its
monitoring UI keeps working.

---

## 1. Where the export actually happens

```
AudioElementPlugin (source track)                 RendererPlugin (monitor track)
  processBlock                                      processBlock
    RemappingProcessor                                copies its INPUT buffer
    Panner3DProcessor  ◄── THE PAN HAPPENS HERE       runs audioProcessors_
      MonoToSpeakerPanner                             …
        admrender::CAdmRenderer  (libspatialaudio)  FileOutputProcessor
    MSProcessor                                       processBlock:124
    TrackMonitorProcessor                               iamfFileWriter_->writeFrame(buffer)
    AudioElementPluginDataPublisher ──ZMQ metadata──►     IAMFFileWriter::encodeBuffer
    SoundFieldProcessor                                     slices buf by
    RoutingProcessor                                        audioElement.firstChannel + i
        │                                                   → labels channels
        └── REAPER track routing (bed audio) ──────────────► iamfEncoder_->Encode(...)
                                                              iamf-tools
```

Concrete anchors:

| Thing | Location |
|---|---|
| Only iamf-tools encoder call site | `common/processors/file_output/iamf_export_utils/IAMFFileWriter.cpp:158` (`CreateFileGeneratingIamfEncoder`) |
| Encode entry per block | `IAMFFileWriter::encodeBuffer`, `…:292` |
| Export driver | `common/processors/file_output/FileOutputProcessor.cpp:124` (`writeFrame`), `:181` (writer construction) |
| Metadata → proto | `IAMFFileWriter::populate*FromRepository` (codec, audio elements, mix presentations) |
| **The pan** | `Panner3DProcessor` → `MonoToSpeakerPanner` → libspatialaudio `CAdmRenderer` (`common/substream_rdr/surround_panner/`) |
| Object position source | panner automation params `X`, `Y`, `Z` (+ `PannerVolume`, `PannerMute`=unmute) |
| Panner ↔ renderer transport | `common/data_structures/src/AudioElementCommunication.h` — ZeroMQ PUB/SUB on `tcp://localhost:5555`, **metadata only** |

### The finding that decides the design

**By the time anything reaches `IAMFFileWriter`, the audio is already a rendered
bed.** `encodeBuffer` reads channel slices at `audioElement.firstChannel + i` and
labels them; there are no objects and no positions left. The panning — the DSP
that the null test is actually about — happened much earlier, on the *source*
track, inside `Panner3DProcessor` via libspatialaudio.

So swapping only `IAMFFileWriter` would replace **iamf-tools with KALA's encoder
while keeping libspatialaudio's render**. The bitstream container would come
from KALA and the audio inside it would still be Eclipsa's pan. That cannot null
against Studio, whose bed comes from KALA's own VBAP (`kala_py.vbap_gains`,
`studio/renderer.py`). The seam has to be upstream of the pan, exactly as the
V2-01 principle says.

---

## 2. Two designs

### Design A — replace `Panner3DProcessor`'s DSP with KALA

Swap `MonoToSpeakerPanner` for a `kala-cabi` render call. Everything downstream
(routing, renderer, exporter) stays as-is; the bed that flows is KALA-rendered,
and `IAMFFileWriter` can then be swapped to KALA's encoder for the container.

- ✅ Small: `Panner3DProcessor`, `IAMFFileWriter`, kala-cabi, CMake.
- ❌ **Violates the brief.** Eclipsa's monitoring path *is* this path — changing
  it changes what the user hears and what the renderer UI draws. The instruction
  is that obr/libspatialaudio stays untouched for monitoring.

### Design B — parallel object capture, KALA renders + encodes at export ✅ recommended

Leave Eclipsa's chain completely alone. Add a tap that captures, per audio
element, the **pre-pan object audio** and its **position**, ships them to the
export stage, and has KALA render *and* encode there.

```
AudioElementPlugin                                  FileOutputProcessor
  processBlock
    ┌── FRIDAY tap (new) ─────────────┐               if FRIDAY_KALA_EXPORT:
    │  mono object PCM (pre-pan)      │                 KalaIamfWriter
    │  az / el / distance / gain      │──ZMQ object──►    kala_render_objects_714()
    │  audio_element_id               │   channel         kala_encode_iamf()
    └─────────────────────────────────┘                 → bridge.iamf
    RemappingProcessor                              else:
    Panner3DProcessor   (untouched — monitoring)      IAMFFileWriter (iamf-tools)
    … rest untouched
```

- ✅ Monitoring untouched; export DSP entirely in KALA; the option is a clean
  on/off (`FRIDAY_KALA_EXPORT`), so upstream behaviour is one CMake flag away.
- ❌ Needs a new object-audio transport. The existing ZMQ channel carries
  metadata only, so it either gets a second topic or a sibling socket.

---

## 3. What KALA receives (Design B seam contract)

Per audio element, per block:

| Field | Source |
|---|---|
| mono object PCM, f32 48 kHz | panner input buffer, before `Panner3DProcessor` |
| azimuth, elevation (deg) | panner `X`/`Y`/`Z` params → polar, matching Studio's `M+030 = L` convention |
| spread, gain_db | `PannerVolume`, spatial-layout repo |
| audio_element_id, first_channel, channel_config | `audio_element_spatial_layout_repository_state` |
| target LKFS | export settings (see §5) |

KALA then does, in one call: VBAP render → 7.1.4 SMPTE bed → BS.1770
normalisation → LPCM IAMF encode. That is the same sequence Studio runs
(`studio/renderer.py` → `kala_py.vbap_gains`, then `kala_py.encode_iamf`), which
is why the null can be exact rather than approximate.

---

## 4. Surgery estimate — **STOP requested before step 4**

Design B touches:

1. `audioelementplugin/src/AudioElementPluginProcessor.{h,cpp}` — capture tap
2. `common/data_structures/src/AudioElementCommunication.h` — object channel
3. new `common/friday/ObjectCapture.{h,cpp}` — ring buffer + serialisation
4. new `common/processors/file_output/iamf_export_utils/KalaIamfWriter.{h,cpp}`
5. `common/processors/file_output/FileOutputProcessor.cpp` — path selection
6. `common/CMakeLists.txt` + root `CMakeLists.txt` — `FRIDAY_KALA_EXPORT`, link kala-cabi
7. `kala-engine/kala-cabi` — new render+encode entry points (Rust + header)

**Seven to nine files across two repos, plus a new real-time transport.** That
is past "a handful", and the transport is a genuine design decision (second ZMQ
topic vs sibling socket vs shared memory) with real-time-safety implications on
the audio thread. Per the brief, this is where I stop and ask.

**Steps 2, 3 and the null harness do not depend on this decision and are being
done now** — they isolate the seam by proving KALA alone can reproduce Studio's
output before any Bridge code is touched.

---

## 5. A finding that changes the null's definition

Studio's export is **loudness-normalised**. `Session.target_lkfs` defaults to
**−16.0 LKFS** (`studio/session.py:136`); the reference export measured −21.0
LKFS raw and applied **+5.0 dB** before encoding:

```
master : B2_ref_714.wav  (-21.0 LKFS, gain +5.0 dB)
meters : -16.00 LKFS · TP -12.98 dBTP
```

So "Bridge IAMF == Studio IAMF" is only meaningful if the Bridge path applies
the *same* mastering gain. The seam must therefore take either a target LKFS or
an explicit gain, and the null harness must state which. A raw KALA render
compared against this reference would sit exactly 5.0 dB off and look like a
failure when the spatial maths is in fact identical.

---

## 6. Status

| Step | State |
|---|---|
| 1. Map + design | ✅ done — this document |
| 2. Studio headless reference | ✅ done — see §7 |
| 3. kala-cabi .so + null vs reference | in progress |
| 4. Bridge swap | ⏸ **blocked on owner approval of Design B** |
| 5. Null harness | partial — kala-cabi vs Studio first |
| 6. B1 regression with export ON | ⏸ blocked on step 4 |

## 7. Step 2 result — Studio stands headless on this bench

- venv `/data/projects/friday/.venv-b2`: numpy 2.5.2, soundfile 0.14.0,
  scipy 1.18.0, h5py 3.16.0, PySide6 6.11.1, pytest 9.1.1.
- Linux `kala_py.so` (the abi3 build from `kala-engine/target/release/`) copied
  to `friday-dsp1-converter/app/native/`, `FRIDAY_DSP1_DIR` pointed at the
  converter checkout — the search order in `studio/compat.py` then resolves.
- **`pytest`: 121 passed.** Matching the expected count needs `PySide6` *and*
  `QT_QPA_PLATFORM=offscreen`; without PySide6 three GUI modules fail to import
  and pytest reports 95 passed / 3 skipped, i.e. 26 tests silently never run.
  `scipy` and `h5py` are also required — the converter's `app/loudness/meter.py`
  imports scipy lazily, so their absence shows up as 17 unrelated-looking
  failures.
- Reference authored and exported:

```
studio_cli.py new ref.fstudio --name "B2 ref" --label "B2 reference"
studio_cli.py add-object ref.fstudio obj997 stem997.wav --az 30 --el 0
studio_cli.py export ref.fstudio --out-dir ref-out --no-mp4 --no-adm --no-binaural
```

`ref-out/B2_ref.iamf` — ipcm, 48 kHz, 24-bit, frame_len 960, one channel
element, `loudspeaker_layout` 7 (7.1.4), one mix presentation. Its master
`B2_ref_714.wav` puts **all** energy in ch1 (L) at peak −12.99 / rms
−16.00 dBFS and silence elsewhere — correct for az +30°, which is exactly the
L speaker (`M+030 = L`), so VBAP gives that one speaker unity gain.
