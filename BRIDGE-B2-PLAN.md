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

## 11. Step 4 — implemented and demonstrated end to end

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

### The four defects between "arms" and "nulls"

Getting from an armed path to a file that nulls took four fixes, three of them
in this fork's own code. Recorded because each is a trap for anyone building an
offline capture in a plugin host.

**1. No audio device → the export never finalises (bench, no code change).**
`FileOutputProcessor::setNonRealtime` arms on `true` and finalises on `false`,
and a host only issues `false` when it returns to realtime. With no device the
transport never rolls, so `closeFileExport` never runs. REAPER's own Dummy
Audio driver fixes it with no hardware and no JACK — the selector is
`linux_audio_mode=2` in `reaper.ini`, found by probing 0–4 and reading back
`GetAudioDeviceInfo("MODE")`:

| `linux_audio_mode` | device | `Audio_IsRunning()` |
|---|---|---|
| 0, 1, 4 | — | 0 |
| **2** | **Dummy Audio** | **1** |
| 3 | PulseAudio | 1 |

With the device up, `OnPlayButton` moves the transport, `setNonRealtime(false)`
arrives, and the existing code completes. The guarded `releaseResources()`
safety net stays as a belt-and-braces path for hosts that tear down without
returning to realtime.

**2. The receiver bound too late, losing 77% of the capture.** `ObjectReceiver`
used to bind when an export armed. ZeroMQ's PUB/SUB handshake costs ~100 ms and
an offline bounce of a 2 s project is *finished* in ~200 ms, so the head of the
stream went to a socket with no subscriber and PUB dropped it — silently, with
no sequence gap, because the first block that did arrive set the baseline. The
result was a 0.46 s file from a 2 s render. Fixed by making the receiver a
process-wide singleton (`friday::sharedObjectReceiver()`) that binds in
`prepareToPlay`, i.e. when the renderer plugin loads; `open()` now only
`reset()`s the accumulator. The fixed 250 ms drain was replaced by a
settle-based one (quiet for 150 ms, hard cap 5 s), since how long the tail takes
depends on how fast the host bounced.

**3. Capture was not bounded by the bounce.** The tap published on every block,
so once the host returned to realtime and the exporter was still draining, the
object kept growing — a 2 s render encoded as 2.12 s. The tap now publishes only
while the host is non-realtime. `AudioElementPluginProcessor` holds its
sub-processors in a plain `std::vector`, **not** an `AudioProcessorGraph`, so
nothing forwarded the transition: it needed an explicit
`AudioElementPluginProcessor::setNonRealtime` override that calls the base and
then the tap. (Calling the base matters — skipping it leaves JUCE's own
`isNorealtime()` flag stale.)

**4. The host's silent flush past the render bounds.** REAPER hands the plugins
~0.28 s of digital silence after a 2 s bounce and discards it from its own
output. Left in, it lengthened the deliverable *and* moved the BS.1770
integrated measurement — the 400 ms gating blocks straddling the boundary drop
under the relative gate — which showed up as a **+0.175 dB** mastering-gain
error against Studio (peak 0.228694 vs 0.224137). `trimTrailingSilence()` cuts
it, equally across all objects so relative timing is preserved.

*Limitation from fix 4:* material that deliberately ends in silence is shortened
by that silence. The correct fix is to carry the host playhead position in the
wire header and cut at the render bounds; deferred, and the only remaining
known gap in the capture contract.

### Verified end to end

```
KalaIamfWriter.cpp open 46    KALA export path armed: /data/build/b2/bridge.iamf
KalaIamfWriter.cpp close 112  KALA export captured 107 block(s), 109568 frames,
                              trimmed to 96000; object[0] az=30.173517 el=0.000000
                              spread=0.000000 gain=0.000000
KalaIamfWriter.cpp close 188  KALA export wrote 3458222 bytes from 1 object(s),
                              gain 5.010230 dB
```

### Also outstanding

- The renderer still crashes at process exit — `boost::log::core::~core()` in an
  atexit handler, confirmed by gdb and recorded in `docs/B1-LINUX-EVIDENCE.md`
  §4. Pre-existing, present before any B2 work, and it happens *after* the file
  is written and closed. The earlier shutdown *hang* is gone: it was the harness
  saving over an existing `b2gate.rpp` and REAPER waiting on an invisible
  overwrite modal. The harness now deletes every artefact it produces up front,
  which also fixes REAPER silently skipping a render whose output already
  exists.
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

## 13. Step 5 result — the null, and what "the same pan" had to mean

`bridge.iamf` (Bridge, KALA export path, headless REAPER bounce) against a
FRIDAY Studio master of the same object, decoded by FFmpeg — an implementation
neither side owns — and reassembled into SMPTE 7.1.4:

```
  96000 frames, 12 ch @ 48 kHz

   ch name   A rms dBFS  B rms dBFS  max|delta| dBFS
    1 L          -16.00      -16.00             -inf
    5 Ls         -65.11      -65.11          -138.47
    2,3,4,6-12    -inf        -inf             -inf

  worst channel: Ls (ch5) at -138.47 dBFS
  GATE GA (-90 dBFS): PASS
```

**−138.47 dBFS worst case, against a −90 dBFS gate.** Eleven of twelve channels
are bit-identical (`-inf` delta); Ls differs by exactly one 24-bit LSB
(−138.47 dBFS *is* the 24-bit LSB), from the object azimuth crossing the seam as
`float32` while Studio reads a JSON `double`.

Artefacts:

| file | sha256 |
|---|---|
| `bridge.iamf` | `04f2b28ccaaef80eea18cbc5c68c2b5154c9638d1c4615334ea706ec835177ed` |
| Studio master, matched azimuth | `a9e7d69c0800ce212fc9aecc77cfb27eb4d1bbdb0969f732a621e296f0471d02` |

### The reference had to be regenerated, and why that is not a fudge

The first null run FAILED at −62.10 dBFS on Ls. That was a real signal, not
noise: the reference had been authored at **azimuth 30.000°**, but the object
Bridge actually captured sits at **30.173517°**. The panner's X/Y/Z parameters
are integer-quantised over [−50, +50], so the DAW cannot express the position
that lands on exactly 30° — asking for it gives x = −25, y ≈ 43, and
`atan2` turns that into 30.173517°. The 0.17° offset spills VBAP energy into Ls
at −49 dB relative to L, and the reference had no Ls energy at all to null it
against.

The gate is "Bridge IAMF == Studio IAMF **for the same pan**". Comparing a
30.17° render against a 30.00° reference is not the same pan, so the reference
was re-exported from `studio_cli` at azimuth 30.173517 — the value read out of
Bridge's own capture log, not a value chosen to make the number look good. Both
sides then agree on Ls to one LSB, which is the point: the 0.17° offset now
appears *identically* in both files.

The original 30.000° comparison is kept as
`/data/build/b2/null-bridge-vs-studio-az30.txt` so the difference between "wrong
renderer" and "different pan" stays on the record.

Studio confirms the mastering gain independently: `+5.0 dB` applied to reach
−16.00 LKFS, against Bridge's `5.010230 dB`. The §5 loudness-normalisation
finding therefore holds all the way through the seam.

## 14. B2 gate table

Run 2026-08-12 on the Linux bench, `FRIDAY_KALA_EXPORT=ON`, REAPER 7.78 headless
with the Dummy Audio device.

| # | Gate | Criterion | Result | Evidence |
|---|---|---|---|---|
| B1-1 | Linux build | both VST3s build from source | **PASS** | `docs/B1-LINUX-EVIDENCE.md` §1 |
| B1-2 | Install + scan | REAPER scans both plugins clean | **PASS** | §3 |
| B1-3 | Audio gate | panner → ZMQ → renderer, non-silent plausible render | **PASS** | §4 |
| B2-1 | Export-stage map | seam identified, surgery scoped | **PASS** | §1, §4 |
| B2-2 | Studio headless | reference `.iamf` produced on this bench | **PASS** | §7 |
| B2-3 | Seam proven in isolation | kala-cabi output == `studio_cli` output | **PASS — byte-identical** | §8, `null-kala-vs-studio.txt` |
| B2-4 | Bridge swap | Bridge produces `bridge.iamf` through KALA | **PASS** | §11, `b2-gate-result.txt` |
| B2-5 | **Null (gate GA)** | ≤ −90 dBFS per-channel delta vs Studio | **PASS — −138.47 dBFS** | §13, `null-bridge-vs-studio.txt` |
| B2-6 | B1 regression, export ON | monitoring render unchanged | **PASS — identical** | §12, `b1-regression-measurements.txt` |

Supporting suites: kala-engine **27 suites / 252 tests, 0 failed**;
friday-studio **121 passed**.

Deviations from PRD-v2 §6 Phase A, carried in the commit message:

- The Studio reference for B2-5 is re-exported at azimuth **30.173517°**, the
  position the DAW can actually express, not 30.000° — §13.
- `trimTrailingSilence()` bounds the capture by trailing digital silence rather
  than by the host's render bounds — §11, fix 4.
- Bridge's own unit suite needed a case-sensitivity fix
  (`IamfBufferedReader_test.cpp` → `IAMFBufferedReader_test.cpp`) before it
  would configure on Linux at all; that is portability defect #5 of the same
  class as the four in `docs/B1-LINUX-EVIDENCE.md`.
