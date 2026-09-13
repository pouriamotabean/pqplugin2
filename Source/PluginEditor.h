#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

class PQAudioProcessorEditor:public juce::AudioProcessorEditor,private juce::Timer{
public: explicit PQAudioProcessorEditor(PQAudioProcessor&); ~PQAudioProcessorEditor() override; void paint(juce::Graphics&)override; void resized()override;
 // Manual EQ (Pro-Q style) is mouse-only - no sliders/knobs at all - so it lives on the chart itself.
 void mouseDown(const juce::MouseEvent&) override; void mouseDrag(const juce::MouseEvent&) override;
 void mouseUp(const juce::MouseEvent&) override; void mouseDoubleClick(const juce::MouseEvent&) override;
 void mouseWheelMove(const juce::MouseEvent&,const juce::MouseWheelDetails&) override;
private:
 PQAudioProcessor& p; juce::TextButton stereo{"STEREO"},mid{"MID"},side{"SIDE"},capture{"CAPTURE"},apply{"APPLY"},save{"SAVE"},load{"LOAD"},clear{"CLEAR"},widthStage{"PRE"},matchGainBtn{"MATCH GAIN"};
 juce::Slider sAmt,mAmt,siAmt,low,high,width,depth;
 // Max dB / Smoothing are no longer exposed as sliders (kept fixed at sane defaults in the
 // processor); this screen space now holds the input/output level meters + trim faders instead.
 juce::Slider inputTrim,outputTrim; juce::Rectangle<float> inputMeterArea,outputMeterArea;
 juce::ComboBox mode; juce::Label status;
 // FIX: FileChooser must stay alive for the duration of the async browse, so it lives here as a
 // member rather than a local variable that would be destroyed the instant onClick() returns.
 std::unique_ptr<juce::FileChooser> chooser;
 void timerCallback()override{repaint();} void setupButton(juce::TextButton&,juce::Colour); void setupSlider(juce::Slider&,double,double,double); void setupVerticalTrim(juce::Slider&); void label(juce::Graphics&,juce::String,juce::Rectangle<float>,juce::Colour);
 void refreshBandButtons();
 void drawCurve(juce::Graphics&,juce::Rectangle<float>,const std::array<std::atomic<float>,PQAudioProcessor::kBins>&,juce::Colour); void drawRef(juce::Graphics&,juce::Rectangle<float>,const std::array<std::atomic<float>,PQAudioProcessor::kBins>&,juce::Colour);
 void drawRangeMask(juce::Graphics&,juce::Rectangle<float>);
 // Minimal vertical bar meter (input/output level), drawn behind the trim slider of the same name.
 // peakDb draws the thin peak-hold line (see PQAudioProcessor::inputPeakDb/outputPeakDb).
 void drawVerticalMeter(juce::Graphics&,juce::Rectangle<float>,float levelDb,float peakDb,juce::Colour);
 // Horizontal dB gridlines + numeric labels (+20..-20dB) along the chart's left edge, plus the
 // 100Hz/10kHz frequency labels alongside the existing 20Hz/1kHz/20kHz ones.
 void drawFreqDbAxis(juce::Graphics&,juce::Rectangle<float>);

 // ---- Manual EQ chart geometry + interaction --------------------------------------------
 juce::Rectangle<float> chartArea; // recomputed every paint(); mouse handlers reuse it
 int draggingBand=-1; bool draggedPastThreshold=false; juce::Point<float> mouseDownPos;
 static constexpr float kManualGainRangeDb=24.f; // the manual EQ node chart shows +/- this many dB
 float xToFreq(float x) const; float freqToX(float hz) const;
 float yToGainDb(float y) const; float gainDbToY(float gainDb) const;
 int findBandNear(juce::Point<float> pos) const; // returns index within grab radius, or -1
 void drawManualEq(juce::Graphics&);
 void showBandTypeMenu(int bandIndex, juce::Point<int> screenPos);
 void showAddBandMenu(juce::Point<float> chartPos, juce::Point<int> screenPos);
 static juce::String manualTypeLabel(PQAudioProcessor::ManualType);
 JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PQAudioProcessorEditor)
};
