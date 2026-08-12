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
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include "ParameterMetaData.h"

class AudioElementParameterTree : public juce::AudioProcessorValueTreeState {
 public:
  AudioElementParameterTree(juce::AudioProcessor& panner)
      : juce::AudioProcessorValueTreeState(
            panner, nullptr, AutoParamMetaData::kTreeType,
            AutoParamMetaData::CreateStaticParameterLayout()) {};

  // float, not int: the position parameters are continuous (see
  // ParameterMetaData.h). Returning int here would re-quantise them at every
  // read and put Eclipsa's own monitoring back out of step with the KALA
  // export, which reads the same parameters' atomics directly.
  float getXPosition() {
    return getParameterAsValue(AutoParamMetaData::xPosition).getValue();
  }

  float getYPosition() {
    return getParameterAsValue(AutoParamMetaData::yPosition).getValue();
  }

  float getZPosition() {
    return getParameterAsValue(AutoParamMetaData::zPosition).getValue();
  }

  float getVolume() {
    return getParameterAsValue(AutoParamMetaData::volumeId).getValue();
  }

  bool getUnmute() {
    return getParameterAsValue(AutoParamMetaData::unmuteId).getValue();
  }

  void setXPosition(float value) {
    getParameterAsValue(AutoParamMetaData::xPosition).setValue(value);
  }

  void setYPosition(float value) {
    getParameterAsValue(AutoParamMetaData::yPosition).setValue(value);
  }

  void setZPosition(float value) {
    getParameterAsValue(AutoParamMetaData::zPosition).setValue(value);
  }

  void setVolume(float value) {
    getParameterAsValue(AutoParamMetaData::volumeId).setValue(value);
  }

  void setUnmute(bool value) {
    getParameterAsValue(AutoParamMetaData::unmuteId).setValue(value);
  }

  void addXPositionListener(
      juce::AudioProcessorValueTreeState::Listener* listener) {
    addParameterListener(AutoParamMetaData::xPosition, listener);
  }

  void addYPositionListener(
      juce::AudioProcessorValueTreeState::Listener* listener) {
    addParameterListener(AutoParamMetaData::yPosition, listener);
  }

  void addZPositionListener(
      juce::AudioProcessorValueTreeState::Listener* listener) {
    addParameterListener(AutoParamMetaData::zPosition, listener);
  }

  void addVolumeListener(
      juce::AudioProcessorValueTreeState::Listener* listener) {
    addParameterListener(AutoParamMetaData::volumeId, listener);
  }

  void addUnmuteListener(
      juce::AudioProcessorValueTreeState::Listener* listener) {
    addParameterListener(AutoParamMetaData::unmuteId, listener);
  }

  void removeXPositionListener(
      juce::AudioProcessorValueTreeState::Listener* listener) {
    removeParameterListener(AutoParamMetaData::xPosition, listener);
  }

  void removeYPositionListener(
      juce::AudioProcessorValueTreeState::Listener* listener) {
    removeParameterListener(AutoParamMetaData::yPosition, listener);
  }

  void removeZPositionListener(
      juce::AudioProcessorValueTreeState::Listener* listener) {
    removeParameterListener(AutoParamMetaData::zPosition, listener);
  }

  void removeVolumeListener(
      juce::AudioProcessorValueTreeState::Listener* listener) {
    removeParameterListener(AutoParamMetaData::volumeId, listener);
  }

  void removeUnmuteListener(
      juce::AudioProcessorValueTreeState::Listener* listener) {
    removeParameterListener(AutoParamMetaData::unmuteId, listener);
  }
};