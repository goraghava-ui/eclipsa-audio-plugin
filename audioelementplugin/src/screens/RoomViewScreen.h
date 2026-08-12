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

#pragma once

#include "../AudioElementPluginProcessor.h"
#include "components/components.h"
#include "data_repository/implementation/AudioElementSpatialLayoutRepository.h"
#include "data_structures/src/AudioElementParameterTree.h"

class RoomViewScreen : public juce::Component,
                       public juce::ValueTree::Listener,
                       juce::Timer {
 public:
  RoomViewScreen(
      AudioElementPluginSyncClient* syncClient,
      AudioElementSpatialLayoutRepository* audioElementSpatialLayoutRepo,
      AudioElementParameterTree* tree, const SpeakerMonitorData& repos);

  ~RoomViewScreen();

  void paint(juce::Graphics& g) override;
  void updateSpeakerSetup(const Speakers::AudioElementSpeakerLayout& layout) {
    room_->setSpeakerLayout(layout);
  }

 private:
  void elevationChangeCallback();
  void timerCallback() override;
  void valueTreePropertyChanged(juce::ValueTree& treeWhosePropertyHasChanged,
                                const juce::Identifier& property) override;

  AudioElementPluginSyncClient* syncClient_;
  AudioElementSpatialLayoutRepository* audioElementSpatialLayoutRepository_;
  AudioElementParameterTree* parameterTree_;
  /// B5: the midnight-scope radar replaces the 3-D rear view. The five
  /// elevation modes survive as the repository state that drives the
  /// elevation listener, and are shown on the radar as constraint contours
  /// rather than as a drawn surface — see BRIDGE-B2-PLAN.md §B5.
  std::unique_ptr<FridayPannerScope> room_;
  SegmentedToggleImageButton selRoomElevation_;
  /// Where the object actually is, in the terms the whole FRIDAY path speaks:
  /// azimuth +left, elevation off the horizon. The X/Y/Z dials stay fully
  /// editable and two-way — this is a readout, not a replacement for them.
  juce::Label positionReadout_;
  std::function<void()> onRoomElevationChange_;
  const SpeakerMonitorData& spkrData_;
};