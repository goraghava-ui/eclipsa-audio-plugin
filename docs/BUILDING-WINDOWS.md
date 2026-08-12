# Building on Windows (B3 — AAX)

First brought up 2026-08-12 on the FRIDAY laptop (Windows 11, VS 2022
Build Tools 14.44). Every pitfall below was hit for real; in order:

## Prerequisites

| Piece | Version / note |
|---|---|
| VS 2022 Build Tools | C++ workload (`Microsoft.VisualStudio.Workload.VCTools`). MFC not needed for the plugins (SDK examples' .rc want `afxres.h` — patch to `winres.h` or install MFC). |
| CMake | **3.31.x — NOT 4.x.** CMake 4 removed single-arg `FetchContent_Populate`, which `cmake/eigen.cmake` (via obr) still uses. |
| vcpkg | Clone **without `--depth 1`**, or `git fetch --unshallow` — the manifest pins `builtin-baseline`, and a shallow clone cannot resolve it. |
| Submodules | `git submodule update --init --recursive` (JUCE, Spatial_Audio_Framework). An empty JUCE dir surfaces later as `Unknown CMake command "juce_set_aax_sdk_path"`. |
| Intel MKL | `winget install Intel.oneMKL`. SAF's OpenBLAS fallback is a dead end on Windows: vcpkg has no LAPACKE (lapack-reference ships none; its `cblas` feature also conflicts with default `noblas`). MKL is the path upstream documents. |
| AAX SDK | 2.8.1 (the pin in `cmake/toolchains/windows.cmake`). 2.9.0 also builds; validator behaviour identical. Keep the SDK outside any public repo. |
| Rust (kala-cabi) | `rustup default stable-x86_64-pc-windows-msvc` (the GNU toolchain's import libs don't fit MSVC). `cargo build --release -p kala-cabi` in the kala-engine checkout → `kala_cabi.dll` + import lib. |

## Configure

```bat
cmake -DVCPKG_ROOT:STRING=<vcpkg> -DVCPKG_TARGET_TRIPLET:STRING=x64-windows ^
  --no-warn-unused-cli -S . -B build -G "Visual Studio 17 2022" -T host=x64 -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/windows.cmake ^
  -DBUILD_AAX=ON -DAAX_SDK_ROOT=<aax-sdk-2-8-1> ^
  -DKALA_CABI_DIR=<kala-engine checkout> ^
  -DMKL_ROOT=<oneAPI>/mkl/latest ^
  -DJUCE_COPY_PLUGIN_AFTER_BUILD=OFF ^
  -DECLIPSA_VERSION=0.0.1
```

- `JUCE_COPY_PLUGIN_AFTER_BUILD=OFF` unless the shell is elevated — the
  AAX copy step targets `C:\Program Files\Common Files\Avid\Audio\Plug-Ins`
  and fails without admin, which MSBuild reports as an opaque MSB3073 on
  the *vcpkg applocal* line (the applocal call itself is fine).
- If MKL_ROOT carries spaces, the 8.3 form (`C:/PROGRA~2/...`) avoids a
  class of quoting bugs when scripting the configure.

## Windows-only source fixes (in-tree since 0a42251)

- `common/processors/friday/*.cpp`: `#undef snprintf` guard —
  `gpac/setup.h` macro-renames it to `_snprintf`, breaking `std::snprintf`.
- `cmake/libspatialaudio.cmake`: `WINDOWS_EXPORT_ALL_SYMBOLS` on
  `spatialaudio-shared` — upstream has no `__declspec` exports, so MSVC
  otherwise emits the DLL without an import `.lib` (LNK1181 at plugin link).

## Runtime DLLs the bundle needs

`vcpkg z-applocal` covers the vcpkg ports and `copy_resources` covers
gpac/iamf_tools/zmq/ssl, but two direct dependencies are copied by
neither — put them in each `*.aaxplugin/Contents/x64/` beside the binary:

- `spatialaudio.dll` (from `build/_deps/libspatialaudio-build/Release/`)
- `kala_cabi.dll` (from the kala-engine `target/release/`)

`dumpbin /DEPENDENTS` on the plugin binary is the quick audit.

## Validating without Pro Tools

DigiShell's interactive console reads the Windows console, not
redirected stdin — scripting it via pipes stalls at the `dsh>` prompt.
Use the **standalone test executables** in
`DigiShell/AAXValidatorResources/Tools/` instead:

```bat
aaxval.test.load_unload.exe --bundle_path "<...>.aaxplugin" --n 3
aaxval.test.describe_validation.exe --bundle_path "<...>.aaxplugin"
```

2026-08-12 results, Element + Renderer:

- `test.load_unload`: **E_COMPLETED_PASS** on both (after the two DLL
  copies above; before them the load fails, which is how the missing
  DLLs were found).
- `test.describe_validation`: reports missing
  ManufacturerID/ProductID/PlugInID and "no process function"
  (−14024 `RequiredPropertyMissing` ×3, −14008 `EffectComponentsMissing`).
  **This is expected outside Pro Tools for an HOA plugin, not a porting
  defect:** with no host `AAX_IFeatureInfo`, JUCE's
  `hostSupportsStemFormat()` falls back to
  `AAX_STEM_FORMAT_INDEX(stem) <= 12`, and every ambisonic stem sits
  above that (Ambi-1 = 14 … Ambi-5 = 32, our main layout). Zero stem
  pairs survive, so zero components get described. The SDK's own
  DemoGain (stereo, index ≤ 12) passes describe in the same
  environment, isolating the cause. Bisect with
  `-DFRIDAY_KALA_EXPORT=OFF` reproduces identically, clearing the
  FRIDAY additions. The real describe gate therefore runs inside
  Pro Tools Developer build, which supplies stem-format feature info.

PACE note: `PACELicenseDServices` must be running for the validator
host pieces (comes with iLok License Manager).
