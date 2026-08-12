/*
 * Copyright 2025 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// FRIDAY Bridge -> Studio handoff (V2-02, B4): write a .fstudio the Bridge's
// export can be opened from in FRIDAY Studio.
//
// The schema is Studio's, from friday-studio/studio/session.py Session.to_dict
// / from_dict. from_dict is tolerant about most keys but REQUIRES `name` and
// `file_path` on every object, and builds Keyframe(**k), so keyframe keys must
// match that dataclass exactly: t, azimuth, elevation, spread, gain_db, curve.
//
// Session.validate() reports an object whose file_path does not exist, so the
// handoff points at the per-audio-element WAVs the export already writes rather
// than at placeholders — a handed-off session should open clean, not with
// problems the user has to interpret.

#pragma once

#include <string>
#include <vector>

#include "FridayObjectTransport.h"

namespace friday {

/// One object track in the written session.
struct SessionObject {
  std::string name;
  std::string file_path;  // "" is legal but makes Session.validate() complain
  std::vector<ObjectReceiver::Keyframe> keyframes;
};

/// Serialise a Studio session. Pure — the tests assert on the string.
std::string buildSessionJson(const std::string& name, int sampleRate,
                             float targetLkfs, const std::string& language,
                             const std::vector<SessionObject>& objects);

/// Write `json` to `path`. Returns false on any I/O failure.
bool writeSessionFile(const std::string& path, const std::string& json);

/// `<export>.iamf` -> `<export>.fstudio`; anything else just gets the suffix.
std::string sessionPathFor(const std::string& exportFilePath);

}  // namespace friday
