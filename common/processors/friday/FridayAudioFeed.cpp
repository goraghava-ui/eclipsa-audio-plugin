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

#include "FridayAudioFeed.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

// gpac/setup.h macro-renames snprintf on Windows; these files want the real
// C++ one. Same guard as FridayStudioSession.cpp.
#ifdef snprintf
#undef snprintf
#endif

#include "FridayObjectTransport.h"

namespace friday {

namespace {

constexpr int kFeedConnectTimeoutMs = 200;
constexpr int kFeedBackoffMinMs = 250;
constexpr int kFeedBackoffMaxMs = 4000;
/// How long the worker sleeps when the ring has nothing whole in it. Short
/// enough that a 512-frame wire block (10.7 ms at 48 kHz) is never waiting on
/// the timer, long enough not to spin a core.
constexpr int kIdleSleepMs = 2;

constexpr size_t kHeaderBytes = 8;
constexpr char kMagic[4] = {'F', 'B', 'A', 'F'};

}  // namespace

struct AudioFeed::Impl {
  juce::StreamingSocket socket;
};

AudioFeed::AudioFeed() : impl_(std::make_unique<Impl>()) {
  // Everything the audio thread and the worker touch is sized once, here.
  scratch_.resize(static_cast<size_t>(kAudioFeedMaxHostBlock) *
                  kAudioFeedMaxChannels);
  sendBuf_.resize(kHeaderBytes + static_cast<size_t>(kAudioFeedBlock) *
                                     kAudioFeedMaxChannels * sizeof(float));
  ring_ = std::make_unique<SpscByteRing>(kAudioFeedRingBytes);
}

AudioFeed::~AudioFeed() { stop(); }

void AudioFeed::start() {
  if (running_.exchange(true, std::memory_order_acq_rel)) return;
  worker_ = std::thread([this] { run(); });
}

void AudioFeed::stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) return;
  if (worker_.joinable()) worker_.join();
}

void AudioFeed::setSampleRate(double sampleRate) {
  const int sr = static_cast<int>(sampleRate + 0.5);
  if (sr <= 0) return;
  if (sampleRate_.exchange(sr, std::memory_order_relaxed) != sr) {
    // The hello already on the wire is now a lie; make the worker redo it.
    generation_.fetch_add(1, std::memory_order_release);
  }
}

//======================================================================
// Wire format — pure, so the tests can assert the exact bytes
//======================================================================

std::string AudioFeed::helloLine(int sampleRate, int channels, int block) {
  char buf[192];
  std::snprintf(buf, sizeof(buf),
                "{\"type\":\"audio_hello\",\"version\":%d,\"sample_rate\":%d,"
                "\"channels\":%d,\"block\":%d}\n",
                kAudioFeedProtocolVersion, sampleRate, channels, block);
  return buf;
}

void AudioFeed::writeFrameHeader(uint8_t* out, uint32_t sequence) noexcept {
  std::memcpy(out, kMagic, sizeof(kMagic));
  // Little-endian on the wire regardless of host order.
  out[4] = static_cast<uint8_t>(sequence & 0xff);
  out[5] = static_cast<uint8_t>((sequence >> 8) & 0xff);
  out[6] = static_cast<uint8_t>((sequence >> 16) & 0xff);
  out[7] = static_cast<uint8_t>((sequence >> 24) & 0xff);
}

//======================================================================
// Audio thread
//======================================================================

void AudioFeed::pushBlock(const juce::AudioBuffer<float>& buffer, int channels,
                          int numSamples) noexcept {
  // Nobody is listening: do nothing at all. An unconnected feed must not fill
  // a ring or inflate a drop count -- there is no stream to drop from.
  if (!connected_.load(std::memory_order_relaxed)) return;
  if (channels <= 0 || numSamples <= 0) return;

  if (channels != channels_.load(std::memory_order_relaxed)) {
    // The bed changed width. Anything already in the ring is interleaved at
    // the old stride, so the worker has to reconnect and re-hello before this
    // block means anything. Drop it and say so.
    channels_.store(channels, std::memory_order_relaxed);
    generation_.fetch_add(1, std::memory_order_release);
    blocksDropped_.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  if (channels > kAudioFeedMaxChannels ||
      numSamples > kAudioFeedMaxHostBlock ||
      channels > buffer.getNumChannels()) {
    blocksDropped_.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  // Planar -> frame-major, which is what bridge_audio.py reshapes to
  // (block, channels): sample s of channel c lands at [s * channels + c].
  float* out = scratch_.data();
  for (int ch = 0; ch < channels; ++ch) {
    const float* src = buffer.getReadPointer(ch);
    float* dst = out + ch;
    for (int s = 0; s < numSamples; ++s) {
      *dst = src[s];
      dst += channels;
    }
  }

  const size_t bytes =
      static_cast<size_t>(numSamples) * channels * sizeof(float);
  if (!ring_->push(out, bytes)) {
    // The receiver is not keeping up. Drop the block; never wait here.
    blocksDropped_.fetch_add(1, std::memory_order_relaxed);
  }
}

//======================================================================
// Worker thread
//======================================================================

bool AudioFeed::connect() {
  return impl_->socket.connect("127.0.0.1", kAudioFeedPort, kFeedConnectTimeoutMs);
}

bool AudioFeed::sendAll(const void* data, size_t bytes) {
  if (!impl_->socket.isConnected()) return false;
  const int n = impl_->socket.write(data, static_cast<int>(bytes));
  return n == static_cast<int>(bytes);
}

void AudioFeed::discardBacklog() {
  // Bytes left from a stream whose stride no longer applies. Throw them away
  // through the same single-consumer path rather than resetting the ring, so
  // the audio thread's producer index is never touched from here.
  uint8_t sink[4096];
  while (ring_->available() >= sizeof(sink)) {
    if (!ring_->pop(sink, sizeof(sink))) break;
  }
  size_t rest = ring_->available();
  while (rest > 0) {
    const size_t take = std::min(rest, sizeof(sink));
    if (!ring_->pop(sink, take)) break;
    rest -= take;
  }
}

void AudioFeed::run() {
  // Studio can close 47801 mid-frame; without this the write below would
  // SIGPIPE and take the DAW with it. See blockSigPipeOnThisThread().
  blockSigPipeOnThisThread();

  int backoffMs = kFeedBackoffMinMs;
  auto nextAttempt = std::chrono::steady_clock::now();
  uint32_t sequence = 0;
  int streamChannels = 0;
  uint32_t streamGeneration = 0;

  while (running_.load(std::memory_order_acquire)) {
    if (!impl_->socket.isConnected()) {
      connected_.store(false, std::memory_order_relaxed);

      if (std::chrono::steady_clock::now() < nextAttempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kIdleSleepMs));
        continue;
      }
      if (!connect()) {
        // Studio is not running. The normal case, not an error.
        nextAttempt = std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(backoffMs);
        backoffMs = std::min(backoffMs * 2, kFeedBackoffMaxMs);
        continue;
      }
      backoffMs = kFeedBackoffMinMs;

      // The audio thread only learns the bed's width by seeing a block, and
      // it only sends blocks once we are connected -- so the FIRST connect
      // has nothing to advertise yet. Announce what we know and let the width
      // check below re-hello as soon as a real block arrives.
      streamChannels = channels_.load(std::memory_order_relaxed);
      if (streamChannels <= 0) streamChannels = 12;  // 7.1.4, the FRIDAY bed
      streamGeneration = generation_.load(std::memory_order_acquire);

      discardBacklog();
      sequence = 0;

      const std::string hello = helloLine(
          sampleRate_.load(std::memory_order_relaxed), streamChannels,
          kAudioFeedBlock);
      if (!sendAll(hello.data(), hello.size())) {
        impl_->socket.close();
        continue;
      }
      // Publishing this LAST is what lets pushBlock stay a single relaxed load
      // on the audio thread: no connection, no work.
      channels_.store(streamChannels, std::memory_order_relaxed);
      connected_.store(true, std::memory_order_relaxed);
    }

    // A width or rate change invalidates the hello. Reconnecting is the whole
    // recovery: the next pass sends a fresh one.
    if (generation_.load(std::memory_order_acquire) != streamGeneration ||
        channels_.load(std::memory_order_relaxed) != streamChannels) {
      connected_.store(false, std::memory_order_relaxed);
      impl_->socket.close();
      continue;
    }

    const size_t payloadBytes =
        static_cast<size_t>(kAudioFeedBlock) * streamChannels * sizeof(float);

    bool sentSomething = false;
    while (ring_->available() >= payloadBytes) {
      if (!ring_->pop(sendBuf_.data() + kHeaderBytes, payloadBytes)) break;
      writeFrameHeader(sendBuf_.data(), sequence);
      if (!sendAll(sendBuf_.data(), kHeaderBytes + payloadBytes)) {
        connected_.store(false, std::memory_order_relaxed);
        impl_->socket.close();
        break;
      }
      ++sequence;
      framesSent_.fetch_add(1, std::memory_order_relaxed);
      sentSomething = true;
    }

    if (!sentSomething) {
      // A departed Studio is only discovered by writing, and with the DAW's
      // transport stopped there is nothing to write -- so without this the
      // feed would sit "connected" to a dead socket until the next block,
      // which might be after the engineer has gone home. This stream is
      // one-way, so ANYTHING readable, EOF included, means it is over.
      if (impl_->socket.waitUntilReady(true, 0) == 1) {
        char discard = 0;
        if (impl_->socket.read(&discard, 1, false) <= 0) {
          connected_.store(false, std::memory_order_relaxed);
          impl_->socket.close();
          continue;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kIdleSleepMs));
    }
  }

  connected_.store(false, std::memory_order_relaxed);
  if (impl_->socket.isConnected()) impl_->socket.close();
}

AudioFeed& sharedAudioFeed() {
  static AudioFeed instance;
  return instance;
}

}  // namespace friday
