#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <functional>

// FIX (item 5): right-click context menus (add-band Type+Target, band Type/Delete) were using the
// host/OS default popup font, which read as far too small next to the rest of this UI. This
// LookAndFeel is applied to those two PopupMenus only (via PopupMenu::setLookAndFeel) so it doesn't
// change any other control's appearance.
class BigPopupLookAndFeel : public juce::LookAndFeel_V4 {
public:
    juce::Font getPopupMenuFont() override { return juce::Font(juce::FontOptions(30.0f)); } // ~3x a normal menu font
};

// FIX (item 2/3): the old show/hide checkboxes were plain ToggleButtons stacked directly under the
// big STEREO/MID/SIDE buttons - their labels and hit-boxes sat close enough to those buttons to be
// fiddly/ambiguous to click. They're now small solid dots living on the header row next to PRESETS,
// coloured to match each target - no text needed, the colour says which is which. Filled = visible,
// hollow ring = hidden. setDotColour() is called once from the editor's constructor (colours live
// in PluginEditor.cpp) rather than passed through the constructor, so this stays a simple, tool-free
// member.
class CircleToggle : public juce::Button {
public:
    CircleToggle():juce::Button({}){ setClickingTogglesState(true); setToggleState(true, juce::dontSendNotification); }
    void setDotColour(juce::Colour c){ colour=c; repaint(); }
    void paintButton(juce::Graphics& g, bool isMouseOver, bool) override {
        auto r = getLocalBounds().toFloat().reduced(1.f);
        if(getToggleState()){ g.setColour(colour); g.fillEllipse(r); }
        else { g.setColour(colour.withAlpha(isMouseOver?0.75f:0.45f)); g.drawEllipse(r,2.0f); }
    }
private:
    juce::Colour colour{juce::Colours::white};
};

// ---- Preset panel (item 6) -------------------------------------------------------------------
// Replaces the old bottom-row SAVE/LOAD buttons with a small top-of-window browser, opened/closed
// from a single PRESETS button. A "preset" is the plugin's full state (reference + manual EQ +
// width + match + trim) - see PQAudioProcessor::saveReference/loadReference/applyStateBlock -
// stored as ordinary .pqref files in a fixed folder so they show up in one list instead of
// scattered wherever a save dialog happened to be pointed.
class PresetPanel : public juce::Component {
public:
    explicit PresetPanel(PQAudioProcessor& proc);
    void resized() override;
    void paint(juce::Graphics&) override;
    void refreshList();
    // Called after a preset loads, so the host editor can resync every slider/button from the
    // processor's (now possibly very different) state.
    std::function<void()> onPresetLoaded;
private:
    PQAudioProcessor& p;
    juce::Label title{"", "PRESETS"};
    juce::ComboBox list;
    juce::TextEditor nameBox;
    juce::TextButton saveBtn{"SAVE"}, deleteBtn{"DELETE"}, closeBtn{"X"};
    static juce::File presetDir();
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PresetPanel)
};

class PQAudioProcessorEditor:public juce::AudioProcessorEditor,private juce::Timer{
public: explicit PQAudioProcessorEditor(PQAudioProcessor&); ~PQAudioProcessorEditor() override; void paint(juce::Graphics&)override; void resized()override;
 // Manual EQ (Pro-Q style) is mouse-only - no sliders/knobs at all - so it lives on the chart itself.
 void mouseDown(const juce::MouseEvent&) override; void mouseDrag(const juce::MouseEvent&) override;
 void mouseUp(const juce::MouseEvent&) override; void mouseDoubleClick(const juce::MouseEvent&) override;
 void mouseWheelMove(const juce::MouseEvent&,const juce::MouseWheelDetails&) override;
 // FIX (item 4): lets a selected manual-EQ node be deleted from the keyboard (Delete/Backspace),
 // not just via the right-click "Delete Band" menu item.
 bool keyPressed(const juce::KeyPress&) override;
private:
 PQAudioProcessor& p; juce::TextButton stereo{"STEREO"},mid{"MID"},side{"SIDE"},capture{"CAPTURE"},apply{"APPLY"},clear{"CLEAR"},widthStage{"PRE"},matchGainBtn{"MATCH GAIN"},presetsBtn{"PRESETS"};
 juce::Slider sAmt,mAmt,siAmt,low,high,width,depth;
 // Max dB / Smoothing are no longer exposed as sliders (kept fixed at sane defaults in the
 // processor); this screen space now holds the input/output level meters + trim faders instead.
 juce::Slider inputTrim,outputTrim; juce::Rectangle<float> inputMeterArea,outputMeterArea;
 juce::ComboBox mode; juce::Label status;
 // FIX (item 6): SAVE/LOAD moved into presetPanel (a small overlay opened by presetsBtn), so the
 // bottom row no longer needs its own FileChooser for reference files. syncControlsFromProcessor()
 // re-reads every editor control from the processor after a preset (which can change far more than
 // just the reference curve) loads.
 PresetPanel presetPanel;
 void syncControlsFromProcessor();
 // FIX (item 3): independent show/hide toggles for each manual-EQ target's curve+nodes, so the
 // chart can be decluttered once a target's editing is finished (still edited via checking it back
 // on - hiding never touches the underlying bands or the audio, purely a display filter).
 CircleToggle stereoEqToggle,midEqToggle,sideEqToggle;
 // FIX (manual-EQ target now driven purely by the three toggles): the toggles used to be a pure
 // display filter, with every newly-added node hardcoded to ManualTarget::Stereo (you then had to
 // right-click -> "Move To" to get a Mid/Side node). Now each toggle switch also declares itself the
 // target for new nodes. Several toggles can be on at once (each shows/edits its own curve
 // independently), so we keep a small recency stack: the most recently switched-on toggle is where
 // new left-click/right-click-add nodes go. Switching a toggle off removes it from the stack; if none
 // are on, clicking the chart adds nothing (nothing is "armed" to receive it).
 juce::Array<PQAudioProcessor::ManualTarget> activeManualTargets;
 void updateActiveTargetStack(PQAudioProcessor::ManualTarget t, bool on);
 bool hasActiveManualTarget() const { return !activeManualTargets.isEmpty(); }
 PQAudioProcessor::ManualTarget currentManualTarget() const { return activeManualTargets.getLast(); }
 BigPopupLookAndFeel bigMenuLnf;
 void timerCallback()override{repaint();} void setupButton(juce::TextButton&,juce::Colour); void setupSlider(juce::Slider&,double,double,double); void setupVerticalTrim(juce::Slider&); void label(juce::Graphics&,juce::String,juce::Rectangle<float>,juce::Colour);
 void refreshBandButtons();
 void drawCurve(juce::Graphics&,juce::Rectangle<float>,const std::array<std::atomic<float>,PQAudioProcessor::kBins>&,juce::Colour); void drawRef(juce::Graphics&,juce::Rectangle<float>,const std::array<std::atomic<float>,PQAudioProcessor::kBins>&,juce::Colour);
 void drawRangeMask(juce::Graphics&,juce::Rectangle<float>);
 // Minimal vertical bar meter (input/output level), drawn behind the trim slider of the same name.
 // peakDb draws the thin peak-hold line (see PQAudioProcessor::inputPeakDb/outputPeakDb).
 // FIX (item 1): now also draws dB graduation marks (0/-6/-12/-24/-40/-60) down the inside of the
 // meter, like a measuring cylinder, instead of being an unmarked bar.
 void drawVerticalMeter(juce::Graphics&,juce::Rectangle<float>,float levelDb,float peakDb,juce::Colour);
 // Horizontal dB gridlines + numeric labels (+20..-20dB) along the chart's left edge, plus the
 // 100Hz/10kHz frequency labels alongside the existing 20Hz/1kHz/20kHz ones.
 void drawFreqDbAxis(juce::Graphics&,juce::Rectangle<float>);

 // ---- Manual EQ chart geometry + interaction --------------------------------------------
 juce::Rectangle<float> chartArea; // recomputed every paint(); mouse handlers reuse it
 int draggingBand=-1; bool draggedPastThreshold=false; juce::Point<float> mouseDownPos;
 // FIX (item 4): the node last clicked (whether or not it's currently being dragged), so Delete/
 // Backspace has something to act on after mouseUp. Cleared on click-to-empty-space or deletion.
 int selectedBand=-1;
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
