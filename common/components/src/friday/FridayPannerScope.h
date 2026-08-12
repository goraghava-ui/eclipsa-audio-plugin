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

// FRIDAY Panner scope (B5 / V2-01-UI) — the room seen from above, as a radar.
//
// Design parity target: FRIDAY Studio's PannerScope
// (friday-studio/studio/ui/widgets.py). That file is a DESIGN reference only —
// palette values, ring fractions, projection and conventions. It is PySide6 and
// this is JUCE; no Qt code, header or asset is copied (CLEAN-ROOM-LOG §1.1.1).
//
// Conventions, shared with Studio and with the capture tap:
//   * azimuth is +LEFT (ITU-R BS.2051-3); M+030 is the L speaker.
//   * elevation is RADIUS: the rim is the horizon, the centre is the zenith
//     (dome projection). Studio calls this the "dome" constraint surface.
//
// Thread discipline: everything here is message-thread only. Positions are READ
// from the parameters' own atomics (getRawParameterValue) and WRITTEN through
// setValueNotifyingHost inside a change gesture, so the host records automation
// and undo exactly as it does from the numeric dials. The audio thread is never
// involved — it only ever loads the same atomics.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <vector>

#include "data_repository/implementation/AudioElementSpatialLayoutRepository.h"
#include "data_structures/src/AudioElementParameterTree.h"
#include "substream_rdr/substream_rdr_utils/Speakers.h"

class FridayPannerScope : public juce::Component, private juce::Timer {
 public:
  /// A speaker as the scope draws it: where it is, and whether it is a height.
  struct Speaker {
    float azimuth = 0.0f;    // degrees, +left
    float elevation = 0.0f;  // degrees
    juce::String label;
  };

  FridayPannerScope(AudioElementParameterTree& parameters,
                    AudioElementSpatialLayoutRepository* layoutRepository);
  ~FridayPannerScope() override;

  void paint(juce::Graphics& g) override;
  void mouseDown(const juce::MouseEvent& e) override;
  void mouseDrag(const juce::MouseEvent& e) override;
  void mouseUp(const juce::MouseEvent& e) override;
  void mouseDoubleClick(const juce::MouseEvent& e) override;

  /// Draw the speakers of `layout`. Empty for layouts with no bed.
  void setSpeakerLayout(Speakers::AudioElementSpeakerLayout layout);

  /// Suppress dragging when the plugin is not in panning mode; the scope still
  /// paints, so the room stays legible.
  void setInteractive(bool canDrag) { interactive_ = canDrag; }

  /// Draw the elevation contours Studio draws for a constrained surface: faint
  /// cyan rings at 30° and 60°, and a brighter one at the object's current
  /// elevation. Off for Eclipsa's "flat" mode, where the user sets height
  /// independently and there is no surface to show — the same distinction
  /// Studio draws between its constrained modes and "manual".
  void setElevationContours(bool show) {
    contours_ = show;
    repaint();
  }

  //== the projection, exposed so the tests can hold it to the reference =======

  /// Studio's `_radius_frac`: normalised radius an elevation sits at. The 0.12
  /// floor is deliberate and matches the reference — at the zenith the orb
  /// stays a disc you can still see and grab, rather than collapsing to a
  /// point under the crosshair.
  static float radiusFraction(float elevationDeg);

  /// Studio's `_pos_to_azel` in dome mode: elevation from normalised radius.
  static float elevationFromRadius(float radiusFraction);

  /// Eclipsa's X/Y/Z -> direction. Same reading as
  /// AudioPanner::convertCartToPolar, so the pad, the panner, the capture tap
  /// and the readouts can never disagree about where an object is.
  static void xyzToAzimuthElevation(float x, float y, float z,
                                    float& azimuthDeg, float& elevationDeg);

  /// Direction -> Eclipsa's X/Y/Z parameter values, all in [-50, +50]. The
  /// inverse of AudioPanner::convertCartToPolar, so what the pad writes is
  /// exactly what the panner and the capture tap read back.
  static void azimuthElevationToXyz(float azimuthDeg, float elevationDeg,
                                    float& x, float& y, float& z);

  /// The speakers of a layout in BS.2051 polar terms. NOT derived from
  /// `SpeakerLookup`'s room-view table: that is a box-room DRAWING geometry
  /// whose height layer works out at 26.6° rather than 45°, and which places
  /// the rear surrounds at ±135° where KALA and Studio use ±150°. The scope
  /// draws the layout KALA actually renders to — see BRIDGE-B2-PLAN.md §B5.
  static std::vector<Speaker> speakersFor(
      Speakers::AudioElementSpeakerLayout layout);

 private:
  void timerCallback() override;
  juce::Point<float> centre() const;
  float radius() const;
  juce::Point<float> toPoint(float azimuthDeg, float elevationDeg) const;
  void pointToAzimuthElevation(juce::Point<float> p, float& azimuthDeg,
                               float& elevationDeg) const;
  void writePosition(float azimuthDeg, float elevationDeg);
  float readParam(const juce::String& id) const;
  void refreshFromParameters();

  AudioElementParameterTree& parameters_;
  AudioElementSpatialLayoutRepository* layoutRepository_;

  std::vector<Speaker> speakers_;
  float azimuth_ = 0.0f;
  float elevation_ = 0.0f;
  bool interactive_ = true;
  bool contours_ = false;
  bool dragging_ = false;

  /// Recent positions, newest last — the amber motion trail.
  std::vector<juce::Point<float>> trail_;  // (azimuth, elevation) pairs
  int trailHold_ = 0;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FridayPannerScope)
};
