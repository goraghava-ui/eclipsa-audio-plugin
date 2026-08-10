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

#include <cstdio>
#include <vector>

#include "logger/logger.h"

extern "C" {
#include "kala_cabi.h"
}

KalaIamfWriter::KalaIamfWriter(FileExportRepository& fileExportRepository,
                               int sampleRate)
    : fileExportRepository_(fileExportRepository), sampleRate_(sampleRate) {}

KalaIamfWriter::~KalaIamfWriter() { receiver_.stop(); }

bool KalaIamfWriter::open(const std::string& filename) {
  if (sampleRate_ != 48000) {
    LOG_ERROR(0, "KALA export is 48 kHz only (PR-1), got " +
                     std::to_string(sampleRate_));
    return false;
  }
  filename_ = filename;
  receiver_.start();
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

bool KalaIamfWriter::close() {
  if (!open_) return false;
  open_ = false;

  // An offline bounce runs far faster than real time, so blocks are still in
  // flight. Let them land before the render is built.
  receiver_.drain(250);
  receiver_.stop();

  std::vector<friday::ObjectReceiver::Object> objects = receiver_.take();
  objectsRendered_ = objects.size();
  if (objects.empty()) {
    LOG_ERROR(0,
              "KALA export: no objects captured — is the Audio Element Plugin "
              "assigned to an Audio Element?");
    return false;
  }
  if (receiver_.gaps() > 0) {
    LOG_WARNING(0, "KALA export: " + std::to_string(receiver_.gaps()) +
                       " sequence gaps in the object stream");
  }

  KalaSession* session = kala_session_new(48000, "7.1.4");
  if (session == nullptr) {
    LOG_ERROR(0, std::string("kala_session_new failed: ") + kala_last_error());
    return false;
  }

  for (const auto& o : objects) {
    if (o.pcm.empty()) continue;
    const int32_t rc = kala_session_add_object(
        session, o.pcm.data(), o.pcm.size(), o.az_deg, o.el_deg, o.spread,
        o.gain_db);
    if (rc != KALA_OK) {
      LOG_ERROR(0, std::string("kala_session_add_object failed: ") +
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
