#include "PluginEditor.h"
namespace {juce::Colour white(){return juce::Colour(0xfff2f4f7);} juce::Colour yellow(){return juce::Colour(0xffffcf3f);} juce::Colour blue(){return juce::Colour(0xff55a8ff);} juce::Colour bg(){return juce::Colour(0xff07090c);} juce::Colour panel(){return juce::Colour(0xff101419);} juce::Colour grid(){return juce::Colour(0xff242a31);} juce::Colour muted(){return juce::Colour(0xff737e89);} juce::Colour dim(){return juce::Colour(0xff3a4148);}}

PQAudioProcessorEditor::PQAudioProcessorEditor(PQAudioProcessor&x):AudioProcessorEditor(&x),p(x){setResizable(true,true);setSize(1180,760);
 setupButton(stereo,white());setupButton(mid,yellow());setupButton(side,blue());for(auto*q:{&capture,&apply,&save,&load,&clear})setupButton(*q,white());

 // FIX (bug #4): these three buttons now actually do something. Clicking one flips whether that
 // band's correction is applied to the audio (stereoEnabled / midEnabled / sideEnabled), and
 // refreshBandButtons() dims the button so the on/off state is visible at a glance.
 stereo.onClick=[this]{p.stereoEnabled.store(!p.stereoEnabled.load()); refreshBandButtons();};
 mid.onClick=[this]{p.midEnabled.store(!p.midEnabled.load()); refreshBandButtons();};
 side.onClick=[this]{p.sideEnabled.store(!p.sideEnabled.load()); refreshBandButtons();};
 refreshBandButtons();

 auto bind=[this](juce::Slider&s,std::atomic<float>&v){setupSlider(s,0,100,.1);s.setValue(v.load()*100);s.onValueChange=[this,&s,&v]{v.store((float)s.getValue()/100.f);p.applyMatch();};};bind(sAmt,p.stereoMatch);bind(mAmt,p.midMatch);bind(siAmt,p.sideMatch);
 setupSlider(low,20,20000,1);setupSlider(high,20,20000,1);setupSlider(maxDb,0,12,.1);setupSlider(smooth,.05,1.5,.01);setupSlider(width,0,100,.1);setupSlider(depth,0,200,1);
 low.setValue(p.lowHz);high.setValue(p.highHz);maxDb.setValue(p.maxCorrectionDb);smooth.setValue(p.smoothingOctaves);width.setValue(p.widthAmount.load()*100);depth.setValue(p.widthDepth.load()*100);
 low.onValueChange=[this]{p.lowHz=low.getValue();p.applyMatch();};high.onValueChange=[this]{p.highHz=high.getValue();p.applyMatch();};maxDb.onValueChange=[this]{p.maxCorrectionDb=maxDb.getValue();p.applyMatch();};smooth.onValueChange=[this]{p.smoothingOctaves=smooth.getValue();p.applyMatch();};width.onValueChange=[this]{p.widthAmount=width.getValue()/100.f;};depth.onValueChange=[this]{p.widthDepth=depth.getValue()/100.f;};
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
         if (p.loadReference(f)){ status.setText("REFERENCE LOADED", juce::dontSendNotification); low.setValue(p.lowHz,juce::dontSendNotification); high.setValue(p.highHz,juce::dontSendNotification); maxDb.setValue(p.maxCorrectionDb,juce::dontSendNotification); smooth.setValue(p.smoothingOctaves,juce::dontSendNotification); }
         else status.setText("LOAD FAILED (bad file)", juce::dontSendNotification);
     });
 };

 addAndMakeVisible(mode);addAndMakeVisible(status);status.setColour(juce::Label::textColourId,muted());status.setJustificationType(juce::Justification::centredRight);startTimerHz(20);
}
PQAudioProcessorEditor::~PQAudioProcessorEditor(){ chooser = nullptr; }

void PQAudioProcessorEditor::refreshBandButtons(){
 auto style=[](juce::TextButton&b,juce::Colour c,bool on){ b.setColour(juce::TextButton::textColourOffId, on?c:dim()); b.setColour(juce::TextButton::textColourOnId, on?c:dim()); b.setColour(juce::TextButton::outlineColourId, on?c.withAlpha(.5f):grid()); };
 style(stereo,white(),p.stereoEnabled.load()); style(mid,yellow(),p.midEnabled.load()); style(side,blue(),p.sideEnabled.load());
}

void PQAudioProcessorEditor::setupButton(juce::TextButton&b,juce::Colour c){addAndMakeVisible(b);b.setColour(juce::TextButton::buttonColourId,panel());b.setColour(juce::TextButton::buttonOnColourId,grid());b.setColour(juce::TextButton::textColourOffId,c);b.setColour(juce::TextButton::textColourOnId,c);b.setColour(juce::TextButton::outlineColourId,grid());}
void PQAudioProcessorEditor::setupSlider(juce::Slider&s,double a,double b,double step){addAndMakeVisible(s);s.setSliderStyle(juce::Slider::LinearHorizontal);s.setTextBoxStyle(juce::Slider::TextBoxRight,false,66,20);s.setRange(a,b,step);s.setColour(juce::Slider::thumbColourId,white());s.setColour(juce::Slider::trackColourId,grid());s.setColour(juce::Slider::textBoxTextColourId,white());s.setColour(juce::Slider::textBoxBackgroundColourId,panel());}
void PQAudioProcessorEditor::label(juce::Graphics&g,juce::String t,juce::Rectangle<float>r,juce::Colour c){g.setColour(c);g.setFont(juce::FontOptions(10));g.drawText(t,r,juce::Justification::left);}
void PQAudioProcessorEditor::drawCurve(juce::Graphics&g,juce::Rectangle<float>r,const std::array<std::atomic<float>,PQAudioProcessor::kBins>&a,juce::Colour c){juce::Path q;for(int i=0;i<PQAudioProcessor::kBins;i++){float x=r.getX()+r.getWidth()*i/(PQAudioProcessor::kBins-1.f),db=a[i].load(),y=r.getBottom()-r.getHeight()*juce::jlimit(0.f,1.f,(db+90)/96.f);if(i==0)q.startNewSubPath(x,y);else q.lineTo(x,y);}g.setColour(c);g.strokePath(q,juce::PathStrokeType(1.8f));}
void PQAudioProcessorEditor::drawRef(juce::Graphics&g,juce::Rectangle<float>r,const std::array<std::atomic<float>,PQAudioProcessor::kBins>&a,juce::Colour c){juce::Path q;for(int i=0;i<PQAudioProcessor::kBins;i++){float x=r.getX()+r.getWidth()*i/(PQAudioProcessor::kBins-1.f),db=a[i].load(),y=r.getBottom()-r.getHeight()*juce::jlimit(0.f,1.f,(db+90)/96.f);if(i==0)q.startNewSubPath(x,y);else q.lineTo(x,y);}g.setColour(c.withAlpha(.22f));g.strokePath(q,juce::PathStrokeType(1.f));}

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

void PQAudioProcessorEditor::paint(juce::Graphics&g){g.fillAll(bg());auto a=getLocalBounds().toFloat();g.setColour(white());g.setFont(juce::FontOptions(29).withStyle("bold"));g.drawText("PQ",28,20,62,32,juce::Justification::left);g.setFont(juce::FontOptions(10));g.setColour(muted());g.drawText("PERFECTION OF MATCH EQ   /   POURIA",91,25,330,22,juce::Justification::left);
 auto chart=a.reduced(24,72).withHeight(a.getHeight()*.48f);g.setColour(panel());g.fillRoundedRectangle(chart,14);for(int i=1;i<7;i++){g.setColour(grid());g.drawHorizontalLine((int)(chart.getY()+chart.getHeight()*i/7),chart.getX(),chart.getRight());}for(int i=1;i<12;i++){float x=chart.getX()+chart.getWidth()*i/12;g.setColour(grid());g.drawVerticalLine((int)x,chart.getY(),chart.getBottom());}
 drawRangeMask(g,chart);
 if(p.hasReference.load()){drawRef(g,chart,p.refStereo,white());drawRef(g,chart,p.refMid,yellow());drawRef(g,chart,p.refSide,blue());}drawCurve(g,chart,p.stereoCurve,white());drawCurve(g,chart,p.midCurve,yellow());drawCurve(g,chart,p.sideCurve,blue());
 label(g,"20 Hz",{chart.getX(),chart.getBottom()-18,60,18},muted());label(g,"1 kHz",{chart.getCentreX()-25,chart.getBottom()-18,50,18},muted());label(g,"20 kHz",{chart.getRight()-60,chart.getBottom()-18,60,18},muted());
 g.setColour(panel());g.fillRoundedRectangle(24,chart.getBottom()+32,a.getWidth()-48,a.getHeight()-chart.getBottom()-56,14);
 label(g,"MATCH AMOUNT",{42,chart.getBottom()+47,150,18},muted());label(g,"FREQUENCY RANGE",{700,chart.getBottom()+47,180,18},muted());label(g,"MONO → STEREO",{42,chart.getBottom()+153,180,18},muted());label(g,"INPUT / OUTPUT",{900,chart.getBottom()+153,150,18},muted());
 int y=(int)chart.getBottom()+70;
 label(g,"STEREO",{45,(float)y+2,80,18},white()); label(g,"MID",{45,(float)y+34,80,18},yellow()); label(g,"SIDE",{45,(float)y+66,80,18},blue());
 label(g,"LOW HZ",{700,(float)y+2,100,18},muted()); label(g,"HIGH HZ",{900,(float)y+2,100,18},muted()); label(g,"MAX dB",{700,(float)y+34,100,18},muted()); label(g,"SMOOTHING",{900,(float)y+34,100,18},muted());
 label(g,"MODE",{210,(float)y+100,100,18},muted()); label(g,"WIDTH AMT",{410,(float)y+100,100,18},muted()); label(g,"DEPTH %",{410,(float)y+132,100,18},muted());
 g.setColour(white());g.drawText(juce::String(p.inputRmsDb.load(),1)+" dB  →  "+juce::String(p.outputRmsDb.load(),1)+" dB",900,(int)(chart.getBottom()+171),200,20,juce::Justification::left);
}
void PQAudioProcessorEditor::resized(){auto a=getLocalBounds();stereo.setBounds(a.getRight()-305,18,90,36);mid.setBounds(a.getRight()-207,18,90,36);side.setBounds(a.getRight()-109,18,90,36);auto chart=a.reduced(24,72).withHeight(a.getHeight()*.48f);int y=chart.getBottom()+70;sAmt.setBounds(145,y,470,22);mAmt.setBounds(145,y+32,470,22);siAmt.setBounds(145,y+64,470,22);low.setBounds(700,y+18,185,22);high.setBounds(900,y+18,185,22);maxDb.setBounds(700,y+50,185,22);smooth.setBounds(900,y+50,185,22);mode.setBounds(210,y+115,180,25);width.setBounds(410,y+115,250,25);depth.setBounds(410,y+147,250,25);capture.setBounds(42,y+190,90,30);apply.setBounds(140,y+190,80,30);save.setBounds(230,y+190,70,30);load.setBounds(308,y+190,70,30);clear.setBounds(386,y+190,75,30);status.setBounds(a.getRight()-320,y+190,300,30);}
