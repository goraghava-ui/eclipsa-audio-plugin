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

// FRIDAY Bridge — the capture tap (B2, Design B).
//
// Sits in the panner plugin's chain immediately BEFORE Panner3DProcessor, so
// what it sees is the object's pre-pan mono signal. It copies that plus the
// current direction into the object transport and leaves the buffer completely
// alone — Eclipsa's own render continues downstream untouched, which is what
// keeps the monitoring path and its UI working.
//
// Compiled in only when FRIDAY_KALA_EXPORT is on.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <atomic>
#include <cmath>

#include "../processor_base/ProcessorBase.h"
#include "data_structures/src/AudioElementParameterTree.h"
#include "FridayObjectTransport.h"
#include "data_repository/implementation/AudioElementSpatialLayoutRepository.h"

class FridayObjectCaptureProcessor final : public ProcessorBase {
 public:
  FridayObjectCaptureProcessor(
      AudioElementSpatialLayoutRepository* spatialLayoutRepository,
      AudioElementParameterTree* automationParameterTree)
      : spatialLayoutRepository_(spatialLayoutRepository),
        automationParameterTree_(automationParameterTree) {}

  ~FridayObjectCaptureProcessor() override = default;

  const juce::String getName() const override {
    return "FRIDAY Object Capture";
  }

  /// EXPORT capture is bounded by the host's offline bounce. Without this the
  /// tail of the stream keeps growing once the host returns to realtime and the
  /// exporter is still draining, so the encoded file ends up longer than the
  /// render — the object stream has to cover exactly the bounce and no more.
  /// Realtime blocks still publish, but as position-only pings that carry no
  /// kFlagOffline and so can never reach the deliverable.
  void setNonRealtime(bool isNonRealtime) noexcept override {
    offline_.store(isNonRealtime, std::memory_order_release);
  }

  void processBlock(juce::AudioBuffer<float>& buffer,
                    juce::MidiBuffer&) override {
    // AUDIO THREAD — publish() only memcpys into a preallocated ring.
    if (buffer.getNumChannels() < 1 || buffer.getNumSamples() < 1) return;
    if (spatialLayoutRepository_ == nullptr ||
        automationParameterTree_ == nullptr) {
      return;
    }

    const AudioElementSpatialLayout layout = spatialLayoutRepository_->get();
    if (layout.getFirstChannel() < 0) {
      return;  // panner not assigned to an Audio Element yet — nothing to send
    }
    if (!automationParameterTree_->getUnmute()) return;

    // Snapshot the name into a plain buffer: the wire header is POD and the
    // audio thread must not touch juce::String's ref-counted storage.
    updateNameCache(layout.getName());

    const float x = static_cast<float>(automationParameterTree_->getXPosition());
    const float y = static_cast<float>(automationParameterTree_->getYPosition());
    const float z = static_cast<float>(automationParameterTree_->getZPosition());

    float az = 0.0f, el = 0.0f;
    cartesianToPolarDegrees(x, y, z, az, el);

    juce::Uuid id = layout.getAudioElementId();
    uint8_t uuid[16];
    std::memcpy(uuid, id.getRawData(), 16);

    const float gain = automationParameterTree_->getVolume();

    if (offline_.load(std::memory_order_acquire)) {
      publisher_.publish(uuid, az, el, /*spread=*/0.0f, gain,
                         buffer.getReadPointer(0),
                         static_cast<uint32_t>(buffer.getNumSamples()),
                         friday::kFlagOffline, name_);
      return;
    }

    // Realtime: a position-only ping for the live Studio link (V2-02). No PCM,
    // so this costs a fixed ~72 bytes into the ring and nothing else; the
    // export never sees these blocks (they carry no kFlagOffline).
    if (++liveTick_ < liveInterval_) return;
    liveTick_ = 0;
    publisher_.publish(uuid, az, el, /*spread=*/0.0f, gain, nullptr, 0,
                       /*flags=*/0, name_);
  }

  void prepareToPlay(double sampleRate, int samplesPerBlock) override {
    // Ping at ~kLiveHz so the link always has a fresh position to send at its
    // own 30 Hz throttle, without putting a block-rate stream on the bus.
    constexpr double kLiveHz = 60.0;
    if (sampleRate > 0.0 && samplesPerBlock > 0) {
      const double blocksPerSecond = sampleRate / samplesPerBlock;
      liveInterval_ =
          std::max(1, static_cast<int>(std::lround(blocksPerSecond / kLiveHz)));
    } else {
      liveInterval_ = 1;
    }
    liveTick_ = liveInterval_;  // publish on the very first block
  }

  uint64_t droppedBlocks() const noexcept { return publisher_.dropped(); }

  /// Same convention as AudioPanner::convertCartToPolar (ITU-R BS.2051-3,
  /// magnitude 50), so a Bridge object ends up where Eclipsa's own monitoring
  /// puts it — and, because Studio uses the same "+left, M+030 = L" reading,
  /// where Studio puts it too.
  static void cartesianToPolarDegrees(float x, float y, float z, float& azDeg,
                                      float& elDeg) {
    constexpr float kMagnitude = 50.0f;
    const float nx = x / kMagnitude;
    const float ny = y / kMagnitude;
    const float nz = z / kMagnitude;
    const float radius = std::sqrt(nx * nx + ny * ny);
    constexpr float kEpsilon = 1e-6f;
    if (radius < kEpsilon && std::abs(nz) < kEpsilon) {
      azDeg = 0.0f;
      elDeg = 0.0f;
      return;
    }
    if (radius < kEpsilon) {
      azDeg = 0.0f;
      elDeg = nz > 0.0f ? 90.0f : -90.0f;
      return;
    }
    float az = -1.0f * std::atan2(nx, ny) * 180.0f / static_cast<float>(M_PI);
    if (az > 180.0f) {
      az -= 360.0f;
    } else if (az <= -180.0f) {
      az += 360.0f;
    }
    azDeg = az;
    elDeg = std::atan(nz / radius) * 180.0f / static_cast<float>(M_PI);
  }

 private:
  /// Copies at most 31 bytes out of the layout name into a POD buffer. The
  /// juce::String itself is never handed further down — the wire header is
  /// memcpy-able and crosses a thread boundary.
  void updateNameCache(const juce::String& name) noexcept {
    const char* utf8 = name.toRawUTF8();
    size_t i = 0;
    for (; i + 1 < sizeof(name_) && utf8[i] != '\0'; ++i) name_[i] = utf8[i];
    for (; i < sizeof(name_); ++i) name_[i] = '\0';
  }

  AudioElementSpatialLayoutRepository* spatialLayoutRepository_;
  AudioElementParameterTree* automationParameterTree_;
  std::atomic<bool> offline_{false};
  char name_[32] = {};
  int liveTick_ = 0;
  int liveInterval_ = 1;
  friday::ObjectPublisher publisher_;
};
