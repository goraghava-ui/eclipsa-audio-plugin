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

// FRIDAY Bridge -> Studio live AUDIO feed (V2-03), the Bridge side.
//
// The renderer taps its rendered bed -- the same samples the monitoring path
// hands back to the DAW -- and streams it to Studio, so Studio can monitor the
// DAW mix through its own audition and binaural folds. The wire contract is
// owned by Studio and documented in friday-studio/studio/bridge_audio.py:
//
//     TCP 127.0.0.1:47801, one-way Bridge -> Studio, one client at a time.
//     On connect, ONE NDJSON line:
//       {"type":"audio_hello","version":1,"sample_rate":48000,
//        "channels":12,"block":512}
//     Then raw frames, each exactly:
//       4 bytes  b"FBAF"
//       4 bytes  u32 LE sequence, +1 per frame
//       block * channels * 4 bytes  float32 LE, FRAME-major
//                                   (sample s of channel c at [s*ch + c])
//
// TRANSPORT ONLY -- no DSP, per V2-01. This copies and interleaves bytes that
// have already been rendered; it never touches a gain, a position or a fold.
//
// Real-time rules, same as the object bus: the audio thread only interleaves
// into a preallocated scratch and pushes into a preallocated lock-free SPSC
// ring. It never allocates, never locks and never waits on the socket. If the
// receiver stalls, the ring fills and blocks are DROPPED (and counted) rather
// than the audio thread being made to wait. If Studio is not running the feed
// idles silently and retries with backoff; nothing in the DAW notices.
//
// `block` is fixed for the whole stream by the hello, so the ring doubles as a
// repacketiser: the host may hand us any buffer size, and the worker emits
// exactly kAudioFeedBlock frames from whatever has accumulated.

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace friday {

/// Studio's audio listener, per bridge_audio.py. The B4 position link keeps
/// 47800 to itself -- NDJSON and binary do not share a socket.
static constexpr int kAudioFeedPort = 47801;
static constexpr int kAudioFeedProtocolVersion = 1;

/// Frames per wire frame. bridge_audio.py rings 8 of these, so at 48 kHz this
/// is ~85 ms of jitter buffer on Studio's side.
static constexpr int kAudioFeedBlock = 512;

/// Widest bed we will carry. The renderer's processing buffer is host-wide
/// (28 ch on this build); the feed only ever sends the channels the monitoring
/// path actually hands back, but the scratch has to cover the widest case.
static constexpr int kAudioFeedMaxChannels = 32;

/// Longest host buffer we will interleave in one go. Offline renders can hand
/// out large blocks; anything past this is dropped and counted rather than
/// allocated for on the audio thread.
static constexpr int kAudioFeedMaxHostBlock = 8192;

/// Ring capacity. ~0.36 s of 12-channel 48 kHz audio -- deep enough to ride
/// out scheduler jitter on the worker, shallow enough that a real stall shows
/// up as dropped blocks quickly rather than as unbounded monitor latency.
static constexpr size_t kAudioFeedRingBytes = 1u << 20;

class AudioFeed {
 public:
  AudioFeed();
  ~AudioFeed();

  /// Idempotent. Spawns the sender thread; safe to call when Studio is absent.
  void start();
  void stop();

  /// Message thread (prepareToPlay). Advertised in the next hello.
  void setSampleRate(double sampleRate);

  /// AUDIO THREAD ONLY.
  ///
  /// Interleaves `channels` x `numSamples` out of `buffer` and enqueues them.
  /// Returns without doing anything if no Studio is connected. Never
  /// allocates, never blocks; a full ring means this block is dropped and
  /// `blocksDropped()` goes up.
  void pushBlock(const juce::AudioBuffer<float>& buffer, int channels,
                 int numSamples) noexcept;

  bool isConnected() const noexcept {
    return connected_.load(std::memory_order_relaxed);
  }
  uint64_t framesSent() const noexcept {
    return framesSent_.load(std::memory_order_relaxed);
  }
  /// Host blocks the audio thread had to throw away: the ring was full (the
  /// receiver is not keeping up) or the block was wider/longer than the
  /// scratch. Either way the audio thread did not wait.
  uint64_t blocksDropped() const noexcept {
    return blocksDropped_.load(std::memory_order_relaxed);
  }

  /// Exposed so the tests can assert the exact bytes on the wire.
  static std::string helloLine(int sampleRate, int channels, int block);
  /// The 8-byte frame header: b"FBAF" then the sequence, little-endian.
  static void writeFrameHeader(uint8_t* out, uint32_t sequence) noexcept;

 private:
  void run();
  bool connect();
  bool sendAll(const void* data, size_t bytes);
  void discardBacklog();

  std::atomic<bool> running_{false};
  std::atomic<bool> connected_{false};
  std::atomic<uint64_t> framesSent_{0};
  std::atomic<uint64_t> blocksDropped_{0};

  /// Set by the audio thread as it observes the bed, read by the worker. A
  /// change means the stream's hello is stale, so the worker reconnects.
  std::atomic<int> channels_{0};
  std::atomic<int> sampleRate_{48000};
  std::atomic<uint32_t> generation_{0};

  std::vector<float> scratch_;     // audio thread: interleave target
  std::vector<uint8_t> sendBuf_;   // worker: one frame, header included
  std::unique_ptr<class SpscByteRing> ring_;

  std::thread worker_;
  struct Impl;
  std::unique_ptr<Impl> impl_;  // hides juce::StreamingSocket
};

/// One feed per process, alongside sharedStudioLink().
AudioFeed& sharedAudioFeed();

}  // namespace friday
