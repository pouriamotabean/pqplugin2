#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

class PQAudioProcessorEditor:public juce::AudioProcessorEditor,private juce::Timer{
public: explicit PQAudioProcessorEditor(PQAudioProcessor&); ~PQAudioProcessorEditor() override; void paint(juce::Graphics&)override; void resized()override;
private:
 PQAudioProcessor& p; juce::TextButton stereo{"STEREO"},mid{"MID"},side{"SIDE"},capture{"CAPTURE"},apply{"APPLY"},save{"SAVE"},load{"LOAD"},clear{"CLEAR"},widthStage{"PRE"};
 juce::Slider sAmt,mAmt,siAmt,low,high,maxDb,smooth,width,depth; juce::ComboBox mode; juce::Label status;
 // FIX: FileChooser must stay alive for the duration of the async browse, so it lives here as a
 // member rather than a local variable that would be destroyed the instant onClick() returns.
 std::unique_ptr<juce::FileChooser> chooser;
 void timerCallback()override{repaint();} void setupButton(juce::TextButton&,juce::Colour); void setupSlider(juce::Slider&,double,double,double); void label(juce::Graphics&,juce::String,juce::Rectangle<float>,juce::Colour);
 void refreshBandButtons();
 void drawCurve(juce::Graphics&,juce::Rectangle<float>,const std::array<std::atomic<float>,PQAudioProcessor::kBins>&,juce::Colour); void drawRef(juce::Graphics&,juce::Rectangle<float>,const std::array<std::atomic<float>,PQAudioProcessor::kBins>&,juce::Colour);
 void drawRangeMask(juce::Graphics&,juce::Rectangle<float>);
 JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PQAudioProcessorEditor)
};
