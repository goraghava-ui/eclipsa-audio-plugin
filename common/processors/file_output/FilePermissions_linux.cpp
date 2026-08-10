// Copyright 2025 Google LLC
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

// Linux has no equivalent of macOS security-scoped bookmarks: a plugin runs
// with the host's own filesystem credentials and there is no Powerbox grant to
// preserve across a save. Same no-op shape as FilePermissions_windows.cpp;
// FilePermissions.h documents the empty/nullptr returns as the contract for
// non-Apple platforms.

#include "FilePermissions.h"

std::string createSecurityScopedBookmark(const std::string&) { return {}; }
void* startSecurityScopedAccess(const std::string&) { return nullptr; }
void stopSecurityScopedAccess(void*) {}
