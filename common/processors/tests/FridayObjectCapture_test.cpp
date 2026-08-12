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

// FRIDAY Bridge B2 — unit coverage for the parts of the object capture path
// that are pure functions of their input. The end-to-end path has its own
// gate (BRIDGE-B2-PLAN.md §13); this covers the logic that gate cannot
// localise a failure in.

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "processors/file_output/iamf_export_utils/KalaIamfWriter.h"
#include "processors/friday/FridayObjectCaptureProcessor.h"
#include "processors/friday/FridayObjectTransport.h"

namespace {

using Object = friday::ObjectReceiver::Object;

Object makeObject(std::vector<float> pcm) {
  Object o;
  o.pcm = std::move(pcm);
  return o;
}

//======================================================================
// trimTrailingSilence — the host's post-render flush
//======================================================================

TEST(FridayObjectCapture, trim_removes_the_hosts_silent_flush) {
  std::vector<Object> objects{makeObject({0.5f, -0.5f, 0.25f, 0.0f, 0.0f})};
  KalaIamfWriter::trimTrailingSilence(objects);
  ASSERT_EQ(objects[0].pcm.size(), 3u);
  EXPECT_FLOAT_EQ(objects[0].pcm.back(), 0.25f);
}

TEST(FridayObjectCapture, trim_keeps_silence_that_is_not_trailing) {
  std::vector<Object> objects{makeObject({0.5f, 0.0f, 0.0f, 0.25f})};
  KalaIamfWriter::trimTrailingSilence(objects);
  EXPECT_EQ(objects[0].pcm.size(), 4u);
}

// The trim has to be COMMON across objects, or a quiet object would be
// shortened relative to a loud one and the render would go out of sync.
TEST(FridayObjectCapture, trim_is_common_across_objects) {
  std::vector<Object> objects{
      makeObject({0.5f, 0.0f, 0.0f, 0.0f}),   // ends early
      makeObject({0.5f, 0.5f, 0.5f, 0.0f}),   // ends later
  };
  KalaIamfWriter::trimTrailingSilence(objects);
  ASSERT_EQ(objects.size(), 2u);
  EXPECT_EQ(objects[0].pcm.size(), 3u);
  EXPECT_EQ(objects[1].pcm.size(), 3u);
}

TEST(FridayObjectCapture, trim_leaves_an_all_silent_capture_alone) {
  // Nothing to anchor a length to; the caller decides what a silent export
  // means rather than being handed an empty buffer.
  std::vector<Object> objects{makeObject({0.0f, 0.0f, 0.0f})};
  KalaIamfWriter::trimTrailingSilence(objects);
  EXPECT_EQ(objects[0].pcm.size(), 3u);
}

TEST(FridayObjectCapture, trim_handles_an_empty_object_list) {
  std::vector<Object> objects;
  KalaIamfWriter::trimTrailingSilence(objects);  // must not crash
  EXPECT_TRUE(objects.empty());
}

//======================================================================
// cartesianToPolarDegrees — the convention that has to match Studio
//======================================================================

// ITU-R BS.2051-3 as Studio reads it: +azimuth is LEFT, so M+030 is the L
// speaker. Getting this backwards mirrors the whole scene and the null fails
// on every channel at once.
TEST(FridayObjectCapture, azimuth_is_positive_to_the_left) {
  float az = 0.0f, el = 0.0f;
  // x = -25, y = +43.3 is 30 degrees to the LEFT.
  FridayObjectCaptureProcessor::cartesianToPolarDegrees(-25.0f, 43.301f, 0.0f,
                                                        az, el);
  EXPECT_NEAR(az, 30.0f, 0.01f);
  EXPECT_NEAR(el, 0.0f, 0.01f);

  FridayObjectCaptureProcessor::cartesianToPolarDegrees(25.0f, 43.301f, 0.0f,
                                                        az, el);
  EXPECT_NEAR(az, -30.0f, 0.01f);
}

TEST(FridayObjectCapture, straight_ahead_is_zero_azimuth) {
  float az = 1.0f, el = 1.0f;
  FridayObjectCaptureProcessor::cartesianToPolarDegrees(0.0f, 50.0f, 0.0f, az,
                                                        el);
  EXPECT_NEAR(az, 0.0f, 0.01f);
  EXPECT_NEAR(el, 0.0f, 0.01f);
}

TEST(FridayObjectCapture, directly_overhead_is_ninety_degrees_elevation) {
  float az = 1.0f, el = 0.0f;
  FridayObjectCaptureProcessor::cartesianToPolarDegrees(0.0f, 0.0f, 50.0f, az,
                                                        el);
  EXPECT_FLOAT_EQ(az, 0.0f);
  EXPECT_FLOAT_EQ(el, 90.0f);

  FridayObjectCaptureProcessor::cartesianToPolarDegrees(0.0f, 0.0f, -50.0f, az,
                                                        el);
  EXPECT_FLOAT_EQ(el, -90.0f);
}

TEST(FridayObjectCapture, the_origin_is_defined_rather_than_nan) {
  float az = 7.0f, el = 7.0f;
  FridayObjectCaptureProcessor::cartesianToPolarDegrees(0.0f, 0.0f, 0.0f, az,
                                                        el);
  EXPECT_FLOAT_EQ(az, 0.0f);
  EXPECT_FLOAT_EQ(el, 0.0f);
}

TEST(FridayObjectCapture, elevation_is_measured_off_the_horizontal_plane) {
  float az = 0.0f, el = 0.0f;
  // Equal horizontal radius and height -> 45 degrees up.
  FridayObjectCaptureProcessor::cartesianToPolarDegrees(0.0f, 30.0f, 30.0f, az,
                                                        el);
  EXPECT_NEAR(el, 45.0f, 0.01f);
}

//======================================================================
// SpscByteRing — the audio thread's only data structure
//======================================================================

TEST(FridayObjectCapture, ring_round_trips_and_wraps) {
  friday::SpscByteRing ring(16);
  const uint8_t in[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  uint8_t out[10] = {};

  // Two pushes of 10 into a 16-byte ring only fit if the reader keeps up,
  // which is exactly the wrap case.
  ASSERT_TRUE(ring.push(in, sizeof(in)));
  ASSERT_TRUE(ring.pop(out, sizeof(out)));
  EXPECT_EQ(0, std::memcmp(in, out, sizeof(in)));

  ASSERT_TRUE(ring.push(in, sizeof(in)));
  ASSERT_TRUE(ring.pop(out, sizeof(out)));
  EXPECT_EQ(0, std::memcmp(in, out, sizeof(in)));
  EXPECT_EQ(ring.available(), 0u);
}

TEST(FridayObjectCapture, ring_refuses_to_overflow_rather_than_blocking) {
  friday::SpscByteRing ring(8);
  const uint8_t in[8] = {};
  ASSERT_TRUE(ring.push(in, sizeof(in)));
  EXPECT_FALSE(ring.push(in, 1));  // full: drop, never stall the audio thread
  EXPECT_EQ(ring.available(), 8u);
}

TEST(FridayObjectCapture, ring_pop_of_an_incomplete_record_fails) {
  friday::SpscByteRing ring(16);
  const uint8_t in[4] = {1, 2, 3, 4};
  uint8_t out[8] = {};
  ASSERT_TRUE(ring.push(in, sizeof(in)));
  EXPECT_FALSE(ring.pop(out, sizeof(out)));  // reader must not read past head
  EXPECT_EQ(ring.available(), 4u);
}

}  // namespace
