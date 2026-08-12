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

// FRIDAY Bridge — the KALA export path (B2, gate GA).
//
// Drop-in alternative to IAMFFileWriter, selected by FRIDAY_KALA_EXPORT. The
// difference is what it encodes: IAMFFileWriter takes the bed Eclipsa has
// already rendered with libspatialaudio, whereas this takes the OBJECTS the
// capture tap collected and lets KALA render them, so Bridge and Studio share
// one renderer and the null can be exact.
//
// Deliberately not streaming: the objects are accumulated for the whole export
// and rendered in one shot at close(), which is exactly what kala-cabi's
// session API expects and what studio_cli does offline.
//
// The receiver is the process-wide one (friday::sharedObjectReceiver), brought
// up when the renderer plugin is prepared rather than when an export arms:
// ZeroMQ's PUB/SUB handshake takes ~100 ms and an offline bounce of a short
// project is over well inside that, so a receiver that binds at arm time
// silently loses the head of the capture.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <string>
#include <vector>

#include "../../friday/FridayObjectTransport.h"
#include "data_repository/implementation/FileExportRepository.h"

class KalaIamfWriter {
 public:
  /// FRIDAY Studio's music-delivery default (studio/session.py:136). The
  /// mastering gain has to match or the null sits exactly that many dB off —
  /// see BRIDGE-B2-PLAN.md §5.
  static constexpr float kDefaultTargetLkfs = -16.0f;

  KalaIamfWriter(FileExportRepository& fileExportRepository, int sampleRate);
  ~KalaIamfWriter();

  bool open(const std::string& filename);
  /// Audio arrives over the object transport, not through here. Kept so the
  /// exporter's per-block call site is identical for both writers.
  bool writeFrame(const juce::AudioBuffer<float>& buffer);
  bool close();

  float appliedGainDb() const { return appliedGainDb_; }
  size_t objectsRendered() const { return objectsRendered_; }

  /// Cuts the host's post-render silent flush off the capture, equally across
  /// every object so their relative timing is preserved. Public for the tests.
  static void trimTrailingSilence(
      std::vector<friday::ObjectReceiver::Object>& objects);

 private:
  FileExportRepository& fileExportRepository_;
  int sampleRate_;
  std::string filename_;
  bool open_ = false;
  float appliedGainDb_ = 0.0f;
  size_t objectsRendered_ = 0;
};
