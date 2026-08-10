# FRIDAY Bridge — Clean-Room / Third-Party License Log

**Subject:** `google/eclipsa-audio-plugin` (upstream), audited as the base for
**FRIDAY Bridge** (PRD-v2 §6 Phase A, rows V2-01..V2-04).

**Audit date:** 2026-08-10
**Audited commit:** `9a54cc21b93e32dcb3789a5cd2ddca57b9c6e254` (main, 2026-08-06,
"Increase tolerance of muxing error to account for AV frame mismatch (#124)")
**Upstream project licence:** Apache-2.0 (root `LICENSE`)
**This copy lives in:** `goraghava-ui/eclipsa-audio-plugin` — the FRIDAY Bridge
fork, cloned to `/data/projects/friday/friday-bridge`. `origin` is the fork;
`upstream` fetches from google/eclipsa-audio-plugin and its **push URL is
disabled** (`DISABLED-never-push-to-google`), so no commit here can reach
Google's repo. The audit itself was performed in a separate read-only checkout
at `/data/build/eclipsa-upstream`, at the same commit.

**Nothing was built for this audit.** It is a pre-build gate per PRD-v2 §8.6.

---

## Owner decisions — 2026-08-10

Recorded here because they are the conditions under which the verdicts in §2
are accepted.

1. **JUCE — proceed under GPLv3 for development.** No distribution yet;
   commercial-licence-vs-open-sourcing Bridge is deferred to the first external
   release. Until then nothing leaves this machine, so the GPLv3 obligations
   (source availability on distribution) are not triggered.
2. **libspatialaudio — option (a), convert to shared for B1 now.** Full
   replacement by KALA's own VBAP/AllRAD lands in B2 as planned.
3. **B1 platform — write the Linux port on this fork.** Windows comes later,
   together with AAX.

---

## 0. Method — why the target link graph, not the directory listing

`third_party/` has eight directories, but directory presence does **not** mean
a component ships in our VST3s. Two traps were found and avoided:

1. **`eclipsa_dependencies` is a dead target.** Root `CMakeLists.txt:194-210`
   defines an INTERFACE library listing `vendored_gpac`, `vendored_iamf_tools`,
   `libzmq`, `saf`, `Libear`, `LUFSMeter`, `spatialaudiolib`, `${IAMF_LIB_NAME}`
   — and **nothing links it** (grep: only its own definition). Three of those
   names do not even exist as targets (`Libear` vs the real `libear`,
   `LUFSMeter` vs `lufs_meter`, `spatialaudiolib` vs `libspatialaudio`); CMake
   never errors because the target is never used. **Reading that list as the
   dependency set would be wrong.**
2. **libiamf is test-only.** Despite `third_party/libiamf/` being a full source
   tree, the `iamf` / `iamfdec_utils` targets are linked *only* from
   `common/processors/tests/CMakeLists.txt`. No shipping plugin target links it.

The real graph, from `juce_add_plugin` down:

```
RendererPlugin (VST3)              AudioElementPlugin / "panner" (VST3)
  ├── eclipsa_common                 ├── substream_rdr
  ├── binary_data                    ├── processors
  └── libzmq                         ├── components
                                     ├── logger
eclipsa_common →                     ├── data_repository
  substream_rdr, processors,         └── data_structures
  components, logger,
  data_repository, data_structures,
  juce::juce_gui_extra + juce recommended_* flags

substream_rdr → libspatialaudio, obr, libear, data_structures
processors    → gpac, libear, iamftools, logger, substream_rdr, libzmq,
                saf, saf_example_ambi_dec, lufs_meter
components    → binary_data, libear, substream_rdr, processors,
                data_structures, data_repository, Eigen
logger        → Boost::log, Boost::log_setup, Boost::filesystem,
                Boost::thread, juce::juce_core
data_structures → iamftools, substream_rdr
```

Both VST3 targets transitively reach the same set. There is **no** component
that the renderer needs and the panner does not (or vice versa) — so "which of
the two plugins is affected" is always *both*.

`third_party/CMakeLists.txt:15` sets `BUILD_SHARED_LIBS OFF` ("Force static
linking for all third-party dependencies"), so anything built from source lands
**inside** the VST3 binary. Only the `SHARED IMPORTED` prebuilts stay external.

---

## 1. Component inventory

Link column: **static** = object code inside our shipped `.vst3`; **shared** =
separate `.dylib`/`.dll` we ship alongside; **headers** = header-only /
compiled-from-source-into-us; **test** = never in a shipping artefact.

### 1.1 JUCE — the §8.6 decision

| Field | Value |
|---|---|
| Source | submodule `third_party/JUCE` → **`https://github.com/WithACX/JUCE`** (a fork of `juce-framework/JUCE`), pinned at `2c75900809c5f6224c323d6fa2325f4dcc43535d` |
| **Edition / version** | **JUCE 7.0.12** (`project(JUCE VERSION 7.0.12 …)`) |
| Licence | **JUCE 7 EULA (proprietary, tiered) OR GPL-3.0-only** — dual, licensee's choice |
| Links into VST3? | **Yes — static**, both plugins |

**Correction to carry back into PRD-v2 §8.6:** the risk row should read
**GPLv3 vs commercial**, not *AGPL* vs commercial. AGPLv3 is JUCE **8**; this
tree is pinned to JUCE **7.0.12**, whose fallback copyleft is plain GPLv3.
That is a materially cheaper obligation (no network-use/SaaS clause) and it
changes what "release under the free licence" would cost us.

Per `LICENSE.md`, four modules are permissive **ISC** —
`juce_audio_basics`, `juce_audio_devices`, `juce_core`, `juce_events`. Every
other module is EULA-or-GPLv3.

Modules Eclipsa actually uses:

| Module | Licence | Used by |
|---|---|---|
| `juce_core` | **ISC** ✅ | `logger` |
| `juce_gui_extra` | EULA/GPLv3 ⚠️ | `eclipsa_common` → both plugins |
| `juce_audio_utils` | EULA/GPLv3 ⚠️ | renderer test target; pulled transitively by the plugin wrapper |
| `juce_cryptography` | EULA/GPLv3 ⚠️ | test builds only (`CI_TEST`/`INTERNAL_TEST`) |
| `juce_audio_processors`, `juce_gui_basics`, `juce_graphics`, `juce_audio_plugin_client` | EULA/GPLv3 ⚠️ | implicit — `juce_add_plugin()` pulls the VST3 wrapper |

The permissive ISC subset is **not** sufficient: a VST3 plugin cannot be built
without the plugin-client/GUI modules.

> **VERDICT: JUCE licence mode = OWNER DECISION.**
> **JUCE 7 commercial (Indie/Pro) vs releasing FRIDAY Bridge under GPLv3.**
> Budgeted in **PRD-v2 §8.6**. Not a technical blocker — nothing to remove or
> replace, only a licence to buy or a copyleft to accept. Note that shipping
> Bridge under GPLv3 would contradict the KALA clean-IP firewall
> (`kalaDsp1/CLAUDE.md` §1-2) if any KALA source were ever linked in; the
> planned `kala-ffi` C ABI boundary (B2) is what keeps that question separable.

### 1.2 Everything else

| # | Component | Version / commit | SPDX | Link into VST3 | Used by our targets? |
|---|---|---|---|---|---|
| 1 | **libspatialaudio** (fork) | `WithACX/libspatialaudio` (fork of `peterStitt/libspatialaudio`), **FetchContent with NO `GIT_TAG` — unpinned, tracks default branch** | **LGPL-2.1-or-later** | **static** (`spatialaudio-static`) | **Yes** — `substream_rdr` → both plugins |
| 2 | **gpac** | prebuilt binary, from `gpac/gpac@4f33ccde09bce9e6d56a56304250fa49893f5019` | **LGPL-2.1-or-later** | **shared** (`libgpac.dylib` / `libgpac.dll`, `SHARED IMPORTED`) | **Yes** — `processors` → both plugins |
| 3 | **Spatial_Audio_Framework** | submodule `leomccormack/Spatial_Audio_Framework@018e06e86ccdbb37cc527ca511a3a26576126b71` | **ISC** (see note) | static | **Yes** — `processors` (`saf`, `saf_example_ambi_dec`) |
| 4 | **LUFSMeter** | vendored source, Klangfreund / Samuel Gaehwiler, 2011-2018 | **MIT** | **static** (INTERFACE `target_sources` — compiled straight into us) | **Yes** — `processors` |
| 5 | **libear** | prebuilt `libear.a` / `libear.lib` | **Apache-2.0** | static | **Yes** — `substream_rdr`, `processors`, `components` |
| 6 | **iamf-tools** | prebuilt, `AOMediaCodec/iamf-tools@7542365c18d02ea4857c492963c50788cf20158e` | **BSD-3-Clause** + **AOM Patent License 1.0** | **shared** (`libiamf_tools.dylib` / `iamf_tools.dll`) | **Yes** — `data_structures`, `processors` (this is the IAMF **export** path B2 targets) |
| 7 | **obr** (Open Binaural Renderer) | prebuilt, `google/obr@212982959ef5fe0de36f666c0450e11c711c58e8` (Abseil 20250512.1) | **BSD-3-Clause** (Google) | shared (macOS) / static-from-source (Windows) | **Yes** — `substream_rdr` |
| 8 | **libzmq** | `zeromq/libzmq`, **FetchContent with NO `GIT_TAG` — unpinned** | **MPL-2.0** | static (`BUILD_SHARED_LIBS OFF` wins over the ignored `CMAKE_ARGS`) | **Yes** — linked *directly* by both plugins |
| 9 | **cppzmq** | `zeromq/cppzmq`, unpinned | **MIT** | headers | Yes — via libzmq usage |
| 10 | **Boost** | 1.86.0 (release tarball) | **BSL-1.0** | static | **Yes** — `logger` (log, log_setup, filesystem, thread) |
| 11 | **Eigen** | 3.4.0 (gitlab tarball) | **MPL-2.0** | headers | **Yes** — `components` (AmbisonicsVisualizer) and `obr` |
| 12 | **pffft** | `marton78/pffft@a9786ad2e709dd2b8024f0da1ced26b083c1ffc9` | **BSD-3-Clause**-like (FFTPACK-derived) | static | **Yes** — via `obr` |
| 13 | **protobuf** | via `cmake/protobuf/*` config + iamf-tools proto objects (declared in `.gitmodules` but **not present** in the tree at HEAD) | **BSD-3-Clause** | static | **Yes** — `data_structures` (`proto-objects`) |
| 14 | **OpenSSL** (`libcryptoMD.dll`, `libsslMD.dll`) | prebuilt, ships beside gpac | **Apache-2.0** (OpenSSL 3.x) | shared, **Windows only** | Yes — gpac's TLS deps |
| 15 | **libsodium** | pulled by libzmq (`WITH_LIBSODIUM ON`) | **ISC** | static | Yes — via libzmq |
| 16 | **Intel oneAPI MKL** | Windows only, optional `SAF_PERFORMANCE_LIB=SAF_USE_INTEL_MKL_LP64`; auto-falls back to **OpenBLAS/LAPACKE** (BSD-3-Clause) when `MKL_ROOT` is unset | **LicenseRef-Intel-Simplified-Software-License** (proprietary, redistributable) | shared | Windows builds only; macOS uses Apple Accelerate (system framework) |
| 17 | vcpkg runtime deps | zlib, bzip2, liblzma, zstd | Zlib / BSD-like | static | Windows only |
| 18 | **libiamf** | `AOMediaCodec/libiamf@48b8b5dc06971e14826d114fd032dab285f4c944` (zip) | **BSD-3-Clause-Clear** + AOM patent | **test** | **No** — only `common/processors/tests` link `iamf`/`iamfdec_utils` |
| 19 | **googletest** | v1.14.0 | **BSD-3-Clause** | **test** | No — `CI_TEST`/`INTERNAL_TEST` only |

**Note on SAF (#3) — ISC, conditionally.** SAF is dual-licensed **ISC / GPL-2.0**.
`framework/include/saf.h` makes the trigger explicit:

```c
#if defined(SAF_ENABLE_TRACKER_MODULE) || defined(SAF_ENABLE_HADES_MODULE)
  #define SAF_LICENSE_STRING "GNU GPLv2"
#else
  #define SAF_LICENSE_STRING "ISC"
```

Both options default **OFF** upstream, and Eclipsa sets neither (it sets only
`SAF_BUILD_TESTS=0`, `SAF_BUILD_EXAMPLES=1`, `saf_example_list=ambi_dec`,
`SAF_PERFORMANCE_LIB`). **In this configuration SAF is ISC — not GPL.**
`saf_sofa_reader` is also ISC; only `saf_tracker` and `saf_hades` are GPLv2.
→ **Action: pin this with a CI assert** that neither `SAF_ENABLE_TRACKER_MODULE`
nor `SAF_ENABLE_HADES_MODULE` is defined. It is a one-flag distance from GPLv2
and nothing in the build currently prevents someone flipping it.

**This clears the `CLAUDE.md` §1 "SPARTA" concern.** SPARTA (the plugin suite)
is GPLv3 and stays forbidden; SAF, the framework underneath it, is a separate
ISC-licensed library in this configuration. No SPARTA source is present.

---

## 2. Verdict table

Rules applied: Apache/BSD/MIT/ISC = fine · MPL = fine (file-level copyleft) ·
LGPL = fine **if dynamically linked**, noted · GPL-only linking into our
shipping VST3 = BLOCKER.

| Component | Licence | Linked into VST3? | Verdict | Action |
|---|---|---|---|---|
| **JUCE 7.0.12** (WithACX fork @2c75900) | JUCE 7 EULA **or GPL-3.0-only** | **Yes — static** | ⚠️ **OWNER DECISION** | Buy JUCE 7 Indie/Pro, or accept GPLv3 for Bridge. Budgeted **PRD-v2 §8.6**. Fix §8.6 wording: **GPLv3**, not AGPL |
| **libspatialaudio** (WithACX fork, unpinned) | **LGPL-2.1-or-later** | **Yes — STATIC** | 🔴 **BLOCKER** (static LGPL) | See §3.1 — convert to shared, or replace with KALA VBAP/AllRAD |
| **gpac** (@4f33ccd) | **LGPL-2.1-or-later** | Yes — **shared** (`.dylib`/`.dll`) | 🟡 **OK, noted** | Keep dynamic. Never `-static`. Ship the lib unmodified + its LGPL text + written offer for source; keep it replaceable by the user (LGPL §6) |
| Spatial_Audio_Framework (@018e06e) | **ISC** (GPLv2 only if tracker/hades enabled) | Yes — static | ✅ Fine | Add CI assert that `SAF_ENABLE_TRACKER_MODULE` / `SAF_ENABLE_HADES_MODULE` stay OFF |
| **LUFSMeter** (Klangfreund) | **MIT** | Yes — static | ✅ **Fine — not a blocker** | None required. Replacing it with KALA's own BS.1770-4 meter (`kala-dsp/lufs.rs`, already calibrated to −18.00 LKFS) is now an *optional consolidation*, not a licence fix |
| libear | Apache-2.0 | Yes — static | ✅ Fine | NOTICE attribution |
| iamf-tools (@7542365) | BSD-3-Clause + AOM Patent 1.0 | Yes — shared | ✅ Fine | Attribution + carry `PATENTS` |
| obr (@2129829) | BSD-3-Clause | Yes | ✅ Fine | Attribution |
| libzmq (unpinned) | **MPL-2.0** | Yes — static | ✅ Fine (file-level) | Pin a tag; publish any modified MPL files if we ever patch it |
| cppzmq | MIT | headers | ✅ Fine | Attribution |
| Boost 1.86.0 | BSL-1.0 | Yes — static | ✅ Fine | Attribution |
| Eigen 3.4.0 | **MPL-2.0** | headers | ✅ Fine (file-level) | Attribution |
| pffft (@a9786ad) | BSD-like | Yes — static | ✅ Fine | Attribution |
| protobuf | BSD-3-Clause | Yes — static | ✅ Fine | Attribution |
| OpenSSL 3.x (Windows) | Apache-2.0 | shared, Win only | ✅ Fine | Attribution |
| libsodium | ISC | Yes — static | ✅ Fine | Attribution |
| Intel MKL (Windows, optional) | Intel Simplified SW Licence | shared, Win only | 🟡 Noted | Prefer the **OpenBLAS** fallback (BSD-3) for shipped builds; leave `MKL_ROOT` unset |
| zlib/bzip2/liblzma/zstd (vcpkg) | Zlib / BSD-like | Win only | ✅ Fine | Attribution |
| libiamf (@48b8b5d) | BSD-3-Clause-Clear | **No — tests only** | ✅ Fine | — |
| googletest v1.14.0 | BSD-3-Clause | **No — tests only** | ✅ Fine | — |

**Bottom line: exactly ONE hard blocker (libspatialaudio, static LGPL) and ONE
owner decision (JUCE).** No GPL-only code links into the shipping VST3s.
The audit found **no GPLv3 component at all** in the plugin link graph — the
scary directory names (`Spatial_Audio_Framework`, `LUFSMeter`) both came back
clean, and the two real copyleft items are LGPL, one of which is already
correctly dynamic.

---

## 3. Blocker detail and options

### 3.1 libspatialaudio — LGPL-2.1 statically linked

`cmake/libspatialaudio.cmake` fetches `WithACX/libspatialaudio` and ends with:

```cmake
target_link_libraries(libspatialaudio INTERFACE spatialaudio-static)
```

with `BUILD_SHARED_LIBS OFF` forced tree-wide. `substream_rdr` links it, so
LGPL object code is **statically absorbed into both shipping VST3s**. Under
LGPL-2.1 §6 that obliges us to let a user relink our plugin against a modified
libspatialaudio — i.e. ship our `.o`/`.a` files or an equivalent relink
mechanism. That is incompatible with shipping a closed Bridge binary, and it
sits outside `kalaDsp1/LICENSE-POLICY.md`, which forbids LGPL outright and
already names libspatialaudio explicitly: *"libspatialaudio (LGPL) is pilot
bench reference only — must never link into the shipped product."*

Options, cheapest first:

- **(a) Make it shared.** Build `spatialaudio` as a `.so`/`.dylib`/`.dll` and
  link that instead of `spatialaudio-static`; ship it beside the plugin with
  its LGPL text. Small CMake change, no code change, immediately satisfies both
  the LGPL and our own "LGPL is fine if dynamic" rule. **Recommended for B1** —
  it unblocks the bring-up without touching DSP.
- **(b) Replace with KALA — the strategic fix.** libspatialaudio is used for
  ambisonic decode / panning inside `substream_rdr`. KALA already owns
  clean-room VBAP (Pulkki 1997) and AllRAD (Zotter & Frank 2012) in
  `kala-render`, with `kala-ambigen` and `kala-nulltest` alongside. Routing
  `substream_rdr` through the planned `kala-ffi` C ABI deletes this dependency
  entirely — and it is the **same swap B2 already plans** for the export stage.
  Slower, but it is the direction PRD-v2 V2-01 points ("all DSP stays in KALA;
  plugins are transport + UI").
- **(c) Drop the code path.** If Bridge's renderer only needs the OBR/libear
  paths for our pan authoring, `libspatialaudio` may be reachable but unused at
  runtime. Not verified — would need a symbol-level check before relying on it.

**Recommendation: (a) now to unblock B1, (b) as part of B2.** Do not ship any
build made with (a) until the LGPL notice + relink offer is in place.

### 3.2 JUCE — owner decision, no technical work

Nothing to remove. Either buy a JUCE 7 licence (Indie $40/mo under 500K USD
revenue; Pro $130/mo) or release FRIDAY Bridge under GPLv3. **PRD-v2 §8.6.**
Flagging again that §8.6 currently says *AGPL* — for JUCE 7.0.12 it is GPLv3.

---

## 4. Non-licence finding that also blocks B1

**Eclipsa does not support Linux.** This is a build-system fact, not a licence
one, but it lands on the same gate:

- `README.md` prerequisites: *"MacOS 14.7.1 **or** Windows 11"*.
- `cmake/toolchains/` contains **only** `macos.cmake` and `windows.cmake`.
- `cmake/prebuiltLibs/` contains **only** `macos.cmake` and `windows.cmake`;
  `third_party/CMakeLists.txt:20` does
  `include(".../prebuiltLibs/${ECLIPSA_PLATFORM}.cmake")`, and `ECLIPSA_PLATFORM`
  is set *only* by those two toolchain files. On Linux the include fails outright.
- The prebuilt binaries for gpac / iamf-tools / obr / libear ship as
  `.dylib`/`.a` (macOS) and `.dll`/`.lib` (Windows) — **no `.so` exists**.

So "REAPER-first VST3 on this Linux bench" needs a **Linux port** first:
author `cmake/toolchains/linux.cmake` + `cmake/prebuiltLibs/linux.cmake`, and
build gpac, iamf-tools, obr and libear from source as Linux `.so`/`.a`. That is
real work, not a flag flip. Alternatives: run B1 on Windows/macOS as upstream
intends, or scope B1 to a Linux port task of its own.

---

## 5. Attribution obligations (for whatever we ship)

Carry `third_party/THIRD_PARTY_LICENSES.txt` plus the upstream Apache-2.0
`LICENSE` and `CONTRIBUTORS.md`; add per-component notices for JUCE (per the
chosen mode), gpac (LGPL text + written offer for source + relink freedom),
libspatialaudio (same, once dynamic), MPL-2.0 components (libzmq, Eigen —
publish any modified files), and the AOM `PATENTS` files from iamf-tools and
libiamf.

---

## 6. Audit provenance

Every claim above was read from the pinned tree or the pinned upstream commit —
no licence was inferred from a directory name or a GitHub API summary (the API
returns `NOASSERTION` for the JUCE, SAF and libspatialaudio forks, which is why
`LICENSE.md` was fetched at each pinned SHA instead).

| Claim | Evidence |
|---|---|
| Link graph | `CMakeLists.txt`, `common/CMakeLists.txt`, `rendererplugin/CMakeLists.txt`, `audioelementplugin/CMakeLists.txt`, `third_party/CMakeLists.txt` |
| `eclipsa_dependencies` unused | grep over all `CMakeLists.txt`/`*.cmake` — one hit, its own definition |
| Static vs shared | `third_party/CMakeLists.txt:15`; `cmake/prebuiltLibs/{macos,windows}.cmake` `SHARED IMPORTED` / `STATIC IMPORTED` |
| JUCE 7.0.12 + licence | `WithACX/JUCE@2c75900` `CMakeLists.txt` + `LICENSE.md` |
| SAF ISC | `leomccormack/Spatial_Audio_Framework@018e06e` `LICENSE.md` + `framework/include/saf.h` + option defaults |
| gpac LGPL-2.1 | `third_party/gpac/LICENSE`, `third_party/gpac/README.md` |
| LUFSMeter MIT | `third_party/LUFSMeter/LICENSE` |
| libear Apache-2.0 | `third_party/libear/LICENSE` |
| libiamf BSD-3-Clear | `third_party/libiamf/LICENSE` |
| libzmq MPL-2.0 | `zeromq/libzmq` `LICENSE` |
| No Linux support | `README.md`; `ls cmake/toolchains cmake/prebuiltLibs` |

---

## 7. Verification pass — 2026-08-10 (audit closed)

### 7.1 Re-verification of the load-bearing claims

The §0 link graph is the claim everything else rests on, so it was re-derived
from the source rather than re-read from this document. Every edge matched:

| Claim | Command / file | Result |
|---|---|---|
| Static linking forced tree-wide | `third_party/CMakeLists.txt:15` | `set(BUILD_SHARED_LIBS OFF)` ✅ |
| `eclipsa_dependencies` is dead | grep over all `CMakeLists.txt` + `*.cmake` | 2 hits, both its own definition (`CMakeLists.txt:194,196`). Nothing links it ✅ |
| Only two shipping plugin targets | grep `juce_add_plugin` | `RendererPlugin` (`rendererplugin/CMakeLists.txt:26`), `AudioElementPlugin` (`audioelementplugin/CMakeLists.txt:26`) ✅ |
| `RendererPlugin` deps | `rendererplugin/CMakeLists.txt:70-74` | `eclipsa_common`, `binary_data`, `libzmq` ✅ |
| `AudioElementPlugin` deps | `audioelementplugin/CMakeLists.txt:68-78` | `substream_rdr`, `processors`, `components`, `logger`, `data_repository`, `data_structures`, juce `recommended_*` flags ✅ |
| `eclipsa_common` deps | `common/CMakeLists.txt:123-134` | same six + `juce::juce_gui_extra` ✅ |
| `substream_rdr` deps | `common/CMakeLists.txt:84-89` | `libspatialaudio`, `obr`, `libear`, `data_structures` ✅ |
| `processors` deps | `common/CMakeLists.txt:90-100` | `gpac`, `libear`, `iamftools`, `logger`, `substream_rdr`, `libzmq`, `saf`, `saf_example_ambi_dec`, `lufs_meter` ✅ |
| **libspatialaudio is static** | `cmake/libspatialaudio.cmake:44` | `target_link_libraries(libspatialaudio INTERFACE spatialaudio-static)` — **BLOCKER confirmed** 🔴 |

No claim in §1–§3 was contradicted. The verdict table in §2 stands unchanged.

### 7.2 One discrepancy examined and dismissed

`cmake/zeromq.cmake:36` passes `-DBUILD_SHARED_LIBS=ON` inside
`FetchContent_Declare(... CMAKE_ARGS ...)`. This looked like it contradicted the
"libzmq is static" row. It does not: `CMAKE_ARGS` is an **`ExternalProject`
option and is silently ignored by `FetchContent_MakeAvailable`**, which uses
`add_subdirectory` and therefore inherits the tree-wide
`set(BUILD_SHARED_LIBS OFF)`. The file itself concedes the ambiguity — lines
42-48 probe for *either* target name (`libzmq` or `libzmq-static`) and
`FATAL_ERROR` otherwise. **Licence impact: none** — libzmq is MPL-2.0, which is
file-level copyleft and fine either statically or dynamically. Row unchanged.

**Full sweep, 2026-08-10 — the mechanism is not misunderstood anywhere else.**
The concern behind the discrepancy was that if `CMAKE_ARGS` is misread in one
place it might be misread somewhere that matters, i.e. a component believed
dynamic could actually be static. Swept every site:

| Site | Setting | Effective? |
|---|---|---|
| `third_party/CMakeLists.txt:15` | `set(BUILD_SHARED_LIBS OFF)` | ✅ **yes** — tree-wide, this is the one that governs |
| `cmake/zeromq.cmake:36` | `-DBUILD_SHARED_LIBS=ON` via `CMAKE_ARGS` | ❌ dead |
| `cmake/boost.cmake:29` | `-DBUILD_SHARED_LIBS=OFF` via `CMAKE_ARGS` | ❌ dead (result happens to match anyway) |
| `third_party/libiamf/CMakeLists.txt:58` | string-replaces the upstream option default `ON`→`OFF` | ✅ yes — but libiamf is test-only |
| `third_party/iamftools/.../proto/CMakeLists.txt:63` | `protobuf_BUILD_SHARED_LIBS OFF` | ✅ yes |
| `cmake/protobuf/protobuf-config-version.cmake:51` | `_check_and_save_build_option(BUILD_SHARED_LIBS OFF)` | ✅ yes (consistency check only) |

**There is no `ExternalProject_Add` anywhere in the tree** — every fetch is
`FetchContent_MakeAvailable`, so *both* `CMAKE_ARGS` sites are dead config and
the tree-wide `OFF` is the only thing deciding static-vs-shared for
built-from-source dependencies. Two consequences that matter:

- **libspatialaudio is static twice over** — by the tree-wide `OFF` *and* by
  explicitly linking `spatialaudio-static`. The blocker is not an artefact of a
  misread flag; it is unambiguous.
- **gpac's "dynamic" status is not at risk from this at all.** gpac is a
  `SHARED IMPORTED` prebuilt binary (`libgpac.dylib`/`.dll`), so it never passes
  through the `BUILD_SHARED_LIBS` machinery. The one LGPL row that depends on
  being dynamic is decided by a different mechanism entirely.

Still **read from CMake semantics, not observed** — no configure has run,
because there is no Linux toolchain yet. The Linux port (§4, now in progress on
branch `linux-port`) will make an empirical check possible: after configure,
`ls _deps/zeromq-build/lib*` settles it. Licence impact remains none either way.

### 7.3 Open items — NOT closed by this audit

1. ~~**`friday-bridge` working clone does not exist.**~~ **CLOSED 2026-08-10** —
   the fork is cloned to `/data/projects/friday/friday-bridge`, `origin` points
   at `goraghava-ui/eclipsa-audio-plugin`, `upstream` fetches from Google with
   its push URL disabled. This file is that clone's first fork commit.
2. **Two unpinned dependencies.** `libspatialaudio` and `libzmq` are both
   `FetchContent` with **no `GIT_TAG`** — they track their default branches, so
   the licence facts recorded here are true of *today's* tip and not
   reproducible. Pin both before any shipped build.
3. **SAF one-flag distance from GPLv2** (§1.2 note) — CI assert still to be
   written.
4. **Linux port** (§4) — unrelated to licensing, but blocks B1 on this bench.
