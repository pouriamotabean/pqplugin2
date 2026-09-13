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

PQAudioProcessorEditor::PQAudioProcessorEditor(PQAudioProcessor&x):AudioProcessorEditor(&x),p(x){setResizable(true,true);setSize(1180,760);
 setupButton(stereo,white());setupButton(mid,yellow());setupButton(side,blue());for(auto*q:{&capture,&apply,&save,&load,&clear})setupButton(*q,white());
 setupButton(widthStage,white()); widthStage.setButtonText(p.widthPostEq.load()?"POST":"PRE");
 // Toggles whether the mono-widener runs before the EQ correction (PRE, so the analyzer/match
 // "hears" the widened signal) or after it (POST, widening is the very last step on the output).
 widthStage.onClick=[this]{ bool now=!p.widthPostEq.load(); p.widthPostEq.store(now); widthStage.setButtonText(now?"POST":"PRE"); };

 // FIX (real solo): clicking a band button now solos it (mutually exclusive) - it mutes the other
 // band and lets you hear/see that one in isolation, pre-correction. Clicking the active one again
 // (or clicking STEREO) returns to the normal full, corrected mix.
 stereo.onClick=[this]{p.solo.store(p.solo.load()==PQAudioProcessor::SoloBand::Stereo?PQAudioProcessor::SoloBand::None:PQAudioProcessor::SoloBand::Stereo); refreshBandButtons();};
 mid.onClick=[this]{p.solo.store(p.solo.load()==PQAudioProcessor::SoloBand::Mid?PQAudioProcessor::SoloBand::None:PQAudioProcessor::SoloBand::Mid); refreshBandButtons();};
 side.onClick=[this]{p.solo.store(p.solo.load()==PQAudioProcessor::SoloBand::Side?PQAudioProcessor::SoloBand::None:PQAudioProcessor::SoloBand::Side); refreshBandButtons();};
 refreshBandButtons();

 auto bind=[this](juce::Slider&s,std::atomic<float>&v){setupSlider(s,0,100,.1);s.setValue(v.load()*100);s.onValueChange=[this,&s,&v]{v.store((float)s.getValue()/100.f);p.applyMatch();};};bind(sAmt,p.stereoMatch);bind(mAmt,p.midMatch);bind(siAmt,p.sideMatch);
 setupSlider(low,20,20000,1);setupSlider(high,20,20000,1);setupSlider(width,0,100,.1);setupSlider(depth,0,200,1);
 // FIX: the frequency chart is drawn on a log scale (20Hz-20kHz), but these sliders were linear -
 // that mismatch is exactly why dragging near 20Hz raced across the whole chart while dragging near
 // 20kHz barely moved the line. A log-style skew around the geometric middle of the range
 // (sqrt(20*20000)) makes the slider's feel match what's actually drawn.
 low.setSkewFactorFromMidPoint(632.45); high.setSkewFactorFromMidPoint(632.45);
 low.setValue(p.lowHz);high.setValue(p.highHz);width.setValue(p.widthAmount.load()*100);depth.setValue(p.widthDepth.load()*100);
 low.onValueChange=[this]{p.lowHz=low.getValue();p.applyMatch();};high.onValueChange=[this]{p.highHz=high.getValue();p.applyMatch();};width.onValueChange=[this]{p.widthAmount=width.getValue()/100.f;};depth.onValueChange=[this]{p.widthDepth=depth.getValue()/100.f;};

 // Max dB / Smoothing used to live here as sliders; they're fixed at sane working values in the
 // processor now (6dB / 0.35 octaves) and no longer exposed. This slot is now the input/output
 // level meters, each doubling as a +/-24dB trim fader, plus a MATCH GAIN button between them.
 setupVerticalTrim(inputTrim); setupVerticalTrim(outputTrim);
 inputTrim.setRange(-24,24,.1); outputTrim.setRange(-24,24,.1);
 inputTrim.setValue(p.inputTrimDb.load()); outputTrim.setValue(p.outputTrimDb.load());
 inputTrim.onValueChange=[this]{p.inputTrimDb.store((float)inputTrim.getValue());};
 outputTrim.onValueChange=[this]{p.outputTrimDb.store((float)outputTrim.getValue());};
 setupButton(matchGainBtn,white());
 matchGainBtn.onClick=[this]{ p.matchGain(); outputTrim.setValue(p.outputTrimDb.load(),juce::dontSendNotification); status.setText("GAIN MATCHED",juce::dontSendNotification); };
 mode.addItem("MICRO SHIFT",1);mode.addItem("HAAS",2);mode.addItem("DECORRELATED",3);mode.setSelectedId((int)p.widthMode.load()+1);mode.onChange=[this]{p.widthMode=(PQAudioProcessor::WidthMode)(mode.getSelectedId()-1);};
 capture.onClick=[this]{p.captureReference();status.setText("REFERENCE CAPTURED",juce::dontSendNotification);};apply.onClick=[this]{p.applyMatch();status.setText("MATCH UPDATED",juce::dontSendNotification);};clear.onClick=[this]{p.clearReference();status.setText("REFERENCE CLEARED",juce::dontSendNotification);};

 // FIX (bug #1): file dialogs are now async (launchAsync + callback) instead of the old blocking
 // browseForFileToSave()/browseForFileToOpen(). Blocking modal dialogs from inside a plugin can hang
 // or misbehave in hosts that run the UI on a message thread they control tightly (Cubase included) -
 // this is the most likely single cause of the host "acting up" that was described.
 save.onClick=[this]{
     chooser = std::make_unique<juce::FileChooser>("Save PQ Reference", juce::File(), "*.pqref");
     auto flags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting;
     chooser->launchAsync(flags, [this](const juce::FileChooser& fc){
         auto f = fc.getResult();
         if (f == juce::File()) return;
         if (!f.hasFileExtension(".pqref")) f = f.withFileExtension(".pqref");
         if (p.saveReference(f)) status.setText("REFERENCE SAVED", juce::dontSendNotification);
         else status.setText("SAVE FAILED", juce::dontSendNotification);
     });
 };
 load.onClick=[this]{
     chooser = std::make_unique<juce::FileChooser>("Load PQ Reference", juce::File(), "*.pqref");
     chooser->launchAsync(juce::FileBrowserComponent::openMode, [this](const juce::FileChooser& fc){
         auto f = fc.getResult();
         if (f == juce::File()) return;
         if (p.loadReference(f)){ status.setText("REFERENCE LOADED", juce::dontSendNotification); low.setValue(p.lowHz,juce::dontSendNotification); high.setValue(p.highHz,juce::dontSendNotification); }
         else status.setText("LOAD FAILED (bad file)", juce::dontSendNotification);
     });
 };

 addAndMakeVisible(mode);addAndMakeVisible(status);status.setColour(juce::Label::textColourId,muted());status.setJustificationType(juce::Justification::centredRight);startTimerHz(20);
}
PQAudioProcessorEditor::~PQAudioProcessorEditor(){ chooser = nullptr; }

void PQAudioProcessorEditor::refreshBandButtons(){
 auto s=p.solo.load(); bool none = s==PQAudioProcessor::SoloBand::None;
 auto style=[](juce::TextButton&b,juce::Colour c,bool on){ b.setColour(juce::TextButton::textColourOffId, on?c:dim()); b.setColour(juce::TextButton::textColourOnId, on?c:dim()); };
 style(stereo,white(),none||s==PQAudioProcessor::SoloBand::Stereo); style(mid,yellow(),none||s==PQAudioProcessor::SoloBand::Mid); style(side,blue(),none||s==PQAudioProcessor::SoloBand::Side);
}

void PQAudioProcessorEditor::setupButton(juce::TextButton&b,juce::Colour c){addAndMakeVisible(b);b.setColour(juce::TextButton::buttonColourId,panel());b.setColour(juce::TextButton::buttonOnColourId,grid());b.setColour(juce::TextButton::textColourOffId,c);b.setColour(juce::TextButton::textColourOnId,c);}
void PQAudioProcessorEditor::setupSlider(juce::Slider&s,double a,double b,double step){addAndMakeVisible(s);s.setSliderStyle(juce::Slider::LinearHorizontal);s.setTextBoxStyle(juce::Slider::TextBoxRight,false,66,20);s.setRange(a,b,step);s.setColour(juce::Slider::thumbColourId,white());s.setColour(juce::Slider::trackColourId,grid());s.setColour(juce::Slider::textBoxTextColourId,white());s.setColour(juce::Slider::textBoxBackgroundColourId,panel());}
// Thin vertical trim fader meant to sit directly on top of drawVerticalMeter() for the same
// rectangle: no text box, transparent track (the meter bar underneath already reads as the track),
// small bright thumb marking the trim position so it doesn't fight visually with the level bar.
void PQAudioProcessorEditor::setupVerticalTrim(juce::Slider&s){addAndMakeVisible(s);s.setSliderStyle(juce::Slider::LinearVertical);s.setTextBoxStyle(juce::Slider::NoTextBox,false,0,0);s.setColour(juce::Slider::thumbColourId,yellow());s.setColour(juce::Slider::trackColourId,juce::Colours::transparentBlack);s.setColour(juce::Slider::backgroundColourId,juce::Colours::transparentBlack);}
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
// Kept monochrome/flat to match the rest of the panel, with a small colour shift only near the
// very top of the range so it still reads as "hot" without introducing a busy gradient.
void PQAudioProcessorEditor::drawVerticalMeter(juce::Graphics&g,juce::Rectangle<float>r,float levelDb,float peakDb,juce::Colour c){
 g.setColour(panel()); g.fillRoundedRectangle(r,4.f);
 g.setColour(grid()); g.drawRoundedRectangle(r,4.f,1.f);
 constexpr float kFloorDb=-60.f;
 float t=juce::jlimit(0.f,1.f,(levelDb-kFloorDb)/(0.f-kFloorDb));
 auto fillR=r.reduced(3.f); float fillH=fillR.getHeight()*t;
 auto bar=juce::Rectangle<float>(fillR.getX(),fillR.getBottom()-fillH,fillR.getWidth(),fillH);
 g.setColour(t>0.92f?yellow():c.withAlpha(.85f)); g.fillRoundedRectangle(bar,3.f);
 // Peak-hold indicator: a thin line at the highest recent peak, which the processor releases
 // slowly rather than snapping straight to the current level (see PQAudioProcessor::processBlock).
 float tp=juce::jlimit(0.f,1.f,(peakDb-kFloorDb)/(0.f-kFloorDb));
 float py=fillR.getBottom()-fillR.getHeight()*tp;
 g.setColour(juce::Colours::white.withAlpha(.9f));
 g.fillRect(juce::Rectangle<float>(fillR.getX(),py-1.f,fillR.getWidth(),2.f));
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
    for(int i=0;i<PQAudioProcessor::kMaxManualBands;++i){
        auto& mb=p.manualBands[(size_t)i]; if(!mb.active.load()) continue;
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
                case T::LowCut: totalDb += (hz<f0) ? -juce::jmin(48.0, 12.0*(-logr)) : 0.0; break;
                case T::HighCut: totalDb += (hz>f0) ? -juce::jmin(48.0, 12.0*logr) : 0.0; break;
            }
        }
        return totalDb;
    };
    auto anyActive=[&](MT target){
        for(int i=0;i<PQAudioProcessor::kMaxManualBands;++i){ auto& mb=p.manualBands[(size_t)i]; if(mb.active.load()&&mb.target.load()==target) return true; }
        return false;
    };
    auto drawTargetCurve=[&](MT target,juce::Colour c){
        if(!anyActive(target)) return;
        constexpr int kPts=200; juce::Path curve;
        for(int px=0; px<=kPts; ++px){
            float t=px/float(kPts); float hz=std::pow(10.f,std::log10(20.f)+t*(std::log10(20000.f)-std::log10(20.f)));
            float x=freqToX(hz), y=gainDbToY((float)computeDb(target,hz));
            if(px==0) curve.startNewSubPath(x,y); else curve.lineTo(x,y);
        }
        g.setColour(c.withAlpha(0.85f)); g.strokePath(curve, juce::PathStrokeType(2.0f));
    };
    drawTargetCurve(MT::Stereo, manualStereoColour());
    drawTargetCurve(MT::Mid, manualMidColour());
    drawTargetCurve(MT::Side, manualSideColour());

    // Draggable node handles - every band is drawn regardless of target, coloured to match.
    for(int i=0;i<PQAudioProcessor::kMaxManualBands;++i){
        auto& mb=p.manualBands[(size_t)i]; if(!mb.active.load()) continue;
        float x=freqToX(mb.freq.load()), y=gainDbToY(mb.gainDb.load());
        bool isDragging=(draggingBand==i);
        juce::Colour c=manualTargetColour(mb.target.load());
        g.setColour(c.withAlpha(isDragging?1.0f:0.9f));
        g.drawEllipse(x-6,y-6,12,12,2.0f);
        if(isDragging){ g.setColour(juce::Colours::white); g.setFont(juce::FontOptions(10));
            g.drawText(manualTypeLabel(mb.type.load())+"  "+juce::String(mb.freq.load(),0)+"Hz  "+juce::String(mb.gainDb.load(),1)+"dB  Q"+juce::String(mb.q.load(),2), (int)x+10,(int)y-20,220,16,juce::Justification::left); }
    }
}

void PQAudioProcessorEditor::mouseDown(const juce::MouseEvent& e){
    if(!chartArea.contains(e.position)){ draggingBand=-1; return; }
    int hit=findBandNear(e.position);
    if(e.mods.isRightButtonDown()){
        if(hit>=0) showBandTypeMenu(hit, e.getScreenPosition());
        else showAddBandMenu(e.position, e.getScreenPosition());
        return;
    }
    // FIX (item 1): left-clicking empty chart space creates a Bell node immediately and starts
    // dragging it right away, instead of waiting for a double-click.
    if(hit<0){
        hit=p.addManualBand(PQAudioProcessor::ManualType::Bell, xToFreq(e.position.x), yToGainDb(e.position.y), 0.7f);
        repaint();
    }
    draggingBand=hit; draggedPastThreshold=false; mouseDownPos=e.position;
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
    float q=p.manualBands[(size_t)hit].q.load();
    q=juce::jlimit(0.1f,18.f, q * (1.0f + w.deltaY*0.6f));
    p.setManualBandQ(hit,q);
    repaint();
}
void PQAudioProcessorEditor::showBandTypeMenu(int bandIndex, juce::Point<int> screenPos){
    using T=PQAudioProcessor::ManualType;
    juce::PopupMenu m;
    m.addItem(1,"Bell"); m.addItem(2,"Low Shelf"); m.addItem(3,"High Shelf"); m.addItem(4,"Low Cut"); m.addItem(5,"High Cut"); m.addItem(6,"Notch");
    m.addSeparator(); m.addItem(7,"Delete Band");
    juce::PopupMenu::Options opts; opts = opts.withTargetScreenArea(juce::Rectangle<int>(screenPos,screenPos));
    m.showMenuAsync(opts, [this,bandIndex](int result){
        if(result==0) return;
        if(result==7){ p.removeManualBand(bandIndex); repaint(); return; }
        static const T types[]={T::Bell,T::LowShelf,T::HighShelf,T::LowCut,T::HighCut,T::Notch};
        p.setManualBandType(bandIndex, types[result-1]); repaint();
    });
}
void PQAudioProcessorEditor::showAddBandMenu(juce::Point<float> chartPos, juce::Point<int> screenPos){
    using T=PQAudioProcessor::ManualType; using MT=PQAudioProcessor::ManualTarget;
    // FIX (item 2): right-click on empty chart now asks for Target (Stereo/Mid/Side) as well as
    // filter Type, via one submenu per target. Result ids are offset per target (Stereo 1-6,
    // Mid 11-16, Side 21-26) so a single callback can decode both from the chosen id.
    auto typeSubMenu=[](int base){
        juce::PopupMenu sub;
        sub.addItem(base+1,"Bell"); sub.addItem(base+2,"Low Shelf"); sub.addItem(base+3,"High Shelf");
        sub.addItem(base+4,"Low Cut"); sub.addItem(base+5,"High Cut"); sub.addItem(base+6,"Notch");
        return sub;
    };
    juce::PopupMenu m;
    m.addSubMenu("Stereo", typeSubMenu(0));
    m.addSubMenu("Mid",    typeSubMenu(10));
    m.addSubMenu("Side",   typeSubMenu(20));
    juce::PopupMenu::Options opts; opts = opts.withTargetScreenArea(juce::Rectangle<int>(screenPos,screenPos));
    float hz=xToFreq(chartPos.x), gainDb=yToGainDb(chartPos.y);
    m.showMenuAsync(opts, [this,hz,gainDb](int result){
        if(result==0) return;
        static const T types[]={T::Bell,T::LowShelf,T::HighShelf,T::LowCut,T::HighCut,T::Notch};
        MT target=MT::Stereo; int typeIdx=result;
        if(result>=21){ target=MT::Side; typeIdx=result-20; }
        else if(result>=11){ target=MT::Mid; typeIdx=result-10; }
        if(typeIdx<1||typeIdx>6) return;
        T type=types[typeIdx-1];
        float g = (type==T::LowCut||type==T::HighCut||type==T::Notch) ? 0.f : gainDb;
        p.addManualBand(type, hz, g, 0.7f, target); repaint();
    });
}

void PQAudioProcessorEditor::paint(juce::Graphics&g){g.fillAll(bg());auto a=getLocalBounds().toFloat();g.setColour(white());g.setFont(juce::FontOptions(29).withStyle("bold"));g.drawText("PQ",28,20,62,32,juce::Justification::left);g.setFont(juce::FontOptions(10));g.setColour(muted());g.drawText("PERFECTION OF MATCH EQ   /   POURIA MOTABEAN",91,25,420,22,juce::Justification::left);
 auto chart=a.reduced(24,72).withHeight(a.getHeight()*.48f);g.setColour(panel());g.fillRoundedRectangle(chart,14);for(int i=1;i<12;i++){float x=chart.getX()+chart.getWidth()*i/12;g.setColour(grid());g.drawVerticalLine((int)x,chart.getY(),chart.getBottom());}
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
 // FIX (solo wasn't "clean"): soloing used to dim the other bands to 15% instead of hiding them, so
 // their live curve *and* their reference curve (drawn unconditionally, below) both still bled
 // through - which is why a soloed STEREO line never looked fully white. Non-soloed bands (both
 // their live curve and their reference trace) are now skipped entirely while a solo is active.
 auto s=p.solo.load(); bool none=s==PQAudioProcessor::SoloBand::None;
 auto visible=[&](PQAudioProcessor::SoloBand b){ return none||s==b; };
 if(p.hasReference.load()){
     if(visible(PQAudioProcessor::SoloBand::Stereo)) drawRef(g,chart,p.refStereo,white());
     if(visible(PQAudioProcessor::SoloBand::Mid)) drawRef(g,chart,p.refMid,yellow());
     if(visible(PQAudioProcessor::SoloBand::Side)) drawRef(g,chart,p.refSide,blue());
 }
 if(visible(PQAudioProcessor::SoloBand::Stereo)) drawCurve(g,chart,p.stereoCurve,white());
 if(visible(PQAudioProcessor::SoloBand::Mid)) drawCurve(g,chart,p.midCurve,yellow());
 if(visible(PQAudioProcessor::SoloBand::Side)) drawCurve(g,chart,p.sideCurve,blue());
 drawManualEq(g);
 g.restoreState();
 label(g,"20 Hz",{chart.getX(),chart.getBottom()-18,60,18},muted());label(g,"1 kHz",{chart.getCentreX()-25,chart.getBottom()-18,50,18},muted());label(g,"20 kHz",{chart.getRight()-60,chart.getBottom()-18,60,18},muted());
 label(g,"CLICK: ADD BAND    RIGHT-CLICK: TYPE+TARGET / DELETE    DRAG: FREQ+GAIN    SCROLL: Q",{chart.getX(),chart.getY()-16,600,14},muted());
 g.setColour(panel());g.fillRoundedRectangle(24,chart.getBottom()+32,a.getWidth()-48,a.getHeight()-chart.getBottom()-56,14);
 label(g,"MATCH AMOUNT",{42,chart.getBottom()+47,150,18},muted());label(g,"FREQUENCY RANGE",{700,chart.getBottom()+47,180,18},muted());label(g,"MONO → STEREO",{42,chart.getBottom()+153,180,18},muted());
 int y=(int)chart.getBottom()+70;
 label(g,"STEREO",{45,(float)y+2,80,18},white()); label(g,"MID",{45,(float)y+34,80,18},yellow()); label(g,"SIDE",{45,(float)y+66,80,18},blue());
 label(g,"LOW HZ",{700,(float)y+2,100,18},muted()); label(g,"HIGH HZ",{900,(float)y+2,100,18},muted());
 label(g,"MODE",{210,(float)y+100,100,18},muted()); label(g,"WIDTH AMT",{410,(float)y+100,100,18},muted()); label(g,"STAGE",{210,(float)y+132,100,18},muted()); label(g,"DEPTH %",{410,(float)y+132,100,18},muted());
 // Input/output level meters (each doubling as a trim fader - see setupVerticalTrim) plus the
 // MATCH GAIN button sitting between them, replacing the old Max dB / Smoothing sliders here.
 g.setFont(juce::FontOptions(9)); g.setColour(muted());
 g.drawText("IN",inputMeterArea.withY(inputMeterArea.getY()-16).withHeight(14),juce::Justification::centred);
 g.drawText("OUT",outputMeterArea.withY(outputMeterArea.getY()-16).withHeight(14),juce::Justification::centred);
 drawVerticalMeter(g,inputMeterArea,p.inputRmsDb.load(),p.inputPeakDb.load(),white());
 drawVerticalMeter(g,outputMeterArea,p.outputRmsDb.load(),p.outputPeakDb.load(),blue());
 g.setColour(muted()); g.setFont(juce::FontOptions(8));
 g.drawText(juce::String(p.inputTrimDb.load(),1)+"dB",inputMeterArea.withY(inputMeterArea.getBottom()+2).withHeight(12),juce::Justification::centred);
 g.drawText(juce::String(p.outputTrimDb.load(),1)+"dB",outputMeterArea.withY(outputMeterArea.getBottom()+2).withHeight(12),juce::Justification::centred);
 // FIX (item 7): "GAIN MATCHED"/etc used to float in the corner with nothing to explain it. It's a
 // shared status line for CAPTURE/APPLY/CLEAR/SAVE/LOAD/MATCH GAIN feedback, so give it a caption
 // instead of removing the (still useful) shared line.
 label(g,"STATUS",{(float)(a.getRight()-320),(float)y+190-16,100,14},muted());
}
void PQAudioProcessorEditor::resized(){auto a=getLocalBounds();stereo.setBounds(a.getRight()-305,18,90,36);mid.setBounds(a.getRight()-207,18,90,36);side.setBounds(a.getRight()-109,18,90,36);auto chart=a.reduced(24,72).withHeight(a.getHeight()*.48f);int y=chart.getBottom()+70;sAmt.setBounds(145,y,470,22);mAmt.setBounds(145,y+32,470,22);siAmt.setBounds(145,y+64,470,22);low.setBounds(700,y+18,185,22);high.setBounds(900,y+18,185,22);
 inputMeterArea={700.f,(float)(y+62),70.f,108.f}; outputMeterArea={900.f,(float)(y+62),70.f,108.f};
 inputTrim.setBounds(inputMeterArea.toNearestInt()); outputTrim.setBounds(outputMeterArea.toNearestInt());
 matchGainBtn.setBounds(785,y+101,100,30);
 mode.setBounds(210,y+115,180,25);width.setBounds(410,y+115,250,25);widthStage.setBounds(210,y+147,90,25);depth.setBounds(410,y+147,250,25);capture.setBounds(42,y+190,90,30);apply.setBounds(140,y+190,80,30);save.setBounds(230,y+190,70,30);load.setBounds(308,y+190,70,30);clear.setBounds(386,y+190,75,30);status.setBounds(a.getRight()-320,y+190,300,30);}
