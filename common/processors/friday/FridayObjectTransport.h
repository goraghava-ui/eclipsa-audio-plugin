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

// FRIDAY Bridge — object-audio transport for the KALA export path (B2).
//
// Why this exists: Eclipsa's export stage only ever sees a rendered bed
// (IAMFFileWriter::encodeBuffer slices the buffer at
// audioElement.firstChannel + i). Gate GA needs Bridge and Studio to share one
// renderer, so KALA has to be handed OBJECTS — pre-pan mono audio plus a
// direction — and do the render itself. This carries them from the panner
// plugin to the exporter.
//
// Eclipsa's own chain is untouched; this is a parallel tap.
//
// Thread discipline (KALA CLAUDE.md §3): the audio thread only memcpys into a
// preallocated lock-free SPSC ring and bumps an atomic. A worker thread drains
// the ring and does the ZeroMQ send, so no allocation, lock or syscall happens
// in processBlock.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace friday {

/// Fixed wire header preceding each object block. POD, memcpy-able.
struct ObjectBlockHeader {
  char magic[4];        // "FROB"
  uint32_t version;     // 1
  uint8_t uuid[16];     // audio element id the object belongs to
  float az_deg;         // +left; M+030 is the L speaker (Studio's convention)
  float el_deg;
  float spread;         // 0..1
  float gain_db;
  uint32_t frames;      // mono samples following this header
  uint32_t seq;         // per-publisher sequence, for gap detection
};

static constexpr uint32_t kObjectWireVersion = 1;
/// Object transport lives on its own port; 5555 stays Eclipsa's metadata bus.
static constexpr int kObjectPort = 5556;

//======================================================================
// Lock-free SPSC byte ring — audio thread writes, worker thread reads.
//======================================================================
class SpscByteRing {
 public:
  explicit SpscByteRing(size_t capacity)
      : buf_(capacity), capacity_(capacity), head_(0), tail_(0) {}

  /// Audio-thread side. Returns false if the ring is full (the block is
  /// dropped rather than blocking — never stall the audio thread).
  bool push(const void* data, size_t bytes) noexcept {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_acquire);
    const size_t used = head - tail;
    if (bytes > capacity_ - used) return false;
    const auto* src = static_cast<const uint8_t*>(data);
    size_t offset = head % capacity_;
    const size_t first = std::min(bytes, capacity_ - offset);
    std::memcpy(buf_.data() + offset, src, first);
    if (bytes > first) std::memcpy(buf_.data(), src + first, bytes - first);
    head_.store(head + bytes, std::memory_order_release);
    return true;
  }

  /// Worker-thread side.
  bool pop(void* out, size_t bytes) noexcept {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t head = head_.load(std::memory_order_acquire);
    if (head - tail < bytes) return false;
    auto* dst = static_cast<uint8_t*>(out);
    size_t offset = tail % capacity_;
    const size_t first = std::min(bytes, capacity_ - offset);
    std::memcpy(dst, buf_.data() + offset, first);
    if (bytes > first) std::memcpy(dst + first, buf_.data(), bytes - first);
    tail_.store(tail + bytes, std::memory_order_release);
    return true;
  }

  size_t available() const noexcept {
    return head_.load(std::memory_order_acquire) -
           tail_.load(std::memory_order_acquire);
  }

 private:
  std::vector<uint8_t> buf_;
  size_t capacity_;
  std::atomic<size_t> head_;
  std::atomic<size_t> tail_;
};

//======================================================================
// Publisher — panner side.
//======================================================================
class ObjectPublisher {
 public:
  ObjectPublisher();
  ~ObjectPublisher();

  /// Audio-thread safe. `mono` is the object's pre-pan signal.
  /// Returns false if the ring was full (block dropped, counted).
  bool publish(const uint8_t uuid[16], float az_deg, float el_deg, float spread,
               float gain_db, const float* mono, uint32_t frames) noexcept;

  uint64_t dropped() const noexcept {
    return dropped_.load(std::memory_order_relaxed);
  }

 private:
  void workerLoop();

  SpscByteRing ring_;
  std::vector<uint8_t> scratch_;  // worker-owned staging buffer
  std::atomic<bool> running_;
  std::atomic<uint64_t> dropped_;
  std::atomic<uint32_t> seq_;
  std::thread worker_;
  struct Impl;
  std::unique_ptr<Impl> impl_;  // hides zmq from callers
};

//======================================================================
// Receiver — exporter side. Accumulates whole objects for the render.
//======================================================================
class ObjectReceiver {
 public:
  struct Object {
    std::array<uint8_t, 16> uuid{};
    float az_deg = 0.0f;
    float el_deg = 0.0f;
    float spread = 0.0f;
    float gain_db = 0.0f;
    std::vector<float> pcm;
  };

  ObjectReceiver();
  ~ObjectReceiver();

  void start();
  void stop();
  /// Block briefly so in-flight blocks land before the render is built.
  void drain(int milliseconds);

  /// Snapshot of everything captured since start().
  std::vector<Object> take();
  size_t objectCount();
  uint32_t gaps() const noexcept { return gaps_.load(std::memory_order_relaxed); }

 private:
  void workerLoop();

  std::atomic<bool> running_;
  std::atomic<uint32_t> gaps_;
  std::thread worker_;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace friday
