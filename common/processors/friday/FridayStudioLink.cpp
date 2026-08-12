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

#include "FridayStudioLink.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace friday {

namespace {

/// 30 Hz. The whole gate budget is this throttle: local TCP is sub-millisecond,
/// so worst-case send latency is one tick (~33 ms) against a 100 ms gate.
constexpr int kTickMs = 33;
constexpr int kConnectTimeoutMs = 200;
constexpr int kBackoffMinMs = 250;
constexpr int kBackoffMaxMs = 4000;

/// Below this a move is not worth a packet — it is far under the scope's
/// pixel resolution and under the panner's own parameter quantisation.
constexpr float kPanEpsilonDeg = 0.01f;
constexpr float kPanEpsilon = 1e-4f;

std::string num(float v) {
  // %.4f rather than the default float formatting: always valid JSON (no
  // "inf"/"nan" spelling, no exponent surprises) and plenty for degrees.
  if (!std::isfinite(v)) v = 0.0f;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.4f", static_cast<double>(v));
  return buf;
}

bool samePosition(const ObjectReceiver::LiveObject& a,
                  const ObjectReceiver::LiveObject& b) {
  return std::fabs(a.az_deg - b.az_deg) < kPanEpsilonDeg &&
         std::fabs(a.el_deg - b.el_deg) < kPanEpsilonDeg &&
         std::fabs(a.spread - b.spread) < kPanEpsilon &&
         std::fabs(a.gain_db - b.gain_db) < kPanEpsilon;
}

/// The object SET changed (count, identity or naming) rather than just a
/// position — Studio replaces its whole remote list on `scene`.
bool sceneChanged(const std::vector<ObjectReceiver::LiveObject>& a,
                  const std::vector<ObjectReceiver::LiveObject>& b) {
  if (a.size() != b.size()) return true;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].uuid != b[i].uuid || a[i].name != b[i].name) return true;
  }
  return false;
}

}  // namespace

struct StudioLink::Impl {
  juce::StreamingSocket socket;
};

StudioLink::StudioLink() : impl_(std::make_unique<Impl>()) {}

StudioLink::~StudioLink() { stop(); }

void StudioLink::start() {
  if (running_.exchange(true, std::memory_order_acq_rel)) return;
  worker_ = std::thread([this] { run(); });
}

void StudioLink::stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) return;
  if (worker_.joinable()) worker_.join();
}

void StudioLink::setSessionName(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  sessionName_ = name;
}

void StudioLink::sendHandoff(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);
  pendingHandoffs_.push_back(path);
}

//======================================================================
// Message construction — pure, so the tests can assert the exact bytes
//======================================================================

std::string StudioLink::jsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c & 0xff);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

std::string StudioLink::helloLine(const std::string& session) {
  return "{\"type\":\"hello\",\"product\":\"friday-bridge\",\"version\":" +
         std::to_string(kStudioLinkProtocolVersion) + ",\"session\":\"" +
         jsonEscape(session) + "\"}\n";
}

std::string StudioLink::sceneLine(
    const std::vector<ObjectReceiver::LiveObject>& objects) {
  std::string out = "{\"type\":\"scene\",\"objects\":[";
  for (size_t i = 0; i < objects.size(); ++i) {
    const ObjectReceiver::LiveObject& o = objects[i];
    if (i > 0) out += ",";
    out += "{\"name\":\"" + jsonEscape(o.name) + "\",\"azimuth\":" +
           num(o.az_deg) + ",\"elevation\":" + num(o.el_deg) + ",\"spread\":" +
           num(o.spread) + ",\"gain_db\":" + num(o.gain_db) + "}";
  }
  out += "]}\n";
  return out;
}

std::string StudioLink::panLine(int index,
                                const ObjectReceiver::LiveObject& object) {
  return "{\"type\":\"pan\",\"index\":" + std::to_string(index) +
         ",\"name\":\"" + jsonEscape(object.name) +
         "\",\"azimuth\":" + num(object.az_deg) +
         ",\"elevation\":" + num(object.el_deg) +
         ",\"spread\":" + num(object.spread) +
         ",\"gain_db\":" + num(object.gain_db) + "}\n";
}

std::string StudioLink::handoffLine(const std::string& path) {
  return "{\"type\":\"handoff\",\"path\":\"" + jsonEscape(path) + "\"}\n";
}

//======================================================================
// Sender thread
//======================================================================

bool StudioLink::connect() {
  return impl_->socket.connect("127.0.0.1", kStudioLinkPort,
                               kConnectTimeoutMs);
}

bool StudioLink::send(const std::string& line) {
  if (!impl_->socket.isConnected()) return false;
  const int n = impl_->socket.write(line.data(), static_cast<int>(line.size()));
  return n == static_cast<int>(line.size());
}

void StudioLink::run() {
  std::vector<ObjectReceiver::LiveObject> sent;
  int backoffMs = kBackoffMinMs;
  auto nextAttempt = std::chrono::steady_clock::now();
  bool needScene = false;

  while (running_.load(std::memory_order_acquire)) {
    const auto tickStart = std::chrono::steady_clock::now();

    if (!impl_->socket.isConnected()) {
      connected_.store(false, std::memory_order_relaxed);
      if (tickStart >= nextAttempt) {
        if (connect()) {
          backoffMs = kBackoffMinMs;
          std::string session;
          {
            std::lock_guard<std::mutex> lock(mutex_);
            session = sessionName_;
          }
          // A fresh Studio knows nothing about us: identify, then hand it the
          // whole scene before any incremental pan can reference an index.
          if (send(helloLine(session))) {
            connected_.store(true, std::memory_order_relaxed);
            sent.clear();
            needScene = true;
          } else {
            impl_->socket.close();
          }
        } else {
          // Studio is not running. This is the normal case, not an error.
          nextAttempt = tickStart + std::chrono::milliseconds(backoffMs);
          backoffMs = std::min(backoffMs * 2, kBackoffMaxMs);
        }
      }
    }

    if (connected_.load(std::memory_order_relaxed)) {
      std::vector<ObjectReceiver::LiveObject> live =
          sharedObjectReceiver().liveSnapshot();

      bool ok = true;
      if (needScene || sceneChanged(sent, live)) {
        ok = send(sceneLine(live));
        sent = live;
        needScene = false;
      } else {
        for (size_t i = 0; i < live.size() && ok; ++i) {
          if (!samePosition(sent[i], live[i])) {
            ok = send(panLine(static_cast<int>(i), live[i]));
            sent[i] = live[i];
            pansSent_.fetch_add(1, std::memory_order_relaxed);
          }
        }
      }

      std::vector<std::string> handoffs;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        handoffs.swap(pendingHandoffs_);
      }
      for (const std::string& path : handoffs) {
        if (!ok) break;
        ok = send(handoffLine(path));
      }

      if (!ok) {
        impl_->socket.close();
        connected_.store(false, std::memory_order_relaxed);
        nextAttempt = std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(kBackoffMinMs);
      }
    }

    // Sleep the remainder of the tick rather than a flat interval, so a slow
    // send does not push the effective rate below 30 Hz.
    const auto elapsed = std::chrono::steady_clock::now() - tickStart;
    const auto budget = std::chrono::milliseconds(kTickMs);
    if (elapsed < budget) std::this_thread::sleep_for(budget - elapsed);
  }

  if (impl_->socket.isConnected()) {
    send("{\"type\":\"bye\"}\n");
    impl_->socket.close();
  }
  connected_.store(false, std::memory_order_relaxed);
}

StudioLink& sharedStudioLink() {
  static StudioLink instance;
  return instance;
}

}  // namespace friday
