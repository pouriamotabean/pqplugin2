#include "PluginEditor.h"
namespace {juce::Colour white(){return juce::Colour(0xfff2f4f7);} juce::Colour yellow(){return juce::Colour(0xffffcf3f);} juce::Colour blue(){return juce::Colour(0xff55a8ff);} juce::Colour bg(){return juce::Colour(0xff07090c);} juce::Colour panel(){return juce::Colour(0xff101419);} juce::Colour grid(){return juce::Colour(0xff242a31);} juce::Colour muted(){return juce::Colour(0xff737e89);} juce::Colour dim(){return juce::Colour(0xff3a4148);}
// Manual-EQ per-target colours (item 2): distinct from, but recognisably related to, the
// STEREO/MID/SIDE button colours above, so a glance at a node/curve tells you which signal it sits on.
juce::Colour manualStereoColour(){return juce::Colour(0xffc7cdd3);} // off-white/grey, family with white()
juce::Colour manualMidColour(){return juce::Colour(0xffb98a12);}    // darker than yellow()
juce::Colour manualSideColour(){return juce::Colour(0xff1f4e91);}   // navy, distinct from blue()
juce::Colour manualTargetColour(PQAudioProcessor::ManualTarget t){
    using MT=PQAudioProcessor::ManualTarget;
    switch(t){ case MT::Mid: return manualMidColour(); case MT::Side: return manualSideColour(); default: return manualStereoColour(); }
}
}

// ---- PresetPanel (restructured - see header comment) --------------------------------------------
juce::File PresetPanel::presetDir(){
    auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("PQ Presets");
    if(!dir.isDirectory()) dir.createDirectory();
    return dir;
}
PresetPanel::PresetPanel(PQAudioProcessor& proc, juce::ComboBox& listRef):p(proc),list(listRef){
    setOpaque(true);
    addAndMakeVisible(title); title.setColour(juce::Label::textColourId, white()); title.setFont(juce::FontOptions(12).withStyle("bold"));
    // `list` now lives in the main editor header (always visible there) - this panel doesn't add it
    // as a child or set its bounds, just wires the load behaviour onto it.
    list.setTextWhenNothingSelected("Select a preset...");
    list.onChange=[this]{
        auto name=list.getText(); if(name.isEmpty()) return;
        auto f=presetDir().getChildFile(name+".pqref");
        // FIX (B2): picking a preset used to load it instantly with no warning - one stray click
        // while mid-tweak could silently throw the work away. Only prompt when there's actually
        // something to lose (p.presetDirty, set by every manual-EQ edit/slider/button that changes
        // saveable state - see PQAudioProcessor::presetDirty); a clean state loads immediately as
        // before, since there's nothing to warn about.
        auto doLoad=[this,f]{ if(p.loadReference(f) && onPresetLoaded) onPresetLoaded(); };
        if(p.presetDirty.load()){
            auto opts=juce::MessageBoxOptions::makeOptionsOkCancel(
                juce::MessageBoxIconType::WarningIcon,"Unsaved Changes",
                "Loading this preset will discard your unsaved changes. Load anyway?","Load","Cancel",this);
            juce::AlertWindow::showAsync(opts,[this,doLoad](int result){ if(result==1) doLoad(); else refreshList(); /* revert the combo box's shown selection */ });
        } else doLoad();
    };
    addAndMakeVisible(nameBox); nameBox.setTextToShowWhenEmpty("New preset name...",muted()); nameBox.setColour(juce::TextEditor::backgroundColourId,bg()); nameBox.setColour(juce::TextEditor::textColourId,white());
    for(auto*b:{&saveBtn,&deleteBtn,&closeBtn}){ addAndMakeVisible(*b); b->setColour(juce::TextButton::buttonColourId,panel()); b->setColour(juce::TextButton::textColourOffId,white()); b->setColour(juce::TextButton::textColourOnId,white()); }
    saveBtn.onClick=[this]{
        auto name=nameBox.getText().trim();
        if(name.isEmpty()) name = "Preset " + juce::Time::getCurrentTime().formatted("%Y-%m-%d %H-%M-%S");
        // Strip characters that aren't safe in a filename, so a pasted-in name can't break the save.
        juce::String safe; for(auto c:name) safe += juce::CharacterFunctions::isLetterOrDigit(c)||c==' '||c=='-'||c=='_' ? juce::String::charToString(c) : juce::String();
        if(safe.isEmpty()) safe="Preset";
        auto f=presetDir().getChildFile(safe+".pqref");
        auto doSave=[this,f]{ p.saveReference(f); nameBox.setText({},juce::dontSendNotification); refreshList(); };
        // FIX (B2): saving over an existing name used to silently replace the file. Now confirms
        // first - only when the target file actually already exists, so the common case (a genuinely
        // new name) still saves in one click.
        if(f.existsAsFile()){
            auto opts=juce::MessageBoxOptions::makeOptionsOkCancel(
                juce::MessageBoxIconType::WarningIcon,"Overwrite Preset",
                "\""+safe+"\" already exists. Overwrite it?","Overwrite","Cancel",this);
            juce::AlertWindow::showAsync(opts,[doSave](int result){ if(result==1) doSave(); });
        } else doSave();
    };
    deleteBtn.onClick=[this]{
        auto name=list.getText(); if(name.isEmpty()) return;
        // FIX (B2): delete used to happen instantly on click with no way back. Always confirm now -
        // unlike Save/Load, there's no "safe, obviously-fine" case for a destructive delete to skip.
        auto opts=juce::MessageBoxOptions::makeOptionsOkCancel(
            juce::MessageBoxIconType::WarningIcon,"Delete Preset",
            "Permanently delete \""+name+"\"? This can't be undone.","Delete","Cancel",this);
        juce::AlertWindow::showAsync(opts,[this,name](int result){
            if(result!=1) return;
            presetDir().getChildFile(name+".pqref").deleteFile();
            refreshList();
        });
    };
    closeBtn.onClick=[this]{ setVisible(false); };
    addAndMakeVisible(exportMatchBtn); addAndMakeVisible(importMatchBtn);
    for(auto*b:{&exportMatchBtn,&importMatchBtn}){ b->setColour(juce::TextButton::buttonColourId,panel()); b->setColour(juce::TextButton::textColourOffId,white()); b->setColour(juce::TextButton::textColourOnId,white()); }
    exportMatchBtn.setTooltip("Save just the captured reference curve (not the rest of this preset) to share or reuse elsewhere.");
    importMatchBtn.setTooltip("Load a reference curve exported from here (or another session) without touching your current EQ/width/trim settings.");
    // I2: exports/imports ONLY the reference curve (see PQAudioProcessor::exportReferenceOnly/
    // importReferenceOnly) - deliberately an unrestricted FileChooser, not presetDir(), since the
    // point is moving this between projects/people, not adding it to the local preset list.
    exportMatchBtn.onClick=[this]{
        if(!p.hasReference.load()) return; // nothing captured yet - silently no-op rather than exporting an empty/meaningless curve
        activeChooser=std::make_unique<juce::FileChooser>("Export reference curve...", juce::File(), "*.pqmatch");
        activeChooser->launchAsync(juce::FileBrowserComponent::saveMode|juce::FileBrowserComponent::warnAboutOverwriting,
            [this](const juce::FileChooser& fc){
                auto f=fc.getResult(); if(f==juce::File()) return;
                if(!f.hasFileExtension(".pqmatch")) f=f.withFileExtension(".pqmatch");
                p.exportReferenceOnly(f);
            });
    };
    importMatchBtn.onClick=[this]{
        activeChooser=std::make_unique<juce::FileChooser>("Import reference curve...", juce::File(), "*.pqmatch");
        activeChooser->launchAsync(juce::FileBrowserComponent::openMode,
            [this](const juce::FileChooser& fc){
                auto f=fc.getResult(); if(f==juce::File()) return;
                if(p.importReferenceOnly(f) && onPresetLoaded) onPresetLoaded();
            });
    };
    refreshList();
}
void PresetPanel::refreshList(){
    list.clear(juce::dontSendNotification);
    auto files = presetDir().findChildFiles(juce::File::findFiles,false,"*.pqref");
    files.sort();
    int id=1;
    for(auto& f:files) list.addItem(f.getFileNameWithoutExtension(), id++);
    list.setSelectedId(0,juce::dontSendNotification);
}
void PresetPanel::paint(juce::Graphics& g){
    g.fillAll(panel());
    g.setColour(grid()); g.drawRect(getLocalBounds(),1);
}
void PresetPanel::resized(){
    // FIX (preset restructure): no more `list` row here - it's a permanent header control now, this
    // panel is purely name/save/delete.
    auto a=getLocalBounds().reduced(10);
    title.setBounds(a.removeFromTop(20));
    closeBtn.setBounds(getLocalBounds().getRight()-28,4,24,20);
    a.removeFromTop(10);
    nameBox.setBounds(a.removeFromTop(26));
    a.removeFromTop(8);
    auto row=a.removeFromTop(28);
    saveBtn.setBounds(row.removeFromLeft(row.getWidth()/2-4));
    row.removeFromLeft(8);
    deleteBtn.setBounds(row);
    a.removeFromTop(10);
    auto matchRow=a.removeFromTop(28);
    exportMatchBtn.setBounds(matchRow.removeFromLeft(matchRow.getWidth()/2-4));
    matchRow.removeFromLeft(8);
    importMatchBtn.setBounds(matchRow);
}

PQAudioProcessorEditor::PQAudioProcessorEditor(PQAudioProcessor&x):AudioProcessorEditor(&x),p(x),presetPanel(x,presetList){setResizable(true,true);setSize(1320,760);
 setWantsKeyboardFocus(true); // FIX (item 4): needed so this component (not a child control) receives Delete/Backspace
 for(auto*t:{&stereo,&mid,&side}) addAndMakeVisible(*t);
 stereo.setDotColour(white()); mid.setDotColour(yellow()); side.setDotColour(blue());
 for(auto*q:{&capture,&apply,&clear})setupButton(*q,white());
 capture.setTooltip("Capture a 5-second running average of the live spectrum as the reference target to match against.");
 apply.setTooltip("Recompute the correction curve for the currently checked target(s) from the captured reference.");
 clear.setTooltip("Discard the captured reference and remove all automatic correction.");
 setupButton(widthStage,white()); widthStage.setButtonText(p.widthPostEq.load()?"POST":"PRE");
 widthStage.setTooltip("PRE: widening happens before EQ correction (the analyzer/match hears the widened signal). POST: widening is the last step, after correction.");
 // Toggles whether the mono-widener runs before the EQ correction (PRE, so the analyzer/match
 // "hears" the widened signal) or after it (POST, widening is the very last step on the output).
 widthStage.onClick=[this]{ bool now=!p.widthPostEq.load(); p.widthPostEq.store(now); widthStage.setButtonText(now?"POST":"PRE"); p.presetDirty.store(true); };

 // FIX (item 4): STEREO/MID/SIDE are now independent on/off toggles instead of a mutually-exclusive
 // solo, so any combination - one alone, two together, or all three - can be shown on the analyzer
 // and heard in the output. See PQAudioProcessor::stereoOn/midOn/sideOn.
 stereo.onClick=[this]{p.stereoOn.store(!p.stereoOn.load()); refreshBandButtons();};
 mid.onClick=[this]{p.midOn.store(!p.midOn.load()); refreshBandButtons();};
 side.onClick=[this]{p.sideOn.store(!p.sideOn.load()); refreshBandButtons();};
 stereo.setTooltip("Show/hide the Stereo (combined L+R) curve on the analyzer.");
 mid.setTooltip("Show/hide the Mid (mono-summed) curve on the analyzer.");
 side.setTooltip("Show/hide the Side (stereo difference) curve on the analyzer.");
 refreshBandButtons();

 // FIX: one show/hide dot per manual-EQ target, tinted to match that target's node/curve colour so
 // it's obvious at a glance which switch controls which line. All three now start OFF (hollow ring,
 // unchecked) - no line exists/edits by default. Switching one ON both reveals that target's curve
 // AND arms it as the target for the next node you add on the chart (see updateActiveTargetStack);
 // switching it back off disarms it. This replaces the old behaviour where a new node always
 // defaulted to Stereo and had to be moved to Mid/Side afterwards via right-click.
 for(auto* t:{&stereoEqToggle,&midEqToggle,&sideEqToggle}) { addAndMakeVisible(*t); t->setToggleState(false,juce::dontSendNotification); }
 stereoEqToggle.onClick=[this]{ updateActiveTargetStack(PQAudioProcessor::ManualTarget::Stereo, stereoEqToggle.getToggleState()); repaint(); };
 midEqToggle.onClick=[this]{ updateActiveTargetStack(PQAudioProcessor::ManualTarget::Mid, midEqToggle.getToggleState()); repaint(); };
 sideEqToggle.onClick=[this]{ updateActiveTargetStack(PQAudioProcessor::ManualTarget::Side, sideEqToggle.getToggleState()); repaint(); };
 stereoEqToggle.setDotColour(manualStereoColour());
 midEqToggle.setDotColour(manualMidColour());
 sideEqToggle.setDotColour(manualSideColour());
 stereoEqToggle.setTooltip("Show manual EQ bands on Stereo, and make Stereo the target for new bands you add.");
 midEqToggle.setTooltip("Show manual EQ bands on Mid, and make Mid the target for new bands you add.");
 sideEqToggle.setTooltip("Show manual EQ bands on Side, and make Side the target for new bands you add.");

 // FIX (match amount too aggressive): full-scale on these sliders used to mean 100% of
 // maxCorrectionDb applied per band - since adjacent bands' corrections stack when several want to
 // push the same region the same way, that read as far more drastic than "100%" suggests. The
 // slider's own range/display stays 0-100% (so it still reads naturally), but what actually reaches
 // the DSP is capped at kMatchAmountCap of that - moving the slider all the way now applies what
 // used to be ~30%, which is where it stopped sounding musical in testing.
 auto bind=[this](juce::Slider&s,std::atomic<float>&v){setupSlider(s,0,100,.1);s.setValue(v.load()/kMatchAmountCap*100.f);s.setDoubleClickReturnValue(true,0.0);s.onValueChange=[this,&s,&v]{v.store((float)s.getValue()/100.f*kMatchAmountCap);p.applyMatch();p.presetDirty.store(true);};};bind(sAmt,p.stereoMatch);bind(mAmt,p.midMatch);bind(siAmt,p.sideMatch);
 sAmt.setTooltip("How strongly the captured reference corrects the Stereo signal, 0-100%.");
 mAmt.setTooltip("How strongly the captured reference corrects the Mid signal, 0-100%.");
 siAmt.setTooltip("How strongly the captured reference corrects the Side signal, 0-100%.");
 // Fader glow (per earlier request): track lights up from the left as you drag right, in each
 // slider's own colour where one exists (Stereo/Mid/Side), or a neutral accent otherwise.
 glowWhite.setGlowColour(white()); glowYellow.setGlowColour(yellow()); glowBlue.setGlowColour(blue()); glowAccent.setGlowColour(juce::Colour(0xffbfe0ff));
 sAmt.setLookAndFeel(&glowWhite); mAmt.setLookAndFeel(&glowYellow); siAmt.setLookAndFeel(&glowBlue);
 setupSlider(low,20,20000,1);setupSlider(high,20,20000,1);setupSlider(width,0,100,.1);setupSlider(depth,0,200,1);
 for(auto*s:{&low,&high,&width,&depth}) s->setLookAndFeel(&glowAccent);
 low.setTooltip("Correction is only applied above this frequency.");
 high.setTooltip("Correction is only applied below this frequency.");
 width.setTooltip("How much of the mono-widener effect to blend in, 0-100%.");
 depth.setTooltip("Scales the widener's delay/depth character - higher pushes the effect further.");
 // Mono Maker: 0% = no effect, 100% = the whole signal collapses to mono. Fixed 24dB/oct slope,
 // always runs after everything else (width included) regardless of PRE/POST - see processBlock.
 setupSlider(monoMaker,0,100,.1); monoMaker.setLookAndFeel(&glowAccent);
 monoMaker.setValue(p.monoMakerAmount.load()*100.0); monoMaker.setDoubleClickReturnValue(true,0.0);
 monoMaker.setTooltip("Collapses Side to mono below a rising cutoff - 0% off, 100% the whole signal goes mono.");
 monoMaker.onValueChange=[this]{ p.monoMakerAmount.store((float)monoMaker.getValue()/100.f); p.manualDirty.store(true); p.presetDirty.store(true); };
 // FIX: the frequency chart is drawn on a log scale (20Hz-20kHz), but these sliders were linear -
 // that mismatch is exactly why dragging near 20Hz raced across the whole chart while dragging near
 // 20kHz barely moved the line. A log-style skew around the geometric middle of the range
 // (sqrt(20*20000)) makes the slider's feel match what's actually drawn.
 low.setSkewFactorFromMidPoint(632.45); high.setSkewFactorFromMidPoint(632.45);
 low.setValue(p.lowHz);high.setValue(p.highHz);width.setValue(p.widthAmount.load()*100);depth.setValue(p.widthDepth.load()*100);
 // Double-click any of these to snap back to its default - cheap, standard JUCE behaviour, and one
 // less reason to hunt for an exact "reset" value by hand.
 low.setDoubleClickReturnValue(true,20.0); high.setDoubleClickReturnValue(true,20000.0);
 width.setDoubleClickReturnValue(true,0.0); depth.setDoubleClickReturnValue(true,100.0);
 low.onValueChange=[this]{p.lowHz=low.getValue();p.applyMatch();p.presetDirty.store(true);};high.onValueChange=[this]{p.highHz=high.getValue();p.applyMatch();p.presetDirty.store(true);};width.onValueChange=[this]{p.widthAmount=width.getValue()/100.f;p.presetDirty.store(true);};depth.onValueChange=[this]{p.widthDepth=depth.getValue()/100.f;p.presetDirty.store(true);};

 // Max dB / Smoothing used to live here as sliders; they're fixed at sane working values in the
 // processor now (6dB / 0.35 octaves) and no longer exposed. This slot is now the input/output
 // level meters, each doubling as a +/-24dB trim fader, plus a MATCH GAIN button between them.
 setupVerticalTrim(inputTrim); setupVerticalTrim(outputTrim);
 inputTrim.setRange(-24,24,.1); outputTrim.setRange(-24,24,.1);
 inputTrim.setValue(p.inputTrimDb.load()); outputTrim.setValue(p.outputTrimDb.load());
 inputTrim.setDoubleClickReturnValue(true,0.0); outputTrim.setDoubleClickReturnValue(true,0.0);
 inputTrim.setTooltip("Input trim, +/-24dB, applied before any processing.");
 outputTrim.setTooltip("Output trim, +/-24dB, applied after everything else - this is what MATCH GAIN adjusts.");
 inputTrim.onValueChange=[this]{p.inputTrimDb.store((float)inputTrim.getValue());p.presetDirty.store(true);};
 outputTrim.onValueChange=[this]{p.outputTrimDb.store((float)outputTrim.getValue());p.presetDirty.store(true);};
 setupButton(matchGainBtn,white());
 auto refreshMatchGainLabel=[this]{ matchGainBtn.setButtonText(p.gainMatchMode.load()==PQAudioProcessor::GainMatchMode::Peak?"MATCH PEAK":"MATCH RMS"); };
 refreshMatchGainLabel();
 matchGainBtn.setTooltip("Left-click: nudge output trim so it matches the input. Right-click: switch between matching Peak or RMS level.");
 matchGainBtn.onClick=[this]{ p.matchGain(); outputTrim.setValue(p.outputTrimDb.load(),juce::dontSendNotification); status.setText(p.gainMatchMode.load()==PQAudioProcessor::GainMatchMode::Peak?"PEAK MATCHED":"RMS MATCHED",juce::dontSendNotification); };
 matchGainBtn.onRightClick=[this,refreshMatchGainLabel]{
     bool nowPeak = p.gainMatchMode.load()==PQAudioProcessor::GainMatchMode::Peak;
     p.gainMatchMode.store(nowPeak?PQAudioProcessor::GainMatchMode::Rms:PQAudioProcessor::GainMatchMode::Peak);
     p.presetDirty.store(true);
     refreshMatchGainLabel();
     status.setText(juce::String("MATCH MODE: ")+(nowPeak?"RMS":"PEAK"),juce::dontSendNotification);
 };
 mode.addItem("MICRO SHIFT",1);mode.addItem("HAAS",2);mode.addItem("DECORRELATED",3);mode.setSelectedId((int)p.widthMode.load()+1);mode.onChange=[this]{p.widthMode=(PQAudioProcessor::WidthMode)(mode.getSelectedId()-1);p.presetDirty.store(true);};
 mode.setTooltip("How mono content is turned into stereo width: Micro Shift (subtle), Haas (delay-based), or Decorrelated (dual-tap, widest).");
 capture.onClick=[this]{p.captureReference();};apply.onClick=[this]{p.applyMatch();status.setText("MATCH UPDATED",juce::dontSendNotification);};clear.onClick=[this]{p.clearReference();status.setText("REFERENCE CLEARED",juce::dontSendNotification);};

 // FIX (preset restructure): the list itself now sits permanently in the header (styled like a
 // normal dropdown, not hidden behind a button) - picking a preset is a single click, same as any
 // other combo box. The kebab icon next to it opens the small "manage" overlay (rename-less for now,
 // just name/save/delete) only when you actually need it.
 addAndMakeVisible(presetList);
 presetList.setColour(juce::ComboBox::backgroundColourId,panel());
 presetList.setColour(juce::ComboBox::textColourId,white());
 presetList.setColour(juce::ComboBox::outlineColourId,grid());
 presetList.setColour(juce::ComboBox::arrowColourId,muted());
 presetList.setTooltip("Load a saved preset.");
 addAndMakeVisible(presetsBtn);
 presetsBtn.setTooltip("Save the current settings as a new preset, or delete/export the selected one.");
 presetsBtn.onClick=[this]{ presetPanel.setVisible(!presetPanel.isVisible()); if(presetPanel.isVisible()) presetPanel.toFront(true); };
 presetPanel.onPresetLoaded=[this]{ syncControlsFromProcessor(); status.setText("PRESET LOADED",juce::dontSendNotification); };
 addChildComponent(presetPanel); // starts hidden

 // Simple true-bypass toggle - deliberately not persisted (see PQAudioProcessor::bypassed), so it
 // always opens un-bypassed regardless of what preset/session is loaded.
 addAndMakeVisible(bypass);
 bypass.setTooltip("Bypass all correction/EQ/width - hear the raw (trim-adjusted) input for A/B comparison. Shortcut: Space.");
 bypass.onClick=[this]{ p.bypassed.store(!p.bypassed.load()); refreshBypassButton(); };
 refreshBypassButton();

 addAndMakeVisible(mode);addAndMakeVisible(status);status.setColour(juce::Label::textColourId,muted());status.setJustificationType(juce::Justification::centredRight);startTimerHz(20);
}
PQAudioProcessorEditor::~PQAudioProcessorEditor(){
    for(auto*s:{&sAmt,&mAmt,&siAmt,&low,&high,&width,&depth,&monoMaker}) s->setLookAndFeel(nullptr);
    setLookAndFeel(nullptr);
}

// I1: reflect the background averaging capture (see PQAudioProcessor::captureReference/
// analyzeAndUpdate) on the CAPTURE button itself - disabled + showing live progress while it runs,
// so a second click can't restart/confuse an in-progress capture.
void PQAudioProcessorEditor::timerCallback(){
    bool capturing=p.capturingReference.load();
    if(capturing){
        capture.setEnabled(false);
        capture.setButtonText("CAPTURING "+juce::String((int)(p.captureProgress.load()*100.f))+"%");
    } else {
        capture.setEnabled(true);
        capture.setButtonText("CAPTURE");
        if(wasCapturingLastFrame) status.setText("REFERENCE CAPTURED",juce::dontSendNotification);
    }
    wasCapturingLastFrame=capturing;
    repaint();
}

// FIX (item 6): after a preset load, far more than just the reference curve may have changed
// (manual EQ is read straight from the processor every paint, but the plain juce::Slider/ComboBox
// controls below the chart cache their own value and need to be told explicitly).
void PQAudioProcessorEditor::syncControlsFromProcessor(){
 sAmt.setValue(p.stereoMatch.load()/kMatchAmountCap*100.f,juce::dontSendNotification);
 mAmt.setValue(p.midMatch.load()/kMatchAmountCap*100.f,juce::dontSendNotification);
 siAmt.setValue(p.sideMatch.load()/kMatchAmountCap*100.f,juce::dontSendNotification);
 low.setValue(p.lowHz.load(),juce::dontSendNotification);
 high.setValue(p.highHz.load(),juce::dontSendNotification);
 width.setValue(p.widthAmount.load()*100,juce::dontSendNotification);
 depth.setValue(p.widthDepth.load()*100,juce::dontSendNotification);
 monoMaker.setValue(p.monoMakerAmount.load()*100.0,juce::dontSendNotification);
 mode.setSelectedId((int)p.widthMode.load()+1,juce::dontSendNotification);
 widthStage.setButtonText(p.widthPostEq.load()?"POST":"PRE");
 inputTrim.setValue(p.inputTrimDb.load(),juce::dontSendNotification);
 outputTrim.setValue(p.outputTrimDb.load(),juce::dontSendNotification);
 matchGainBtn.setButtonText(p.gainMatchMode.load()==PQAudioProcessor::GainMatchMode::Peak?"MATCH PEAK":"MATCH RMS");
 repaint();
}

// See header: the toggle that was most recently switched ON is where new manual-EQ nodes go.
// Several toggles can be on together (each keeps showing/editing its own curve); only the stack
// order changes which one is "armed" for the very next added node.
void PQAudioProcessorEditor::updateActiveTargetStack(PQAudioProcessor::ManualTarget t, bool on){
    activeManualTargets.removeAllInstancesOf(t);
    if(on) activeManualTargets.add(t);
}

void PQAudioProcessorEditor::refreshBandButtons(){
 stereo.setToggleState(p.stereoOn.load(),juce::dontSendNotification); stereo.repaint();
 mid.setToggleState(p.midOn.load(),juce::dontSendNotification); mid.repaint();
 side.setToggleState(p.sideOn.load(),juce::dontSendNotification); side.repaint();
}
// Bypass reads as muted grey with a hollow power icon when off, and switches to a solid amber fill
// with a filled black icon when engaged - unmistakable at a glance, the way a hardware bypass
// switch's LED would be. See BypassButton::paintButton for the actual drawing.
void PQAudioProcessorEditor::refreshBypassButton(){
 bypass.setOn(p.bypassed.load());
}

void PQAudioProcessorEditor::setupButton(juce::TextButton&b,juce::Colour c){addAndMakeVisible(b);b.setColour(juce::TextButton::buttonColourId,panel());b.setColour(juce::TextButton::buttonOnColourId,grid());b.setColour(juce::TextButton::textColourOffId,c);b.setColour(juce::TextButton::textColourOnId,c);}
void PQAudioProcessorEditor::setupSlider(juce::Slider&s,double a,double b,double step){addAndMakeVisible(s);s.setSliderStyle(juce::Slider::LinearHorizontal);s.setTextBoxStyle(juce::Slider::TextBoxRight,false,66,20);s.setRange(a,b,step);s.setColour(juce::Slider::thumbColourId,white());s.setColour(juce::Slider::trackColourId,grid());s.setColour(juce::Slider::textBoxTextColourId,white());s.setColour(juce::Slider::textBoxBackgroundColourId,panel());}
// Thin vertical trim fader meant to sit directly on top of drawVerticalMeter() for the same
// rectangle: no text box, transparent track (the meter bar underneath already reads as the track),
// a slim pale marker for the thumb - just enough to find and drag, without reading as its own
// separate control competing with the level bar for attention.
void PQAudioProcessorEditor::setupVerticalTrim(juce::Slider&s){addAndMakeVisible(s);s.setSliderStyle(juce::Slider::LinearVertical);s.setTextBoxStyle(juce::Slider::NoTextBox,false,0,0);s.setColour(juce::Slider::thumbColourId,juce::Colours::white.withAlpha(0.55f));s.setColour(juce::Slider::trackColourId,juce::Colours::transparentBlack);s.setColour(juce::Slider::backgroundColourId,juce::Colours::transparentBlack);}
void PQAudioProcessorEditor::label(juce::Graphics&g,juce::String t,juce::Rectangle<float>r,juce::Colour c){g.setColour(c);g.setFont(juce::FontOptions(10));g.drawText(t,r,juce::Justification::left);}
namespace {
// FIX (low-end still looked steppy after the FFT/averaging fixes): the path used to connect raw
// points with straight lines, which draws every remaining quantization step as a visible corner.
// Routing each segment through a quadratic curve (control point = the real point, end point = the
// midpoint to the next one) keeps the same data but removes the sharp corners, which is what read
// as a smooth curve at the top of the spectrum and a "pixelated line" at the bottom.
void addSmoothedPoint(juce::Path& q, juce::Point<float> prev, juce::Point<float> cur, bool first){
    if(first){ q.startNewSubPath(prev); return; }
    juce::Point<float> mid=prev+(cur-prev)*0.5f;
    q.quadraticTo(prev,mid);
}
// FIX (glow not showing on jagged curves): this used to be a single vertical gradient anchored to
// the curve's overall peak point, fading to transparent over a fixed distance below it. That works
// for a smooth curve that stays near one height, but for the jagged live analyzer curve - which
// swings from near +20 down to -20 across the chart - only the few pixels right at the single
// highest peak ever fell inside that fade window; everywhere else the gradient had already reached
// zero alpha long before reaching the actual curve at that x position, so it looked like no glow at
// all. A flat, uniform semi-transparent fill (same colour as the line, one alpha, no gradient) is
// both simpler and what was actually asked for - a plain translucent area under the curve everywhere
// along its length, not just near the peak.
void glowUnder(juce::Graphics& g, const juce::Path& lineOnly, float bottom, float left, float right, juce::Colour c, float alpha=0.18f){
    juce::Path fillPath(lineOnly);
    fillPath.lineTo(right,bottom);
    fillPath.lineTo(left,bottom);
    fillPath.closeSubPath();
    g.setColour(c.withAlpha(alpha));
    g.fillPath(fillPath);
}
}
void PQAudioProcessorEditor::drawCurve(juce::Graphics&g,juce::Rectangle<float>r,const std::array<std::atomic<float>,PQAudioProcessor::kBins>&a,juce::Colour c){
 juce::Path q; juce::Point<float> prev;
 for(int i=0;i<PQAudioProcessor::kBins;i++){
     float x=r.getX()+r.getWidth()*i/(PQAudioProcessor::kBins-1.f),db=a[i].load(),y=r.getBottom()-r.getHeight()*juce::jlimit(0.f,1.f,(db+90)/96.f);
     juce::Point<float> cur(x,y);
     addSmoothedPoint(q,prev,cur,i==0);
     prev=cur;
 }
 q.lineTo(prev);
 // FIX (bug: giant diagonal shadow triangle) - DropShadow::drawForPath() FILLS whatever path you
 // give it before blurring it. `q` here is an open polyline (left edge of the chart to the right
 // edge) with no closing segment, so JUCE was implicitly closing it with a straight line from the
 // last point back to the first - exactly the diagonal edge that was showing up as a huge black
 // triangle. The fix is to shadow the actual STROKED ribbon-shape of the line (its outline as a thin
 // closed shape hugging the curve on both sides) instead of the raw center-line path.
 juce::Path strokeShape; juce::PathStrokeType(1.8f).createStrokedPath(strokeShape,q);
 juce::DropShadow curveShadow(juce::Colours::black.withAlpha(0.55f),8,juce::Point<int>(0,3));
 curveShadow.drawForPath(g,strokeShape);
 glowUnder(g,q,r.getBottom(),r.getX(),r.getRight(),c,0.16f);
 g.setColour(c);g.strokePath(q,juce::PathStrokeType(1.8f));
}
void PQAudioProcessorEditor::drawRef(juce::Graphics&g,juce::Rectangle<float>r,const std::array<std::atomic<float>,PQAudioProcessor::kBins>&a,juce::Colour c){
 juce::Path q; juce::Point<float> prev;
 for(int i=0;i<PQAudioProcessor::kBins;i++){
     float x=r.getX()+r.getWidth()*i/(PQAudioProcessor::kBins-1.f),db=a[i].load(),y=r.getBottom()-r.getHeight()*juce::jlimit(0.f,1.f,(db+90)/96.f);
     juce::Point<float> cur(x,y);
     addSmoothedPoint(q,prev,cur,i==0);
     prev=cur;
 }
 q.lineTo(prev);
 g.setColour(c.withAlpha(.22f));g.strokePath(q,juce::PathStrokeType(1.f));
}

// Delta overlay: renders corrStereo/corrMid/corrSide (the actual per-band correction gain the
// auto-match is applying, in dB) using the same log-frequency x-mapping and +/-kManualGainRangeDb
// y-mapping (freqToX/gainDbToY) as the manual EQ chart, so it lines up exactly with what the manual
// nodes are doing. Points are placed at the kBands correction-band centre frequencies
// (PQAudioProcessor::bandHz), not the kBins analyzer resolution - there are far fewer of them, so
// the curve is naturally smooth without needing the analyzer's own smoothing pass.
void PQAudioProcessorEditor::drawDeltaCurve(juce::Graphics& g, const std::array<std::atomic<float>,PQAudioProcessor::kBands>& corr, juce::Colour c){
    juce::Path q;
    for(int i=0;i<PQAudioProcessor::kBands;++i){
        float t=i/float(PQAudioProcessor::kBands-1);
        float hz=std::exp(std::log(20.f)+t*(std::log(20000.f)-std::log(20.f)));
        float x=freqToX(hz), y=gainDbToY(corr[(size_t)i].load());
        if(i==0) q.startNewSubPath(x,y); else q.lineTo(x,y);
    }
    juce::Path dashed; float dashLengths[]={5.f,4.f};
    juce::PathStrokeType(1.4f).createDashedStroke(dashed,q,dashLengths,2);
    g.setColour(c.withAlpha(0.6f)); g.fillPath(dashed);
}

// New: shades the parts of the analyzer that fall outside the current Low/High Hz match range,
// so the frequency-range sliders now have a visible effect on the chart itself (this was
// previously invisible - the range only affected the DSP, not what you could see).
void PQAudioProcessorEditor::drawRangeMask(juce::Graphics&g,juce::Rectangle<float>r){
 auto xForHz=[&](float hz){ float t=(std::log10(juce::jlimit(20.f,20000.f,hz))-std::log10(20.f))/(std::log10(20000.f)-std::log10(20.f)); return r.getX()+r.getWidth()*t; };
 float lo=p.lowHz.load(), hi=p.highHz.load();
 float xLo=xForHz(lo), xHi=xForHz(hi);
 g.setColour(juce::Colours::black.withAlpha(.45f));
 if (xLo>r.getX()) g.fillRect(juce::Rectangle<float>(r.getX(),r.getY(),xLo-r.getX(),r.getHeight()));
 if (xHi<r.getRight()) g.fillRect(juce::Rectangle<float>(xHi,r.getY(),r.getRight()-xHi,r.getHeight()));
 g.setColour(muted().withAlpha(.6f));
 g.drawVerticalLine((int)xLo, r.getY(), r.getBottom());
 g.drawVerticalLine((int)xHi, r.getY(), r.getBottom());
}

// Minimal vertical bar meter for input/output level (-60dB..0dB mapped to the full height).
// FIX ("ناب" cleanup): dropped the 0/-6/-12/-24/-40/-60 graduation ticks entirely - on a meter this
// narrow they read as clutter more than a usable scale, and the live number already printed above/
// below the bar (see paint()) gives the precise reading they were trying to provide. The flat fill
// is replaced with a soft vertical gradient (dim at the bottom, full colour at the top) so the bar
// itself looks less like a flat block and more like a proper level meter.
void PQAudioProcessorEditor::drawVerticalMeter(juce::Graphics&g,juce::Rectangle<float>r,float levelDb,float peakDb,juce::Colour c){
 g.setColour(panel()); g.fillRoundedRectangle(r,5.f);
 g.setColour(grid()); g.drawRoundedRectangle(r,5.f,1.f);
 constexpr float kFloorDb=-60.f;
 auto fillR=r.reduced(4.f);
 // FIX (requested): faint graduation ticks behind the fill, so the ear can be backed up by the eye -
 // no numbers (that was the earlier, busier version we deliberately simplified away from), just short
 // marks at a few standard reference points, dim enough to read as texture rather than clutter.
 {
     static const float ticks[]={0.f,-6.f,-12.f,-24.f,-40.f};
     g.setColour(juce::Colours::white.withAlpha(0.10f));
     for(float db:ticks){
         float tt=juce::jlimit(0.f,1.f,(db-kFloorDb)/(0.f-kFloorDb));
         float ty=fillR.getBottom()-fillR.getHeight()*tt;
         g.fillRect(juce::Rectangle<float>(fillR.getX()+2.f,ty-0.5f,fillR.getWidth()-4.f,1.f));
     }
 }
 float t=juce::jlimit(0.f,1.f,(levelDb-kFloorDb)/(0.f-kFloorDb));
 float fillH=fillR.getHeight()*t;
 auto bar=juce::Rectangle<float>(fillR.getX(),fillR.getBottom()-fillH,fillR.getWidth(),fillH);
 juce::ColourGradient grad(c.withAlpha(.35f),bar.getX(),fillR.getBottom(),
                            t>0.92f?yellow():c.withAlpha(.95f),bar.getX(),fillR.getY(),false);
 g.setGradientFill(grad); g.fillRoundedRectangle(bar,4.f);
 // Peak-hold indicator: a thin line at the highest recent peak, which the processor releases
 // slowly rather than snapping straight to the current level (see PQAudioProcessor::processBlock).
 float tp=juce::jlimit(0.f,1.f,(peakDb-kFloorDb)/(0.f-kFloorDb));
 float py=fillR.getBottom()-fillR.getHeight()*tp;
 g.setColour(juce::Colours::white.withAlpha(.85f));
 g.fillRect(juce::Rectangle<float>(fillR.getX(),py-0.75f,fillR.getWidth(),1.5f));
}

void PQAudioProcessorEditor::drawFreqDbAxis(juce::Graphics& g, juce::Rectangle<float> chart){
 // Horizontal dB gridlines + numeric labels, tied to the same +/-kManualGainRangeDb coordinate
 // system the manual EQ nodes use (gainDbToY), so the axis actually matches what dragging a node
 // does to the chart.
 static const float dbTicks[]={20.f,10.f,0.f,-10.f,-20.f};
 g.setFont(juce::FontOptions(9));
 for(float db:dbTicks){
     float y=gainDbToY(db);
     g.setColour(grid().withAlpha(db==0.f?0.9f:0.5f));
     g.drawHorizontalLine((int)y,chart.getX(),chart.getRight());
     g.setColour(muted());
     juce::String txt=(db>0.f?"+":"")+juce::String((int)db);
     g.drawText(txt,chart.getX()+4.f,y-11.f,32.f,12.f,juce::Justification::left);
 }
 // Extra frequency labels alongside the existing 20Hz/1kHz/20kHz ones drawn in paint().
 auto freqLabel=[&](float hz,juce::String txt){
     float x=freqToX(hz);
     g.setColour(muted());
     g.drawText(txt,x-20.f,chart.getBottom()-18.f,40.f,18.f,juce::Justification::centred);
 };
 freqLabel(100.f,"100 Hz");
 freqLabel(10000.f,"10 kHz");
}

// ---- Manual EQ chart geometry -------------------------------------------------------------
float PQAudioProcessorEditor::xToFreq(float x) const{ float t=juce::jlimit(0.f,1.f,(x-chartArea.getX())/chartArea.getWidth()); return std::pow(10.f, std::log10(20.f)+t*(std::log10(20000.f)-std::log10(20.f))); }
float PQAudioProcessorEditor::freqToX(float hz) const{ float t=(std::log10(juce::jlimit(20.f,20000.f,hz))-std::log10(20.f))/(std::log10(20000.f)-std::log10(20.f)); return chartArea.getX()+chartArea.getWidth()*t; }
float PQAudioProcessorEditor::yToGainDb(float y) const{ float t=juce::jlimit(0.f,1.f,(y-chartArea.getY())/chartArea.getHeight()); return juce::jmap(t,0.f,1.f,kManualGainRangeDb,-kManualGainRangeDb); }
float PQAudioProcessorEditor::gainDbToY(float gainDb) const{ float t=juce::jmap(juce::jlimit(-kManualGainRangeDb,kManualGainRangeDb,gainDb),kManualGainRangeDb,-kManualGainRangeDb,0.f,1.f); return chartArea.getY()+chartArea.getHeight()*t; }

int PQAudioProcessorEditor::findBandNear(juce::Point<float> pos) const{
    constexpr float grabRadius=14.f; int best=-1; float bestDist=grabRadius;
    using MT=PQAudioProcessor::ManualTarget;
    for(int i=0;i<PQAudioProcessor::kMaxManualBands;++i){
        auto& mb=p.manualBands[(size_t)i]; if(!mb.active.load()) continue;
        // FIX (item 3): a hidden target's nodes shouldn't be grabbable either - otherwise unchecking
        // a line still lets you accidentally drag an invisible node.
        auto target=mb.target.load();
        bool visible = target==MT::Mid ? midEqToggle.getToggleState() : target==MT::Side ? sideEqToggle.getToggleState() : stereoEqToggle.getToggleState();
        if(!visible) continue;
        juce::Point<float> node(freqToX(mb.freq.load()), gainDbToY(mb.gainDb.load()));
        float d=node.getDistanceFrom(pos);
        if(d<bestDist){ bestDist=d; best=i; }
    }
    return best;
}

juce::String PQAudioProcessorEditor::manualTypeLabel(PQAudioProcessor::ManualType t){
    using T=PQAudioProcessor::ManualType;
    switch(t){ case T::Bell:return "BELL"; case T::LowShelf:return "LOW SHELF"; case T::HighShelf:return "HIGH SHELF"; case T::LowCut:return "LOW CUT"; case T::HighCut:return "HIGH CUT"; case T::Notch:return "NOTCH"; }
    return {};
}

void PQAudioProcessorEditor::drawManualEq(juce::Graphics& g){
    using T=PQAudioProcessor::ManualType; using MT=PQAudioProcessor::ManualTarget;
    // FIX (item 1): a 0dB reference line is always visible, even with zero active bands, so the
    // chart never looks "empty/broken" the moment it's opened.
    {
        float y0=gainDbToY(0.f);
        g.setColour(muted().withAlpha(0.55f));
        g.drawLine(chartArea.getX(),y0,chartArea.getRight(),y0,1.0f);
    }
    // Per-target response curve, computed purely for display (matches the DSP formulas in
    // PluginProcessor's makeManualCoeff, evaluated as a magnitude response instead of run as audio).
    // Split by target (item 2/3): Mid/Side bands only ever shape the Mid/Side signal, so their
    // on-screen curve is drawn separately from the Stereo one, in that target's colour.
    auto computeDb=[&](MT target,float hz)->double{
        double totalDb=0.0;
        for(int i=0;i<PQAudioProcessor::kMaxManualBands;++i){
            auto& mb=p.manualBands[(size_t)i]; if(!mb.active.load()||mb.target.load()!=target) continue;
            float f0=mb.freq.load(), gain=mb.gainDb.load(), q=juce::jmax(0.1f,mb.q.load());
            double ratio=hz/(double)f0, logr=std::log2(juce::jmax(1e-6,ratio));
            switch(mb.type.load()){
                case T::Bell: { double bw=1.0/q; totalDb += gain*std::exp(-(logr*logr)/(2.0*bw*bw)); break; }
                case T::Notch: { double bw=0.3/q; totalDb += -24.0*std::exp(-(logr*logr)/(2.0*bw*bw)); break; }
                case T::LowShelf: totalDb += gain*(1.0/(1.0+std::exp(4.0*logr))); break;
                case T::HighShelf: totalDb += gain*(1.0/(1.0+std::exp(-4.0*logr))); break;
                case T::LowCut: totalDb += (hz<f0) ? -juce::jmin(96.0, (double)mb.slopeOrder.load()*(-logr)) : 0.0; break;
                case T::HighCut: totalDb += (hz>f0) ? -juce::jmin(96.0, (double)mb.slopeOrder.load()*logr) : 0.0; break;
            }
        }
        return totalDb;
    };
    auto anyActive=[&](MT target){
        for(int i=0;i<PQAudioProcessor::kMaxManualBands;++i){ auto& mb=p.manualBands[(size_t)i]; if(mb.active.load()&&mb.target.load()==target) return true; }
        return false;
    };
    // FIX (item 3): per-target visibility checkboxes. Purely a display filter - hidden bands keep
    // running in the audio and keep responding to drag/scroll, they just aren't drawn.
    auto targetVisible=[&](MT target){
        switch(target){ case MT::Mid: return midEqToggle.getToggleState(); case MT::Side: return sideEqToggle.getToggleState(); default: return stereoEqToggle.getToggleState(); }
    };
    auto drawTargetCurve=[&](MT target,juce::Colour c){
        if(!targetVisible(target)) return;
        if(!anyActive(target)) return;
        constexpr int kPts=200; juce::Path curve;
        for(int px=0; px<=kPts; ++px){
            float t=px/float(kPts); float hz=std::pow(10.f,std::log10(20.f)+t*(std::log10(20000.f)-std::log10(20.f)));
            float x=freqToX(hz), y=gainDbToY((float)computeDb(target,hz));
            if(px==0) curve.startNewSubPath(x,y); else curve.lineTo(x,y);
        }
        // FIX (shadow bug + glow strength): same fixed shadow technique as drawCurve (stroked
        // outline, not the raw open path - see the comment there for why), and the glow bumped up
        // from a barely-visible 0.16 to something that actually reads next to the analyzer's own.
        juce::Path curveStrokeShape; juce::PathStrokeType(2.0f).createStrokedPath(curveStrokeShape,curve);
        juce::DropShadow(juce::Colours::black.withAlpha(0.4f),6,juce::Point<int>(0,2)).drawForPath(g,curveStrokeShape);
        glowUnder(g,curve,chartArea.getBottom(),chartArea.getX(),chartArea.getRight(),c,0.13f);
        g.setColour(c.withAlpha(0.9f)); g.strokePath(curve, juce::PathStrokeType(2.0f));
    };
    drawTargetCurve(MT::Stereo, manualStereoColour());
    drawTargetCurve(MT::Mid, manualMidColour());
    drawTargetCurve(MT::Side, manualSideColour());

    // Draggable node handles - every visible-target band is drawn, coloured to match.
    for(int i=0;i<PQAudioProcessor::kMaxManualBands;++i){
        auto& mb=p.manualBands[(size_t)i]; if(!mb.active.load()) continue;
        if(!targetVisible(mb.target.load())) continue;
        float x=freqToX(mb.freq.load()), y=gainDbToY(mb.gainDb.load());
        bool isDragging=(draggingBand==i);
        // FIX (item 4): the selected node (last one clicked, whether or not it's mid-drag right now)
        // gets a filled centre dot so it's visually clear which node Delete/Backspace will remove.
        bool isSelected=(selectedBand==i);
        juce::Colour c=manualTargetColour(mb.target.load());
        g.setColour(c.withAlpha(isDragging?1.0f:0.9f));
        g.drawEllipse(x-6,y-6,12,12,2.0f);
        if(isSelected) g.fillEllipse(x-2.5f,y-2.5f,5.f,5.f);
        if(isDragging){ g.setColour(juce::Colours::white); g.setFont(juce::FontOptions(10));
            using T2=PQAudioProcessor::ManualType; bool isCut=(mb.type.load()==T2::LowCut||mb.type.load()==T2::HighCut);
            juce::String detail = isCut ? (juce::String(mb.slopeOrder.load())+"dB/oct") : ("Q"+juce::String(mb.q.load(),2));
            g.drawText(manualTypeLabel(mb.type.load())+"  "+juce::String(mb.freq.load(),0)+"Hz  "+juce::String(mb.gainDb.load(),1)+"dB  "+detail, (int)x+10,(int)y-20,220,16,juce::Justification::left); }
    }
}

void PQAudioProcessorEditor::mouseDown(const juce::MouseEvent& e){
    grabKeyboardFocus(); // FIX (item 4): so a subsequent Delete/Backspace reaches keyPressed() below
    if(!chartArea.contains(e.position)){ draggingBand=-1; selectedBand=-1; return; }
    int hit=findBandNear(e.position);
    if(e.mods.isRightButtonDown()){
        if(hit>=0){ selectedBand=hit; showBandTypeMenu(hit, e.getScreenPosition()); }
        else if(hasActiveManualTarget()) showAddBandMenu(e.position, e.getScreenPosition());
        return;
    }
    // FIX (item 1): left-clicking empty chart space creates a Bell node immediately and starts
    // dragging it right away, instead of waiting for a double-click.
    // FIX (target now comes from the toggles): a new node is only created if at least one of the
    // Stereo/Mid/Side toggles is switched on, and it's created on whichever one was switched on most
    // recently - not hardcoded to Stereo anymore. With nothing armed, clicking empty chart space does
    // nothing (there's nothing to add a line for).
    if(hit<0){
        if(!hasActiveManualTarget()){ draggingBand=-1; selectedBand=-1; return; }
        hit=p.addManualBand(PQAudioProcessor::ManualType::Bell, xToFreq(e.position.x), yToGainDb(e.position.y), 0.7f, currentManualTarget());
        repaint();
    }
    draggingBand=hit; selectedBand=hit; draggedPastThreshold=false; mouseDownPos=e.position;
}
void PQAudioProcessorEditor::mouseDrag(const juce::MouseEvent& e){
    if(draggingBand<0) return;
    if(!draggedPastThreshold && e.position.getDistanceFrom(mouseDownPos)<2.0f) return;
    draggedPastThreshold=true;
    float hz=xToFreq(e.position.x), gainDb=yToGainDb(e.position.y);
    p.setManualBandFreqGain(draggingBand, hz, gainDb);
    repaint();
}
void PQAudioProcessorEditor::mouseUp(const juce::MouseEvent&){ draggingBand=-1; draggedPastThreshold=false; }
void PQAudioProcessorEditor::mouseDoubleClick(const juce::MouseEvent&){
    // FIX (item 1): node creation now happens on the initial mouseDown (see above), so
    // double-click no longer has any separate add/delete/merge behaviour of its own.
}
void PQAudioProcessorEditor::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w){
    if(!chartArea.contains(e.position)) return;
    int hit=findBandNear(e.position); if(hit<0) return;
    using T=PQAudioProcessor::ManualType;
    auto& mb=p.manualBands[(size_t)hit];
    if(mb.type.load()==T::LowCut || mb.type.load()==T::HighCut){
        // FIX: scroll on a Low/High Cut node used to adjust Q, which only nudges the resonance
        // bump right at the cutoff - it never changed the actual roll-off, so nothing looked or
        // sounded different. Scroll now steps through the standard slopes instead, same as it
        // already did (via Q) for Bell/Notch/Shelf - see setManualBandSlope() for the real DSP change.
        const auto& steps=PQAudioProcessor::kCutSlopeSteps;
        int cur=mb.slopeOrder.load(), idx=0;
        for(int i=0;i<(int)steps.size();++i) if(steps[(size_t)i]==cur){ idx=i; break; }
        idx=juce::jlimit(0,(int)steps.size()-1, idx + (w.deltaY>0?1:-1));
        p.setManualBandSlope(hit, steps[(size_t)idx]);
    } else {
        float q=mb.q.load();
        q=juce::jlimit(0.1f,18.f, q * (1.0f + w.deltaY*0.6f));
        p.setManualBandQ(hit,q);
    }
    repaint();
}
// FIX (item 4): Delete/Backspace removes the currently selected manual-EQ node (see mouseDown,
// which sets selectedBand on every left- or right-click that hits an existing node).
bool PQAudioProcessorEditor::keyPressed(const juce::KeyPress& k){
    // Space = toggle Bypass, the way most host transports/plugins treat it - skipped while a text
    // editor (e.g. the preset name box) has focus, so typing a space in a preset name still works.
    if(k==juce::KeyPress::spaceKey && dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent())==nullptr){
        p.bypassed.store(!p.bypassed.load()); refreshBypassButton(); return true;
    }
    if(selectedBand>=0 && (k==juce::KeyPress::deleteKey || k==juce::KeyPress::backspaceKey)){
        p.removeManualBand(selectedBand);
        if(draggingBand==selectedBand) draggingBand=-1;
        selectedBand=-1;
        repaint();
        return true;
    }
    return false;
}
void PQAudioProcessorEditor::showBandTypeMenu(int bandIndex, juce::Point<int> screenPos){
    using T=PQAudioProcessor::ManualType;
    // FIX (target now owned by the top toggles, not this menu): a node's Stereo/Mid/Side target used
    // to be changeable here via a "Move To" submenu. That's now redundant and removed - which target
    // a node belongs to is fixed at creation by whichever toggle was armed (see mouseDown /
    // updateActiveTargetStack), so this menu is Type + Delete only.
    juce::PopupMenu m; m.setLookAndFeel(&bigMenuLnf); // FIX (item 5): 3x larger menu text
    m.addItem(1,"Bell"); m.addItem(2,"Low Shelf"); m.addItem(3,"High Shelf"); m.addItem(4,"Low Cut"); m.addItem(5,"High Cut"); m.addItem(6,"Notch");
    m.addSeparator(); m.addItem(7,"Delete Band");
    juce::PopupMenu::Options opts; opts = opts.withTargetScreenArea(juce::Rectangle<int>(screenPos,screenPos));
    m.showMenuAsync(opts, [this,bandIndex](int result){
        if(result==0) return;
        if(result==7){ p.removeManualBand(bandIndex); if(selectedBand==bandIndex) selectedBand=-1; repaint(); return; }
        static const T types[]={T::Bell,T::LowShelf,T::HighShelf,T::LowCut,T::HighCut,T::Notch};
        p.setManualBandType(bandIndex, types[result-1]); repaint();
    });
}
void PQAudioProcessorEditor::showAddBandMenu(juce::Point<float> chartPos, juce::Point<int> screenPos){
    using T=PQAudioProcessor::ManualType;
    // FIX (target now owned by the top toggles): right-click-on-empty-chart used to ask for
    // Target (Stereo/Mid/Side) as well as filter Type, via one submenu per target. Target selection
    // here is now redundant (see showBandTypeMenu comment above) - the new node always goes to
    // currentManualTarget(), the toggle that's currently armed. Caller (mouseDown) only opens this
    // menu when hasActiveManualTarget() is true, so there's always a valid target here.
    juce::PopupMenu m; m.setLookAndFeel(&bigMenuLnf); // FIX (item 5): 3x larger menu text
    m.addItem(1,"Bell"); m.addItem(2,"Low Shelf"); m.addItem(3,"High Shelf"); m.addItem(4,"Low Cut"); m.addItem(5,"High Cut"); m.addItem(6,"Notch");
    juce::PopupMenu::Options opts; opts = opts.withTargetScreenArea(juce::Rectangle<int>(screenPos,screenPos));
    float hz=xToFreq(chartPos.x), gainDb=yToGainDb(chartPos.y);
    auto target=currentManualTarget();
    m.showMenuAsync(opts, [this,hz,gainDb,target](int result){
        if(result<1||result>6) return;
        static const T types[]={T::Bell,T::LowShelf,T::HighShelf,T::LowCut,T::HighCut,T::Notch};
        T type=types[result-1];
        float g = (type==T::LowCut||type==T::HighCut||type==T::Notch) ? 0.f : gainDb;
        p.addManualBand(type, hz, g, 0.7f, target); repaint();
    });
}

void PQAudioProcessorEditor::paint(juce::Graphics&g){
 // FIX ("still looks the same" feedback): the previous gradient was too subtle to register as a
 // change at all. Pushed the contrast much further - a noticeably lighter cool navy at the top,
 // fading to near-black - plus a soft radial glow seated behind the chart (like a light source),
 // which is what actually reads as "premium" rather than a barely-there tint.
 juce::ColourGradient bgGrad(juce::Colour(0xff1c2530),0,0,juce::Colour(0xff030406),0,(float)getHeight(),false);
 g.setGradientFill(bgGrad); g.fillRect(getLocalBounds());
 auto a=getLocalBounds().toFloat();
 {
     juce::ColourGradient glow(juce::Colour(0xff2a3f66).withAlpha(0.35f),a.getCentreX(),40.f,
                                juce::Colour(0xff2a3f66).withAlpha(0.f),a.getCentreX(),a.getHeight()*0.55f,false);
     g.setGradientFill(glow); g.fillRect(getLocalBounds());
 }
 // FIX (fonts request - "PQ" specifically): bumped a size, added letter-spacing (kerning), and a
 // soft white-to-cool-blue gradient fill instead of flat white, so the wordmark reads a bit more
 // like a designed logo and less like a plain bold label.
 {
     juce::Font pqFont(juce::FontOptions(34).withStyle("bold"));
     pqFont=pqFont.withExtraKerningFactor(0.04f);
     g.setFont(pqFont);
     juce::ColourGradient pqGrad(white(),28,20,juce::Colour(0xffbfe0ff),28,52,false);
     g.setGradientFill(pqGrad);
     g.drawText("PQ",28,18,66,36,juce::Justification::left);
 }
 g.setFont(juce::Font(juce::FontOptions(10)).withExtraKerningFactor(0.02f));g.setColour(muted());g.drawText("PERFECTION OF MATCH EQ   /   POURIA MOTABEAN",91,25,420,22,juce::Justification::left);
 // FIX (layout - meters move beside the chart, matching the reference): the analyzer chart is now
 // narrower, with a small dedicated panel reserved on its right for IN/OUT + MATCH GAIN, instead of
 // those living down in the "FREQUENCY RANGE" box. Split identically in resized() below, since that's
 // where the actual meter/trim/button components get their bounds.
 auto chartFull=a.reduced(24,72).withHeight(a.getHeight()*.48f);
 auto meterPanel=chartFull.removeFromRight(120.f);
 chartFull.removeFromRight(16.f); // gap between chart and meter panel
 auto chart=chartFull;
 // Drop shadow under the analyzer panel, drawn before the panel itself so the panel sits on top of it.
 {
     juce::Path chartShadowPath; chartShadowPath.addRoundedRectangle(chart,14.f);
     juce::DropShadow shadow(juce::Colours::black.withAlpha(0.55f), 18, juce::Point<int>(0,7));
     shadow.drawForPath(g, chartShadowPath);
 }
 g.setColour(panel());g.fillRoundedRectangle(chart,14);for(int i=1;i<12;i++){float x=chart.getX()+chart.getWidth()*i/12;g.setColour(grid());g.drawVerticalLine((int)x,chart.getY(),chart.getBottom());}
 chartArea=chart; // remembered for mouseDown/Drag/Up hit-testing and coordinate mapping
 drawFreqDbAxis(g,chart); // horizontal dB gridlines (replaces the old un-labelled 7-line grid) + extra freq labels
 drawRangeMask(g,chart);
 // FIX (item 6): clip everything drawn on top of the chart to its exact rounded-rectangle shape.
 // Without this, a curve value pushed to the extreme of its range (e.g. a Low/High Shelf at max
 // gain) draws right up to the rectangular bounds of `chart`, which pokes past the panel's rounded
 // corners - most visibly at the top-left, since that's the tightest corner relative to typical
 // curve shapes. Clipping to the same rounded-rect path the panel itself is filled with guarantees
 // nothing can ever visually escape it, regardless of the underlying data.
 juce::Path chartClip; chartClip.addRoundedRectangle(chart,14.f);
 g.saveState(); g.reduceClipRegion(chartClip);
 // Subtle brand watermark - low enough alpha to read as texture, not compete with the curves drawn
 // on top of it. Bottom-right corner, same spot most plugins tuck their mark into.
 {
     g.setColour(juce::Colours::white.withAlpha(0.035f));
     g.setFont(juce::FontOptions(72).withStyle("bold"));
     g.drawText("PQ",chart.reduced(18.f),juce::Justification::bottomRight);
 }
 // FIX (item 4): each of Stereo/Mid/Side is now drawn purely from its own independent on/off flag,
 // so any combination is visible at once - not just whichever single one used to be "soloed".
 if(p.hasReference.load()){
     if(p.stereoOn.load()) drawRef(g,chart,p.refStereo,white());
     if(p.midOn.load()) drawRef(g,chart,p.refMid,yellow());
     if(p.sideOn.load()) drawRef(g,chart,p.refSide,blue());
 }
 if(p.stereoOn.load()) drawCurve(g,chart,p.stereoCurve,white());
 if(p.midOn.load()) drawCurve(g,chart,p.midCurve,yellow());
 if(p.sideOn.load()) drawCurve(g,chart,p.sideCurve,blue());
 // Delta overlay (per request): the actual correction curve being applied, dashed, per visible
 // target - only meaningful once there's something to correct toward.
 if(p.hasReference.load()){
     if(p.stereoOn.load()) drawDeltaCurve(g,p.corrStereo,white());
     if(p.midOn.load()) drawDeltaCurve(g,p.corrMid,yellow());
     if(p.sideOn.load()) drawDeltaCurve(g,p.corrSide,blue());
 }
 drawManualEq(g);
 g.restoreState();
 label(g,"20 Hz",{chart.getX(),chart.getBottom()-18,60,18},muted());label(g,"1 kHz",{chart.getCentreX()-25,chart.getBottom()-18,50,18},muted());label(g,"20 kHz",{chart.getRight()-60,chart.getBottom()-18,60,18},muted());
 label(g,"CLICK: ADD BAND   RIGHT-CLICK: TYPE+TARGET/DELETE   DRAG: FREQ+GAIN   SCROLL: Q   DEL: REMOVE SELECTED",{chart.getX(),chart.getY()-16,700,14},muted());
 {
     auto ctrlPanel=juce::Rectangle<float>(24.f,chart.getBottom()+32.f,a.getWidth()-48.f,a.getHeight()-chart.getBottom()-56.f);
     juce::ColourGradient panelGrad(juce::Colour(0xff141a22),ctrlPanel.getX(),ctrlPanel.getY(),juce::Colour(0xff0c0f14),ctrlPanel.getX(),ctrlPanel.getBottom(),false);
     g.setGradientFill(panelGrad); g.fillRoundedRectangle(ctrlPanel,14.f);
 }
 // FIX (layout balance, take 2): the previous "symmetric proportional halves" fix let every slider
 // grow with the window - on a wide window that reads as absurdly long/stretched rather than
 // balanced (exactly what showed up in testing). Professional plugins fix their control widths and
 // just leave extra space as margin around a fixed-width block when the window's made bigger, so
 // that's what this does now: every slider/combo width below is capped, and only the LEFT margin
 // grows/shrinks with the window. Identical block in resized() below positions the real components.
 const float sliderIndent=103.f; // room for the row label before the slider starts
 const float maxMatchSliderW=380.f, maxRightHalfW=260.f, gutter=60.f, rightGap=40.f;
 const float leftColX=42.f;
 const float rightColX=leftColX+sliderIndent+maxMatchSliderW+gutter;
 const float rightCol2X=rightColX+maxRightHalfW+rightGap;
 label(g,"MATCH AMOUNT",{leftColX,chart.getBottom()+47,150,18},muted());label(g,"FREQUENCY RANGE",{rightColX,chart.getBottom()+47,180,18},muted());
 int y=(int)chart.getBottom()+70;
 label(g,"STEREO",{leftColX+3,(float)y+2,80,18},white()); label(g,"MID",{leftColX+3,(float)y+44,80,18},yellow()); label(g,"SIDE",{leftColX+3,(float)y+86,80,18},blue());
 // Mono Maker sits right under the match-amount sliders, in the space that column otherwise left
 // empty - both fixes the layout balance and gives the column a genuine second purpose.
 label(g,"MONO MAKER",{leftColX+3,(float)y+128,110,18},juce::Colour(0xffbfe0ff));
 label(g,"LOW HZ",{rightColX,(float)y+2,100,18},muted()); label(g,"HIGH HZ",{rightCol2X,(float)y+2,100,18},muted());
 // FIX (layout - your mockup): MODE/WIDTH/STAGE/DEPTH moved out of their old disconnected spot
 // (floating below MATCH AMOUNT, in a column with nothing above or below it) and into the same right
 // column as FREQUENCY RANGE, directly under LOW/HIGH HZ - fills the space that used to just sit
 // empty there once the meters moved out, and reads as one coherent box instead of three scattered ones.
 label(g,"STEREOIZATION",{rightColX,(float)y+88,180,16},muted());
 label(g,"MODE",{rightColX,(float)y+100,100,18},muted()); label(g,"WIDTH AMT",{rightCol2X,(float)y+100,100,18},muted()); label(g,"STAGE",{rightColX,(float)y+132,100,18},muted()); label(g,"DEPTH %",{rightCol2X,(float)y+132,100,18},muted());
 // FIX (layout - meters relocated beside the chart, per the reference): its own small panel, with
 // the same shadow+gradient treatment as the main chart/control panels, sitting directly right of
 // the analyzer instead of buried in the bottom "FREQUENCY RANGE" box.
 {
     juce::Path mpShadowPath; mpShadowPath.addRoundedRectangle(meterPanel,14.f);
     juce::DropShadow mpShadow(juce::Colours::black.withAlpha(0.5f),14,juce::Point<int>(0,5));
     mpShadow.drawForPath(g,mpShadowPath);
     juce::ColourGradient mpGrad(juce::Colour(0xff161c25),meterPanel.getX(),meterPanel.getY(),juce::Colour(0xff0a0d12),meterPanel.getX(),meterPanel.getBottom(),false);
     g.setGradientFill(mpGrad); g.fillRoundedRectangle(meterPanel,14.f);
     g.setColour(grid()); g.drawRoundedRectangle(meterPanel.reduced(0.5f),14.f,1.f);
 }
 // FIX ("ناب" cleanup): one label above (just "IN"/"OUT"), one number below (the live level) - the
 // separate always-on TRIM readout is gone; the fader thumb's position on the bar already shows the
 // trim, and this halves the amount of small text crowding the two meters.
 g.setFont(juce::FontOptions(9).withStyle("bold")); g.setColour(muted());
 g.drawText("IN",inputMeterArea.withY(inputMeterArea.getY()-16).withHeight(14),juce::Justification::centred);
 g.drawText("OUT",outputMeterArea.withY(outputMeterArea.getY()-16).withHeight(14),juce::Justification::centred);
 drawVerticalMeter(g,inputMeterArea,p.inputRmsDb.load(),p.inputPeakDb.load(),white());
 drawVerticalMeter(g,outputMeterArea,p.outputRmsDb.load(),p.outputPeakDb.load(),blue());
 g.setColour(white()); g.setFont(juce::FontOptions(10));
 g.drawText(juce::String(p.inputRmsDb.load(),1),inputMeterArea.withY(inputMeterArea.getBottom()+3).withHeight(14),juce::Justification::centred);
 g.drawText(juce::String(p.outputRmsDb.load(),1),outputMeterArea.withY(outputMeterArea.getBottom()+3).withHeight(14),juce::Justification::centred);
 // FIX ("STATUS" confusion): the separate caption label is gone - it just floated there even when
 // idle with nothing next to it, which is exactly what was confusing. The status Label itself
 // (below, in resized()) is self-explanatory whenever it actually has something to say (e.g. "PEAK
 // MATCHED"), and renders as nothing at all when empty.
}
void PQAudioProcessorEditor::resized(){auto a=getLocalBounds();
 // FIX (preset restructure): the preset dropdown + kebab now live on the LEFT of the header, right
 // after the PQ/title text - matching where a "Default v" preset picker normally sits, and well
 // clear of the STEREO/MID/SIDE cluster which stays anchored to the right.
 presetList.setBounds(540,18,190,36);
 presetsBtn.setBounds(540+190+8,18,36,36);
 // FIX (item 2/3): the three show/hide dots sit inline on the header row, to the left of the
 // STEREO/MID/SIDE group. No text needed - colour (white/yellow/blue) says which target each one is.
 {
     constexpr int dot=18, gap=8, groupW=dot*3+gap*2;
     int groupLeft=(a.getRight()-305)-12-groupW;
     int dotY=18+(36-dot)/2;
     stereoEqToggle.setBounds(groupLeft,dotY,dot,dot);
     midEqToggle.setBounds(groupLeft+dot+gap,dotY,dot,dot);
     sideEqToggle.setBounds(groupLeft+2*(dot+gap),dotY,dot,dot);
     // Bypass sits further left again, in the open header space - it's the button an engineer
     // reaches for first, so it gets its own clearly separate spot rather than being squeezed in
     // next to the small visibility dots.
     bypass.setBounds(groupLeft-12-90,18,90,36);
 }
 stereo.setBounds(a.getRight()-305,18,90,36);mid.setBounds(a.getRight()-207,18,90,36);side.setBounds(a.getRight()-109,18,90,36);
 // FIX (layout - matches paint()): same chart/meterPanel split, since this is where the actual
 // meter/trim/button *components* (not just their drawing) get positioned.
 auto chartFull=a.reduced(24,72).withHeight(a.getHeight()*.48f);
 auto meterPanel=chartFull.removeFromRight(120.f);
 chartFull.removeFromRight(16.f);
 auto chart=chartFull;
 int y=chart.getBottom()+70;
 // FIX (layout balance, take 2 - matches paint()): fixed-width capped columns, only the left margin
 // grows/shrinks with the window - see the comment in paint() for why.
 const float sliderIndent=103.f;
 const float maxMatchSliderW=380.f, maxRightHalfW=260.f, gutter=60.f, rightGap=40.f;
 const float leftColX=42.f;
 const float rightColX=leftColX+sliderIndent+maxMatchSliderW+gutter;
 const float rightCol2X=rightColX+maxRightHalfW+rightGap;
 const float matchSliderX=leftColX+sliderIndent;
 sAmt.setBounds((int)matchSliderX,y,(int)maxMatchSliderW,22);mAmt.setBounds((int)matchSliderX,y+42,(int)maxMatchSliderW,22);siAmt.setBounds((int)matchSliderX,y+84,(int)maxMatchSliderW,22);
 monoMaker.setBounds((int)matchSliderX,y+126,(int)maxMatchSliderW,22);
 low.setBounds((int)rightColX,y+18,(int)maxRightHalfW,22);high.setBounds((int)rightCol2X,y+18,(int)maxRightHalfW,22);
 // FIX (layout - meters relocated beside the chart): IN bar | OUT bar side by side near the top of
 // the panel, MATCH GAIN spanning the full width below them - replaces the old fixed 700/900,
 // y+62-relative spots that used to live down in the "FREQUENCY RANGE" box.
 {
     auto mp=meterPanel.reduced(10.f);
     mp.removeFromTop(18.f); // room for the IN/OUT labels drawn in paint()
     auto btnRow=mp.removeFromBottom(30.f); mp.removeFromBottom(8.f);
     mp.removeFromBottom(18.f); // room for the live-level numbers drawn in paint()
     float gap=12.f, barW=(mp.getWidth()-gap)/2.f;
     auto inBar=mp.removeFromLeft(barW); mp.removeFromLeft(gap); auto outBar=mp;
     inputMeterArea=inBar.toFloat(); outputMeterArea=outBar.toFloat();
     inputTrim.setBounds(inputMeterArea.toNearestInt()); outputTrim.setBounds(outputMeterArea.toNearestInt());
     matchGainBtn.setBounds(btnRow.toNearestInt());
 }
 mode.setBounds((int)rightColX,y+115,(int)maxRightHalfW,25);width.setBounds((int)rightCol2X,y+115,(int)maxRightHalfW,25);widthStage.setBounds((int)rightColX,y+147,90,25);depth.setBounds((int)rightCol2X,y+147,(int)maxRightHalfW,25);capture.setBounds(42,y+190,90,30);apply.setBounds(140,y+190,80,30);clear.setBounds(230,y+190,75,30);
 status.setBounds(325,y+190,300,30);
 // FIX (preset restructure): overlay now anchored under the preset list/kebab on the LEFT, where
 // those controls actually live - and shrunk (no more list row inside it) to just fit name/save/
 // delete. Still only visible while the kebab is toggled on.
 presetPanel.setBounds(540,60,300,190);
}
