// Copyright 2026 Friday Pictures
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Windows delay-load resolver: load our bundled DLLs from THIS module's
// own directory.
//
// The plugin ships its runtime DLLs (gpac, iamf_tools, zmq, ssl,
// spatialaudio, kala_cabi, vcpkg ports) beside the plugin binary inside
// the bundle (Contents/x86_64-win, Contents/x64). The default delay-load
// helper resolves with a bare LoadLibrary(name), whose search order
// starts at the HOST EXECUTABLE's directory and never looks in the
// plugin's — an AAX host papers over this with SetDllDirectory on the
// bundle, but VST3 hosts (REAPER) do not, so every delay-loaded import
// fails and the plugin never comes up outside Pro Tools. This hook makes
// the bundle self-contained: if the requested DLL exists next to this
// module, load it from there; otherwise fall back to the default search.
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <delayimp.h>

#include <string>

namespace {

HMODULE thisModule() {
  HMODULE h = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&thisModule), &h);
  return h;
}

FARPROC WINAPI fridayDelayLoadHook(unsigned dliNotify, PDelayLoadInfo pdli) {
  if (dliNotify != dliNotePreLoadLibrary || pdli == nullptr ||
      pdli->szDll == nullptr) {
    return nullptr;
  }
  wchar_t modPath[MAX_PATH];
  const DWORD n = GetModuleFileNameW(thisModule(), modPath, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    return nullptr;
  }
  std::wstring dir(modPath, n);
  const size_t slash = dir.find_last_of(L"\\/");
  if (slash == std::wstring::npos) {
    return nullptr;
  }
  dir.resize(slash + 1);

  const int wlen =
      MultiByteToWideChar(CP_ACP, 0, pdli->szDll, -1, nullptr, 0);
  if (wlen <= 1) {
    return nullptr;
  }
  std::wstring name(static_cast<size_t>(wlen), L'\0');
  MultiByteToWideChar(CP_ACP, 0, pdli->szDll, -1, name.data(), wlen);
  name.resize(static_cast<size_t>(wlen) - 1);  // drop the NUL

  const std::wstring full = dir + name;
  if (GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return nullptr;  // not bundled — let the default search handle it
  }
  return reinterpret_cast<FARPROC>(
      LoadLibraryExW(full.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
}

}  // namespace

extern "C" const PfnDliHook __pfnDliNotifyHook2 = fridayDelayLoadHook;

#endif  // _WIN32
