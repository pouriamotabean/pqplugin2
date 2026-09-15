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

// Replaces the plain STEREO/MID/SIDE TextButtons with a rounded pill + coloured dot + label, matching
// the reference look. Toggle state is driven externally (from PQAudioProcessor::stereoOn/midOn/sideOn
// via refreshBandButtons()) rather than JUCE's own click-toggle, since the source of truth already
// lives on the processor - this is purely how that existing on/off state is drawn.
class DotTabButton : public juce::Button {
public:
    explicit DotTabButton(const juce::String& label):juce::Button(label){}
    void setDotColour(juce::Colour c){ colour=c; repaint(); }
    void paintButton(juce::Graphics& g, bool isMouseOver, bool) override {
        auto r=getLocalBounds().toFloat();
        bool on=getToggleState();
        g.setColour(juce::Colour(0xff101419)); g.fillRoundedRectangle(r,r.getHeight()*0.5f);
        g.setColour(on?colour.withAlpha(0.85f):juce::Colour(0xff242a31));
        g.drawRoundedRectangle(r.reduced(0.75f),r.getHeight()*0.5f,1.2f);
        float dotR=4.f, dotX=r.getX()+16.f, dotY=r.getCentreY();
        g.setColour(on?colour:colour.withAlpha(isMouseOver?0.6f:0.35f));
        g.fillEllipse(dotX-dotR,dotY-dotR,dotR*2.f,dotR*2.f);
        g.setColour(on?juce::Colours::white:juce::Colour(0xff737e89));
        g.setFont(juce::FontOptions(13).withStyle("bold"));
        float textX=dotX+dotR+10.f;
        g.drawText(getButtonText(), textX, 0.f, r.getRight()-textX-8.f, r.getHeight(), juce::Justification::centredLeft);
    }
private:
    juce::Colour colour{juce::Colours::white};
};

// Small square "manage presets" icon button (a vertical kebab/more-options glyph) - replaces the old
// wide PRESETS text button now that the preset list itself is always visible in the header (see
// PQAudioProcessorEditor::presetList); this just opens/closes the Save/Delete overlay (PresetPanel).
class KebabButton : public juce::Button {
public:
    KebabButton():juce::Button({}){}
    void paintButton(juce::Graphics& g, bool isMouseOver, bool) override {
        auto r=getLocalBounds().toFloat();
        g.setColour(juce::Colour(0xff101419)); g.fillRoundedRectangle(r,8.f);
        g.setColour(juce::Colour(0xff242a31)); g.drawRoundedRectangle(r.reduced(0.75f),8.f,1.f);
        g.setColour(isMouseOver?juce::Colour(0xfff2f4f7):juce::Colour(0xff737e89));
        float cx=r.getCentreX(), cy=r.getCentreY(), spacing=6.f, rad=1.7f;
        for(int i=-1;i<=1;++i) g.fillEllipse(cx-rad,cy+(float)i*spacing-rad,rad*2.f,rad*2.f);
    }
};

// ---- Preset panel (item 6, restructured) ------------------------------------------------------
// The preset list itself is now always visible in the main header (PQAudioProcessorEditor::
// presetList) rather than hidden inside this panel - picking a preset is a one-click affair, same
// as any normal dropdown. This panel is now just the "manage" popover (opened from the small kebab
// icon next to the list): naming and saving a new preset, or deleting the currently-selected one.
// A "preset" is still the plugin's full state (reference + manual EQ + width + match + trim) - see
// PQAudioProcessor::saveReference/loadReference/applyStateBlock - stored as ordinary .pqref files in
// a fixed folder so they show up in one list instead of scattered wherever a save dialog pointed.
class PresetPanel : public juce::Component {
public:
    PresetPanel(PQAudioProcessor& proc, juce::ComboBox& listRef);
    void resized() override;
    void paint(juce::Graphics&) override;
    void refreshList();
    // Called after a preset loads, so the host editor can resync every slider/button from the
    // processor's (now possibly very different) state.
    std::function<void()> onPresetLoaded;
private:
    PQAudioProcessor& p;
    juce::ComboBox& list; // lives in the main editor header now, not in this panel - see above
    juce::Label title{"", "MANAGE PRESETS"};
    juce::TextEditor nameBox;
    juce::TextButton saveBtn{"SAVE"}, deleteBtn{"DELETE"}, closeBtn{"X"};
    // I2: the reference curve alone, as its own .pqmatch file - separate from a full .pqref preset.
    // FileChoosers here are unrestricted (not locked to presetDir()), since the whole point is
    // handing this to another project or another person, not browsing a fixed local list.
    juce::TextButton exportMatchBtn{"EXPORT MATCH"}, importMatchBtn{"IMPORT MATCH"};
    std::unique_ptr<juce::FileChooser> activeChooser; // keeps the async FileChooser alive until it completes
    static juce::File presetDir();
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PresetPanel)
};

// I3: MATCH GAIN needs two distinct actions on one small button - left-click performs the match
// (the normal onClick), right-click just cycles which pair of meters it reads (Peak/RMS) without
// also performing a match. Plain TextButton::onClick fires for either mouse button, so the mode
// switch would otherwise also trigger a match every time; overriding mouseUp lets the two stay separate.
class MatchGainButton : public juce::TextButton {
public:
    using juce::TextButton::TextButton;
    std::function<void()> onRightClick;
    void mouseUp(const juce::MouseEvent& e) override {
        if(e.mods.isRightButtonDown() && onRightClick){ onRightClick(); return; }
        juce::TextButton::mouseUp(e);
    }
};

// The glow/colour-shift-on-drag effect requested for the horizontal sliders (WIDTH AMT, DEPTH%,
// LOW/HIGH HZ, and the three MATCH AMOUNT faders): the track fills with a soft glowing colour from
// the left up to the thumb, instead of JUCE's default flat grey track. One instance is shared by
// however many sliders should glow the same colour (set once via setGlowColour(), see the editor
// constructor); non-horizontal sliders fall back to the normal LookAndFeel_V4 drawing untouched.
class GlowSliderLookAndFeel : public juce::LookAndFeel_V4 {
public:
    void setGlowColour(juce::Colour c){ glowColour=c; }
    void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                           float minSliderPos, float maxSliderPos,
                           const juce::Slider::SliderStyle style, juce::Slider& slider) override {
        if(style!=juce::Slider::SliderStyle::LinearHorizontal){
            juce::LookAndFeel_V4::drawLinearSlider(g,x,y,width,height,sliderPos,minSliderPos,maxSliderPos,style,slider);
            return;
        }
        float trackH=4.f, cy=(float)y+(float)height*0.5f;
        juce::Rectangle<float> full((float)x,cy-trackH*0.5f,(float)width,trackH);
        g.setColour(juce::Colour(0xff242a31)); g.fillRoundedRectangle(full,trackH*0.5f);
        float filledW=juce::jmax(0.f, sliderPos-(float)x);
        if(filledW>1.f){
            juce::Rectangle<float> filled((float)x,cy-trackH*0.5f,filledW,trackH);
            juce::Path filledPath; filledPath.addRoundedRectangle(filled,trackH*0.5f);
            juce::DropShadow glow(glowColour.withAlpha(0.6f),10,juce::Point<int>(0,0));
            glow.drawForPath(g,filledPath);
            g.setColour(glowColour); g.fillRoundedRectangle(filled,trackH*0.5f);
        }
        float thumbR=6.5f;
        juce::Path thumbPath; thumbPath.addEllipse(sliderPos-thumbR,cy-thumbR,thumbR*2.f,thumbR*2.f);
        juce::DropShadow thumbGlow(glowColour.withAlpha(0.8f),9,juce::Point<int>(0,0));
        thumbGlow.drawForPath(g,thumbPath);
        g.setColour(juce::Colours::white); g.fillEllipse(sliderPos-thumbR,cy-thumbR,thumbR*2.f,thumbR*2.f);
        g.setColour(glowColour); g.drawEllipse(sliderPos-thumbR,cy-thumbR,thumbR*2.f,thumbR*2.f,1.5f);
    }
private:
    juce::Colour glowColour{juce::Colours::white};
};

// A real power/bypass glyph (the universal "circle with a break at top + vertical line through it")
// next to the text, instead of a plain text-only button - small detail, but this kind of thing is
// what reads as "made with care" rather than a placeholder. on/off state is pushed in externally via
// setOn() (see PQAudioProcessorEditor::refreshBypassButton()), same reasoning as DotTabButton above.
class BypassButton : public juce::TextButton {
public:
    using juce::TextButton::TextButton;
    void setOn(bool v){ on=v; repaint(); }
    void paintButton(juce::Graphics& g, bool isMouseOver, bool) override {
        auto r=getLocalBounds().toFloat();
        juce::Colour amber(0xffff9a3f);
        g.setColour(on?amber:juce::Colour(0xff101419)); g.fillRoundedRectangle(r,8.f);
        g.setColour(on?amber:(isMouseOver?juce::Colour(0xff3a4148):juce::Colour(0xff242a31))); g.drawRoundedRectangle(r.reduced(0.75f),8.f,1.f);
        float iconR=7.f, cx=r.getX()+22.f, cy=r.getCentreY();
        juce::Colour iconColour = on ? juce::Colours::black : juce::Colour(0xff737e89);
        juce::Path arc; arc.addCentredArc(cx,cy,iconR,iconR,0.f,juce::MathConstants<float>::pi*0.22f,juce::MathConstants<float>::pi*1.78f,true);
        g.setColour(iconColour); g.strokePath(arc,juce::PathStrokeType(1.8f));
        g.drawLine(cx,cy-iconR-1.f,cx,cy-2.f,1.8f);
        g.setColour(on?juce::Colours::black:juce::Colour(0xfff2f4f7));
        g.setFont(juce::FontOptions(13).withStyle("bold"));
        float textX=cx+iconR+10.f;
        g.drawText(getButtonText(),textX,0.f,r.getRight()-textX-8.f,r.getHeight(),juce::Justification::centredLeft);
    }
private:
    bool on=false;
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
 PQAudioProcessor& p; DotTabButton stereo{"STEREO"},mid{"MID"},side{"SIDE"}; juce::TextButton capture{"CAPTURE"},apply{"APPLY"},clear{"CLEAR"},widthStage{"PRE"}; BypassButton bypass{"BYPASS"}; MatchGainButton matchGainBtn{"MATCH GAIN"}; KebabButton presetsBtn;
 juce::Slider sAmt,mAmt,siAmt,low,high,width,depth;
 // Mono Maker (0-100%, log-mapped to a 20Hz-20kHz cutoff on Side - see PQAudioProcessor::
 // monoMakerAmount): sits in the MATCH AMOUNT column, right under STEREO/MID/SIDE, filling the space
 // that column otherwise leaves empty and giving it a genuine second purpose beyond match correction.
 juce::Slider monoMaker;
 // Max dB / Smoothing are no longer exposed as sliders (kept fixed at sane defaults in the
 // processor); this screen space now holds the input/output level meters + trim faders instead.
 juce::Slider inputTrim,outputTrim; juce::Rectangle<float> inputMeterArea,outputMeterArea;
 juce::ComboBox mode; juce::Label status;
 // FIX (preset restructure): the list is now a permanent, always-visible header control (like any
 // normal "Default v" dropdown) - declared before presetPanel because its constructor takes a
 // reference to this and member initialisation follows declaration order, not initialiser-list order.
 juce::ComboBox presetList;
 // FIX (item 6): SAVE/LOAD moved into presetPanel (a small overlay opened by the kebab icon), so the
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
 // One glow colour per slider group - see GlowSliderLookAndFeel above. sAmt/mAmt/siAmt use each
 // target's established colour; the rest share a neutral accent (same light blue as the PQ logo
 // gradient) since they don't have a colour identity of their own.
 GlowSliderLookAndFeel glowWhite, glowYellow, glowBlue, glowAccent;
 // I6: JUCE shows a tooltip automatically for any component with setTooltip() text, as long as one
 // TooltipWindow exists somewhere in the plugin's component tree - this is that one instance.
 juce::TooltipWindow tooltipWindow{this, 500};
 // I1: edge-detects capturingReference going false->true->false so the "REFERENCE CAPTURED" status
 // message fires exactly once, right when a capture finishes (see timerCallback in the .cpp).
 bool wasCapturingLastFrame=false;
 void timerCallback()override; void setupButton(juce::TextButton&,juce::Colour); void setupSlider(juce::Slider&,double,double,double); void setupVerticalTrim(juce::Slider&); void label(juce::Graphics&,juce::String,juce::Rectangle<float>,juce::Colour);
 void refreshBandButtons();
 void refreshBypassButton();
 void drawCurve(juce::Graphics&,juce::Rectangle<float>,const std::array<std::atomic<float>,PQAudioProcessor::kBins>&,juce::Colour); void drawRef(juce::Graphics&,juce::Rectangle<float>,const std::array<std::atomic<float>,PQAudioProcessor::kBins>&,juce::Colour);
 // Delta overlay: draws the auto-match correction curve itself (PQAudioProcessor::corrStereo/Mid/
 // Side) - i.e. what the automatic matching is actually adding/removing, which is exactly what
 // "difference between input and output" means for an EQ. Dashed so it reads as an overlay, not a
 // fourth solid curve competing with Stereo/Mid/Side.
 void drawDeltaCurve(juce::Graphics&,const std::array<std::atomic<float>,PQAudioProcessor::kBands>&,juce::Colour);
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
 // FIX (match amount too aggressive): see the constructor - the STEREO/MID/SIDE sliders' displayed
 // 0-100% maps to 0-kMatchAmountCap of actual applied correction, since adjacent EQ bands stacking
 // made 100% far more drastic than the label suggested.
 static constexpr float kMatchAmountCap=0.30f;
 float xToFreq(float x) const; float freqToX(float hz) const;
 float yToGainDb(float y) const; float gainDbToY(float gainDb) const;
 int findBandNear(juce::Point<float> pos) const; // returns index within grab radius, or -1
 void drawManualEq(juce::Graphics&);
 void showBandTypeMenu(int bandIndex, juce::Point<int> screenPos);
 void showAddBandMenu(juce::Point<float> chartPos, juce::Point<int> screenPos);
 static juce::String manualTypeLabel(PQAudioProcessor::ManualType);
 JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PQAudioProcessorEditor)
};
