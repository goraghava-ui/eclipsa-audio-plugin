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

// V2-03 sender tests.
//
// The contract lives in friday-studio/studio/bridge_audio.py, so these assert
// bytes, not intentions: the exact hello line, the exact frame header, and the
// exact interleave order Studio reshapes to (block, channels).
//
// The socket ones drive a REAL listener on 47801 against the real feed, for
// the same reason the B4 gate uses Studio's own BridgeLink: a hand-rolled
// stand-in would only prove the stand-in agrees with itself.

#include <gtest/gtest.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "processors/friday/FridayAudioFeed.h"

using friday::AudioFeed;

namespace {

constexpr int kChannels = 12;

/// A minimal Studio: accepts one client, reads the hello line, then reads
/// whole frames. Deliberately dumb — the point is to record what arrived.
class FakeStudio {
 public:
  FakeStudio() {
    listening_ = server_.createListener(friday::kAudioFeedPort, "127.0.0.1");
  }

  ~FakeStudio() { stop(); }

  bool listening() const { return listening_; }

  void serve(int channels, int frameLimit) {
    thread_ = std::thread([this, channels, frameLimit] {
      std::unique_ptr<juce::StreamingSocket> client(
          server_.waitForNextConnection());
      if (client == nullptr) {
        done_ = true;
        return;
      }

      const size_t frameBytes =
          8 + static_cast<size_t>(friday::kAudioFeedBlock) * channels *
                  sizeof(float);
      std::vector<uint8_t> buf;
      uint8_t chunk[65536];
      bool haveHello = false;

      // Poll rather than block: a blocking read would outlive the test when
      // the feed stops sending, and stop() could never join.
      while (running_) {
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (static_cast<int>(frames_.size()) >= frameLimit) break;
        }
        const int ready = client->waitUntilReady(true, 100);
        if (ready < 0) break;
        if (ready == 0) continue;
        const int n = client->read(chunk, sizeof(chunk), false);
        if (n <= 0) break;
        buf.insert(buf.end(), chunk, chunk + n);

        if (!haveHello) {
          const auto nl = std::find(buf.begin(), buf.end(), '\n');
          if (nl == buf.end()) continue;
          std::lock_guard<std::mutex> lock(mutex_);
          hello_.assign(buf.begin(), nl);
          buf.erase(buf.begin(), nl + 1);
          haveHello = true;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        while (buf.size() >= frameBytes &&
               static_cast<int>(frames_.size()) < frameLimit) {
          frames_.emplace_back(buf.begin(), buf.begin() + frameBytes);
          buf.erase(buf.begin(), buf.begin() + frameBytes);
        }
      }
      done_ = true;
    });
  }

  void stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
  }

  /// Stop ACCEPTING too. Without this the feed simply reconnects to the
  /// unattended listener and reports itself connected again -- which is
  /// correct behaviour, and would make a hangup test assert nothing.
  void closeListener() { server_.close(); }

  bool waitForFrames(size_t n, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
      if (frameCount() >= n) return true;
      if (done_) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return frameCount() >= n;
  }

  size_t frameCount() {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_.size();
  }
  std::string hello() {
    std::lock_guard<std::mutex> lock(mutex_);
    return hello_;
  }
  std::vector<std::vector<uint8_t>> frames() {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_;
  }

 private:
  juce::StreamingSocket server_;
  bool listening_ = false;
  std::atomic<bool> running_{true};
  std::atomic<bool> done_{false};
  std::mutex mutex_;
  std::string hello_;
  std::vector<std::vector<uint8_t>> frames_;
  std::thread thread_;
};

uint32_t readSeq(const std::vector<uint8_t>& frame) {
  return static_cast<uint32_t>(frame[4]) |
         (static_cast<uint32_t>(frame[5]) << 8) |
         (static_cast<uint32_t>(frame[6]) << 16) |
         (static_cast<uint32_t>(frame[7]) << 24);
}

float sampleAt(const std::vector<uint8_t>& frame, int s, int ch,
               int channels) {
  float v = 0.0f;
  std::memcpy(&v, frame.data() + 8 + (s * channels + ch) * sizeof(float),
              sizeof(float));
  return v;
}

/// A bed whose every sample says which channel and which sample it is, so a
/// transposed or off-by-one interleave cannot pass.
juce::AudioBuffer<float> markedBed(int channels, int numSamples, int base) {
  juce::AudioBuffer<float> b(channels, numSamples);
  for (int ch = 0; ch < channels; ++ch) {
    for (int s = 0; s < numSamples; ++s) {
      b.setSample(ch, s, static_cast<float>(ch) +
                             static_cast<float>(base + s) / 100000.0f);
    }
  }
  return b;
}

/// Push until the feed has handed `frames` whole wire frames to the worker, or
/// we run out of patience.
void pump(AudioFeed& feed, int channels, int hostBlock, int blocks) {
  for (int i = 0; i < blocks; ++i) {
    const juce::AudioBuffer<float> bed =
        markedBed(channels, hostBlock, i * hostBlock);
    feed.pushBlock(bed, channels, hostBlock);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

bool waitConnected(AudioFeed& feed, int timeoutMs) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (feed.isConnected()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

}  // namespace

//======================================================================
// The wire format, asserted as bytes
//======================================================================

TEST(FridayAudioFeed, hello_is_exactly_what_bridge_audio_parses) {
  EXPECT_EQ(AudioFeed::helloLine(48000, 12, 512),
            "{\"type\":\"audio_hello\",\"version\":1,\"sample_rate\":48000,"
            "\"channels\":12,\"block\":512}\n");
}

TEST(FridayAudioFeed, the_frame_header_is_magic_then_little_endian_sequence) {
  uint8_t hdr[8] = {0};
  AudioFeed::writeFrameHeader(hdr, 0x01020304u);
  EXPECT_EQ(std::string(reinterpret_cast<char*>(hdr), 4), "FBAF");
  EXPECT_EQ(hdr[4], 0x04);
  EXPECT_EQ(hdr[5], 0x03);
  EXPECT_EQ(hdr[6], 0x02);
  EXPECT_EQ(hdr[7], 0x01);

  // Wraps, because bridge_audio.py compares against (seq + 1) & 0xFFFFFFFF.
  AudioFeed::writeFrameHeader(hdr, 0xFFFFFFFFu);
  EXPECT_EQ(hdr[4], 0xFF);
  EXPECT_EQ(hdr[7], 0xFF);
}

//======================================================================
// Against a real listener
//======================================================================

TEST(FridayAudioFeed, an_unconnected_feed_does_nothing_at_all) {
  // No Studio. The renderer still calls pushBlock on every block, and that
  // must cost nothing and must NOT count as a drop -- there is no stream to
  // drop from, and a drop counter that ticks when nobody is listening would
  // make the gate's "zero drops" meaningless.
  AudioFeed feed;
  feed.setSampleRate(48000);
  feed.start();
  pump(feed, kChannels, 256, 20);
  EXPECT_FALSE(feed.isConnected());
  EXPECT_EQ(feed.framesSent(), 0u);
  EXPECT_EQ(feed.blocksDropped(), 0u);
  feed.stop();
}

TEST(FridayAudioFeed, a_stream_is_a_hello_then_numbered_frame_major_frames) {
  FakeStudio studio;
  ASSERT_TRUE(studio.listening()) << "could not listen on 47801";
  studio.serve(kChannels, 4);

  AudioFeed feed;
  feed.setSampleRate(48000);
  feed.start();
  ASSERT_TRUE(waitConnected(feed, 3000)) << "the feed never connected";

  // 512-frame host blocks, so one host block is exactly one wire frame and
  // the sample arithmetic below stays readable.
  pump(feed, kChannels, friday::kAudioFeedBlock, 6);
  ASSERT_TRUE(studio.waitForFrames(3, 3000)) << "frames did not arrive";
  studio.stop();
  feed.stop();

  EXPECT_EQ(studio.hello(),
            "{\"type\":\"audio_hello\",\"version\":1,\"sample_rate\":48000,"
            "\"channels\":12,\"block\":512}");

  const std::vector<std::vector<uint8_t>> got = studio.frames();

  // Sequence starts at 0 and increments by 1.
  for (size_t i = 0; i < got.size(); ++i) {
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(got[i].data()), 4),
              "FBAF");
    EXPECT_EQ(readSeq(got[i]), static_cast<uint32_t>(i));
  }

  // Frame-major: sample s of channel c sits at [s * channels + c]. The bed was
  // marked ch + (absolute sample)/100000, so both indices are checkable.
  const std::vector<uint8_t>& first = got.front();
  for (int ch = 0; ch < kChannels; ++ch) {
    EXPECT_NEAR(sampleAt(first, 0, ch, kChannels), static_cast<float>(ch),
                1e-5f)
        << "channel " << ch << " is not where Studio will look for it";
    EXPECT_NEAR(sampleAt(first, 7, ch, kChannels),
                static_cast<float>(ch) + 7.0f / 100000.0f, 1e-5f);
  }
}

TEST(FridayAudioFeed, host_blocks_are_repacketised_to_the_wire_block) {
  // The hello fixes `block` for the whole stream, but a host may hand us any
  // buffer size. 128-sample blocks must come out as 512-sample frames with no
  // samples invented and none lost across the seam.
  FakeStudio studio;
  ASSERT_TRUE(studio.listening()) << "could not listen on 47801";
  studio.serve(kChannels, 2);

  AudioFeed feed;
  feed.setSampleRate(48000);
  feed.start();
  ASSERT_TRUE(waitConnected(feed, 3000)) << "the feed never connected";

  pump(feed, kChannels, 128, 12);  // 12 * 128 = 1536 = three wire frames
  ASSERT_TRUE(studio.waitForFrames(2, 3000)) << "frames did not arrive";
  studio.stop();
  feed.stop();

  // The stream is continuous across the frame boundary: sample 511 of frame 0
  // and sample 0 of frame 1 are consecutive in the bed the renderer pushed.
  const std::vector<std::vector<uint8_t>> got = studio.frames();
  ASSERT_GE(got.size(), 2u);
  const float lastOfFirst = sampleAt(got[0], 511, 0, kChannels);
  const float firstOfSecond = sampleAt(got[1], 0, 0, kChannels);
  EXPECT_NEAR(lastOfFirst, 511.0f / 100000.0f, 1e-5f);
  EXPECT_NEAR(firstOfSecond, 512.0f / 100000.0f, 1e-5f);
}

TEST(FridayAudioFeed, studio_disappearing_mid_stream_does_not_take_the_host_down) {
  // Regression, and the reason blockSigPipeOnThisThread() exists.
  //
  // JUCE sets SO_NOSIGPIPE on macOS ONLY (juce_Network_linux.cpp:385 sits
  // inside #if JUCE_MAC), so on Linux a write to a socket whose peer has gone
  // raises SIGPIPE -- and SIGPIPE's default action terminates the process,
  // which in the field is the DAW. Studio quitting mid-mix is not an edge
  // case; it is Tuesday.
  //
  // If the guard is ever removed this test does not fail, it DIES: the whole
  // gtest binary is killed by the signal partway through. That is the point.
  AudioFeed feed;
  feed.setSampleRate(48000);
  feed.start();

  {
    FakeStudio studio;
    ASSERT_TRUE(studio.listening()) << "could not listen on 47801";
    studio.serve(kChannels, 1);
    ASSERT_TRUE(waitConnected(feed, 3000)) << "the feed never connected";
    pump(feed, kChannels, friday::kAudioFeedBlock, 2);
    ASSERT_TRUE(studio.waitForFrames(1, 3000)) << "no frame before the hangup";

    // Studio goes away completely -- the client AND the listener, so the feed
    // cannot simply reconnect to an unattended socket.
    studio.stop();
    studio.closeListener();

    // Keep the renderer pushing straight into the hangup, hard enough that the
    // worker is certainly writing while the peer is gone.
    pump(feed, kChannels, friday::kAudioFeedBlock, 40);
  }

  // Surviving is the assertion. Noticing is the bonus.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
  while (feed.isConnected() && std::chrono::steady_clock::now() < deadline) {
    pump(feed, kChannels, friday::kAudioFeedBlock, 2);
  }
  EXPECT_FALSE(feed.isConnected())
      << "the feed should have noticed the peer was gone";

  // And it recovers: a Studio that comes back gets a fresh hello, not silence.
  FakeStudio again;
  ASSERT_TRUE(again.listening()) << "could not re-listen on 47801";
  again.serve(kChannels, 1);
  EXPECT_TRUE(waitConnected(feed, 8000)) << "the feed never came back";
  pump(feed, kChannels, friday::kAudioFeedBlock, 4);
  EXPECT_TRUE(again.waitForFrames(1, 5000)) << "no frames after recovery";
  EXPECT_EQ(again.hello(),
            "{\"type\":\"audio_hello\",\"version\":1,\"sample_rate\":48000,"
            "\"channels\":12,\"block\":512}");
  again.stop();
  feed.stop();
}
