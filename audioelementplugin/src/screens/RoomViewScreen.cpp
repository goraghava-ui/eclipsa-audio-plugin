// Copyright 2025 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "RoomViewScreen.h"

#include "components/src/EclipsaColours.h"

RoomViewScreen::RoomViewScreen(
    AudioElementPluginSyncClient* syncClient,
    AudioElementSpatialLayoutRepository* audioElementSpatialLayoutRepo,
    AudioElementParameterTree* tree, const SpeakerMonitorData& monitorData)
    : syncClient_(syncClient),
      audioElementSpatialLayoutRepository_(audioElementSpatialLayoutRepo),
      parameterTree_(tree),
      onRoomElevationChange_([this] { elevationChangeCallback(); }),
      room_(std::make_unique<FridayPannerScope>(
          *tree, audioElementSpatialLayoutRepo)),
      selRoomElevation_({IconStore::getInstance().getFlatElevationIcon(),
                         IconStore::getInstance().getTentElevationIcon(),
                         IconStore::getInstance().getArchElevationIcon(),
                         IconStore::getInstance().getDomeElevationIcon(),
                         IconStore::getInstance().getCurveElevationIcon()},
                        true),
      spkrData_(monitorData) {
  audioElementSpatialLayoutRepository_->registerListener(this);

  // If the audio element plugin is implementing a valid audio element, display
  // the speaker layout.
  if (!audioElementSpatialLayoutRepository_->get()
           .getAudioElementId()
           .isNull()) {
    room_->setSpeakerLayout(
        audioElementSpatialLayoutRepository_->get().getChannelLayout());
  }
  room_->setInteractive(
      audioElementSpatialLayoutRepository_->get().isPanningEnabled());
  room_->setElevationContours(
      audioElementSpatialLayoutRepository_->get().getElevation() !=
      AudioElementSpatialLayout::Elevation::kFlat);
  addAndMakeVisible(room_.get());

  // Configure the roof selection, but only make visible if panning is enabled

  positionReadout_.setJustificationType(juce::Justification::centred);
  positionReadout_.setFont(
      juce::Font(juce::Font::getDefaultMonospacedFontName(), 13.0f,
                 juce::Font::plain));
  positionReadout_.setColour(juce::Label::textColourId, EclipsaColours::amber);
  addAndMakeVisible(positionReadout_);

  addAndMakeVisible(selRoomElevation_);
  selRoomElevation_.onChange(onRoomElevationChange_);
  selRoomElevation_.setToggled(static_cast<int>(
      audioElementSpatialLayoutRepository_->get().getElevation()));
  selRoomElevation_.setVisible(
      audioElementSpatialLayoutRepository_->get().isPanningEnabled());

  // This timer sets the refresh rate at which the room view is redrawn.
  startTimerHz(60);
}

RoomViewScreen::~RoomViewScreen() {
  setLookAndFeel(nullptr);
  audioElementSpatialLayoutRepository_->deregisterListener(this);
}

void RoomViewScreen::paint(juce::Graphics& g) {
  // Split the bounds into the control buttons and the audio element
  // monitoring view
  auto bounds = getLocalBounds();
  // create a copy for reference
  auto viewScreenBounds = bounds;

  auto roomViewBounds =
      bounds.removeFromTop(viewScreenBounds.proportionOfHeight(0.9f));
  auto readoutBounds =
      roomViewBounds.removeFromBottom(viewScreenBounds.proportionOfHeight(0.05f));
  room_->setBounds(roomViewBounds);
  positionReadout_.setBounds(readoutBounds);

  auto elevationToggleBounds = bounds;
  elevationToggleBounds.reduce(viewScreenBounds.proportionOfWidth(0.11f), 0.f);
  elevationToggleBounds.removeFromBottom(
      viewScreenBounds.proportionOfHeight(0.03f));
  selRoomElevation_.setBounds(elevationToggleBounds);
}

void RoomViewScreen::elevationChangeCallback() {
  // Update the elevation in the AudioElementSpatialLayout repository to update
  // the elevation listener/calculator.
  AudioElementSpatialLayout::Elevation newElevation =
      static_cast<AudioElementSpatialLayout::Elevation>(
          selRoomElevation_.getToggled());
  AudioElementSpatialLayout toUpdate =
      audioElementSpatialLayoutRepository_->get();
  toUpdate.setElevation(newElevation);
  audioElementSpatialLayoutRepository_->update(toUpdate);
  // If the new elevation is 'Flat', set the starting height to +30.
  if (newElevation == AudioElementSpatialLayout::Elevation::kFlat) {
    parameterTree_->setZPosition(30);
  }

  // The radar shows a constrained surface as contours; "flat" means the user
  // owns height independently, so there is no surface to draw.
  room_->setElevationContours(newElevation !=
                              AudioElementSpatialLayout::Elevation::kFlat);
}

// On the same timer for rendering the tracks, add height data if the selected
// elevation is 'Flat'.
void RoomViewScreen::timerCallback() {
  // The scope polls the parameter atomics on its own timer and repaints itself,
  // so there is nothing to push at it here. The screen's timer stays for the
  // repository-driven state the scope cannot see for itself.
  room_->setInteractive(
      audioElementSpatialLayoutRepository_->get().isPanningEnabled());

  const float x = static_cast<float>(parameterTree_->getXPosition());
  const float y = static_cast<float>(parameterTree_->getYPosition());
  const float z = static_cast<float>(parameterTree_->getZPosition());
  float az = 0.0f, el = 0.0f;
  FridayPannerScope::xyzToAzimuthElevation(x, y, z, az, el);
  positionReadout_.setText(
      juce::String::formatted("AZ %+6.1f\xc2\xb0   EL %+5.1f\xc2\xb0", az, el),
      juce::dontSendNotification);
}

void RoomViewScreen::valueTreePropertyChanged(
    juce::ValueTree& treeWhosePropertyHasChanged,
    const juce::Identifier& property) {
  LOG_ANALYTICS(AudioElementPluginProcessor::instanceId_,
                "RoomViewScreen::updateSpeakerSetup" +
                    audioElementSpatialLayoutRepository_->get()
                        .getChannelLayout()
                        .toString()
                        .toStdString());

  if (property == AudioElementSpatialLayout::kLayout) {
    room_->setSpeakerLayout(
        audioElementSpatialLayoutRepository_->get().getChannelLayout());
  }

  if (property == AudioElementSpatialLayout::kPanningEnabled) {
    selRoomElevation_.setVisible(
        audioElementSpatialLayoutRepository_->get().isPanningEnabled());
  }
}
