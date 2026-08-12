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

#include "FridayPannerScope.h"

#include <cmath>

#include "../EclipsaColours.h"
#include "data_structures/src/ParameterMetaData.h"

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDeg = 180.0f / kPi;
constexpr float kRad = kPi / 180.0f;

/// Ring fractions, spoke spacing and the rim fraction, all from the reference.
constexpr float kRimFraction = 0.94f;
constexpr float kRings[] = {0.94f, 0.62f, 0.31f};
constexpr int kSpokeStepDeg = 30;
/// Inset from the component edge, so labels have room outside the rim.
constexpr float kMargin = 30.0f;

/// Below this the object is a floor source and gets no height cue.
constexpr float kElevatedDeg = 2.0f;

/// How many positions the motion trail remembers, and how long it lingers
/// after the drag stops (timer ticks at 30 Hz).
constexpr size_t kTrailLength = 48;
constexpr int kTrailHoldTicks = 45;

juce::Font monoFont(float height) {
  return juce::Font(juce::Font::getDefaultMonospacedFontName(), height,
                    juce::Font::plain);
}

}  // namespace

//======================================================================
// Projection — held to Studio's PannerScope
//======================================================================

float FridayPannerScope::radiusFraction(float elevationDeg) {
  const float el = juce::jlimit(-5.0f, 90.0f, elevationDeg);
  return juce::jmax(0.12f, std::cos(el * kRad) * kRimFraction);
}

float FridayPannerScope::elevationFromRadius(float radiusFraction) {
  const float f = juce::jlimit(0.0f, 1.0f, radiusFraction);
  return std::acos(juce::jmin(1.0f, f / kRimFraction)) * kDeg;
}

void FridayPannerScope::xyzToAzimuthElevation(float x, float y, float z,
                                              float& azimuthDeg,
                                              float& elevationDeg) {
  constexpr float kMagnitude = 50.0f;
  const float nx = x / kMagnitude;
  const float ny = y / kMagnitude;
  const float nz = z / kMagnitude;
  const float horizontal = std::sqrt(nx * nx + ny * ny);
  if (horizontal < 1e-6f) {
    azimuthDeg = 0.0f;
    elevationDeg = nz > 0.0f ? 90.0f : (nz < 0.0f ? -90.0f : 0.0f);
    return;
  }
  azimuthDeg = -std::atan2(nx, ny) * kDeg;
  elevationDeg = std::atan(nz / horizontal) * kDeg;
}

void FridayPannerScope::azimuthElevationToXyz(float azimuthDeg,
                                              float elevationDeg, float& x,
                                              float& y, float& z) {
  // AudioPanner::convertCartToPolar normalises by 50 and reads
  // az = -atan2(x/50, y/50), el = atan(z/50 / hypot(x/50, y/50)). Inverting on
  // the unit sphere and scaling back by 50 round-trips exactly.
  constexpr float kMagnitude = 50.0f;
  const float az = azimuthDeg * kRad;
  const float el = juce::jlimit(-90.0f, 90.0f, elevationDeg) * kRad;
  const float horizontal = std::cos(el);
  x = -std::sin(az) * horizontal * kMagnitude;
  y = std::cos(az) * horizontal * kMagnitude;
  z = std::sin(el) * kMagnitude;
}

std::vector<FridayPannerScope::Speaker> FridayPannerScope::speakersFor(
    Speakers::AudioElementSpeakerLayout layout) {
  // BS.2051 nominal angles, matching kala_py's smpte_714_layout() so the scope
  // shows the layout the FRIDAY path actually renders to. LFE has no direction
  // and is not drawn.
  const Speaker L{30.0f, 0.0f, "L"};
  const Speaker R{-30.0f, 0.0f, "R"};
  const Speaker C{0.0f, 0.0f, "C"};
  const Speaker Lss{90.0f, 0.0f, "Lss"};
  const Speaker Rss{-90.0f, 0.0f, "Rss"};
  const Speaker Lrs{150.0f, 0.0f, "Lrs"};
  const Speaker Rrs{-150.0f, 0.0f, "Rrs"};
  const Speaker Ls{110.0f, 0.0f, "Ls"};
  const Speaker Rs{-110.0f, 0.0f, "Rs"};
  const Speaker Ltf{45.0f, 45.0f, "Ltf"};
  const Speaker Rtf{-45.0f, 45.0f, "Rtf"};
  const Speaker Ltb{135.0f, 45.0f, "Ltb"};
  const Speaker Rtb{-135.0f, 45.0f, "Rtb"};
  const Speaker Ltm{90.0f, 45.0f, "Ltm"};
  const Speaker Rtm{-90.0f, 45.0f, "Rtm"};

  if (layout == Speakers::kMono) return {C};
  if (layout == Speakers::kStereo) return {L, R};
  if (layout == Speakers::k5Point1) return {L, R, C, Ls, Rs};
  if (layout == Speakers::k5Point1Point2) return {L, R, C, Ls, Rs, Ltm, Rtm};
  if (layout == Speakers::k5Point1Point4)
    return {L, R, C, Ls, Rs, Ltf, Rtf, Ltb, Rtb};
  if (layout == Speakers::k7Point1) return {L, R, C, Lss, Rss, Lrs, Rrs};
  if (layout == Speakers::k7Point1Point2)
    return {L, R, C, Lss, Rss, Lrs, Rrs, Ltm, Rtm};
  if (layout == Speakers::k7Point1Point4)
    return {L, R, C, Lss, Rss, Lrs, Rrs, Ltf, Rtf, Ltb, Rtb};
  if (layout == Speakers::k3Point1Point2) return {L, R, C, Ltf, Rtf};
  return {};  // binaural / ambisonics have no speaker ring to draw
}

//======================================================================
// Lifetime
//======================================================================

FridayPannerScope::FridayPannerScope(
    AudioElementParameterTree& parameters,
    AudioElementSpatialLayoutRepository* layoutRepository)
    : parameters_(parameters), layoutRepository_(layoutRepository) {
  setOpaque(false);
  refreshFromParameters();
  // Poll rather than listen: this catches a dial edit, host automation and the
  // pad's own writes through one path, and 30 Hz is under the eye's ability to
  // see a step in a drag.
  startTimerHz(30);
}

FridayPannerScope::~FridayPannerScope() { stopTimer(); }

void FridayPannerScope::setSpeakerLayout(
    Speakers::AudioElementSpeakerLayout layout) {
  speakers_ = speakersFor(layout);
  repaint();
}

//======================================================================
// Geometry
//======================================================================

juce::Point<float> FridayPannerScope::centre() const {
  return {getWidth() * 0.5f, getHeight() * 0.5f};
}

float FridayPannerScope::radius() const {
  return juce::jmax(40.0f, juce::jmin(getWidth(), getHeight()) * 0.5f - kMargin);
}

juce::Point<float> FridayPannerScope::toPoint(float azimuthDeg,
                                              float elevationDeg) const {
  const juce::Point<float> c = centre();
  const float r = radius() * radiusFraction(elevationDeg);
  const float a = azimuthDeg * kRad;
  return {c.x - std::sin(a) * r, c.y - std::cos(a) * r};
}

void FridayPannerScope::pointToAzimuthElevation(juce::Point<float> p,
                                                float& azimuthDeg,
                                                float& elevationDeg) const {
  const juce::Point<float> c = centre();
  azimuthDeg = std::atan2(-(p.x - c.x), -(p.y - c.y)) * kDeg;
  const float rim = juce::jmax(radius() * kRimFraction, 1e-6f);
  elevationDeg = elevationFromRadius(c.getDistanceFrom(p) / rim);
}

//======================================================================
// Parameters
//======================================================================

float FridayPannerScope::readParam(const juce::String& id) const {
  if (const std::atomic<float>* v = parameters_.getRawParameterValue(id)) {
    return v->load(std::memory_order_relaxed);
  }
  return 0.0f;
}

void FridayPannerScope::refreshFromParameters() {
  xyzToAzimuthElevation(readParam(AutoParamMetaData::xPosition),
                        readParam(AutoParamMetaData::yPosition),
                        readParam(AutoParamMetaData::zPosition), azimuth_,
                        elevation_);
}

void FridayPannerScope::writePosition(float azimuthDeg, float elevationDeg) {
  float x = 0.0f, y = 0.0f, z = 0.0f;
  azimuthElevationToXyz(azimuthDeg, elevationDeg, x, y, z);

  const struct {
    const juce::String& id;
    float value;
  } writes[] = {{AutoParamMetaData::xPosition, x},
                {AutoParamMetaData::yPosition, y},
                {AutoParamMetaData::zPosition, z}};

  for (const auto& w : writes) {
    if (juce::RangedAudioParameter* p = parameters_.getParameter(w.id)) {
      // setValueNotifyingHost, not a direct tree poke: the host has to see
      // this as a parameter move so automation writes and undo behave exactly
      // as they do when the numeric dials are used.
      p->setValueNotifyingHost(p->convertTo0to1(w.value));
    }
  }
}

//======================================================================
// Interaction
//======================================================================

void FridayPannerScope::mouseDown(const juce::MouseEvent& e) {
  if (!interactive_) return;
  dragging_ = true;
  trail_.clear();
  for (const auto& id :
       {AutoParamMetaData::xPosition, AutoParamMetaData::yPosition,
        AutoParamMetaData::zPosition}) {
    if (juce::RangedAudioParameter* p = parameters_.getParameter(id)) {
      p->beginChangeGesture();
    }
  }
  mouseDrag(e);
}

void FridayPannerScope::mouseDrag(const juce::MouseEvent& e) {
  if (!interactive_ || !dragging_) return;
  float az = 0.0f, el = 0.0f;
  pointToAzimuthElevation(e.position, az, el);
  writePosition(az, el);
  refreshFromParameters();
  trail_.push_back({azimuth_, elevation_});
  if (trail_.size() > kTrailLength) {
    trail_.erase(trail_.begin());
  }
  trailHold_ = kTrailHoldTicks;
  repaint();
}

void FridayPannerScope::mouseUp(const juce::MouseEvent&) {
  if (!dragging_) return;
  dragging_ = false;
  for (const auto& id :
       {AutoParamMetaData::xPosition, AutoParamMetaData::yPosition,
        AutoParamMetaData::zPosition}) {
    if (juce::RangedAudioParameter* p = parameters_.getParameter(id)) {
      p->endChangeGesture();
    }
  }
}

void FridayPannerScope::mouseDoubleClick(const juce::MouseEvent&) {
  if (!interactive_) return;
  // Straight ahead, on the horizon — the one position a user always wants
  // back and cannot hit precisely by dragging.
  for (const auto& id :
       {AutoParamMetaData::xPosition, AutoParamMetaData::yPosition,
        AutoParamMetaData::zPosition}) {
    if (juce::RangedAudioParameter* p = parameters_.getParameter(id)) {
      p->beginChangeGesture();
    }
  }
  writePosition(0.0f, 0.0f);
  for (const auto& id :
       {AutoParamMetaData::xPosition, AutoParamMetaData::yPosition,
        AutoParamMetaData::zPosition}) {
    if (juce::RangedAudioParameter* p = parameters_.getParameter(id)) {
      p->endChangeGesture();
    }
  }
  refreshFromParameters();
  trail_.clear();
  repaint();
}

void FridayPannerScope::timerCallback() {
  const float az = azimuth_;
  const float el = elevation_;
  refreshFromParameters();
  bool dirty = std::abs(az - azimuth_) > 1e-3f || std::abs(el - elevation_) > 1e-3f;
  if (trailHold_ > 0 && --trailHold_ == 0 && !trail_.empty()) {
    trail_.clear();
    dirty = true;
  }
  if (dirty) repaint();
}

//======================================================================
// Painting
//======================================================================

void FridayPannerScope::paint(juce::Graphics& g) {
  // Read the parameters HERE rather than relying on the timer having run.
  // The timer's job is to notice a change and ask for a repaint; it is not the
  // only thing that can cause one (a resize, an occlusion, an offscreen
  // render). Painting from cached state meant a repaint the timer had not
  // preceded drew the object at a stale position — which is exactly how the
  // first parity screenshots came out showing the wrong pose.
  refreshFromParameters();

  const juce::Point<float> c = centre();
  const float r = radius();

  // Scope ground: radial falloff into the panel colour, so the disc reads as
  // a lit surface rather than a flat circle.
  juce::ColourGradient ground(EclipsaColours::scopeInner, c.x, c.y,
                              EclipsaColours::bg1, c.x + r * 1.25f, c.y, true);
  ground.addColour(0.75, EclipsaColours::scopeOuter);
  g.setGradientFill(ground);
  g.fillEllipse(c.x - r * 1.08f, c.y - r * 1.08f, r * 2.16f, r * 2.16f);

  // Rings and spokes.
  g.setColour(EclipsaColours::scopeGrid);
  for (const float frac : kRings) {
    const float rr = r * frac;
    g.drawEllipse(c.x - rr, c.y - rr, rr * 2.0f, rr * 2.0f, 1.0f);
  }
  for (int deg = 0; deg < 360; deg += kSpokeStepDeg) {
    const float a = deg * kRad;
    g.drawLine(c.x - std::sin(a) * r * 0.1f, c.y - std::cos(a) * r * 0.1f,
               c.x - std::sin(a) * r * 0.98f, c.y - std::cos(a) * r * 0.98f,
               1.0f);
  }

  // Elevation contours: the ring the object rides now, plus 30°/60° guides.
  if (contours_) {
    const struct {
      float elevation;
      float alpha;
    } guides[] = {{30.0f, 0.10f}, {60.0f, 0.10f}, {elevation_, 0.30f}};
    for (const auto& guide : guides) {
      const float rr = r * radiusFraction(guide.elevation);
      g.setColour(EclipsaColours::cyanSignal.withAlpha(guide.alpha));
      g.drawEllipse(c.x - rr, c.y - rr, rr * 2.0f, rr * 2.0f, 1.2f);
    }
  }

  // Compass. Azimuth is +left, so L sits at +90 and R at -90 — the single
  // convention the whole FRIDAY path shares.
  g.setFont(monoFont(11.0f));
  g.setColour(EclipsaColours::textFaint);
  const struct {
    const char* text;
    float az;
  } compass[] = {{"FRONT", 0.0f}, {"L", 90.0f}, {"R", -90.0f}, {"REAR", 180.0f}};
  for (const auto& label : compass) {
    const juce::Point<float> at = toPoint(label.az, 0.0f);
    juce::Rectangle<float> box(52.0f, 14.0f);
    juce::Point<float> anchor = at;
    if (label.az == 0.0f) {
      anchor.y -= 14.0f;
    } else if (label.az == 180.0f) {
      anchor.y += 14.0f;
    } else if (label.az > 0.0f) {
      anchor.x += 18.0f;
    } else {
      anchor.x -= 18.0f;
    }
    g.drawText(label.text, box.withCentre(anchor), juce::Justification::centred,
               false);
  }

  // Speakers: floor as an outward notch plus a dot, heights as cyan dots.
  for (const Speaker& s : speakers_) {
    const juce::Point<float> at = toPoint(s.azimuth, s.elevation);
    if (s.elevation < 15.0f) {
      const float a = s.azimuth * kRad;
      const juce::Point<float> out(at.x - std::sin(a) * 10.0f,
                                   at.y - std::cos(a) * 10.0f);
      g.setColour(EclipsaColours::speakerNotch);
      g.drawLine(at.x, at.y, out.x, out.y, 2.4f);
      g.setColour(EclipsaColours::speakerDot);
      g.fillEllipse(at.x - 2.4f, at.y - 2.4f, 4.8f, 4.8f);
    } else {
      g.setColour(EclipsaColours::speakerTop);
      g.fillEllipse(at.x - 3.0f, at.y - 3.0f, 6.0f, 6.0f);
    }
  }

  // Motion trail: amber fading in towards the newest sample.
  if (trail_.size() >= 2) {
    for (size_t i = 0; i + 1 < trail_.size(); ++i) {
      const juce::Point<float> a = toPoint(trail_[i].x, trail_[i].y);
      const juce::Point<float> b = toPoint(trail_[i + 1].x, trail_[i + 1].y);
      const float t = static_cast<float>(i) / static_cast<float>(trail_.size() - 1);
      g.setColour(EclipsaColours::amber.withAlpha(0.06f + 0.5f * t));
      g.drawLine(a.x, a.y, b.x, b.y, 2.0f);
    }
  }

  // The object: layered glow, then a white-hot core.
  const juce::Point<float> obj = toPoint(azimuth_, elevation_);
  juce::ColourGradient halo(EclipsaColours::amber.withAlpha(0.55f), obj.x, obj.y,
                            EclipsaColours::amber.withAlpha(0.0f), obj.x + 26.0f,
                            obj.y, true);
  g.setGradientFill(halo);
  g.fillEllipse(obj.x - 26.0f, obj.y - 26.0f, 52.0f, 52.0f);
  g.setColour(EclipsaColours::amber);
  g.fillEllipse(obj.x - 7.0f, obj.y - 7.0f, 14.0f, 14.0f);
  g.setColour(EclipsaColours::amberCore);
  g.drawEllipse(obj.x - 7.0f, obj.y - 7.0f, 14.0f, 14.0f, 1.4f);

  // Height cue: where the object would sit on the floor, and the stem up to
  // it. Without this an elevated object is indistinguishable from one that is
  // simply closer to the centre.
  if (elevation_ > kElevatedDeg) {
    const juce::Point<float> floor = toPoint(azimuth_, 0.0f);
    juce::Path ring;
    ring.addEllipse(floor.x - 5.0f, floor.y - 5.0f, 10.0f, 10.0f);
    ring.startNewSubPath(floor);
    ring.lineTo(obj);
    const float dashes[] = {3.0f, 3.0f};
    juce::Path dashed;
    juce::PathStrokeType(1.0f).createDashedStroke(dashed, ring, dashes, 2);
    g.setColour(EclipsaColours::amberDim);
    g.fillPath(dashed);
  }
}
