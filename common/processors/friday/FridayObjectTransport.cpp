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

#include "FridayObjectTransport.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>

#include "zmq.hpp"

namespace friday {

namespace {
constexpr size_t kRingBytes = 8u << 20;   // 8 MiB — ~45 s of 48k mono
constexpr size_t kMaxBlockFrames = 65536; // sanity bound on a wire block
const char* kEndpoint = "tcp://localhost:5556";
/// PR-1 is 48 kHz only (KalaIamfWriter::open rejects anything else), so
/// keyframe times can be derived from the captured sample count.
constexpr double kCaptureSampleRate = 48000.0;
}  // namespace

//======================================================================
// Publisher
//======================================================================
struct ObjectPublisher::Impl {
  zmq::context_t context{1};
  zmq::socket_t socket;
};

ObjectPublisher::ObjectPublisher()
    : ring_(kRingBytes),
      scratch_(sizeof(ObjectBlockHeader) + kMaxBlockFrames * sizeof(float)),
      running_(true),
      dropped_(0),
      seq_(0),
      impl_(std::make_unique<Impl>()) {
  impl_->socket = zmq::socket_t(impl_->context, ZMQ_PUB);
  // Mirrors Eclipsa's metadata bus: the consumer binds, producers connect.
  impl_->socket.connect(kEndpoint);
  worker_ = std::thread([this] { workerLoop(); });
}

ObjectPublisher::~ObjectPublisher() {
  running_.store(false, std::memory_order_release);
  if (worker_.joinable()) worker_.join();
  impl_->socket.close();
}

bool ObjectPublisher::publish(const uint8_t uuid[16], float az_deg,
                              float el_deg, float spread, float gain_db,
                              const float* mono, uint32_t frames,
                              uint32_t flags, const char* name) noexcept {
  // AUDIO THREAD. No allocation, no lock, no syscall — just two memcpys into
  // the preallocated ring.
  if (frames > kMaxBlockFrames) return false;
  if (frames > 0 && mono == nullptr) return false;

  ObjectBlockHeader h{};
  std::memcpy(h.magic, "FROB", 4);
  h.version = kObjectWireVersion;
  std::memcpy(h.uuid, uuid, 16);
  h.az_deg = az_deg;
  h.el_deg = el_deg;
  h.spread = spread;
  h.gain_db = gain_db;
  h.frames = frames;
  h.seq = seq_.fetch_add(1, std::memory_order_relaxed);
  h.flags = flags;
  if (name != nullptr) {
    // strncpy without the library call's warning noise; h is already zeroed so
    // the result is always NUL-terminated.
    for (size_t i = 0; i + 1 < sizeof(h.name) && name[i] != '\0'; ++i) {
      h.name[i] = name[i];
    }
  }

  const size_t payload = static_cast<size_t>(frames) * sizeof(float);
  if (ring_.available() + sizeof(h) + payload > kRingBytes) {
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (!ring_.push(&h, sizeof(h))) {
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (payload > 0 && !ring_.push(mono, payload)) {
    // Header already committed; the reader will see a short block and resync
    // on the next magic. Counted so it is visible rather than silent.
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  return true;
}

void ObjectPublisher::workerLoop() {
  ObjectBlockHeader h{};
  // The post-shutdown drain is BOUNDED. The audio thread can still be pushing
  // while the destructor runs, so "drain until empty" can spin forever and the
  // join in ~ObjectPublisher never returns — a hang, not a leak.
  int drainBudget = 4096;
  while (running_.load(std::memory_order_acquire) ||
         (ring_.available() > 0 && drainBudget-- > 0)) {
    if (!ring_.pop(&h, sizeof(h))) {
      if (!running_.load(std::memory_order_acquire)) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }
    if (std::memcmp(h.magic, "FROB", 4) != 0 || h.frames > kMaxBlockFrames) {
      continue;  // desync — drop and look for the next header
    }
    const size_t payload = static_cast<size_t>(h.frames) * sizeof(float);
    if (scratch_.size() < sizeof(h) + payload) {
      scratch_.resize(sizeof(h) + payload);
    }
    std::memcpy(scratch_.data(), &h, sizeof(h));
    // Spin briefly for the payload the audio thread is still writing.
    int spins = 0;
    while (!ring_.pop(scratch_.data() + sizeof(h), payload) && spins++ < 1000) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    if (spins >= 1000) continue;

    zmq::message_t msg(sizeof(h) + payload);
    std::memcpy(msg.data(), scratch_.data(), sizeof(h) + payload);
    try {
      impl_->socket.send(msg, zmq::send_flags::dontwait);
    } catch (const zmq::error_t&) {
      // A dropped publish is not worth killing the worker over.
    }
  }
}

//======================================================================
// Receiver
//======================================================================
struct ObjectReceiver::Impl {
  zmq::context_t context{1};
  zmq::socket_t socket;
  std::mutex mutex;
  // insertion-ordered so the render is deterministic
  std::vector<Object> objects;
  std::unordered_map<std::string, size_t> index;
  // Live positions, independent of any export and not cleared by reset().
  std::vector<LiveObject> live;
  std::unordered_map<std::string, size_t> liveIndex;
  uint32_t lastSeq = 0;
  bool haveSeq = false;
};

ObjectReceiver::ObjectReceiver()
    : running_(false),
      gaps_(0),
      blocks_(0),
      liveRevision_(0),
      impl_(std::make_unique<Impl>()) {}

ObjectReceiver::~ObjectReceiver() { stop(); }

void ObjectReceiver::start() {
  if (running_.load(std::memory_order_acquire)) return;
  impl_->socket = zmq::socket_t(impl_->context, ZMQ_SUB);
  impl_->socket.set(zmq::sockopt::subscribe, "");
  impl_->socket.set(zmq::sockopt::rcvtimeo, 100);
  try {
    impl_->socket.bind(kEndpoint);
  } catch (const zmq::error_t&) {
    // Another instance already owns the port — the first one wins and
    // collects; a second exporter in the same session is not a supported
    // configuration.
    return;
  }
  reset();
  running_.store(true, std::memory_order_release);
  worker_ = std::thread([this] { workerLoop(); });
}

void ObjectReceiver::reset() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->objects.clear();
  impl_->index.clear();
  impl_->haveSeq = false;
  gaps_.store(0, std::memory_order_relaxed);
  blocks_.store(0, std::memory_order_relaxed);
}

void ObjectReceiver::stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) return;
  if (worker_.joinable()) worker_.join();
  impl_->socket.close();
}

void ObjectReceiver::drain(int quietMs, int maxMs) {
  // PUB/SUB is asynchronous and an offline bounce runs far faster than real
  // time, so blocks are still in flight when the exporter closes. Wait for the
  // stream to go quiet rather than for a fixed interval — a fixed sleep is
  // either a truncated capture or wasted wall clock, depending on how fast the
  // host bounced.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(maxMs);
  uint64_t seen = blocks_.load(std::memory_order_relaxed);
  auto lastChange = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const uint64_t now = blocks_.load(std::memory_order_relaxed);
    if (now != seen) {
      seen = now;
      lastChange = std::chrono::steady_clock::now();
      continue;
    }
    if (std::chrono::steady_clock::now() - lastChange >=
        std::chrono::milliseconds(quietMs)) {
      return;
    }
  }
}

void ObjectReceiver::workerLoop() {
  while (running_.load(std::memory_order_acquire)) {
    zmq::message_t msg;
    zmq::recv_result_t res;
    try {
      res = impl_->socket.recv(msg, zmq::recv_flags::none);
    } catch (const zmq::error_t&) {
      continue;
    }
    if (!res.has_value()) continue;  // timeout
    if (msg.size() < sizeof(ObjectBlockHeader)) continue;

    ObjectBlockHeader h{};
    std::memcpy(&h, msg.data(), sizeof(h));
    if (std::memcmp(h.magic, "FROB", 4) != 0 ||
        h.version != kObjectWireVersion) {
      continue;
    }
    const size_t payload = static_cast<size_t>(h.frames) * sizeof(float);
    if (msg.size() != sizeof(h) + payload) continue;

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->haveSeq && h.seq != impl_->lastSeq + 1) {
      gaps_.fetch_add(1, std::memory_order_relaxed);
    }
    impl_->lastSeq = h.seq;
    impl_->haveSeq = true;

    const std::string key(reinterpret_cast<const char*>(h.uuid), 16);
    const std::string name(h.name,
                           strnlen(h.name, sizeof(h.name)));

    // -- live position, from every block regardless of flags ---------------
    {
      auto lit = impl_->liveIndex.find(key);
      if (lit == impl_->liveIndex.end()) {
        LiveObject lo;
        std::memcpy(lo.uuid.data(), h.uuid, 16);
        impl_->liveIndex[key] = impl_->live.size();
        impl_->live.push_back(std::move(lo));
        lit = impl_->liveIndex.find(key);
      }
      LiveObject& live = impl_->live[lit->second];
      const bool changed =
          live.az_deg != h.az_deg || live.el_deg != h.el_deg ||
          live.spread != h.spread || live.gain_db != h.gain_db ||
          live.name != name;
      live.az_deg = h.az_deg;
      live.el_deg = h.el_deg;
      live.spread = h.spread;
      live.gain_db = h.gain_db;
      if (!name.empty()) live.name = name;
      if (changed) liveRevision_.fetch_add(1, std::memory_order_relaxed);
    }

    // -- export accumulation, OFFLINE blocks only --------------------------
    if ((h.flags & kFlagOffline) == 0 || h.frames == 0) continue;

    auto it = impl_->index.find(key);
    if (it == impl_->index.end()) {
      Object o;
      std::memcpy(o.uuid.data(), h.uuid, 16);
      impl_->index[key] = impl_->objects.size();
      impl_->objects.push_back(std::move(o));
      it = impl_->index.find(key);
    }
    Object& obj = impl_->objects[it->second];
    if (!name.empty()) obj.name = name;
    obj.az_deg = h.az_deg;
    obj.el_deg = h.el_deg;
    obj.spread = h.spread;
    obj.gain_db = h.gain_db;

    // Automation, not one frozen point: a keyframe whenever the position
    // actually moves, timed from what has been captured so far. This is what
    // makes a .fstudio handoff carry the move the user performed (V2-02).
    const double t = static_cast<double>(obj.pcm.size()) / kCaptureSampleRate;
    if (obj.keyframes.empty()) {
      obj.keyframes.push_back({0.0, h.az_deg, h.el_deg, h.spread, h.gain_db});
    } else {
      const Keyframe& last = obj.keyframes.back();
      constexpr float kMoveEpsilonDeg = 0.05f;
      constexpr float kMoveEpsilon = 1e-4f;
      if (std::fabs(last.az_deg - h.az_deg) > kMoveEpsilonDeg ||
          std::fabs(last.el_deg - h.el_deg) > kMoveEpsilonDeg ||
          std::fabs(last.spread - h.spread) > kMoveEpsilon ||
          std::fabs(last.gain_db - h.gain_db) > kMoveEpsilon) {
        obj.keyframes.push_back({t, h.az_deg, h.el_deg, h.spread, h.gain_db});
      }
    }

    const auto* samples =
        reinterpret_cast<const float*>(static_cast<const uint8_t*>(msg.data()) +
                                       sizeof(h));
    obj.pcm.insert(obj.pcm.end(), samples, samples + h.frames);
    blocks_.fetch_add(1, std::memory_order_relaxed);
  }
}

std::vector<ObjectReceiver::LiveObject> ObjectReceiver::liveSnapshot() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->live;
}

std::vector<ObjectReceiver::Object> ObjectReceiver::take() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->objects;
}

size_t ObjectReceiver::objectCount() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->objects.size();
}

ObjectReceiver& sharedObjectReceiver() {
  static ObjectReceiver instance;
  return instance;
}

}  // namespace friday
