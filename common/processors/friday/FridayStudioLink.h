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

// FRIDAY Bridge ⇄ Studio live link (V2-02, B4) — the Bridge side.
//
// Streams the DAW's object positions to a running FRIDAY Studio so the mix
// appears live on its PannerScope. The wire contract is owned by Studio and
// documented in friday-studio/studio/bridge_link.py: NDJSON over TCP to
// 127.0.0.1:47800, one-way Bridge -> Studio, protocol version 1.
//
//     hello   {product, version, session}   on every (re)connect
//     scene   {objects: [{name, azimuth, elevation, spread, gain_db}]}
//     pan     {index, name?, azimuth?, elevation?, spread?, gain_db?}
//     handoff {path}                        a .fstudio the Bridge just wrote
//     bye                                   on shutdown
//
// Transport and UI ONLY — no DSP, per V2-01. Positions come from the same
// captured-position source the KALA export uses (friday::sharedObjectReceiver),
// which is fed off the SPSC ring by the object bus consumer, so nothing here
// runs on or blocks the audio thread. If Studio is not running the link idles
// silently and retries with backoff; nothing in the DAW notices.

#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "FridayObjectTransport.h"

namespace friday {

/// Studio's listener, per bridge_link.py.
static constexpr int kStudioLinkPort = 47800;
static constexpr int kStudioLinkProtocolVersion = 1;

class StudioLink {
 public:
  StudioLink();
  ~StudioLink();

  /// Idempotent. Spawns the sender thread; safe to call when Studio is absent.
  void start();
  void stop();

  /// DAW project name, sent in `hello`. Applied on the next connect.
  void setSessionName(const std::string& name);

  /// Queue a `handoff` for the .fstudio just written. Delivered on the sender
  /// thread; dropped if no Studio ever connects, which is the intended
  /// behaviour — a handoff is a notification, not a guarantee.
  void sendHandoff(const std::string& path);

  bool isConnected() const noexcept {
    return connected_.load(std::memory_order_relaxed);
  }
  uint64_t pansSent() const noexcept {
    return pansSent_.load(std::memory_order_relaxed);
  }

  /// Exposed for tests: the exact NDJSON line for each message type.
  static std::string helloLine(const std::string& session);
  static std::string sceneLine(
      const std::vector<ObjectReceiver::LiveObject>& objects);
  static std::string panLine(int index,
                             const ObjectReceiver::LiveObject& object);
  static std::string handoffLine(const std::string& path);
  static std::string jsonEscape(const std::string& s);

 private:
  void run();
  bool connect();
  bool send(const std::string& line);

  std::atomic<bool> running_{false};
  std::atomic<bool> connected_{false};
  std::atomic<uint64_t> pansSent_{0};
  std::thread worker_;

  std::mutex mutex_;
  std::string sessionName_{"REAPER"};
  std::vector<std::string> pendingHandoffs_;

  struct Impl;
  std::unique_ptr<Impl> impl_;  // hides juce::StreamingSocket
};

/// One link per process, alongside sharedObjectReceiver().
StudioLink& sharedStudioLink();

}  // namespace friday
