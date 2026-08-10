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
| 3. kala-cabi .so + null vs reference | ✅ **done — byte-identical, see §8** |
| 4. Bridge swap | ⚠️ **implemented and building; does NOT finalise on this bench — see §11** |
| 5. Null harness | ✅ built and validated on the kala-cabi ↔ Studio pair (§9); re-runs unchanged against `bridge.iamf` once step 4 lands |
| 6. B1 regression with export ON | ✅ **PASS — monitoring unchanged, see §12** |

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

---

## 8. Step 3 result — the seam is proven, and it is exact

`kala-cabi` now carries a session API over the C ABI — the shape the Bridge
export path will call:

```
kala_session_new(48000, "7.1.4")
kala_session_add_object(mono, frames, az_deg, el_deg, spread, gain_db)   xN
kala_session_render()
kala_session_normalize(target_lkfs, &applied_gain_db)
kala_session_encode(bit_depth, frame_len, language, label, &buf, &len)
kala_session_free()
```

Built as `libkala_cabi.so` (the crate also emits a staticlib for the MSVC
path). Header `kala-cabi/include/kala_cabi.h` is kept in lockstep by the
crate's `header_matches_exports` test, now covering the seven new symbols.

A C driver (`docs/evidence/b2/kala_cabi_test.c`) exercises the ABI through
nothing but the public header. It first runs negative tests — non-48 kHz
rejected, non-7.1.4 layout rejected, null PCM rejected, encode-before-render
refused, add-object-after-render refused, `free(NULL)` a no-op — then renders
the same input Studio was given.

**Result: byte-identical output.**

```
17f60f17600c410f010716d2941418d08bf25a8efa9fda9090db4ba7d443d029  kala.iamf     (kala-cabi, via C)
17f60f17600c410f010716d2941418d08bf25a8efa9fda9090db4ba7d443d029  B2_ref.iamf   (studio_cli export)
```

Both 3,458,212 bytes. Intermediate agreement, which is what makes the result
trustworthy rather than a coincidence:

| Quantity | kala-cabi (C) | Studio |
|---|---|---|
| Raw render, active channel | ch1 / L only | ch1 / L only |
| Raw render peak / rms | −18.00 / −21.01 dBFS | −18.00 / −21.01 dBFS |
| Normalisation gain | **+5.010 dB** | **+5.0 dB** (reported) |
| Encoded size | 3,458,212 B | 3,458,212 B |

az +30° is exactly the L speaker, so VBAP gives that one speaker unity and
everything else zero — the render being confined to ch1 is the correct answer,
not a degenerate one.

This isolates the seam **before any Bridge code is touched**: whatever happens
in step 4, the KALA side is not the variable.

## 9. Step 5 — the null harness

`docs/evidence/b2/null_harness.py`. FFmpeg — an independent IAMF
implementation, so neither side is graded by its own encoder — decodes each
file's seven substreams (`-map 0:a:i`) and they are reassembled into SMPTE
order via `SUBSTREAM_SMPTE_714 = ((0,1),(4,5),(6,7),(8,9),(10,11),(2,),(3,))`.
Per-substream placement matters: a whole-file energy check would hide a
channel-mapping mistake, which is precisely the class of bug this gate exists
to catch.

Note FFmpeg's default stream selection on an `.iamf` yields the **stereo**
mix presentation, not 7.1.4 — comparing that would silently test the wrong
thing.

Run on the step-3 pair:

```
   ch name   A rms dBFS  B rms dBFS  max|delta| dBFS
    1 L          -16.00      -16.00             -inf
    2 R            -inf        -inf             -inf
    …          (ch3–12 all silent on both sides, delta -inf)

  worst channel: -inf dBFS      sample-exact: True
  GATE GA (-90 dBFS): PASS
```

The harness takes `--gate-db` (default −90) and exits non-zero on failure, so
it drops straight into CI. Against `bridge.iamf` it runs unchanged.

## 10. What is needed from the owner

Approval of **Design B** (§2) before step 4. The specific decision is the
object-audio transport, since the existing ZeroMQ channel carries metadata
only:

| Option | Trade-off |
|---|---|
| Second ZMQ topic on the existing socket | Reuses proven plumbing; adds serialisation to the audio thread — needs a lock-free hand-off to a sender thread to stay RT-safe |
| Sibling ZMQ socket, audio only | Cleaner separation, same RT concern, one more port to manage |
| Shared-memory ring per audio element | Best RT behaviour, no copies; most new code, and lifetime/cleanup across plugin instances is fiddly |

Recommendation: **lock-free SPSC ring on the audio thread feeding a second ZMQ
topic from a worker thread** — it keeps `processBlock` allocation-free and
lock-free (KALA's own RT rule, `CLAUDE.md` §3) while reusing the transport that
already works between these two plugins.

---

## 11. Step 4 — implemented, not yet demonstrated end to end

Design B is built and compiles into both VST3 plugins with
`FRIDAY_KALA_EXPORT=ON` (default in this fork; `OFF` restores upstream exactly).

| Piece | File |
|---|---|
| Lock-free SPSC ring + ZeroMQ object bus (port 5556; 5555 stays Eclipsa's metadata bus) | `common/processors/friday/FridayObjectTransport.{h,cpp}` |
| Capture tap, inserted before `Panner3DProcessor` | `common/processors/friday/FridayObjectCaptureProcessor.h` |
| KALA writer (objects → `kala_session_*` → `.iamf`) | `common/processors/file_output/iamf_export_utils/KalaIamfWriter.{h,cpp}` |
| Path selection | `common/processors/file_output/FileOutputProcessor.{h,cpp}` |
| Option + kala-cabi linkage | root `CMakeLists.txt`, `common/CMakeLists.txt` |

`processBlock` on the audio thread only memcpys into a preallocated ring and
bumps an atomic; a worker thread does the serialisation and the ZeroMQ send, so
KALA's RT rule (`CLAUDE.md` §3: no allocation, no locks, no syscalls) holds.

**What works:** the path arms through Eclipsa's real export lifecycle. The
plugin's own log, from a headless bounce driven by ReaScript:

```
FileOutputProcessor.cpp initializeFileExport 143  Beginning .iamf file export
KalaIamfWriter.cpp open 43                        KALA export path armed: …/bridge.iamf
```

**What does not:** the export never finalises here, so **no `bridge.iamf` is
produced and gate GA is NOT claimed.**

Root cause is upstream's finalisation trigger, not the KALA code.
`FileOutputProcessor::setNonRealtime` arms on `true` and finalises on `false`,
and a host only issues `false` when it next returns to realtime. This bench has
no working audio device — JACK is not running and REAPER falls back to nothing —
so the transport never rolls, `setNonRealtime(false)` is never delivered, and
`closeFileExport` is never called. Driving `OnPlayButton`/`OnStopButton` from
ReaScript does not help for the same reason.

A `releaseResources()` safety net was added (guarded by `FRIDAY_KALA_EXPORT`, so
upstream semantics are untouched) to finalise any export still open when JUCE
tears the processor down. It does not fire before the harness's timeout either,
and the session now hangs at shutdown rather than reaching it.

### To finish this, in order

1. **Give REAPER a working audio device** (ALSA on the X-Fi, or a dummy device).
   That alone should let `setNonRealtime(false)` arrive and the existing code
   complete — it is the smallest change and touches no plugin code.
2. If that is not wanted on the bench, add an explicit finalise trigger that
   does not depend on the host transport — e.g. a `FileExport` repository flag
   the harness can set, which is also the honest fix for any offline/CI export.
3. Then run the null harness (§9) against `bridge.iamf` unchanged.

### Also outstanding

- The shutdown hang above needs a real diagnosis. The publisher's post-shutdown
  drain was made bounded (an unbounded "drain until empty" can spin forever
  while the audio thread is still pushing, so the join in `~ObjectPublisher`
  never returns), but that did not clear it. Note the pre-existing renderer
  crash at exit — `boost::log::core::~core()` in an atexit handler, confirmed
  by gdb and recorded in `docs/B1-LINUX-EVIDENCE.md` §4 — is a separate,
  older defect present before any B2 work.
- The object wire format carries one position per block and the receiver keeps
  the last one. Fine for the static pan the gate uses; automation needs
  per-block positions preserved, matching Studio's block-ramped render.

## 12. Step 6 — B1 regression PASSES

The B1 audio gate re-run against the `FRIDAY_KALA_EXPORT=ON` build, unchanged
harness. Eclipsa's monitoring render is **numerically identical** to the B1
result, on every active channel:

| ch | speaker | peak dBFS | rms dBFS | B1 baseline |
|---|---|---|---|---|
| 3 | C | −30.74 | −33.77 | identical |
| 5 | Ls | −27.75 | −30.77 | identical |
| 6 | Rs | −27.75 | −30.77 | identical |
| 7 | Lrs | −26.24 | −29.26 | identical |
| 8 | Rrs | −26.24 | −29.26 | identical |
| 1, 2, 4, 9–12 | | silent | silent | identical |

That is the property Design B was chosen for: the capture tap only reads the
buffer, so obr/libspatialaudio and the monitoring UI behave exactly as before.
