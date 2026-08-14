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

#include "KalaIamfWriter.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include "logger/logger.h"

extern "C" {
#include "kala_cabi.h"
}

KalaIamfWriter::KalaIamfWriter(FileExportRepository& fileExportRepository,
                               int sampleRate)
    : fileExportRepository_(fileExportRepository), sampleRate_(sampleRate) {}

KalaIamfWriter::~KalaIamfWriter() = default;

bool KalaIamfWriter::open(const std::string& filename) {
  if (sampleRate_ != 48000) {
    LOG_ERROR(0, "KALA export is 48 kHz only (PR-1), got " +
                     std::to_string(sampleRate_));
    return false;
  }
  filename_ = filename;
  // start() is idempotent; the link is normally already up from prepareToPlay.
  friday::sharedObjectReceiver().start();
  friday::sharedObjectReceiver().reset();
  open_ = true;
  LOG_ANALYTICS(0, "KALA export path armed: " + filename);
  return true;
}

bool KalaIamfWriter::writeFrame(const juce::AudioBuffer<float>&) {
  // Objects reach us over the transport; nothing to do per block. Returning
  // true keeps FileOutputProcessor's error accounting identical for both
  // writers.
  return open_;
}

void KalaIamfWriter::trimTrailingSilence(
    std::vector<friday::ObjectReceiver::Object>& objects) {
  // A host flushes extra blocks after the render bounds -- REAPER hands the
  // plugins ~0.28 s of digital silence past the end of a 2 s bounce and then
  // discards it from its own output file. Left in, that silence lengthens the
  // deliverable past the render AND shifts the BS.1770 integrated measurement
  // (the 400 ms blocks straddling the boundary land under the relative gate),
  // which showed up as a +0.175 dB mastering-gain error against Studio.
  //
  // The trim is COMMON across objects so their relative timing is untouched.
  //
  // Limitation: material that deliberately ends in silence is shortened by
  // that silence. The proper fix is to carry the host playhead position in the
  // wire header and cut at the render bounds -- BRIDGE-B2-PLAN.md §11.
  size_t keep = 0;
  for (const auto& o : objects) {
    size_t last = 0;
    for (size_t i = o.pcm.size(); i > 0; --i) {
      if (o.pcm[i - 1] != 0.0f) {
        last = i;
        break;
      }
    }
    keep = std::max(keep, last);
  }
  if (keep == 0) return;  // all silent -- leave it alone, let the caller judge
  for (auto& o : objects) {
    if (o.pcm.size() > keep) o.pcm.resize(keep);
  }
}

bool KalaIamfWriter::close() {
  if (!open_) return false;
  open_ = false;

  // An offline bounce runs far faster than real time, so blocks are still in
  // flight. Let the stream go quiet before the render is built. The receiver
  // is NOT stopped — it stays bound for the next export.
  friday::ObjectReceiver& receiver = friday::sharedObjectReceiver();
  receiver.drain(/*quietMs=*/150, /*maxMs=*/5000);

  std::vector<friday::ObjectReceiver::Object> objects = receiver.take();
  objectsRendered_ = objects.size();
  if (objects.empty()) {
    LOG_ERROR(0,
              "KALA export: no objects captured — is the Audio Element Plugin "
              "assigned to an Audio Element?");
    return false;
  }
  if (receiver.gaps() > 0) {
    LOG_WARNING(0, "KALA export: " + std::to_string(receiver.gaps()) +
                       " sequence gaps in the object stream");
  }
  const size_t capturedFrames = objects[0].pcm.size();
  trimTrailingSilence(objects);
  LOG_ANALYTICS(0, "KALA export captured " +
                       std::to_string(receiver.blocksReceived()) +
                       " block(s), " + std::to_string(capturedFrames) +
                       " frames, trimmed to " +
                       std::to_string(objects[0].pcm.size()) + "; object[0] az=" +
                       std::to_string(objects[0].az_deg) + " el=" +
                       std::to_string(objects[0].el_deg) + " spread=" +
                       std::to_string(objects[0].spread) + " gain=" +
                       std::to_string(objects[0].gain_db));

  rendered_ = objects;  // for the .fstudio handoff, before the encode consumes
                        // anything

  KalaSession* session = kala_session_new(48000, "7.1.4");
  if (session == nullptr) {
    LOG_ERROR(0, std::string("kala_session_new failed: ") + kala_last_error());
    return false;
  }

  for (const auto& o : objects) {
    if (o.pcm.empty()) continue;

    // The AUTOMATED entry point, not the static one. The capture carries a
    // position per processBlock; feeding KALA only `o.az_deg` rendered the
    // whole take at one direction -- a 1 s sweep from -90 to +90 came out
    // parked at the last position, nulling at -14.00 dBFS against Studio
    // rendering the very same keyframes. See BRIDGE-B2-PLAN.md section 11.
    //
    // KALA does the ramping, mirroring studio/renderer.py: gains at each
    // 1024-frame block edge from the interpolated state, linear across the
    // block. The pan stays in KALA (V2-01); all that happens here is handing
    // over the timeline that was already captured.
    std::vector<KalaKeyframe> keys;
    keys.reserve(o.keyframes.size());
    for (const auto& k : o.keyframes) {
      keys.push_back(KalaKeyframe{k.t, k.az_deg, k.el_deg, k.spread,
                                  k.gain_db});
    }
    if (keys.empty()) {
      // The receiver guarantees at least one, but a static object must render
      // identically either way, so make that explicit rather than assumed.
      keys.push_back(KalaKeyframe{0.0, o.az_deg, o.el_deg, o.spread,
                                  o.gain_db});
    }

    const int32_t rc = kala_session_add_object_automated(
        session, o.pcm.data(), o.pcm.size(), keys.data(), keys.size());
    if (rc != KALA_OK) {
      LOG_ERROR(0, std::string("kala_session_add_object_automated failed: ") +
                       kala_last_error());
      kala_session_free(session);
      return false;
    }
  }

  if (kala_session_render(session) != KALA_OK) {
    LOG_ERROR(0, std::string("kala_session_render failed: ") +
                     kala_last_error());
    kala_session_free(session);
    return false;
  }

  float applied = 0.0f;
  if (kala_session_normalize(session, kDefaultTargetLkfs, &applied) !=
      KALA_OK) {
    LOG_ERROR(0, std::string("kala_session_normalize failed: ") +
                     kala_last_error());
    kala_session_free(session);
    return false;
  }
  appliedGainDb_ = applied;

  const FileExport config = fileExportRepository_.get();
  const uint8_t bitDepth = static_cast<uint8_t>(config.getBitDepth());

  uint8_t* seq = nullptr;
  size_t seqLen = 0;
  const int32_t rc =
      kala_session_encode(session, bitDepth == 0 ? 24 : bitDepth, 960, "en",
                          "FRIDAY Bridge mix", &seq, &seqLen);
  kala_session_free(session);
  if (rc != KALA_OK || seq == nullptr) {
    LOG_ERROR(0,
              std::string("kala_session_encode failed: ") + kala_last_error());
    return false;
  }

  std::FILE* f = std::fopen(filename_.c_str(), "wb");
  if (f == nullptr) {
    LOG_ERROR(0, "KALA export: cannot open " + filename_);
    kala_buffer_free(seq, seqLen);
    return false;
  }
  const size_t written = std::fwrite(seq, 1, seqLen, f);
  std::fclose(f);
  kala_buffer_free(seq, seqLen);

  if (written != seqLen) {
    LOG_ERROR(0, "KALA export: short write to " + filename_);
    return false;
  }

  LOG_ANALYTICS(0, "KALA export wrote " + std::to_string(seqLen) +
                       " bytes from " + std::to_string(objectsRendered_) +
                       " object(s), gain " + std::to_string(appliedGainDb_) +
                       " dB");
  return true;
}
