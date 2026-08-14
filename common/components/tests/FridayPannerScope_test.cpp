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
// Elevation modes
//
// The pad does not reimplement Eclipsa's surfaces — it writes X/Y and lets
// ElevationListener derive Z. These wire up a real listener, so what is being
// tested is that combination, which is what the user actually operates.
//======================================================================

class ScopeModeFixture : public ScopeFixture {
 protected:
  void useMode(AudioElementSpatialLayout::Elevation mode) {
    AudioElementSpatialLayout l = layout_->get();
    l.setElevation(mode);
    layout_->update(l);
    scope_->setElevationMode(mode);
  }

  void dragTo(juce::Point<float> at) {
    const juce::MouseEvent e(juce::Desktop::getInstance().getMainMouseSource(),
                             at, juce::ModifierKeys::leftButtonModifier, 1.0f,
                             0.0f, 0.0f, 0.0f, 0.0f, scope_.get(), scope_.get(),
                             juce::Time::getCurrentTime(), at,
                             juce::Time::getCurrentTime(), 1, false);
    scope_->mouseDown(e);
    scope_->mouseDrag(e);
    scope_->mouseUp(e);
  }

  float azimuth() const {
    float az = 0.0f, el = 0.0f;
    FridayPannerScope::xyzToAzimuthElevation(
        xyz(AutoParamMetaData::xPosition), xyz(AutoParamMetaData::yPosition),
        xyz(AutoParamMetaData::zPosition), az, el);
    return az;
  }
  float floorRadius() const {
    const float x = xyz(AutoParamMetaData::xPosition);
    const float y = xyz(AutoParamMetaData::yPosition);
    return std::sqrt(x * x + y * y) / 50.0f;
  }

  /// The height Eclipsa's surface assigns to the X/Y the pad just wrote.
  ///
  /// Deliberately calls the surface directly rather than going through a live
  /// ElevationListener: that listener is an APVTS listener, APVTS dispatches
  /// those on the MESSAGE THREAD, and a plugin build has
  /// JUCE_MODAL_LOOPS_PERMITTED off so a test cannot pump one. Asserting the
  /// composition this way tests the same two contracts — the pad writes X/Y,
  /// the surface derives Z — without depending on a loop that is not there.
  float surfaceHeight(AudioElementSpatialLayout::Elevation mode) const {
    const float x = xyz(AutoParamMetaData::xPosition) / 50.0f;
    const float y = xyz(AutoParamMetaData::yPosition) / 50.0f;
    switch (mode) {
      case AudioElementSpatialLayout::Elevation::kTent:
        return ElevationListener::getTentElevationPt({x, y, 0.f}).a[1] * 50.0f;
      case AudioElementSpatialLayout::Elevation::kArch:
        return ElevationListener::getArchElevationPt({x, y, 0.f}).a[1] * 50.0f;
      case AudioElementSpatialLayout::Elevation::kDome:
        return ElevationListener::getDomeElevationPtClamped({x, y, 0.f}, {})
                   .a[1] *
               50.0f;
      case AudioElementSpatialLayout::Elevation::kCurve:
        // The listener negates Y for the curve; mirror it exactly.
        return ElevationListener::getCurveElevationPt({x, -y, 0.f}).a[1] * 50.0f;
      default:
        return xyz(AutoParamMetaData::zPosition);
    }
  }
};

TEST_F(ScopeModeFixture, flat_mode_drags_azimuth_only) {
  useMode(AudioElementSpatialLayout::Elevation::kFlat);
  setXyz(0.0f, 50.0f, 20.0f);  // dead ahead, on the rim, lifted by the dial
  const float heightBefore = xyz(AutoParamMetaData::zPosition);
  const float radiusBefore = floorRadius();

  // Drag towards the centre AND to the left. Only the turn should take.
  dragTo({160.0f, 200.0f});

  EXPECT_GT(azimuth(), 10.0f) << "the object should have turned left";
  EXPECT_NEAR(floorRadius(), radiusBefore, 0.02f)
      << "flat mode must not pull the object in or out";
  EXPECT_NEAR(xyz(AutoParamMetaData::zPosition), heightBefore, 0.5f)
      << "flat mode leaves height to the Z dial";
}

TEST_F(ScopeModeFixture, flat_mode_can_still_move_an_object_at_the_origin) {
  // A fresh panner sits at (0,0,0). Preserving a radius of zero would make the
  // pad inert there — every drag writing (0,0) and nothing moving.
  useMode(AudioElementSpatialLayout::Elevation::kFlat);
  setXyz(0.0f, 0.0f, 0.0f);
  dragTo({160.0f, 200.0f});
  EXPECT_GT(floorRadius(), 0.1f) << "the object must leave the origin";
  EXPECT_GT(azimuth(), 10.0f);
}

// Eclipsa's dome IS Studio's dome now: the unit sphere, el = acos(r), rim at
// the horizon and centre at the zenith.
//
// It used to be height = 2*sqrt(1 - x^2 - y^2) - 1 — a dome over a room whose
// floor is at -1, so the rim sat at floor level and half radius read 55.7 deg
// where the sphere reads 60. The pad has always deferred to whatever
// ElevationListener writes into Z, because that surface is what the monitoring
// render and the exported file obey; closing the gap therefore meant changing
// the surface, not the pad. Done in BRIDGE-B2-PLAN.md §B7 and gated there.
TEST_F(ScopeModeFixture, dome_mode_rides_the_unit_sphere) {
  useMode(AudioElementSpatialLayout::Elevation::kDome);
  dragTo({280.0f, 280.0f - 235.0f * 0.5f});  // halfway in, dead ahead

  const float x = xyz(AutoParamMetaData::xPosition) / 50.0f;
  const float y = xyz(AutoParamMetaData::yPosition) / 50.0f;
  const float z =
      surfaceHeight(AudioElementSpatialLayout::Elevation::kDome) / 50.0f;
  const float r2 = x * x + y * y;
  EXPECT_NEAR(z, std::sqrt(1.0f - r2), 0.02f);
  EXPECT_GT(z, 0.0f) << "inside the rim means elevated";
  // And it IS a dome: the centre is the top of it, the rim is the horizon.
  EXPECT_NEAR(ElevationListener::getDomeElevationPtClamped({0.f, 0.f, 0.f}, {})
                  .a[1],
              1.0f, 1e-4f);
  EXPECT_NEAR(ElevationListener::getDomeElevationPtClamped({0.f, 1.f, 0.f}, {})
                  .a[1],
              0.0f, 1e-4f);
}

// The parity that the equation change bought, stated against Studio's own
// numbers rather than against a restatement of Eclipsa's formula.
//
// Studio's `PannerScope.constraint_elevation("dome")` in
// friday-studio/studio/ui/widgets.py is `degrees(acos(r))` over the normalised
// radius. These are its values at the five sample radii, transcribed; the
// surface has to land on them within 0.01 deg, which is far tighter than the
// 4.3 deg the old equation was out by at half radius.
TEST(FridayPannerSurface, dome_matches_studio_constraint_elevation) {
  struct Sample {
    float r;
    double studioElevationDeg;  // degrees(acos(r))
  };
  constexpr Sample kSamples[] = {
      {0.00f, 90.0},
      {0.25f, 75.52248781407008},
      {0.50f, 60.0},
      {0.75f, 41.40962210927086},
      {1.00f, 0.0},
  };

  for (const auto& s : kSamples) {
    // Walk out along +Y so the radius is unambiguous, and read the height the
    // one shared surface assigns there.
    const auto pt =
        ElevationListener::getDomeElevationPtClamped({0.0f, s.r, 0.0f}, {});
    const double height = pt.a[1];
    // Elevation of the point the surface put us on, measured from the
    // listening plane at the origin.
    const double elevationDeg =
        std::atan2(height, (double)s.r) * 180.0 / juce::MathConstants<double>::pi;

    EXPECT_NEAR(elevationDeg, s.studioElevationDeg, 0.01)
        << "r = " << s.r << ": Eclipsa's dome must agree with Studio's";
  }
}

//======================================================================
// The curve's log domain (§B8)
//======================================================================

namespace {
/// The curve's constants, and the unguarded expression exactly as it stood
/// before the domain guard. The tests below use this as the reference the
/// guarded surface has to reproduce.
constexpr int kCurveAmp = 272;
constexpr float kCurveOffs = 1.11f;
constexpr float kCurveScale = 0.336f;
constexpr float kCurveShift = 0.946f;

float unguardedCurveHeight(float u) {
  return std::max(-1.f,
                  kCurveScale * std::log(kCurveAmp * (u + kCurveShift)) -
                      kCurveOffs);
}
}  // namespace

// The curve takes a log, and its argument goes NEGATIVE inside the room.
//
// ElevationListener hands the surface u = -Y/50, so the argument is
// 272*(u + 0.946): it reaches zero at u = -0.946 and is negative for every Y
// past 47.3, i.e. the front five per cent of the depth axis. std::log of a
// negative is NaN, and nothing but the argument ORDER of the std::max below it
// kept that NaN from escaping -- max(a, b) returns `a < b ? b : a`, and every
// comparison against NaN is false, so it returned the -1.f that happened to be
// first. Written the other way round, or under -ffast-math, the same line
// returns NaN into a position parameter.
//
// The surface now tests the domain instead of relying on that. Y = +50 is the
// front rim, the pose a user reaches by dragging the pad as far forward as it
// goes -- not an edge case they have to construct.
TEST(FridayPannerSurface, curve_is_finite_across_the_whole_depth_axis) {
  struct Sample {
    float y;             // the Y parameter, in its [-50, +50] units
    float expectHeight;  // what the surface must answer
  };
  // Pinned, and the last one is the whole point: 272*(-1 + 0.946) < 0.
  constexpr Sample kSamples[] = {
      {-50.0f, 0.9972502f},   // rear rim
      {0.0f, 0.7548972f},     // centre
      {45.0f, -0.2610328f},   // still inside the log's domain
      {50.0f, -1.0f},         // front rim -- past it
  };

  for (const auto& s : kSamples) {
    // Mirror the listener exactly: it negates Y before calling the surface.
    const float height =
        ElevationListener::getCurveElevationPt({0.0f, -s.y / 50.0f, 0.0f}).a[1];

    EXPECT_FALSE(std::isnan(height)) << "Y = " << s.y << " produced NaN";
    EXPECT_TRUE(std::isfinite(height)) << "Y = " << s.y;
    EXPECT_GE(height, -1.0f) << "Y = " << s.y << ": below the room floor";
    EXPECT_NEAR(height, s.expectHeight, 1e-6f) << "Y = " << s.y;
  }
}

// The guard may only change the answer where the old code had none. Everywhere
// the log's argument was positive, the arithmetic is untouched and the result
// has to be bit-for-bit what it always was -- this mode is NOT being aligned to
// anything, so a drag under it must land exactly where it used to.
TEST(FridayPannerSurface, curve_is_bit_identical_wherever_the_log_was_defined) {
  int compared = 0;
  for (int i = -10000; i <= 10000; ++i) {
    const float u = i / 10000.0f;  // the value the listener passes through
    if (!(kCurveAmp * (u + kCurveShift) > 0.0f)) {
      continue;  // the old code went NaN here; nothing to preserve
    }
    const float now =
        ElevationListener::getCurveElevationPt({0.0f, u, 0.0f}).a[1];
    const float before = unguardedCurveHeight(u);
    ASSERT_EQ(now, before) << "u = " << u << " moved";
    ++compared;
  }
  // Guard against the loop silently skipping everything.
  EXPECT_GT(compared, 19000) << "the sweep did not cover the defined domain";
}

TEST_F(ScopeModeFixture, a_constrained_mode_lifts_the_object_off_the_floor) {
  // Whatever the surface's shape, moving IN from the rim must gain height —
  // that is what makes the radar readable as a room rather than a flat map.
  for (const auto mode : {AudioElementSpatialLayout::Elevation::kTent,
                          AudioElementSpatialLayout::Elevation::kArch,
                          AudioElementSpatialLayout::Elevation::kDome,
                          AudioElementSpatialLayout::Elevation::kCurve}) {
    useMode(mode);
    dragTo({280.0f, 280.0f - 235.0f});  // at the rim
    const float atRim = surfaceHeight(mode);
    dragTo({280.0f, 280.0f - 235.0f * 0.25f});  // well inside it
    const float inside = surfaceHeight(mode);
    EXPECT_GT(inside, atRim) << "mode " << static_cast<int>(mode);
  }
}

TEST_F(ScopeModeFixture, azimuth_survives_every_mode) {
  // The surface owns height; it must never take the direction with it.
  for (const auto mode : {AudioElementSpatialLayout::Elevation::kFlat,
                          AudioElementSpatialLayout::Elevation::kTent,
                          AudioElementSpatialLayout::Elevation::kArch,
                          AudioElementSpatialLayout::Elevation::kDome,
                          AudioElementSpatialLayout::Elevation::kCurve}) {
    useMode(mode);
    setXyz(0.0f, 50.0f, 0.0f);
    const float a = 60.0f * 3.14159265f / 180.0f;  // 60 deg LEFT, at the rim
    dragTo({280.0f - std::sin(a) * 235.0f, 280.0f - std::cos(a) * 235.0f});
    EXPECT_NEAR(azimuth(), 60.0f, 1.0f) << "mode " << static_cast<int>(mode);
  }
}

TEST_F(ScopeModeFixture, the_floor_radius_follows_the_drag_when_constrained) {
  useMode(AudioElementSpatialLayout::Elevation::kTent);
  dragTo({280.0f, 280.0f - 235.0f * 0.6f});
  EXPECT_NEAR(floorRadius(), 0.6f, 0.05f)
      << "a constrained drag places the object where the cursor is on the "
         "floor plan; only its height is the surface's business";
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

  // The two parity poses, plus one on a NON-DOME surface (§B6-2): tent puts the
  // object off the unit sphere, where the scope has to draw it at its floor
  // radius rather than at cos(elevation).
  const struct {
    const char* name;
    float az;
    float el;
    bool tent;
  } poses[] = {{"az+30_el0", 30.0f, 0.0f, false},
               {"az0_el60", 0.0f, 60.0f, false},
               {"tent_az+30", 30.0f, 0.0f, true}};

  for (const auto& pose : poses) {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    FridayPannerScope::azimuthElevationToXyz(pose.az, pose.el, x, y, z);
    if (pose.tent) {
      // Halfway in from the rim on the tent surface: height comes from the
      // depth axis, so the object is NOT on the sphere.
      x = -std::sin(pose.az * 3.14159265f / 180.0f) * 0.5f * 50.0f;
      y = std::cos(pose.az * 3.14159265f / 180.0f) * 0.5f * 50.0f;
      z = ElevationListener::getTentElevationPt({x / 50.0f, y / 50.0f, 0.f}).a[1] *
          50.0f;
      scope_->setElevationMode(AudioElementSpatialLayout::Elevation::kTent);
      scope_->setElevationContours(false);  // not a dome; see RoomViewScreen
    } else {
      scope_->setElevationMode(AudioElementSpatialLayout::Elevation::kDome);
    }
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
