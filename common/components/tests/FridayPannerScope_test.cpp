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

// FRIDAY Panner scope (B5 / V2-01-UI).
//
// The gate for this milestone is a screenshot pair, and a picture cannot say
// WHY it differs. These pin the things a visual comparison would only hint at:
// the projection matches Studio's, the azimuth convention is +left, and what
// the pad writes is exactly what the panner reads back.
//
// FRIDAY_B5_SHOTS=<dir> also makes it render the parity poses to PNG, so the
// screenshot half of the gate comes out of the same build as the tests.

#include <gtest/gtest.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <cstdlib>
#include <memory>

#include "components/src/EclipsaColours.h"
#include "components/src/friday/FridayPannerScope.h"
#include "data_structures/src/AudioElementParameterTree.h"
#include "data_structures/src/Elevation.h"
#include "data_structures/src/ParameterMetaData.h"

namespace {

/// The parameter tree needs an AudioProcessor to hang off; nothing here
/// processes audio.
class ScopeHostProcessor : public juce::AudioProcessor {
 public:
  const juce::String getName() const override { return "ScopeHost"; }
  void prepareToPlay(double, int) override {}
  void releaseResources() override {}
  void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
  juce::AudioProcessorEditor* createEditor() override { return nullptr; }
  bool hasEditor() const override { return false; }
  bool acceptsMidi() const override { return false; }
  bool producesMidi() const override { return false; }
  double getTailLengthSeconds() const override { return 0.0; }
  int getNumPrograms() override { return 1; }
  int getCurrentProgram() override { return 0; }
  void setCurrentProgram(int) override {}
  const juce::String getProgramName(int) override { return {}; }
  void changeProgramName(int, const juce::String&) override {}
  void getStateInformation(juce::MemoryBlock&) override {}
  void setStateInformation(const void*, int) override {}
};

class ScopeFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    processor_ = std::make_unique<ScopeHostProcessor>();
    params_ = std::make_unique<AudioElementParameterTree>(*processor_);
    tree_ = juce::ValueTree("scopeTest");
    layout_ = std::make_unique<AudioElementSpatialLayoutRepository>(
        tree_.getOrCreateChildWithName("layout", nullptr));
    scope_ = std::make_unique<FridayPannerScope>(*params_, layout_.get());
    scope_->setSize(560, 560);
    scope_->setSpeakerLayout(Speakers::k7Point1Point4);
  }
  void TearDown() override {
    scope_.reset();
    layout_.reset();
    params_.reset();
    processor_.reset();
  }

  void setXyz(float x, float y, float z) {
    const struct {
      const juce::String& id;
      float v;
    } writes[] = {{AutoParamMetaData::xPosition, x},
                  {AutoParamMetaData::yPosition, y},
                  {AutoParamMetaData::zPosition, z}};
    for (const auto& w : writes) {
      juce::RangedAudioParameter* p = params_->getParameter(w.id);
      ASSERT_NE(p, nullptr);
      p->setValueNotifyingHost(p->convertTo0to1(w.v));
    }
  }

  float xyz(const juce::String& id) const {
    return params_->getRawParameterValue(id)->load();
  }

  std::unique_ptr<ScopeHostProcessor> processor_;
  std::unique_ptr<AudioElementParameterTree> params_;
  juce::ValueTree tree_;
  std::unique_ptr<AudioElementSpatialLayoutRepository> layout_;
  std::unique_ptr<FridayPannerScope> scope_;
};

//======================================================================
// The projection — held to Studio's PannerScope
//======================================================================

TEST(FridayPannerScopeGeometry, the_rim_is_the_horizon_and_the_centre_is_zenith) {
  // Studio's dome: elevation IS radius. Getting this backwards would put
  // height sources at the rim and still look plausible.
  EXPECT_NEAR(FridayPannerScope::radiusFraction(0.0f), 0.94f, 1e-4f);
  EXPECT_NEAR(FridayPannerScope::elevationFromRadius(1.0f), 0.0f, 1e-3f);
  EXPECT_NEAR(FridayPannerScope::elevationFromRadius(0.0f), 90.0f, 1e-3f);
  EXPECT_LT(FridayPannerScope::radiusFraction(60.0f),
            FridayPannerScope::radiusFraction(30.0f));
}

TEST(FridayPannerScopeGeometry, the_zenith_orb_stays_grabbable) {
  // The reference floors the radius at 0.12 so the orb never collapses under
  // the crosshair at the zenith. Matching it is deliberate, not an accident.
  EXPECT_NEAR(FridayPannerScope::radiusFraction(90.0f), 0.12f, 1e-4f);
}

TEST(FridayPannerScopeGeometry, elevation_round_trips_through_radius) {
  for (const float el : {0.0f, 15.0f, 30.0f, 45.0f, 60.0f}) {
    const float back = FridayPannerScope::elevationFromRadius(
        std::cos(el * 3.14159265f / 180.0f) * 0.94f / 0.94f * 0.94f);
    EXPECT_NEAR(back, el, 0.5f) << "elevation " << el;
  }
}

TEST(FridayPannerScopeGeometry, azimuth_is_positive_to_the_left) {
  // The one convention the whole FRIDAY path shares (BS.2051): M+030 is L.
  float x = 0.0f, y = 0.0f, z = 0.0f;
  FridayPannerScope::azimuthElevationToXyz(30.0f, 0.0f, x, y, z);
  EXPECT_LT(x, 0.0f) << "+30 deg must be to the LEFT, i.e. negative X";
  EXPECT_GT(y, 0.0f);
  EXPECT_NEAR(z, 0.0f, 1e-3f);
}

TEST(FridayPannerScopeGeometry, the_xyz_conversion_round_trips) {
  // The pad writes X/Y/Z and the panner and capture tap read them back. If
  // these two disagree the object renders somewhere other than where it is
  // drawn — silently.
  for (const float az : {0.0f, 30.0f, -45.0f, 90.0f, 179.0f, -150.0f}) {
    for (const float el : {0.0f, 20.0f, 45.0f, 75.0f}) {
      float x = 0.0f, y = 0.0f, z = 0.0f;
      FridayPannerScope::azimuthElevationToXyz(az, el, x, y, z);
      float backAz = 0.0f, backEl = 0.0f;
      FridayPannerScope::xyzToAzimuthElevation(x, y, z, backAz, backEl);
      EXPECT_NEAR(backAz, az, 0.01f) << "az " << az << " el " << el;
      EXPECT_NEAR(backEl, el, 0.01f) << "az " << az << " el " << el;
    }
  }
}

TEST(FridayPannerScopeGeometry, the_speaker_ring_is_the_layout_kala_renders) {
  const auto speakers =
      FridayPannerScope::speakersFor(Speakers::k7Point1Point4);
  ASSERT_EQ(speakers.size(), 11u) << "11 directions + LFE, which has none";
  auto find = [&](const char* label) {
    for (const auto& s : speakers) {
      if (s.label == label) return s;
    }
    ADD_FAILURE() << "no speaker " << label;
    return FridayPannerScope::Speaker{};
  };
  EXPECT_FLOAT_EQ(find("L").azimuth, 30.0f);
  EXPECT_FLOAT_EQ(find("R").azimuth, -30.0f);
  EXPECT_FLOAT_EQ(find("C").azimuth, 0.0f);
  // KALA and Studio put the rear surrounds at +/-150. Eclipsa's room-view
  // drawing table says +/-135; the scope follows the renderer, not the
  // drawing. BRIDGE-B2-PLAN.md section B5.
  EXPECT_FLOAT_EQ(find("Lrs").azimuth, 150.0f);
  // Heights at 45 deg elevation, not the room table's 26.6.
  EXPECT_FLOAT_EQ(find("Ltf").elevation, 45.0f);
  for (const auto& s : speakers) EXPECT_NE(s.label, "LFE");
}

//======================================================================
// The pad as a control
//======================================================================

TEST_F(ScopeFixture, dragging_writes_the_parameters) {
  const juce::MouseEvent down(
      juce::Desktop::getInstance().getMainMouseSource(),
      juce::Point<float>(280.0f, 60.0f), juce::ModifierKeys::leftButtonModifier,
      1.0f, 0.0f, 0.0f, 0.0f, 0.0f, scope_.get(), scope_.get(),
      juce::Time::getCurrentTime(), juce::Point<float>(280.0f, 60.0f),
      juce::Time::getCurrentTime(), 1, false);
  scope_->mouseDown(down);
  scope_->mouseUp(down);

  // Straight up the screen from centre is dead ahead, on the horizon.
  float az = 0.0f, el = 0.0f;
  FridayPannerScope::xyzToAzimuthElevation(
      xyz(AutoParamMetaData::xPosition), xyz(AutoParamMetaData::yPosition),
      xyz(AutoParamMetaData::zPosition), az, el);
  EXPECT_NEAR(az, 0.0f, 1.5f);
  EXPECT_LT(el, 15.0f) << "a point near the rim is near the horizon";
}

TEST_F(ScopeFixture, a_click_left_of_centre_pans_left) {
  const juce::Point<float> left(60.0f, 280.0f);
  const juce::MouseEvent e(juce::Desktop::getInstance().getMainMouseSource(),
                           left, juce::ModifierKeys::leftButtonModifier, 1.0f,
                           0.0f, 0.0f, 0.0f, 0.0f, scope_.get(), scope_.get(),
                           juce::Time::getCurrentTime(), left,
                           juce::Time::getCurrentTime(), 1, false);
  scope_->mouseDown(e);
  scope_->mouseUp(e);
  float az = 0.0f, el = 0.0f;
  FridayPannerScope::xyzToAzimuthElevation(
      xyz(AutoParamMetaData::xPosition), xyz(AutoParamMetaData::yPosition),
      xyz(AutoParamMetaData::zPosition), az, el);
  EXPECT_GT(az, 45.0f) << "the left of the scope must be POSITIVE azimuth";
}

TEST_F(ScopeFixture, a_dial_edit_moves_the_pad) {
  // Two-way: the numeric entry stays authoritative, per the B5 decision. The
  // pad must follow it, not fight it.
  setXyz(-25.0f, 43.0f, 0.0f);  // the pan the B2 gate uses: az +30.17, el 0
  juce::Image shot(juce::Image::ARGB, 560, 560, true);
  {
    juce::Graphics g(shot);
    scope_->paintEntireComponent(g, false);
  }

  // Where the projection says the orb must be, computed the same way the
  // component does rather than by eye: upper-LEFT, because azimuth is +left.
  float az = 0.0f, el = 0.0f;
  FridayPannerScope::xyzToAzimuthElevation(-25.0f, 43.0f, 0.0f, az, el);
  const float r = 560.0f / 2.0f - 30.0f;
  const float rr = r * FridayPannerScope::radiusFraction(el);
  const float a = az * 3.14159265f / 180.0f;
  const int px = static_cast<int>(280.0f - std::sin(a) * rr);
  const int py = static_cast<int>(280.0f - std::cos(a) * rr);
  ASSERT_LT(px, 280) << "azimuth +30 must land LEFT of centre";
  ASSERT_LT(py, 280) << "and in front";

  const juce::Colour orb = shot.getPixelAt(px, py);
  EXPECT_GT(orb.getFloatRed(), 0.8f) << "the orb core is amber";
  EXPECT_LT(orb.getFloatBlue(), orb.getFloatRed());
  // The mirrored position must NOT be lit, or this test would pass on any
  // symmetric artwork.
  const juce::Colour mirrored =
      shot.getPixelAt(560 - px, py);
  EXPECT_LT(mirrored.getFloatRed(), 0.5f)
      << "the right-hand mirror of the orb should be empty scope";
}

// B2 follow-up: the position parameters are continuous now. While they were
// AudioParameterInt over [-50, +50] a DAW could not express most directions —
// asking for azimuth 30.000 gave 30.173517 — and the B2 null had to be taken
// against a reference regenerated at whatever angle came out.
TEST_F(ScopeFixture, the_parameters_can_hold_a_direction_exactly) {
  for (const float wantAz : {30.0f, 0.5f, -17.25f, 123.75f}) {
    for (const float wantEl : {0.0f, 22.5f, 47.125f}) {
      float x = 0.0f, y = 0.0f, z = 0.0f;
      FridayPannerScope::azimuthElevationToXyz(wantAz, wantEl, x, y, z);
      setXyz(x, y, z);

      // Read back through the parameters, which is the round trip that used to
      // lose the fractional part.
      float az = 0.0f, el = 0.0f;
      FridayPannerScope::xyzToAzimuthElevation(
          xyz(AutoParamMetaData::xPosition), xyz(AutoParamMetaData::yPosition),
          xyz(AutoParamMetaData::zPosition), az, el);
      EXPECT_NEAR(az, wantAz, 0.01f) << "az " << wantAz << " el " << wantEl;
      EXPECT_NEAR(el, wantEl, 0.01f) << "az " << wantAz << " el " << wantEl;
    }
  }
}

TEST_F(ScopeFixture, the_parameter_range_is_unchanged_so_sessions_still_load) {
  // A finer INTEGER scale would have been the other way to add resolution, and
  // it would have changed what a stored number means: every saved x=43 would
  // read back as 0.43 and every object in every existing project would move.
  // Same range, finer type, so the stored value keeps its meaning.
  for (const auto& id :
       {AutoParamMetaData::xPosition, AutoParamMetaData::yPosition,
        AutoParamMetaData::zPosition}) {
    juce::RangedAudioParameter* p = params_->getParameter(id);
    ASSERT_NE(p, nullptr) << id;
    EXPECT_FLOAT_EQ(p->getNormalisableRange().start, -50.0f) << id;
    EXPECT_FLOAT_EQ(p->getNormalisableRange().end, 50.0f) << id;
    // Continuous: a stepped parameter reports its step count here.
    EXPECT_EQ(p->getNumSteps(), juce::AudioProcessor::getDefaultNumParameterSteps())
        << id << " should be continuous, not stepped";
  }
}

TEST_F(ScopeFixture, a_non_interactive_pad_ignores_drags) {
  setXyz(0.0f, 50.0f, 0.0f);
  scope_->setInteractive(false);
  const juce::Point<float> left(60.0f, 280.0f);
  const juce::MouseEvent e(juce::Desktop::getInstance().getMainMouseSource(),
                           left, juce::ModifierKeys::leftButtonModifier, 1.0f,
                           0.0f, 0.0f, 0.0f, 0.0f, scope_.get(), scope_.get(),
                           juce::Time::getCurrentTime(), left,
                           juce::Time::getCurrentTime(), 1, false);
  scope_->mouseDown(e);
  scope_->mouseDrag(e);
  scope_->mouseUp(e);
  EXPECT_NEAR(xyz(AutoParamMetaData::xPosition), 0.0f, 0.5f);
  EXPECT_NEAR(xyz(AutoParamMetaData::yPosition), 50.0f, 0.5f);
}

//======================================================================
// The parity screenshots
//======================================================================

TEST_F(ScopeFixture, renders_the_parity_poses) {
  const char* dir = std::getenv("FRIDAY_B5_SHOTS");
  if (dir == nullptr) {
    GTEST_SKIP() << "set FRIDAY_B5_SHOTS=<dir> to write the parity PNGs";
  }
  scope_->setElevationContours(true);  // dome, matching Studio's constraint

  const struct {
    const char* name;
    float az;
    float el;
  } poses[] = {{"az+30_el0", 30.0f, 0.0f}, {"az0_el60", 0.0f, 60.0f}};

  for (const auto& pose : poses) {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    FridayPannerScope::azimuthElevationToXyz(pose.az, pose.el, x, y, z);
    setXyz(x, y, z);

    juce::Image shot(juce::Image::ARGB, 560, 560, true);
    {
      juce::Graphics g(shot);
      // The window ground, so the scope's vignette falls off into the same
      // colour it does in the plugin and in Studio.
      g.fillAll(EclipsaColours::bg0);
      scope_->paintEntireComponent(g, false);
    }
    const juce::File out =
        juce::File(dir).getChildFile(juce::String("bridge_") + pose.name +
                                     ".png");
    out.deleteFile();
    juce::FileOutputStream stream(out);
    ASSERT_TRUE(stream.openedOk());
    ASSERT_TRUE(juce::PNGImageFormat().writeImageToStream(shot, stream));
    stream.flush();
  }
  SUCCEED();
}

}  // namespace
