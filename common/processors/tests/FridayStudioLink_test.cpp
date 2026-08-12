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

// FRIDAY Bridge ⇄ Studio link and handoff (V2-02, B4).
//
// The wire is a contract with another product, so these assert the bytes.
// The end-to-end gate lives in docs/evidence/b4; what it cannot localise is
// which side of the seam a malformed message came from — that is this file.

#include <gtest/gtest.h>
#include <juce_core/juce_core.h>

#include <cstdio>
#include <chrono>
#include <cstring>
#include <functional>
#include <thread>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "processors/friday/FridayStudioLink.h"
#include "processors/friday/FridayStudioSession.h"

namespace {

using friday::ObjectReceiver;
using friday::StudioLink;

/// Every line the Bridge emits has to be one parseable JSON object followed by
/// exactly one newline — Studio's reader splits on \n and json.loads the rest.
::testing::AssertionResult isNdjsonObject(const std::string& line) {
  if (line.empty() || line.back() != '\n') {
    return ::testing::AssertionFailure() << "not newline-terminated: " << line;
  }
  if (line.find('\n') != line.size() - 1) {
    return ::testing::AssertionFailure() << "embedded newline: " << line;
  }
  const juce::var parsed = juce::JSON::parse(juce::String(line));
  if (!parsed.isObject()) {
    return ::testing::AssertionFailure() << "not a JSON object: " << line;
  }
  return ::testing::AssertionSuccess();
}

juce::var field(const std::string& line, const char* key) {
  return juce::JSON::parse(juce::String(line)).getProperty(key, juce::var());
}

ObjectReceiver::LiveObject makeLive(const std::string& name, float az,
                                    float el = 0.0f, float spread = 0.0f,
                                    float gain = 0.0f) {
  ObjectReceiver::LiveObject o;
  o.name = name;
  o.az_deg = az;
  o.el_deg = el;
  o.spread = spread;
  o.gain_db = gain;
  return o;
}

//======================================================================
// The wire
//======================================================================

TEST(FridayStudioLink, hello_identifies_the_product_and_version) {
  const std::string line = StudioLink::helloLine("My Project");
  ASSERT_TRUE(isNdjsonObject(line));
  EXPECT_EQ(field(line, "type").toString(), "hello");
  EXPECT_EQ(field(line, "product").toString(), "friday-bridge");
  EXPECT_EQ(static_cast<int>(field(line, "version")), 1);
  EXPECT_EQ(field(line, "session").toString(), "My Project");
}

TEST(FridayStudioLink, scene_carries_every_object_in_order) {
  const std::vector<ObjectReceiver::LiveObject> objects{
      makeLive("kick", 30.0f, 0.0f, 0.25f, -3.0f),
      makeLive("vox", -12.5f, 20.0f),
  };
  const std::string line = StudioLink::sceneLine(objects);
  ASSERT_TRUE(isNdjsonObject(line));
  EXPECT_EQ(field(line, "type").toString(), "scene");

  const juce::var objs = field(line, "objects");
  ASSERT_TRUE(objs.isArray());
  ASSERT_EQ(objs.size(), 2);
  EXPECT_EQ(objs[0].getProperty("name", {}).toString(), "kick");
  EXPECT_NEAR(static_cast<double>(objs[0].getProperty("azimuth", {})), 30.0,
              1e-4);
  EXPECT_NEAR(static_cast<double>(objs[0].getProperty("spread", {})), 0.25,
              1e-4);
  EXPECT_NEAR(static_cast<double>(objs[0].getProperty("gain_db", {})), -3.0,
              1e-4);
  EXPECT_EQ(objs[1].getProperty("name", {}).toString(), "vox");
  EXPECT_NEAR(static_cast<double>(objs[1].getProperty("elevation", {})), 20.0,
              1e-4);
}

TEST(FridayStudioLink, an_empty_scene_is_still_valid_json) {
  // Studio replaces its whole remote list on `scene`, so an empty one is how
  // "the DAW has no objects any more" is expressed. It must not be malformed.
  const std::string line = StudioLink::sceneLine({});
  ASSERT_TRUE(isNdjsonObject(line));
  EXPECT_TRUE(field(line, "objects").isArray());
  EXPECT_EQ(field(line, "objects").size(), 0);
}

TEST(FridayStudioLink, pan_addresses_an_object_by_index) {
  const std::string line = StudioLink::panLine(3, makeLive("gtr", -47.25f));
  ASSERT_TRUE(isNdjsonObject(line));
  EXPECT_EQ(field(line, "type").toString(), "pan");
  EXPECT_EQ(static_cast<int>(field(line, "index")), 3);
  EXPECT_EQ(field(line, "name").toString(), "gtr");
  EXPECT_NEAR(static_cast<double>(field(line, "azimuth")), -47.25, 1e-4);
}

TEST(FridayStudioLink, handoff_carries_the_session_path) {
  const std::string line = StudioLink::handoffLine("/tmp/a b/mix.fstudio");
  ASSERT_TRUE(isNdjsonObject(line));
  EXPECT_EQ(field(line, "type").toString(), "handoff");
  EXPECT_EQ(field(line, "path").toString(), "/tmp/a b/mix.fstudio");
}

// A track named with a quote is not exotic; unescaped it would truncate the
// JSON object and Studio would silently drop the line.
TEST(FridayStudioLink, names_with_json_metacharacters_survive) {
  const std::string ugly = "he said \"hi\"\\ \n\ttab";
  const std::string line = StudioLink::sceneLine({makeLive(ugly, 0.0f)});
  ASSERT_TRUE(isNdjsonObject(line));
  EXPECT_EQ(field(line, "objects")[0].getProperty("name", {}).toString(),
            juce::String(ugly));
}

// Non-finite values would be spelled "nan"/"inf", which is not JSON.
TEST(FridayStudioLink, non_finite_positions_do_not_produce_invalid_json) {
  const std::string line = StudioLink::panLine(
      0, makeLive("bad", std::numeric_limits<float>::quiet_NaN(),
                  std::numeric_limits<float>::infinity()));
  ASSERT_TRUE(isNdjsonObject(line));
  EXPECT_EQ(static_cast<double>(field(line, "azimuth")), 0.0);
  EXPECT_EQ(static_cast<double>(field(line, "elevation")), 0.0);
}

//======================================================================
// The handoff session
//======================================================================

TEST(FridayStudioSession, session_json_matches_studios_schema) {
  friday::SessionObject o;
  o.name = "vox";
  o.file_path = "/tmp/vox.wav";
  o.keyframes = {{0.0, 30.0f, 0.0f, 0.0f, 0.0f},
                 {1.5, -20.0f, 10.0f, 0.5f, -6.0f}};

  const juce::var s = juce::JSON::parse(juce::String(
      friday::buildSessionJson("mix", 48000, -16.0f, "en", {o})));
  ASSERT_TRUE(s.isObject());
  // from_dict rejects anything without this marker.
  EXPECT_EQ(static_cast<int>(s.getProperty("friday_studio", {})), 1);
  EXPECT_EQ(static_cast<int>(s.getProperty("sample_rate", {})), 48000);
  EXPECT_EQ(s.getProperty("name", {}).toString(), "mix");

  const juce::var objs = s.getProperty("objects", {});
  ASSERT_TRUE(objs.isArray());
  ASSERT_EQ(objs.size(), 1);
  // from_dict indexes these two rather than .get()ing them.
  EXPECT_EQ(objs[0].getProperty("name", {}).toString(), "vox");
  EXPECT_EQ(objs[0].getProperty("file_path", {}).toString(), "/tmp/vox.wav");

  const juce::var kfs = objs[0].getProperty("keyframes", {});
  ASSERT_TRUE(kfs.isArray());
  ASSERT_EQ(kfs.size(), 2);
  // Keyframe(**k): every key has to be a field of Studio's dataclass.
  for (int i = 0; i < kfs.size(); ++i) {
    for (const char* key :
         {"t", "azimuth", "elevation", "spread", "gain_db", "curve"}) {
      EXPECT_TRUE(kfs[i].hasProperty(key)) << key << " missing on keyframe " << i;
    }
    EXPECT_EQ(kfs[i].getDynamicObject()->getProperties().size(), 6);
  }
  EXPECT_NEAR(static_cast<double>(kfs[1].getProperty("t", {})), 1.5, 1e-6);
  EXPECT_NEAR(static_cast<double>(kfs[1].getProperty("azimuth", {})), -20.0,
              1e-4);
}

TEST(FridayStudioSession, an_object_with_no_keyframes_still_gets_one) {
  friday::SessionObject o;
  o.name = "x";
  const juce::var s = juce::JSON::parse(juce::String(
      friday::buildSessionJson("mix", 48000, -16.0f, "en", {o})));
  const juce::var kfs = s.getProperty("objects", {})[0].getProperty(
      "keyframes", {});
  ASSERT_TRUE(kfs.isArray());
  EXPECT_EQ(kfs.size(), 1);
}

TEST(FridayStudioSession, a_session_with_no_objects_is_still_valid_json) {
  const juce::var s = juce::JSON::parse(
      juce::String(friday::buildSessionJson("empty", 48000, -16.0f, "en", {})));
  ASSERT_TRUE(s.isObject());
  EXPECT_EQ(s.getProperty("objects", {}).size(), 0);
}

TEST(FridayStudioSession, session_path_replaces_the_iamf_suffix) {
  EXPECT_EQ(friday::sessionPathFor("/a/b/mix.iamf"), "/a/b/mix.fstudio");
  // Not an .iamf: append rather than mangle whatever it is.
  EXPECT_EQ(friday::sessionPathFor("/a/b/mix"), "/a/b/mix.fstudio");
  EXPECT_EQ(friday::sessionPathFor("/a/b/mix.iamf.bak"),
            "/a/b/mix.iamf.bak.fstudio");
}

TEST(FridayStudioSession, stem_paths_are_filesystem_safe) {
  EXPECT_EQ(friday::stemPathFor("/a/b/mix.fstudio", "vox"),
            "/a/b/mix_vox.wav");
  // A track name is user text: separators and spaces must not escape it.
  EXPECT_EQ(friday::stemPathFor("/a/b/mix.fstudio", "../../etc/passwd"),
            "/a/b/mix_______etc_passwd.wav");
  EXPECT_EQ(friday::stemPathFor("/a/b/mix.fstudio", ""), "/a/b/mix_object.wav");
}

TEST(FridayStudioSession, mono_stem_is_a_readable_float_wav) {
  const juce::File tmp =
      juce::File::createTempFile("friday_stem_test.wav");
  const std::vector<float> pcm{0.0f, 0.5f, -0.5f, 1.0f};
  ASSERT_TRUE(friday::writeMonoWav(tmp.getFullPathName().toStdString(), pcm,
                                   48000));

  juce::MemoryBlock raw;
  ASSERT_TRUE(tmp.loadFileAsData(raw));
  ASSERT_EQ(raw.getSize(), 44u + pcm.size() * sizeof(float));
  const char* d = static_cast<const char*>(raw.getData());
  EXPECT_EQ(std::string(d, 4), "RIFF");
  EXPECT_EQ(std::string(d + 8, 4), "WAVE");
  EXPECT_EQ(std::string(d + 36, 4), "data");

  // fmt chunk: WAVE_FORMAT_IEEE_FLOAT, 1 channel, 48 kHz, 32 bits. libsndfile
  // (what Studio reads with) keys off exactly these.
  auto u16 = [d](int at) {
    return static_cast<uint16_t>(static_cast<unsigned char>(d[at]) |
                                 (static_cast<unsigned char>(d[at + 1]) << 8));
  };
  auto u32 = [d](int at) {
    return static_cast<uint32_t>(static_cast<unsigned char>(d[at]) |
                                 (static_cast<unsigned char>(d[at + 1]) << 8) |
                                 (static_cast<unsigned char>(d[at + 2]) << 16) |
                                 (static_cast<unsigned char>(d[at + 3]) << 24));
  };
  EXPECT_EQ(u16(20), 3);       // IEEE float, not PCM
  EXPECT_EQ(u16(22), 1);       // mono — Studio's ObjectTrack is a mono stem
  EXPECT_EQ(u32(24), 48000u);
  EXPECT_EQ(u16(32), 4);       // block align
  EXPECT_EQ(u16(34), 32);      // bits
  EXPECT_EQ(u32(40), pcm.size() * sizeof(float));
  EXPECT_EQ(u32(4), 36u + pcm.size() * sizeof(float));  // RIFF size

  // Samples land verbatim: the handoff must not requantise the capture.
  std::vector<float> back(pcm.size());
  std::memcpy(back.data(), d + 44, pcm.size() * sizeof(float));
  for (size_t i = 0; i < pcm.size(); ++i) EXPECT_FLOAT_EQ(back[i], pcm[i]);
  tmp.deleteFile();
}

TEST(FridayStudioSession, an_empty_stem_writes_a_valid_header) {
  const juce::File tmp = juce::File::createTempFile("friday_empty_test.wav");
  ASSERT_TRUE(
      friday::writeMonoWav(tmp.getFullPathName().toStdString(), {}, 48000));
  EXPECT_EQ(tmp.getSize(), 44);
  tmp.deleteFile();
}

//======================================================================
// The transport round trip
//
// These drive the REAL ZeroMQ path — a publisher into the process-wide
// receiver — because the live scene's two hardest behaviours (a realtime ping
// must never become export material, a departing track must not leave a ghost)
// are properties of the wire, not of any one function. They bind the object
// port, so they stop the receiver again on the way out.
//======================================================================

class FridayObjectBus : public ::testing::Test {
 protected:
  void SetUp() override {
    friday::sharedObjectReceiver().start();
    friday::sharedObjectReceiver().reset();
    ASSERT_TRUE(friday::sharedObjectReceiver().isRunning())
        << "the receiver could not bind the object port — something else in "
           "this process, or another REAPER, already owns it";
  }
  void TearDown() override { friday::sharedObjectReceiver().stop(); }

  static void ping(friday::ObjectPublisher& pub, const uint8_t (&uuid)[16],
                   float az, const char* name, uint32_t flags = 0) {
    pub.publish(uuid, az, 0.0f, 0.0f, 0.0f, nullptr, 0, flags, name);
  }

  /// Publish until the receiver shows what we are waiting for.
  ///
  /// A ZeroMQ PUB drops everything sent before the SUB's subscription has
  /// propagated, so a single send at t=0 is guaranteed to vanish. Repeating is
  /// not a workaround for flakiness — it is what the capture tap does, which
  /// pings at ~60 Hz for as long as the plugin is loaded.
  static bool pumpUntil(const std::function<void()>& send,
                        const std::function<bool()>& done, int ms = 15000) {
    // Generous, because it costs nothing when healthy — the loop returns the
    // moment the condition holds. Under a loaded full-suite run the PUB/SUB
    // handshake plus thread scheduling once overran a 5 s budget, and a test
    // that fails only when the machine is busy is worse than a slow one.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
      if (done()) return true;
      send();
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    return done();
  }

  /// Wait for something already in flight, sending nothing more.
  static bool waitFor(const std::function<bool()>& done, int ms = 3000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
      if (done()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
  }
};

TEST_F(FridayObjectBus, a_realtime_ping_reaches_the_live_scene) {
  friday::ObjectPublisher pub;
  const uint8_t uuid[16] = {1};
  ASSERT_TRUE(pumpUntil([&] { ping(pub, uuid, 42.0f, "kick"); },
                        [] {
                          return friday::sharedObjectReceiver()
                                     .liveSnapshot()
                                     .size() == 1;
                        }))
      << "no live object arrived";
  const auto live = friday::sharedObjectReceiver().liveSnapshot();
  EXPECT_EQ(live[0].name, "kick");
  EXPECT_FLOAT_EQ(live[0].az_deg, 42.0f);
}

// The B2 null depends on this: a ping arriving while an export drains must not
// become, or move, exported material.
TEST_F(FridayObjectBus, a_realtime_ping_is_not_export_material) {
  friday::ObjectPublisher pub;
  const uint8_t uuid[16] = {2};
  ASSERT_TRUE(pumpUntil([&] { ping(pub, uuid, 10.0f, "gtr"); }, [] {
    return !friday::sharedObjectReceiver().liveSnapshot().empty();
  }));
  EXPECT_EQ(friday::sharedObjectReceiver().objectCount(), 0u)
      << "a position ping was accumulated for export";
  EXPECT_TRUE(friday::sharedObjectReceiver().take().empty());
  // drain() waits on offline blocks only; pings must not keep it alive.
  EXPECT_EQ(friday::sharedObjectReceiver().blocksReceived(), 0u);
}

TEST_F(FridayObjectBus, a_departing_publisher_leaves_the_live_scene) {
  friday::ObjectPublisher pub;
  const uint8_t a[16] = {3};
  const uint8_t b[16] = {4};
  ASSERT_TRUE(pumpUntil(
      [&] {
        ping(pub, a, 0.0f, "one");
        ping(pub, b, 0.0f, "two");
      },
      [] {
        return friday::sharedObjectReceiver().liveSnapshot().size() == 2;
      }));

  // Stop pinging `a` and announce its departure — the link is established by
  // now, so this one does not need repeating.
  ping(pub, a, 0.0f, "one", friday::kFlagGone);
  ASSERT_TRUE(waitFor([] {
    return friday::sharedObjectReceiver().liveSnapshot().size() == 1;
  })) << "the removed object is still on the scope";
  // The survivor keeps its identity — the index rebuild must not scramble it.
  EXPECT_EQ(friday::sharedObjectReceiver().liveSnapshot()[0].name, "two");
}

TEST_F(FridayObjectBus, a_scene_handoff_with_nothing_on_the_bus_writes_nothing) {
  EXPECT_TRUE(friday::writeSceneHandoff("/tmp/nothing.iamf", "x", 48000, -16.0f)
                  .empty());
}

TEST_F(FridayObjectBus, a_scene_handoff_carries_names_and_positions) {
  friday::ObjectPublisher pub;
  const uint8_t uuid[16] = {5};
  ASSERT_TRUE(pumpUntil([&] { ping(pub, uuid, -33.5f, "vox"); }, [] {
    return !friday::sharedObjectReceiver().liveSnapshot().empty();
  }));

  const juce::File out =
      juce::File::createTempFile("friday_scene_handoff.iamf");
  const std::string path = friday::writeSceneHandoff(
      out.getFullPathName().toStdString(), "scene", 48000, -16.0f);
  ASSERT_FALSE(path.empty());

  const juce::var s = juce::JSON::parse(juce::File(path).loadFileAsString());
  ASSERT_TRUE(s.isObject());
  const juce::var objs = s.getProperty("objects", {});
  ASSERT_EQ(objs.size(), 1);
  EXPECT_EQ(objs[0].getProperty("name", {}).toString(), "vox");
  // No audio exists until a bounce captures it, so the stem is deliberately
  // empty and Studio will report it — see writeSceneHandoff's contract.
  EXPECT_EQ(objs[0].getProperty("file_path", {}).toString(), "");
  const juce::var kfs = objs[0].getProperty("keyframes", {});
  ASSERT_EQ(kfs.size(), 1);
  EXPECT_NEAR(static_cast<double>(kfs[0].getProperty("azimuth", {})), -33.5,
              1e-3);
  juce::File(path).deleteFile();
  out.deleteFile();
}

}  // namespace
