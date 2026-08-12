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

// Room-view speaker geometry (§B6-3).
//
// SpeakerLookup's vectors were hand-written box-room drawing geometry and were
// wrong twice over against what the FRIDAY path actually renders: every height
// speaker worked out at 26.6° of elevation instead of 45° (their Y was 0.5
// against a horizontal magnitude of 1.0, so they were never normalised), and
// the rear surrounds sat at ±135° where KALA and Studio put them at ±150°.
//
// These read the table back out in polar terms and hold it to the layout KALA
// renders — the same source the panner pad uses. Drawing only: nothing in the
// renderer, panner or export path reads this table.
//
// FRIDAY_B6_SHOTS=<dir> also renders a top-down plot of the table, which is the
// before/after evidence for this change.

#include <gtest/gtest.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "components/src/EclipsaColours.h"
#include "components/src/friday/FridayPannerScope.h"
#include "components/src/room_views/SpeakerLookup.h"

namespace {

constexpr float kDeg = 180.0f / 3.14159265358979323846f;

/// Read a room-view vector back as BS.2051 angles. Room axes: X = right,
/// Y = up, Z = back; azimuth is +left off the front.
void toPolar(const Coordinates::Point4D& p, float& azDeg, float& elDeg) {
  const float x = p.a[0], y = p.a[1], z = p.a[2];
  const float horizontal = std::sqrt(x * x + z * z);
  azDeg = std::atan2(-x, -z) * kDeg;
  elDeg = horizontal < 1e-6f ? (y > 0.f ? 90.f : 0.f)
                             : std::atan2(y, horizontal) * kDeg;
}

struct Polar {
  float az;
  float el;
};

Polar polarOf(const std::string& label,
              Speakers::AudioElementSpeakerLayout layout) {
  for (const auto& s : SpeakerLookup::getRoomViewSpeakers(layout)) {
    if (s.name == label) {
      Polar p{};
      toPolar(s.pos, p.az, p.el);
      return p;
    }
  }
  ADD_FAILURE() << "no speaker " << label << " in this layout";
  return {};
}

TEST(RoomViewSpeakers, the_height_layer_is_at_forty_five_degrees) {
  // The defect: Y = 0.5 with a horizontal magnitude of 1.0 is 26.57 deg, and
  // every height speaker in every layout had it.
  for (const char* label : {"Ltf", "Rtf", "Ltb", "Rtb"}) {
    const Polar p = polarOf(label, Speakers::k7Point1Point4);
    EXPECT_NEAR(p.el, 45.0f, 0.01f) << label;
  }
}

TEST(RoomViewSpeakers, the_rear_surrounds_match_the_renderer) {
  // KALA's smpte_714_layout() and Studio both use +/-150; the table said 135.
  EXPECT_NEAR(polarOf("Lrs", Speakers::k7Point1Point4).az, 150.0f, 0.01f);
  EXPECT_NEAR(polarOf("Rrs", Speakers::k7Point1Point4).az, -150.0f, 0.01f);
}

TEST(RoomViewSpeakers, the_floor_layer_was_already_right_and_stays_right) {
  // Regression guard: converting the table to polar must not move the speakers
  // that were correct.
  EXPECT_NEAR(polarOf("L", Speakers::k7Point1Point4).az, 30.0f, 0.01f);
  EXPECT_NEAR(polarOf("R", Speakers::k7Point1Point4).az, -30.0f, 0.01f);
  EXPECT_NEAR(polarOf("C", Speakers::k7Point1Point4).az, 0.0f, 0.01f);
  EXPECT_NEAR(polarOf("Lss", Speakers::k7Point1Point4).az, 90.0f, 0.01f);
  EXPECT_NEAR(polarOf("Ls", Speakers::k5Point1).az, 110.0f, 0.01f);
  for (const char* label : {"L", "R", "C", "Lss", "Rss", "Lrs", "Rrs"}) {
    EXPECT_NEAR(polarOf(label, Speakers::k7Point1Point4).el, 0.0f, 0.01f)
        << label << " is a floor speaker";
  }
}

TEST(RoomViewSpeakers, every_directional_speaker_is_on_the_unit_sphere) {
  // What "never normalised" meant: the old height vectors had magnitude 1.118.
  for (const auto layout :
       {Speakers::k7Point1Point4, Speakers::k5Point1Point4,
        Speakers::k7Point1Point2, Speakers::k5Point1, Speakers::kStereo}) {
    for (const auto& s : SpeakerLookup::getRoomViewSpeakers(layout)) {
      if (s.name == "LFE") continue;  // no direction, sits at the origin
      const float m = std::sqrt(s.pos.a[0] * s.pos.a[0] +
                                s.pos.a[1] * s.pos.a[1] +
                                s.pos.a[2] * s.pos.a[2]);
      EXPECT_NEAR(m, 1.0f, 0.01f) << s.name << " magnitude";
    }
  }
}

// The room view and the panner pad must agree about the room. They read
// different tables, so nothing but a test keeps them in step.
TEST(RoomViewSpeakers, the_room_view_agrees_with_the_panner_pad) {
  const auto padSpeakers =
      FridayPannerScope::speakersFor(Speakers::k7Point1Point4);
  for (const auto& pad : padSpeakers) {
    const Polar room = polarOf(pad.label.toStdString(),
                               Speakers::k7Point1Point4);
    EXPECT_NEAR(room.az, pad.azimuth, 0.01f) << pad.label << " azimuth";
    EXPECT_NEAR(room.el, pad.elevation, 0.01f) << pad.label << " elevation";
  }
}

//======================================================================
// Evidence: a top-down plot of the table itself
//======================================================================

TEST(RoomViewSpeakers, renders_the_speaker_plot) {
  const char* dir = std::getenv("FRIDAY_B6_SHOTS");
  if (dir == nullptr) {
    GTEST_SKIP() << "set FRIDAY_B6_SHOTS=<dir> to write the speaker plot";
  }
  constexpr int kSize = 560;
  juce::Image shot(juce::Image::ARGB, kSize, kSize, true);
  {
    juce::Graphics g(shot);
    g.fillAll(EclipsaColours::bg0);
    const juce::Point<float> c(kSize * 0.5f, kSize * 0.5f);
    const float r = kSize * 0.5f - 40.0f;

    g.setColour(EclipsaColours::scopeGrid);
    for (const float frac : {1.0f, 0.707f, 0.5f}) {
      g.drawEllipse(c.x - r * frac, c.y - r * frac, r * frac * 2.0f,
                    r * frac * 2.0f, 1.0f);
    }
    for (int deg = 0; deg < 360; deg += 30) {
      const float a = deg / kDeg;
      g.drawLine(c.x, c.y, c.x - std::sin(a) * r, c.y - std::cos(a) * r, 1.0f);
    }

    g.setFont(juce::Font(juce::Font::getDefaultMonospacedFontName(), 11.0f,
                         juce::Font::plain));
    for (const auto& s : SpeakerLookup::getRoomViewSpeakers(
             Speakers::k7Point1Point4)) {
      if (s.name == "LFE") continue;
      float az = 0.0f, el = 0.0f;
      toPolar(s.pos, az, el);
      // Plot at cos(elevation), so a height speaker sits INSIDE the floor ring
      // by exactly as much as its elevation says.
      const float rr = r * std::cos(el / kDeg);
      const juce::Point<float> at(c.x - std::sin(az / kDeg) * rr,
                                  c.y - std::cos(az / kDeg) * rr);
      const bool height = el > 15.0f;
      g.setColour(height ? EclipsaColours::speakerTop
                         : EclipsaColours::speakerDot);
      g.fillEllipse(at.x - 4.0f, at.y - 4.0f, 8.0f, 8.0f);
      g.setColour(EclipsaColours::textDim);
      g.drawText(juce::String(s.name) + "  " +
                     juce::String(az, 0) + "/" + juce::String(el, 0),
                 juce::Rectangle<float>(96.0f, 14.0f)
                     .withCentre(at + juce::Point<float>(0.0f, -13.0f)),
                 juce::Justification::centred, false);
    }
    g.setColour(EclipsaColours::textBright);
    g.drawText("SpeakerLookup 7.1.4 — azimuth/elevation, plotted at cos(el)",
               juce::Rectangle<int>(8, 6, kSize - 16, 18),
               juce::Justification::centredLeft, false);
  }
  const juce::File out =
      juce::File(dir).getChildFile("roomview_speakers.png");
  out.deleteFile();
  juce::FileOutputStream stream(out);
  ASSERT_TRUE(stream.openedOk());
  ASSERT_TRUE(juce::PNGImageFormat().writeImageToStream(shot, stream));
  stream.flush();
  SUCCEED();
}

}  // namespace
