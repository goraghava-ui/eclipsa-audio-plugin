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

#include "FridayStudioSession.h"

#include <cmath>
#include <cstdio>

// gpac/setup.h macro-renames snprintf to _snprintf on Windows, which breaks
// std::snprintf; these files want the real C++ one.
#ifdef snprintf
#undef snprintf
#endif


#include "FridayStudioLink.h"

namespace friday {

namespace {

std::string num(double v, int decimals = 4) {
  if (!std::isfinite(v)) v = 0.0;
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
  return buf;
}

std::string keyframeJson(const ObjectReceiver::Keyframe& k) {
  // 6 decimals on t: at 48 kHz one sample is ~2e-5 s, so 6 keeps keyframe
  // times distinguishable at block resolution.
  return "{\"t\":" + num(k.t, 6) + ",\"azimuth\":" + num(k.az_deg) +
         ",\"elevation\":" + num(k.el_deg) + ",\"spread\":" + num(k.spread) +
         ",\"gain_db\":" + num(k.gain_db) + ",\"curve\":\"linear\"}";
}

}  // namespace

std::string buildSessionJson(const std::string& name, int sampleRate,
                             float targetLkfs, const std::string& language,
                             const std::vector<SessionObject>& objects) {
  const std::string esc = StudioLink::jsonEscape(name);
  std::string out;
  out += "{\n";
  out += "  \"friday_studio\": 1,\n";
  out += "  \"name\": \"" + esc + "\",\n";
  out += "  \"sample_rate\": " + std::to_string(sampleRate) + ",\n";
  out += "  \"target_lkfs\": " + num(targetLkfs, 2) + ",\n";
  out += "  \"language\": \"" + StudioLink::jsonEscape(language) + "\",\n";
  out += "  \"label\": \"FRIDAY Bridge handoff\",\n";
  out += "  \"binaural_on_headphones\": true,\n";
  out += "  \"presentations\": [],\n";
  out += "  \"video_path\": \"\",\n";
  out += "  \"versions\": [],\n";
  out += "  \"scene_snapshots\": [],\n";
  out += "  \"layout_trims\": {},\n";
  out += "  \"beds\": [],\n";
  out += "  \"objects\": [";
  for (size_t i = 0; i < objects.size(); ++i) {
    const SessionObject& o = objects[i];
    out += i > 0 ? ",\n    " : "\n    ";
    out += "{\"name\": \"" + StudioLink::jsonEscape(o.name) +
           "\", \"file_path\": \"" + StudioLink::jsonEscape(o.file_path) +
           "\", \"keyframes\": [";
    if (o.keyframes.empty()) {
      // from_dict defaults a missing list to one keyframe at t=0; be explicit
      // rather than relying on that, so the file reads the same as it loads.
      out += keyframeJson(ObjectReceiver::Keyframe{});
    } else {
      for (size_t k = 0; k < o.keyframes.size(); ++k) {
        if (k > 0) out += ", ";
        out += keyframeJson(o.keyframes[k]);
      }
    }
    out += "], \"mute\": false, \"binaural_mode\": \"mid\", \"group\": \"\", "
           "\"zone\": \"\", \"snap\": false}";
  }
  out += objects.empty() ? "]\n" : "\n  ]\n";
  out += "}\n";
  return out;
}

bool writeSessionFile(const std::string& path, const std::string& json) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return false;
  const size_t written = std::fwrite(json.data(), 1, json.size(), f);
  const bool closed = std::fclose(f) == 0;
  return closed && written == json.size();
}

namespace {

void put32(std::string& out, uint32_t v) {
  out += static_cast<char>(v & 0xff);
  out += static_cast<char>((v >> 8) & 0xff);
  out += static_cast<char>((v >> 16) & 0xff);
  out += static_cast<char>((v >> 24) & 0xff);
}

void put16(std::string& out, uint16_t v) {
  out += static_cast<char>(v & 0xff);
  out += static_cast<char>((v >> 8) & 0xff);
}

}  // namespace

bool writeMonoWav(const std::string& path, const std::vector<float>& pcm,
                  int sampleRate) {
  // Hand-rolled RIFF rather than juce::WavAudioFormat so this file stays free
  // of JUCE and unit-testable on its own. WAVE_FORMAT_IEEE_FLOAT (3), 1 ch.
  const uint32_t dataBytes =
      static_cast<uint32_t>(pcm.size() * sizeof(float));
  std::string hdr;
  hdr += "RIFF";
  put32(hdr, 36 + dataBytes);
  hdr += "WAVE";
  hdr += "fmt ";
  put32(hdr, 16);
  put16(hdr, 3);                                   // IEEE float
  put16(hdr, 1);                                   // mono
  put32(hdr, static_cast<uint32_t>(sampleRate));
  put32(hdr, static_cast<uint32_t>(sampleRate) * 4);  // byte rate
  put16(hdr, 4);                                   // block align
  put16(hdr, 32);                                  // bits per sample
  hdr += "data";
  put32(hdr, dataBytes);

  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return false;
  bool ok = std::fwrite(hdr.data(), 1, hdr.size(), f) == hdr.size();
  if (ok && dataBytes > 0) {
    ok = std::fwrite(pcm.data(), 1, dataBytes, f) == dataBytes;
  }
  return (std::fclose(f) == 0) && ok;
}

std::string stemPathFor(const std::string& sessionPath,
                        const std::string& objectName) {
  const std::string kSuffix = ".fstudio";
  std::string base = sessionPath;
  if (base.size() >= kSuffix.size() &&
      base.compare(base.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0) {
    base = base.substr(0, base.size() - kSuffix.size());
  }
  std::string safe;
  for (const char c : objectName) {
    const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '-' || c == '_';
    safe += plain ? c : '_';
  }
  if (safe.empty()) safe = "object";
  return base + "_" + safe + ".wav";
}

std::string writeSceneHandoff(const std::string& exportFilePath,
                              const std::string& sessionName, int sampleRate,
                              float targetLkfs) {
  const std::vector<ObjectReceiver::LiveObject> live =
      sharedObjectReceiver().liveSnapshot();
  if (live.empty()) return {};

  std::vector<SessionObject> objects;
  objects.reserve(live.size());
  for (size_t i = 0; i < live.size(); ++i) {
    SessionObject o;
    o.name = live[i].name.empty() ? ("object " + std::to_string(i + 1))
                                  : live[i].name;
    o.keyframes.push_back({0.0, live[i].az_deg, live[i].el_deg, live[i].spread,
                           live[i].gain_db});
    objects.push_back(std::move(o));
  }

  const std::string path = sessionPathFor(exportFilePath);
  if (!writeSessionFile(path, buildSessionJson(sessionName, sampleRate,
                                               targetLkfs, "en", objects))) {
    return {};
  }
  sharedStudioLink().sendHandoff(path);
  return path;
}

std::string sessionPathFor(const std::string& exportFilePath) {
  const std::string kSuffix = ".iamf";
  if (exportFilePath.size() >= kSuffix.size() &&
      exportFilePath.compare(exportFilePath.size() - kSuffix.size(),
                             kSuffix.size(), kSuffix) == 0) {
    return exportFilePath.substr(0, exportFilePath.size() - kSuffix.size()) +
           ".fstudio";
  }
  return exportFilePath + ".fstudio";
}

}  // namespace friday
