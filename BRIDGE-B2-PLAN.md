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

### Superseded 2026-08-12 — the pan is exact now

The parameters are continuous (see §B6-1), so the DAW can express azimuth
30.000 and the null no longer needs a reference regenerated at a captured
angle. Against the **original, untouched** 30.000° Studio reference:

| pan | reference | worst channel | sample-exact |
|---|---|---|---|
| az +30.173517 (integer params, old) | regenerated at 30.173517 | −138.47 dBFS | no, 1 LSB on Ls |
| **az +30.000000 (continuous params)** | **original `B2_ref.iamf`, unchanged** | **−inf dBFS** | **yes** |

`bridge_p1.iamf` sha256 `21ff9e1b…` against reference `17f60f17…`; the decoded
12-channel content is identical sample for sample on every channel. The account
below is kept because it is why the reference was regenerated at the time, and
because the diagnosis — that the DAW could not express the angle — is what led
to fixing the parameters.

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
friday-studio **121 passed**; Bridge's own suite **267 tests, 263 passed**
(see below).

### Bridge's unit suite — first run on Linux

It had never been built on this bench. Enabling it surfaced three separate
problems, only one of them from this work:

1. **Configure hard-failed.** `common/processors/tests/CMakeLists.txt` named
   `IamfBufferedReader_test.cpp`; the file is `IAMFBufferedReader_test.cpp`.
   Resolves on macOS, fatal on Linux. Portability defect #5.
2. **Two tests segfault out-of-tree.** `test_loudness_proc.verify_metadata` and
   `test_ebu128_measurements.loudness_test` locate their WAV fixtures relative
   to `current_path()` and assume the build directory is `<source>/build`. Run
   from `/data/build/bridge-build` they read a nonexistent file and crash
   rather than fail. Not a code defect — a run-directory requirement, now in
   `docs/BUILDING-LINUX.md` §7.
3. **53 tests failed from this work.** `FileOutputTests` (47) and
   `IAMFFileReaderTest` (6) drive `FileOutputProcessor` with bed buffers and no
   object bus, and cover FLAC, Opus and arbitrary element layouts. The KALA
   path encodes captured objects and implements LPCM 7.1.4 only, so it is
   deliberately **not** a drop-in and produced no file. Fixed by making the
   writer choice runtime-switchable (`setKalaExportEnabled`) so upstream's suite
   keeps testing upstream's writer. `FridayObjectCapture_test.cpp` adds 15 tests
   over the parts of the capture path the null gate cannot localise a failure
   in: the trim, the azimuth convention that has to match Studio, and the SPSC
   ring.

**Four failures remain, all in code this work does not touch:**

| test | why |
|---|---|
| `FileOutputTests.validate_file_checksum`, `.pp_validate_file_checksum` | compare a fresh `.iamf` sha256 against a reference recorded on upstream's macOS toolchain. This bench builds iamf-tools from source against different abseil/protobuf with LTO, so the bytes differ. The KALA path is disabled in these tests, so the encoder they exercise is upstream's. Also order-dependent: `pp_validate_file_checksum` passes when the two are run alone. |
| `LoggerTest.LogFromMultipleThreads`, `.LoggerInitMultipleCalls` | the logger finds no messages in the files it scans. Fails in isolation too, so it is not contaminated by other suites; nothing in this work touches `common/logger`. |

Neither has been fixed here — they are upstream/bench issues that predate the
B2 work and are recorded rather than quietly absorbed.

Both gates were re-run against the committed tree after the unit-suite work
changed `FileOutputProcessor`. `bridge.iamf` came out byte-identical
(`04f2b28c…`) from a capture of 105 blocks where the first run captured 107 —
the trim is what makes the result independent of the host's flush jitter. The
B1 regression re-ran to the same per-channel numbers; its WAV **hash** is not
reproducible, because REAPER stamps a BWF `bext` chunk with the origination
date and time.

Deviations from PRD-v2 §6 Phase A, carried in the commit message:

- The Studio reference for B2-5 is re-exported at azimuth **30.173517°**, the
  position the DAW can actually express, not 30.000° — §13.
- `trimTrailingSilence()` bounds the capture by trailing digital silence rather
  than by the host's render bounds — §11, fix 4.
- Bridge's own unit suite needed a case-sensitivity fix
  (`IamfBufferedReader_test.cpp` → `IAMFBufferedReader_test.cpp`) before it
  would configure on Linux at all; that is portability defect #5 of the same
  class as the four in `docs/B1-LINUX-EVIDENCE.md`.

---

## 15. B4 / V2-02 — Bridge ⇄ Studio handoff and live scope IPC

**Gate:** pan in REAPER → Studio PannerScope in under 100 ms, and a `.fstudio`
written by the Bridge opens in Studio with the same objects and positions.

**Result: PASS on both halves — 36.25 ms worst case, and a session that not
only opens clean but round-trips.**

The wire contract is Studio's, documented in
`friday-studio/studio/bridge_link.py` (NDJSON over TCP to `127.0.0.1:47800`,
one-way Bridge → Studio, protocol v1). This fork implements the sending half.
Per V2-01 the link is transport and UI only: it carries no DSP and computes no
positions, it reads the same captured-position source the KALA export uses.

| Piece | File |
|---|---|
| Sender: connect, backoff, hello/scene/pan/handoff/bye | `common/processors/friday/FridayStudioLink.{h,cpp}` |
| `.fstudio` + mono object stems | `common/processors/friday/FridayStudioSession.{h,cpp}` |
| Wire v2 (flags, name), live position map | `common/processors/friday/FridayObjectTransport.{h,cpp}` |
| Realtime pings vs offline capture | `common/processors/friday/FridayObjectCaptureProcessor.h` |
| Link start + handoff on export | `common/processors/file_output/FileOutputProcessor.cpp` |

### One stream, two consumers

The B2 tap only published while the host was non-realtime, which is exactly
wrong for a live link: nothing would reach Studio until the user bounced.
Making it publish always, without reopening the B2 null, needed the two uses
kept separable. The wire header goes to v2 with a `flags` field:

- **`kFlagOffline`** blocks carry PCM and are the only ones the exporter
  accumulates. The deliverable is unchanged.
- **realtime** blocks are position-only pings (`frames == 0`, ~72 bytes)
  throttled to ~60 Hz at the tap, feeding a live map that survives `reset()`
  so the scope does not blink when an export arms.

Two traps that fell out of that, both of which would have been silent:

- `blocksReceived()` counts **offline** blocks only. `drain()` waits for the
  stream to go quiet, and realtime pings never stop, so counting them would
  have made every export sit out the full 5 s cap.
- the export accumulator ignores realtime blocks entirely. Keyed by uuid, one
  ping arriving during the post-bounce drain would have overwritten the
  exported object's azimuth with wherever the user's mouse had drifted to.

### Latency — and the 211 ms that had to be fixed

The first measured run **FAILED at 211 ms worst case**, twice the gate. The
measurement was not at fault: the harness calibrates REAPER's monotonic clock
against the wall clock once, bracketed, and reported ±1.8 ms.

The cause was upstream's parameter access. `AudioElementParameterTree`'s
getters read positions through `getParameterAsValue()` — the APVTS **ValueTree**
— which the host→tree sync only flushes from a message-thread timer. The tap
now reads the parameters' own atomics with `getRawParameterValue`, JUCE's
documented audio-thread path, which also takes a ValueTree read off the audio
thread. The captured *values* are identical either way (X/Y/Z are
`AudioParameterInt`, so the atomic already holds the same integer), which is
why B2 is unaffected — confirmed byte-identical below.

| | worst | median | min |
|---|---|---|---|
| through the APVTS ValueTree | 210.65 ms | 95.55 ms | 51.05 ms |
| through the parameter atomics | **36.25 ms** | 23.81 ms | 7.28 ms |

Remaining budget at 36 ms: one dummy-device block (1024 @ 48 kHz = 21 ms), the
tap's ~60 Hz ping, ZeroMQ, and the sender's 30 Hz tick (~33 ms). Local TCP and
the Python callback are under a millisecond.

Measuring it honestly needed a harness fix too: timestamping each pan with
`io.popen("date")` put the fork of a multi-hundred-MB process inside every
measurement, and inflated the first run by ~20 ms on top of the real latency.

### The handoff, and what an ObjectTrack actually is

The first implementation pointed each object at the per-audio-element WAV the
export already writes. That was wrong twice over:

1. Studio's `ObjectTrack.file_path` is a **mono object stem** which Studio
   renders itself (`studio/renderer.py` `_load_mono`). The per-element WAV is
   the already-rendered 12-channel bed — handing it over would have Studio
   re-render a finished mix as a point source.
2. Upstream **deletes** those WAVs at the end of the same `closeFileExport`
   unless `exportAudioElements` is set, so the path dangled. That is how it
   surfaced: `Session.validate()` reporting a missing file.

The Bridge now writes each captured object's own mono float32 stem beside the
session, so nothing is requantised and the session validates clean.

```
  loaded: name='bridge_b4' sample_rate=48000 target_lkfs=-16.0 language='en'
  object 'source'  file_path: /data/build/b4/bridge_b4_source.wav
    t=0.000000  az=+15.1541  el=+0.0000  spread=0.000  gain=+0.00  linear
  validate(): clean (0 problems)
  captured pan vs the .fstudio's last keyframe:
    'source': az +15.1541 vs +15.1541 (d=0.0000)
```

**The handoff round-trips.** `studio_cli export` renders the handed-off
`.fstudio`, and that render nulls against the Bridge's own `.iamf` at
**−126.43 dBFS** — same objects, same positions, same audio, through a
different product. That is a much stronger claim than "the file opens".

Keyframes come from a captured timeline (a point whenever the position moves),
not one frozen value. The gate's own bounce is a static pan, so the sample
session carries one keyframe — correct, not a truncation.

### B4 regression table

| Check | Before B4 | After B4 |
|---|---|---|
| B2-5 null vs Studio | −138.47 dBFS | **−138.47 dBFS, file byte-identical `04f2b28c…`** |
| captured azimuth | 30.173517° | **30.173517°** |
| Bridge unit suite | 267 / 263 passed | **281 / 277 passed** (+12 link, +2) |
| — its 4 known failures | checksum ×2, Logger ×2 | **unchanged, same 4** |
| kala-engine | 27 suites / 252 tests | **27 suites / 252 tests, 0 failed** |
| friday-studio | 141 passed | **141 passed** (`8d6e54a`) |

Evidence in `docs/evidence/b4/`: the harness, the receiver, the two analysis
scripts, the raw message log, the latency table, the validation output, the
round-trip null and a sample `.fstudio`.

### Not done

- The wire carries one position per block and Studio's scope shows the latest;
  per-block automation *inside* a single Studio render still needs the
  block-ramped path noted in §11.
- Handoff fires on export only. A menu/parameter trigger for "hand off without
  exporting" is not wired.
- One client at a time, localhost only — that is the protocol's own limit.

---

## §B5. Midnight-scope panner UI — inventory and swap plan

**Gate:** the FRIDAY Panner (element plugin) pad reaches design parity with
Studio's `PannerScope`; a side-by-side screenshot pair is the evidence.

### What is there now

The element plugin's editor is `AudioElementPluginEditor` (397 lines) hosting
three screens side by side:

| Component | Role | Lines | B5 verdict |
|---|---|---|---|
| `AudioElementPluginEditor` | window chrome, title bar, track-name box, audio-element selector, screen layout | 397 + 116 h | **repaint** — LookAndFeel + colours |
| `screens/RoomViewScreen` | hosts the pad, elevation-mode toggle | 154 + 52 h | **repaint** — swaps which pad it hosts |
| `components/room_views/AudioElementPluginRearView` | **the pad itself** | part of `PerspectiveRoomViews.cpp` (469) | **replace** — new component |
| `components/room_views/PerspectiveRoomView` (base) | 3-D room: faces, gridlines, 4×4 transforms, speaker/track projection | 314 + 136 h | **untouched** — still used by the renderer plugin's four views |
| `screens/PositionSelectionScreen` | X/Y/Z dials, spread, LFE | 171 + 76 h | **repaint** — its own `PositionSelectionLookAndFeel` already exists |
| `screens/TrackMonitorScreen` | per-track meters | 155 + 59 h | **repaint** — meter colours to the theme's level law |
| `components/src/EclipsaColours.h` | the palette everything reads | 1 header | **extend** — add the midnight-scope tokens beside the existing ones |

### How X/Y/Z reach the pad today

`AutoParamMetaData::CreateStaticParameterLayout()` makes X, Y and Z as
**`juce::AudioParameterInt`** over [−50, +50], inside
`AudioElementParameterTree` (an `AudioProcessorValueTreeState`).

- The current pad is **display-only.** There is no `mouseDown`/`mouseDrag`
  anywhere in `room_views/` or `audioelementplugin/src/` — positions are typed
  or nudged in `PositionSelectionScreen`'s dials, and the room view only draws.
  **Dragging is new behaviour, not a re-skin of existing behaviour.**
- The UI reads positions through the tree's getters
  (`getParameterAsValue()` → ValueTree). B4 already established that path is a
  message-thread-timer behind and not audio-thread safe; the capture tap now
  uses `getRawParameterValue`. The pad should write through
  `getParameter(id)->setValueNotifyingHost()` so the host sees automation and
  undo exactly as it does from the dials.

### Verdict: this is more than a re-skin — **STOPPING for your call**

The brief's own threshold is "custom LookAndFeel + a custom pad component", and
this is both, plus one thing the threshold did not anticipate:

1. **A new custom component** (~400–500 lines) painting the radar: radial
   gradient + vignette, rings at 0.94/0.62/0.31, 30° spokes, FRONT/L/R/REAR
   compass, speakers as notch+dot / cyan dots, layered amber glow orb with a
   white-hot core, dashed floor-projection ring + stem, motion trail.
2. **A custom LookAndFeel** for sliders, labels and combo boxes, plus new
   palette tokens.
3. **New interaction that does not exist today**: drag-to-pan, with the dome
   projection (centre = zenith) mapped back into integer X/Y/Z. This is where
   the risk is — see below.

### The two decisions I need from you

**(a) Does the pad become the primary control, or stay a display?**
Adding drag makes the pad authoritative and the dials a readout. That is what
Studio's scope does and what "design parity" implies, but it changes how the
plugin is *operated*, not just how it looks. Upstream deliberately kept
position entry numeric.

**(b) Elevation mapping — the pad and the parameters do not agree.**
Studio's scope maps elevation to *radius* (centre = zenith, dome projection).
Eclipsa's Z is an independent [−50, +50] parameter, and
`RoomViewScreen` has an elevation-mode toggle with five modes (flat, tent,
arch, dome, curve) that already reinterpret height. Parity means picking one:

- **dome-only** — radius *is* elevation, matching Studio exactly; the existing
  elevation-mode toggle becomes meaningless and should go.
- **keep the modes** — the pad's radius follows whichever mode is selected,
  which is richer but is *not* pixel-parity with Studio's scope.

I recommend **dome-only for the pad's drag math, with the mode toggle
retained for the other modes' rendering**, so the screenshot pair is a true
parity comparison and no existing feature is silently removed. Say the word if
you would rather I drop the toggle entirely.

### What I will not do without being asked

Touch `PerspectiveRoomView` itself — the renderer plugin's Top/Side/Rear/Iso
views inherit from it, and a change there is a change to a screen this
milestone is not about.

### Cost estimate

| Step | Files | Rough size |
|---|---|---|
| 2 — pad component + drag | 1 new `.h/.cpp` pair, `RoomViewScreen` swap | ~500 lines new |
| 3 — LookAndFeel + palette | `EclipsaColours.h`, 1 new LookAndFeel, 3 screens repainted | ~250 lines |
| 4 — screenshots | offscreen render harness + Studio render | evidence only |
| 5 — regression | no code | — |

**Nothing in steps 2–5 has been written yet.** Waiting on (a) and (b).

### §B5 result — steps 2–4 done, gate PASSED

Decisions taken (owner, 2026-08-12): the pad becomes the **primary control**
with the numeric dials kept fully editable and two-way; **dome-only** drag math;
the flat/tent/arch/dome/curve toggle **kept**.

| Piece | File | Size |
|---|---|---|
| The radar | `common/components/src/friday/FridayPannerScope.{h,cpp}` | ~430 |
| Palette tokens | `common/components/src/EclipsaColours.h` | +45 |
| Chrome repaint | `AudioElementPluginEditor.cpp` (`CustomLookAndFeel`) | +60 |
| Host swap + readout | `screens/RoomViewScreen.{h,cpp}` | ~40 changed |
| Tests | `common/components/tests/FridayPannerScope_test.cpp` | ~280 |

**Parity evidence:** `docs/evidence/b5/parity_az+30_el0.png` and
`parity_az0_el60.png` — Bridge (JUCE) beside Studio (Qt), same poses, same
speaker layout, dome constraint on both. Same geometry (rings 0.94/0.62/0.31,
30° spokes, rim = horizon, centre = zenith), same palette, same conventions
(+left azimuth, so +30° is upper-LEFT on both), same cues (notch+dot floor
speakers, cyan height dots, layered amber orb, dashed floor projection + stem).

### Two findings from building it

**1. Eclipsa's room-view table and KALA disagree about the speakers.**
`SpeakerLookup`'s geometry is a *box room for drawing*: its height layer works
out at **26.6°**, not 45°, and it places the rear surrounds at **±135°** where
KALA's `smpte_714_layout()` and Studio use **±150°**. The scope draws the
layout KALA actually renders to, since that is what the exported file contains
and what the parity gate compares against. The room views in the renderer
plugin still use the old table — they are a different screen and out of scope
here, but the disagreement is now on the record.

**2. Painting from cached state produced a silently wrong screenshot.**
The pad first refreshed its position only on its 30 Hz timer. In an offscreen
render there is no message loop, so the first parity PNGs drew the object at
its construction position while claiming to show az +30. Nothing errored. The
pad now reads the parameters in `paint()` itself; the timer's only job is to
notice a change and ask for a repaint. Anything that repaints without the timer
having run — a resize, an occlusion, an offscreen render — is now correct.

### Regression

| Check | Before B5 | After B5 |
|---|---|---|
| B2-5 null vs Studio | −138.47 dBFS | **−138.47 dBFS, byte-identical `04f2b28c…`** |
| captured azimuth | 30.173517° | **30.173517°** |
| Bridge unit suite | 286 / 282 | **297 / 292** (+11: 6 geometry, 5 pad-as-control) |
| — its 4 known failures | checksum ×2, Logger ×2 | **unchanged, same 4** |
| REAPER scan + load | clean | **clean** — the B2 gate run instantiates both plugins |
| kala-engine | 27 suites / 252 | **untouched by B5** |

### B5 follow-up (noted, not done)

Align the five Eclipsa elevation modes with Studio's constraint surfaces
(Manual / Wedge / Dome / Ceiling) so a drag under any mode produces the motion
that mode describes. Today the drag is dome for all of them, and the modes
survive as (a) the repository state driving the elevation listener and (b) the
contour rings the scope draws. `FridayPannerScope::radiusFraction` /
`elevationFromRadius` are the two functions a per-mode surface would replace.


---

## §B6. Bridge polish — the three recorded follow-ups

### §B6-1. Position parameters are continuous (was: B2 follow-up)

X, Y and Z were `juce::AudioParameterInt` over [−50, +50] — 101 steps — so most
directions were simply not on the grid. Asking for azimuth 30.000 gave x=−25,
y=43 and therefore **30.173517**, which is why the B2 null had to be taken
against a reference regenerated at that captured angle.

They are now **`AudioParameterFloat` over the same [−50, +50]**.

**Why float and not a finer integer scale.** A rescaled integer (hundredths
over [−5000, +5000]) would have changed what a stored number *means*: every
session holding x=43 would load as 0.43 and every object in every existing
project would silently move. No version hint undoes that after the fact. Same
range, finer type, so a saved 43 loads as 43.0 and nothing moves. Host
automation is unaffected in both directions — automation is recorded normalised
0..1 over an unchanged range, and the parameter IDs and version hint are
untouched, so existing VST3 lanes stay bound and replay to the same positions.

The getters had to widen too. `AudioElementParameterTree::get{X,Y,Z}Position()`
returned **`int`**, and `Panner3DProcessor` stored them as `int`. Left alone,
Eclipsa's own monitoring would have kept quantising while the KALA export — which
reads the parameters' atomics directly — did not, and the two would have drifted
apart. `ElevationListener`'s `std::round` on the dome write went for the same
reason.

**Gate: PASS, and better than asked.** A scripted REAPER pan at az +30.000
captures `az=30.000000` (asked: ±0.01°), and the end-to-end null against the
**original** Studio reference is **−inf dBFS on all 12 channels, sample-exact** —
no reference regeneration. Evidence in `docs/evidence/b6/`.

### §B6-2. The pad honours the selected elevation mode (was: B5 follow-up)

The pad's drag was dome-only. It now respects the mode — but **not** by
implementing per-mode `radiusFraction`/`elevationFromRadius` pairs as the
follow-up originally imagined, because Eclipsa already owns those surfaces:
`ElevationListener::get{Tent,Arch,Dome,Curve}ElevationPt` compute height from
the horizontal position, they are wired to the X and Y parameters, and they are
what the monitoring render and the exported file obey. A second copy in the UI
would have been free to drift from the one that actually decides where the
object is.

So the split is:

| mode | what a drag writes | who owns height |
|---|---|---|
| flat / none | azimuth only, distance and height preserved | the Z dial (Studio's "manual") |
| tent, arch, dome, curve | X and Y | `ElevationListener`, from that mode's surface |

Two consequences worth stating:

- **The radar is a floor plan.** The object is drawn at `hypot(x, y)`, not at
  `cos(elevation)`. On a sphere those are the same and the scope is identical to
  Studio's; tent, arch and curve put the object *off* the sphere, where the
  floor radius is the only honest answer.
- **Eclipsa's "dome" is not Studio's dome.** Studio's is the unit sphere
  (`el = acos(r)`); Eclipsa's is `height = 2·√(1−x²−y²) − 1`, a dome over a room
  whose floor is at −1. At half radius Studio says 60° and Eclipsa says 55.7°.
  The pad defers to Eclipsa's, because that is the surface that moves audio.
  Making them identical means changing Eclipsa's dome equation, which changes
  the render and the deliverable — **not done, flagged.**
  → **Done and gated in §B7.**

Two real defects surfaced while testing this, both of which would have shipped:

1. **The drag read cached state.** `mouseDown` used the last polled position, so
   a gesture could act on data a frame old — and in manual mode that data
   decides the distance the object keeps. It now refreshes first. Same class of
   bug as the stale-paint one in §B5.
2. **An object at the origin could not be moved in manual mode.** Preserving its
   distance meant preserving zero: every drag wrote (0, 0) and a fresh panner's
   pad was inert. It now adopts the cursor's radius when there is no distance to
   keep.

### §B6-3. Room views draw the layout that gets rendered (was: B5 finding)

`SpeakerLookup`'s vectors were hand-written box-room drawing geometry and were
wrong twice over against what the FRIDAY path renders:

| | was | now |
|---|---|---|
| height layer elevation | **26.6°** (Y=0.5 against a horizontal magnitude of 1.0 — never normalised) | **45°** |
| rear surrounds | **±135°** | **±150°** |

Every directional speaker is now built by `SpeakerLookup::fromPolar(azimuth,
elevation, …)` from its BS.2051 angles, so the numbers in that file are the
angles a reader can check against the standard instead of vectors that have to
be reverse-engineered. 25 speakers converted; binaural and LFE keep their
hand-placed positions, having no meaningful polar angle.

This is **drawing only**. `SpeakerLookup` lives in `components/room_views/` and
is read by `PerspectiveRoomView` to place dots; no renderer, panner or export
path reads it. Eclipsa's monitoring DSP is untouched.

### §B6 regression

| Check | Result |
|---|---|
| B2-5 null, az +30.000 vs **original** `B2_ref.iamf` | **−inf dBFS, sample-exact** (§B6-1 gate) |
| B2-5 null, old harness (az 30.000723) vs `B2_ref.iamf` | **−109.53 dBFS PASS** |
| Bridge unit suite | **311 tests / 305 passed** (was 297/292) |
| — its 4 known failures | **unchanged** — upstream checksum refs, Logger ×2 |
| kala-engine | **27 suites / 252 tests, 0 failed** |
| REAPER scan + load | **clean** — the gate runs instantiate both plugins |

One evidence file is now **obsolete**: `ref-bridgeaz/B2_ref_(bridge_az).iamf`,
the reference regenerated at the captured 30.173517°. With continuous parameters
the old harness lands on 30.000723 instead, so it no longer matches that
reference (−62.14 dBFS) and matches the original one instead (−109.53 dBFS). The
regression target for B2-5 is the **original** `B2_ref.iamf` from here on; the
regenerated one is kept only as the record of why it existed.

---

## §B7. The dome surface becomes Studio's dome

The last item §B6-2 left open, treated as a gated change rather than a patch
because three deliverable-producing paths read this surface.

### §B7-1. What changed

`ElevationListener::getDomeElevationPtClamped` computed
`height = 2·√(1 − x² − y²) − 1` — a dome over a room whose floor is at −1, so
its rim sat at floor level. It now computes `height = √(1 − x² − y²)`, the unit
sphere, which is Studio's `PannerScope.constraint_elevation("dome")`:
`el = acos(r)`, rim at the horizon, centre at the zenith.

**One implementation, so agreement is structural.** That function is the only
dome surface in the tree. The panner pad, the room views, Eclipsa's monitoring
render and the KALA export all reach it through the same call, so they moved
together; there was no second copy to keep in step. tent, arch, curve and flat
are untouched.

**What it means for a session.** A dome-constrained object at full radius now
sits at ear level rather than on the floor — `getDomeElevationPtClamped` returns
a height in [0, 1] where it returned [−1, 1]. That is the change; it is why this
was gated.

### §B7-2. A second defect, found by gating

`parameterChanged` opened with `int newZ = currentZ`, three lines above the
comment §B6-1 left promising the dome write would not be rounded. Every derived
height was truncated to a whole unit. At normalised radius 0.5 the sphere wants
Z = 43.30127 and the int gave 43 — 59.83° instead of 60.000°, small but enough
to move the VBAP gains against a layout whose nearest speakers are at 45°, and
enough to keep the elevated gate below sample-exact.

`newZ` is now `float`. tent and arch keep the truncation they have always had,
written as an explicit `std::trunc` instead of left to an implicit conversion;
curve's `std::ceil` already produced an integral value. Only the dome's height
changes.

### §B7-3. The gates

Both are headless REAPER 7.78 → KALA export → FFmpeg null against a
`studio_cli` reference, the B2-5 harness unchanged. Evidence in
`docs/evidence/b7/`.

**A — the regression.** az +30.000, el 0, elevation mode "none". An
azimuth-only pan never touches the dome surface, so this number had to stay
exactly where §B6-1 left it. It did: **−inf dBFS, sample-exact on all 12
channels**, against the **original** `B2_ref.iamf` — no reference regeneration,
same captured `X=−25.0000000 Y=43.3012695`.

**B — the pose the old surface got wrong.** az 0, el 60, elevation mode "dome".
X = 0 and Y = 25 arrive with the plugin state and `ElevationListener` derives
the height; the script writes no Z, so the gate cannot pass by asserting its own
arithmetic. Captured **Z = 43.3012695** — 50·√(1 − 0.5²) to the last digit the
parameter holds — and the null against a `studio_cli` reference at the same
pose is **−inf dBFS, sample-exact**: TFL/TFR at −19.03, TRL/TRR at −41.70 on
both sides, everything else silent on both sides.

**The counterfactual, so the fix has a size.** The same Bridge export against a
Studio reference authored at **55.6725°** — the elevation the old surface
assigned at this radius — nulls at only **−40.29 dBFS** and fails the −90 dBFS
gate outright. That is the error that was in the deliverable.

| gate | pose | reference | result |
|---|---|---|---|
| B7-A | az +30.000, el 0, none | original `B2_ref.iamf` | **−inf dBFS, sample-exact** |
| B7-B | az 0, el 60, **dome** | `B7_ref.iamf` (az 0, el 60) | **−inf dBFS, sample-exact** |
| B7-B′ | az 0, el 60, **dome** | `B7_old.iamf` (az 0, el 55.6725) | **−40.29 dBFS, FAIL** — the defect, measured |

### §B7-4. Parity screenshots

Regenerated for both B5 poses. The Studio panels come back **byte-identical** —
Studio did not move, the Bridge moved to meet it — and the Bridge panels
changed on both dome poses while `bridge_tent_az+30.png` is byte-identical,
which is the blast radius the change was supposed to have.

The sheets are now composed by `docs/evidence/b5/parity_sheet.py`, committed so
they are reproducible. It places the two 560×560 panels exactly where the
original B5 sheets had them (the panel regions diff to zero); only the caption
strip re-renders, in whatever sans the bench resolves.

### §B7 regression

| Check | Before B7 | After B7 |
|---|---|---|
| B2-5 null, az +30.000 vs original `B2_ref.iamf` | −inf dBFS, sample-exact | **−inf dBFS, sample-exact** |
| Elevated null, az 0 / el 60, dome | not gated — the pose was wrong by 4.33° | **−inf dBFS, sample-exact** |
| Dome vs Studio at r = 0, ¼, ½, ¾, 1 | 90 / 71.2 / 55.7 / 39.5 / −90° | **exact, asserted to 0.01°** |
| Bridge unit suite | 311 / 305, 2 skipped | **312 / 306, 2 skipped** (+1 parity test) |
| — its 4 known failures | checksum ×2, Logger ×2 | **unchanged, same 4** |
| kala-engine | 27 suites / 252 tests | **27 suites / 252 tests, 0 failed** |
| REAPER scan + load | clean | **clean** — both gate runs instantiate both plugins |

Two operational notes from this run, neither a product defect: REAPER does not
always exit cleanly after `Main_OnCommand(40004)` on this bench — gate A hung
and gate B segfaulted, both **after** the export was written and verified — so
the harnesses are run under `timeout` and judged by their logs and artefacts.
And the unit suite must still be run from an in-tree `build/`, per
`docs/BUILDING-LINUX.md`; out of tree eight `FileOutputTests` fail on missing
fixtures and `verify_metadata` segfaults.
