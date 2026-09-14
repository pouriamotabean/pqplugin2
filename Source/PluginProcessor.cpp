#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace { constexpr float kFloor=-90.f; constexpr float kMaxCap=12.f; }

PQAudioProcessor::PQAudioProcessor():AudioProcessor(BusesProperties().withInput("Input",juce::AudioChannelSet::stereo(),true).withOutput("Output",juce::AudioChannelSet::stereo(),true))
{
    for(auto& x:stereoCurve)x.store(kFloor); for(auto& x:midCurve)x.store(kFloor); for(auto& x:sideCurve)x.store(kFloor);
    for(auto& x:refStereo)x.store(kFloor); for(auto& x:refMid)x.store(kFloor); for(auto& x:refSide)x.store(kFloor);
}

void PQAudioProcessor::prepareToPlay(double sampleRate,int samplesPerBlock){
    juce::ignoreUnused(samplesPerBlock); sr=sampleRate; fftPos=0; hopCounter=0; prevMono=0;
    // Size the width delay line for up to ~50ms at this sample rate so Haas/decorrelation modes
    // have enough room to use musically real delay times (see applyWidth in processBlock).
    widthBufSize = juce::jmax(64,(int)std::round(sr*0.05)+8); widthDelay.assign((size_t)widthBufSize,0.f); widthWriteIdx=0;
    for(int i=0;i<kBands;++i){float t=i/float(kBands-1); bandHz[i]=std::exp(std::log(20.f)+t*(std::log(20000.f)-std::log(20.f)));}
    stStereoL.fill({});stStereoR.fill({});stMid.fill({});stSide.fill({}); manualStateL.fill({}); manualStateR.fill({}); manualStateMid.fill({}); manualStateSide.fill({});
    dirty.store(true); manualDirty.store(true); rebuildCoefficients(); rebuildManualCoefficients();
}

bool PQAudioProcessor::isBusesLayoutSupported(const BusesLayout& l) const { auto in=l.getMainInputChannelSet(),out=l.getMainOutputChannelSet(); return (in==juce::AudioChannelSet::mono()||in==juce::AudioChannelSet::stereo()) && out==juce::AudioChannelSet::stereo(); }
float PQAudioProcessor::logFreq(float f){return std::log10(juce::jlimit(20.f,20000.f,f));}
float PQAudioProcessor::interp(const std::array<float,kBins>& a,float hz){float t=(logFreq(hz)-logFreq(20.f))/(logFreq(20000.f)-logFreq(20.f));float p=t*(kBins-1);int i=juce::jlimit(0,kBins-2,(int)std::floor(p));float f=p-i;return a[(size_t)i]+(a[(size_t)i+1]-a[(size_t)i])*f;}
// Same log-frequency interpolation as interp(), but reads from an atomic array (used for the
// reference curves, which can be written by the UI thread at any time).
float PQAudioProcessor::interpAtomic(const std::array<std::atomic<float>,kBins>& a,float hz){float t=(logFreq(hz)-logFreq(20.f))/(logFreq(20000.f)-logFreq(20.f));float p=t*(kBins-1);int i=juce::jlimit(0,kBins-2,(int)std::floor(p));float f=p-i;float a0=a[(size_t)i].load(),a1=a[(size_t)i+1].load();return a0+(a1-a0)*f;}

void PQAudioProcessor::processBlock(juce::AudioBuffer<float>& b,juce::MidiBuffer&){
    juce::ScopedNoDenormals nd; const int ch=b.getNumChannels(), n=b.getNumSamples(); if(ch==0)return;
    // FIX (mono bus / spurious side content): whether the source is "really mono" must come from the
    // actual input bus layout, not from the buffer's channel count. Our output bus is always forced
    // to stereo (see isBusesLayoutSupported), so when a host loads this on a mono track the buffer
    // itself will usually still have 2 channels - channel 1 is NOT guaranteed to be a duplicate of
    // channel 0 in that case, it can be silence/garbage. Trusting ch>1 there was turning a mono
    // signal into L vs 0, i.e. an artificial side signal equal to half the mid content.
    const bool trueMono = getTotalNumInputChannels()<=1;
    float inSum=0,outSum=0;
    // FIX (bug #1/#2): stereoOn/midOn/sideOn are now PURELY a display concern (which curve the
    // analyzer draws - see PluginEditor::paint). They must never again gate what the audio itself
    // does: Mid/Side used to be hard-zeroed here when their button was off, and since Stereo has no
    // signal of its own (it only corrects the already-recombined L/R), "soloing" Stereo by turning
    // Mid+Side off left it correcting silence - exactly the reported "input but no output" bug.
    // Mid, Side, and Stereo correction now always run, unconditionally, regardless of button state.
    const float widthAmt=juce::jlimit(0.f,1.f,widthAmount.load());
    const float widthDep=widthDepth.load();
    const WidthMode wMode=widthMode.load();
    const bool widthPost=widthPostEq.load();
    if(manualDirty.exchange(false)) rebuildManualCoefficients();
    const float inGain=juce::Decibels::decibelsToGain(inputTrimDb.load());
    const float outGain=juce::Decibels::decibelsToGain(outputTrimDb.load());
    float inPeakLin=0.f, outPeakLin=0.f;
    for(int i=0;i<n;++i){
        float L=b.getSample(0,i)*inGain, R=(trueMono?L:b.getSample(1,i)*inGain); inSum += .5f*(L*L+R*R);
        inPeakLin = juce::jmax(inPeakLin, std::abs(L), std::abs(R));

        // FIX (widener strength): delay times are now musically meaningful (ms, scaled to sample
        // rate) instead of a fixed ~1.3ms 64-sample buffer, so each mode is actually audible.
        auto applyWidth=[&](float& Lx,float& Rx){
            if(widthAmt<=0.0001f) return;
            float monoIn = trueMono ? Lx : 0.5f*(Lx+Rx);
            widthDelay[(size_t)widthWriteIdx]=monoIn;
            auto msToSamples=[&](float ms){ return juce::jlimit(1,widthBufSize-1,(int)std::round(ms*0.001f*(float)sr)); };
            auto tapAt=[&](int samplesBack){ int idx=widthWriteIdx-samplesBack; while(idx<0) idx+=widthBufSize; return widthDelay[(size_t)idx]; };
            float side=0.f;
            switch(wMode){
                case WidthMode::MicroShift: { float d=tapAt(msToSamples(2.5f)); side=(monoIn-d)*0.5f; break; }
                case WidthMode::Haas: { float d=tapAt(msToSamples(15.f)); side=monoIn-d; break; }
                case WidthMode::Decorrelated: { float d1=tapAt(msToSamples(7.f)), d2=tapAt(msToSamples(23.f)); side=(monoIn-0.5f*(d1+d2))*0.7071f; break; }
            }
            widthWriteIdx=(widthWriteIdx+1)%widthBufSize;
            // FIX (width still too subtle at max): a flat +35% boost on top of the existing
            // amount*depth scaling, so turning the sliders all the way up now reads as noticeably
            // wider than before, instead of the previous max still feeling conservative.
            constexpr float kWidthBoost=1.35f;
            side*=widthAmt*widthDep*kWidthBoost; Lx=monoIn+side; Rx=monoIn-side;
        };

        if(!widthPost) applyWidth(L,R);

        float m=.5f*(L+R), s=.5f*(L-R);
        // Analyzer/reference matching always sees the raw, pre-correction M/S content, regardless of
        // solo state - soloing a band to listen to it never affects what the analyzer measures.
        fftMid[(size_t)fftPos]=m; fftSide[(size_t)fftPos]=s;

        // FIX (item 4): coefficients must stay current regardless of which legs are on/off (e.g.
        // Mid muted but Side/Stereo still need up-to-date correction), so this now runs unconditionally
        // instead of only inside the old "no solo" branch.
        if(dirty.exchange(false)) rebuildCoefficients();
        // Mid: correction + any manual bands targeting Mid always run now (see FIX note above).
        for(int k=0;k<kBands;++k) m=process(midCoeff[k],stMid[k],m);
        for(int mbI=0;mbI<kMaxManualBands;++mbI){
            auto& band=manualBands[(size_t)mbI]; if(!band.active.load()||band.target.load()!=ManualTarget::Mid) continue;
            m=process(manualCoeff[(size_t)mbI],manualStateMid[(size_t)mbI],m);
        }
        // Side: same.
        for(int k=0;k<kBands;++k) s=process(sideCoeff[k],stSide[k],s);
        for(int mbI=0;mbI<kMaxManualBands;++mbI){
            auto& band=manualBands[(size_t)mbI]; if(!band.active.load()||band.target.load()!=ManualTarget::Side) continue;
            s=process(manualCoeff[(size_t)mbI],manualStateSide[(size_t)mbI],s);
        }
        L=m+s; R=m-s;
        // Stereo: final stage on the recombined L/R - also always runs now.
        for(int k=0;k<kBands;++k){L=process(stereoCoeff[k],stStereoL[k],L); R=process(stereoCoeff[k],stStereoR[k],R);}
        if(widthPost) applyWidth(L,R);
        for(int mbI=0;mbI<kMaxManualBands;++mbI){
            auto& band=manualBands[(size_t)mbI]; if(!band.active.load()||band.target.load()!=ManualTarget::Stereo) continue;
            L=process(manualCoeff[(size_t)mbI],manualStateL[(size_t)mbI],L);
            R=process(manualCoeff[(size_t)mbI],manualStateR[(size_t)mbI],R);
        }
        L*=outGain; R*=outGain;
        b.setSample(0,i,L); if(ch>1)b.setSample(1,i,R); outSum += .5f*(L*L+R*R);
        outPeakLin = juce::jmax(outPeakLin, std::abs(L), std::abs(R));
        // FIX (analyzer latency): advance the circular history buffer every sample, but only run the
        // (expensive) analysis every kHopSize samples - a 75% overlap - instead of once per full
        // kFFTSize block, so the on-screen curve updates ~4x more often with much lower perceived lag.
        fftPos=(fftPos+1)%kFFTSize;
        if(++hopCounter>=kHopSize){hopCounter=0; analyzeAndUpdate();}
    }
    // FIX (meter/match stability): one-pole smoothing block-to-block, both so the new vertical
    // meters don't flicker and so matchGain() (below) isn't reading a noisy instantaneous value.
    {
        float inDb=juce::Decibels::gainToDecibels(std::sqrt(inSum/juce::jmax(1,n))+1e-9f);
        float outDb=juce::Decibels::gainToDecibels(std::sqrt(outSum/juce::jmax(1,n))+1e-9f);
        constexpr float meterSmooth=0.25f;
        float prevIn=inputRmsDb.load(), prevOut=outputRmsDb.load();
        inputRmsDb.store(prevIn+meterSmooth*(inDb-prevIn));
        outputRmsDb.store(prevOut+meterSmooth*(outDb-prevOut));

        // Peak-hold: jump up instantly to this block's true peak, otherwise release the held value
        // at a fixed dB/sec rate - a thin peak-hold line next to the RMS bar (see drawVerticalMeter)
        // instead of just the current level. matchGain() below reads these, not the RMS pair, so it
        // can neutralize an actual peak change rather than just an average-loudness one.
        constexpr float kPeakDecayDbPerSec=12.f;
        const float decayThisBlock = kPeakDecayDbPerSec * (float)n / (float)juce::jmax(1.0,sr);
        auto updatePeak=[&](std::atomic<float>& peakAtomic,float peakLin){
            float instDb=juce::Decibels::gainToDecibels(peakLin+1e-9f);
            float decayed=peakAtomic.load()-decayThisBlock;
            peakAtomic.store(juce::jmax(instDb,decayed,kFloor));
        };
        updatePeak(inputPeakDb,inPeakLin);
        updatePeak(outputPeakDb,outPeakLin);
    }
}

void PQAudioProcessor::analyzeAndUpdate(){
    std::array<float,kFFTSize*2> buf{};
    // FIX (graphics quality): the analyzer used to look up a single nearest FFT bin per on-screen
    // point. That made low frequencies look like a staircase (many points sharing one wide bin
    // range) and high frequencies look noisy/spiky (many bins collapsed onto one point by picking
    // just one of them at random). Averaging power across the bin range each point actually
    // represents fixes both, without losing frequency accuracy.
    auto hzAt=[&](float t){ return std::exp(std::log(20.f)+t*(std::log(20000.f)-std::log(20.f))); };
    auto read=[&](const std::array<float,kFFTSize>& src,std::array<float,kBins>& dst){
        // FIX (corrupted spectrum): JUCE's performRealOnlyForwardTransform expects the raw input
        // samples packed *contiguously* into the first half of the (2x size) buffer - this used to
        // write buf[2*i]=src[i], which scatters every other sample into the wrong half of the array
        // entirely. That silently destroyed half the analysis window and aliased the rest, which is
        // the main reason the on-screen curve looked corrupted/pixelated rather than just "chunky".
        // `src` is also read here as a circular history buffer (oldest sample first) starting at the
        // current write cursor `fftPos`, which is what lets analysis run on overlapping frames.
        for(int i=0;i<kFFTSize;++i) buf[i]=src[(size_t)((fftPos+i)%kFFTSize)];
        for(int i=kFFTSize;i<kFFTSize*2;++i) buf[i]=0.f;
        window.multiplyWithWindowingTable(buf.data(),kFFTSize); fft.performRealOnlyForwardTransform(buf.data());
        auto powerAt=[&](int bin)->double{ bin=juce::jlimit(1,kFFTSize/2-1,bin); float re=buf[2*bin],im=buf[2*bin+1]; return (double)(re*re+im*im); };
        for(int i=0;i<kBins;++i){
            float t=i/float(kBins-1);
            float tLo = (i==0) ? t : 0.5f*(t + (i-1)/float(kBins-1));
            float tHi = (i==kBins-1) ? t : 0.5f*(t + (i+1)/float(kBins-1));
            float binPosLo = hzAt(tLo)*kFFTSize/sr, binPosHi = hzAt(tHi)*kFFTSize/sr;
            double sumPow;
            // FIX (item 7 - low end still "pixelated"): the real FFT bin spacing is sr/kFFTSize
            // (~2.7Hz even at the bigger 16384-point size). Below a few hundred Hz, one on-screen
            // point's Hz range (tLo..tHi) covers *less than one real bin*, so the old floor/ceil
            // range-average kept reusing the exact same one or two bins across many consecutive
            // points - a flat run that reads as a staircase step. Once the range is sub-bin-width we
            // now linearly interpolate power between the two nearest real bins at the exact
            // fractional bin position instead, so the value changes smoothly point-to-point even
            // though the underlying frequency resolution hasn't changed. Above that point (most of
            // the spectrum) nothing changes - the original multi-bin averaging still runs.
            if (binPosHi - binPosLo < 1.0f) {
                float centerPos = 0.5f*(binPosLo+binPosHi);
                int b0 = juce::jlimit(1,kFFTSize/2-2,(int)std::floor(centerPos));
                float frac = centerPos - (float)b0;
                double p0 = powerAt(b0), p1 = powerAt(b0+1);
                sumPow = p0 + (p1-p0)*(double)frac;
            } else {
                int binLo = juce::jlimit(1,kFFTSize/2-1,(int)std::floor(binPosLo));
                int binHi = juce::jlimit(binLo,kFFTSize/2-1,(int)std::ceil(binPosHi));
                double sp=0.0; int count=0;
                for(int bIdx=binLo;bIdx<=binHi;++bIdx){ sp+=powerAt(bIdx); ++count; }
                sumPow = sp/juce::jmax(1,count);
            }
            float mag = (float)std::sqrt(juce::jmax(0.0,sumPow));
            dst[(size_t)i]=juce::Decibels::gainToDecibels(mag/(float)kFFTSize+1e-9f);
        }
    };
    read(fftMid,liveMid); read(fftSide,liveSide);
    // FIX (graphics quality): light temporal smoothing (one-pole glide toward the new value) so the
    // curve eases between analysis frames instead of snapping - the previous instant-replace read as
    // cheap/jumpy motion.
    const float smoothing=0.35f;
    auto smoothStore=[&](std::array<std::atomic<float>,kBins>& curve,float newVal,int i){
        float prev=curve[(size_t)i].load(); float target=juce::jlimit(kFloor,6.f,newVal);
        curve[(size_t)i].store(prev + smoothing*(target-prev));
    };
    for(int i=0;i<kBins;++i){
        float m=juce::Decibels::decibelsToGain(liveMid[i]),s=juce::Decibels::decibelsToGain(liveSide[i]);
        liveStereo[i]=juce::Decibels::gainToDecibels(std::sqrt(m*m+s*s)+1e-9f);
        smoothStore(stereoCurve,liveStereo[i],i); smoothStore(midCurve,liveMid[i],i); smoothStore(sideCurve,liveSide[i],i);
    }
    if(hasReference.load()){buildCorrection(corrMid,refMid,liveMid,midMatch.load());buildCorrection(corrSide,refSide,liveSide,sideMatch.load());buildCorrection(corrStereo,refStereo,liveStereo,stereoMatch.load());dirty.store(true);}
}

void PQAudioProcessor::buildCorrection(std::array<float,kBands>& out,const std::array<std::atomic<float>,kBins>& ref,const std::array<float,kBins>& live,float amount){
    float lo=juce::jlimit(20.f,20000.f,lowHz.load()), hi=juce::jlimit(lo,20000.f,highHz.load()), cap=juce::jlimit(0.f,kMaxCap,maxCorrectionDb.load()), amt=juce::jlimit(0.f,1.f,amount);
    for(int i=0;i<kBands;++i){float h=bandHz[i];float d=(h>=lo&&h<=hi)?interpAtomic(ref,h)-interp(live,h):0;out[i]=juce::jlimit(-cap,cap,d)*amt;}
    const int radius=juce::jlimit(1,8,(int)std::round(smoothingOctaves.load()*4.f)); auto c=out;
    for(int i=0;i<kBands;++i){float sum=0,w=0;for(int j=juce::jmax(0,i-radius);j<=juce::jmin(kBands-1,i+radius);++j){float q=float(j-i)/radius,ww=std::exp(-2*q*q);sum+=c[j]*ww;w+=ww;}out[i]=sum/w;}
}

PQAudioProcessor::Coeff PQAudioProcessor::peak(float hz,float gainDb,float Q,float sampleRate){double A=std::pow(10.0,gainDb/40.0),w=2.0*juce::MathConstants<double>::pi*hz/sampleRate,alpha=std::sin(w)/(2.0*Q),c=std::cos(w);double b0=1+alpha*A,b1=-2*c,b2=1-alpha*A,a0=1+alpha/A,a1=-2*c,a2=1-alpha/A;return {(float)(b0/a0),(float)(b1/a0),(float)(b2/a0),(float)(a1/a0),(float)(a2/a0)};}
float PQAudioProcessor::process(const Coeff&c,State&z,float x){float y=c.b0*x+z.z1;z.z1=c.b1*x-c.a1*y+z.z2;z.z2=c.b2*x-c.a2*y;return y;}

// ---- Manual EQ: standard RBJ "cookbook" biquads, one shape per ManualType. -------------------
PQAudioProcessor::Coeff PQAudioProcessor::makeManualCoeff(ManualType type,float hz,float gainDb,float q,double sampleRate){
    if(sampleRate<=0.0) sampleRate=44100.0;
    hz=juce::jlimit(20.f,20000.f,hz); q=juce::jlimit(0.1f,18.f,q);
    double w0=2.0*juce::MathConstants<double>::pi*hz/sampleRate, c=std::cos(w0), sN=std::sin(w0), alpha=sN/(2.0*q);
    double b0=1,b1=0,b2=0,a0=1,a1=0,a2=0;
    switch(type){
        case ManualType::Bell: { double A=std::pow(10.0,gainDb/40.0); b0=1+alpha*A;b1=-2*c;b2=1-alpha*A;a0=1+alpha/A;a1=-2*c;a2=1-alpha/A; break; }
        case ManualType::Notch: { b0=1;b1=-2*c;b2=1;a0=1+alpha;a1=-2*c;a2=1-alpha; break; }
        case ManualType::LowCut: { b0=(1+c)/2;b1=-(1+c);b2=(1+c)/2;a0=1+alpha;a1=-2*c;a2=1-alpha; break; }
        case ManualType::HighCut: { b0=(1-c)/2;b1=1-c;b2=(1-c)/2;a0=1+alpha;a1=-2*c;a2=1-alpha; break; }
        case ManualType::LowShelf: { double A=std::pow(10.0,gainDb/40.0),sq=2.0*std::sqrt(A)*alpha;
            b0=A*((A+1)-(A-1)*c+sq); b1=2*A*((A-1)-(A+1)*c); b2=A*((A+1)-(A-1)*c-sq);
            a0=(A+1)+(A-1)*c+sq; a1=-2*((A-1)+(A+1)*c); a2=(A+1)+(A-1)*c-sq; break; }
        case ManualType::HighShelf: { double A=std::pow(10.0,gainDb/40.0),sq=2.0*std::sqrt(A)*alpha;
            b0=A*((A+1)+(A-1)*c+sq); b1=-2*A*((A-1)+(A+1)*c); b2=A*((A+1)+(A-1)*c-sq);
            a0=(A+1)-(A-1)*c+sq; a1=2*((A-1)-(A+1)*c); a2=(A+1)-(A-1)*c-sq; break; }
    }
    return {(float)(b0/a0),(float)(b1/a0),(float)(b2/a0),(float)(a1/a0),(float)(a2/a0)};
}
void PQAudioProcessor::rebuildManualCoefficients(){
    for(int i=0;i<kMaxManualBands;++i){ auto& mb=manualBands[(size_t)i];
        manualCoeff[(size_t)i]=makeManualCoeff(mb.type.load(),mb.freq.load(),mb.gainDb.load(),mb.q.load(),sr);
    }
}
int PQAudioProcessor::addManualBand(ManualType type,float freq,float gainDb,float q,ManualTarget target){
    for(int i=0;i<kMaxManualBands;++i){ auto& mb=manualBands[(size_t)i];
        if(!mb.active.load()){
            mb.type.store(type); mb.target.store(target); mb.freq.store(juce::jlimit(20.f,20000.f,freq)); mb.gainDb.store(juce::jlimit(-24.f,24.f,gainDb)); mb.q.store(juce::jlimit(0.1f,18.f,q));
            // Slot may be reused from a previously deleted band; clear all four possible filter
            // states so no stale history from that old band (or an old target) leaks in.
            manualStateL[(size_t)i]={}; manualStateR[(size_t)i]={}; manualStateMid[(size_t)i]={}; manualStateSide[(size_t)i]={};
            mb.active.store(true); manualDirty.store(true); return i;
        }
    }
    return -1; // no free slot (kMaxManualBands reached)
}
void PQAudioProcessor::removeManualBand(int index){ if(index<0||index>=kMaxManualBands)return; manualBands[(size_t)index].active.store(false); manualDirty.store(true); }
void PQAudioProcessor::setManualBandTarget(int index,ManualTarget target){
    if(index<0||index>=kMaxManualBands)return; auto& mb=manualBands[(size_t)index];
    if(mb.target.load()==target) return;
    mb.target.store(target);
    // Reset every path's state on the switch (see header comment on setManualBandTarget) so the
    // band starts clean on its new signal instead of continuing from unrelated filter history.
    manualStateL[(size_t)index]={}; manualStateR[(size_t)index]={}; manualStateMid[(size_t)index]={}; manualStateSide[(size_t)index]={};
}
void PQAudioProcessor::setManualBand(int index,ManualType type,float freq,float gainDb,float q){ if(index<0||index>=kMaxManualBands)return; auto& mb=manualBands[(size_t)index]; mb.type.store(type); mb.freq.store(juce::jlimit(20.f,20000.f,freq)); mb.gainDb.store(juce::jlimit(-24.f,24.f,gainDb)); mb.q.store(juce::jlimit(0.1f,18.f,q)); manualDirty.store(true); }
void PQAudioProcessor::setManualBandType(int index,ManualType type){ if(index<0||index>=kMaxManualBands)return; manualBands[(size_t)index].type.store(type); manualDirty.store(true); }
void PQAudioProcessor::setManualBandFreqGain(int index,float freq,float gainDb){ if(index<0||index>=kMaxManualBands)return; auto& mb=manualBands[(size_t)index]; mb.freq.store(juce::jlimit(20.f,20000.f,freq)); mb.gainDb.store(juce::jlimit(-24.f,24.f,gainDb)); manualDirty.store(true); }
void PQAudioProcessor::setManualBandQ(int index,float q){ if(index<0||index>=kMaxManualBands)return; manualBands[(size_t)index].q.store(juce::jlimit(0.1f,18.f,q)); manualDirty.store(true); }

void PQAudioProcessor::rebuildCoefficients(){if(sr<=0)return;for(int i=0;i<kBands;++i){stereoCoeff[i]=peak(bandHz[i],corrStereo[i],.8f,(float)sr);midCoeff[i]=peak(bandHz[i],corrMid[i],.8f,(float)sr);sideCoeff[i]=peak(bandHz[i],corrSide[i],.8f,(float)sr);}generation.fetch_add(1);}
void PQAudioProcessor::captureReference(){for(int i=0;i<kBins;++i){refStereo[i].store(stereoCurve[i].load());refMid[i].store(midCurve[i].load());refSide[i].store(sideCurve[i].load());}hasReference.store(true);applyMatch();}
void PQAudioProcessor::clearReference(){hasReference.store(false);corrStereo.fill(0);corrMid.fill(0);corrSide.fill(0);dirty.store(true);}
void PQAudioProcessor::applyMatch(){if(!hasReference.load())return;buildCorrection(corrMid,refMid,liveMid,midMatch.load());buildCorrection(corrSide,refSide,liveSide,sideMatch.load());buildCorrection(corrStereo,refStereo,liveStereo,stereoMatch.load());dirty.store(true);}

// "MATCH GAIN": nudges outputTrimDb so the (already-trimmed) output's true peak reads the same as
// the input's true peak - not the average (RMS) level. If the EQ/match/width processing raised or
// lowered the output's peak versus the input, that's exactly what this cancels out, so an A/B
// against the input isn't biased by a peak change the frequency correction introduced. Since
// outputTrimDb is a plain linear gain in dB, and outputPeakDb was measured *with* the current trim
// already applied, the correction is just the remaining gap between the two peak-hold values.
void PQAudioProcessor::matchGain(){
    float diff=inputPeakDb.load()-outputPeakDb.load();
    outputTrimDb.store(juce::jlimit(-24.f,24.f,outputTrimDb.load()+diff));
}

// FIX (item 6): a "preset" is now defined as the *entire* plugin state - reference curves, manual
// EQ bands, width, match amounts and trims - not just the reference curve. Both save and load now
// go through getStateInformation()/applyStateBlock(), the exact same code path the host uses for
// session save/restore, so there is only ever one serialization format to keep correct.
bool PQAudioProcessor::saveReference(const juce::File& f){
    juce::MemoryBlock mb; getStateInformation(mb);
    return f.replaceWithData(mb.getData(),mb.getSize());
}
bool PQAudioProcessor::loadReference(const juce::File& f){
    juce::MemoryBlock mb; if(!f.loadFileAsData(mb))return false;
    return applyStateBlock(mb.getData(),(int)mb.getSize());
}

void PQAudioProcessor::getStateInformation(juce::MemoryBlock& d){
    juce::MemoryOutputStream o(d,true);
    o.writeInt(0x50515338); // FIX: version bumped (was 0x50515337) to add per-band manual-EQ target (Stereo/Mid/Side)
    for(float v:{stereoMatch.load(),midMatch.load(),sideMatch.load(),lowHz.load(),highHz.load(),maxCorrectionDb.load(),smoothingOctaves.load(),widthAmount.load(),widthDepth.load()})o.writeFloat(v);
    o.writeInt((int)widthMode.load());
    o.writeBool(widthPostEq.load());
    o.writeFloat(inputTrimDb.load()); o.writeFloat(outputTrimDb.load());
    o.writeBool(hasReference.load());
    if(hasReference.load()) for(auto* a:{&refStereo,&refMid,&refSide}) for(int i=0;i<kBins;++i) o.writeFloat((*a)[(size_t)i].load());
    o.writeInt(kMaxManualBands);
    for(auto& mb:manualBands){ o.writeBool(mb.active.load()); o.writeInt((int)mb.type.load()); o.writeFloat(mb.freq.load()); o.writeFloat(mb.gainDb.load()); o.writeFloat(mb.q.load()); o.writeInt((int)mb.target.load()); }
}
void PQAudioProcessor::setStateInformation(const void* data,int size){ applyStateBlock(data,size); }

bool PQAudioProcessor::applyStateBlock(const void* data,int size){
    juce::MemoryInputStream in(data,(size_t)size,false);
    auto magic=in.readInt();
    // Legacy reference-only .pqref (pre-preset era). Load *only* the reference + range/limits and
    // stop - every other current setting (manual EQ, width, trims, match amounts) is left exactly
    // as it was, so opening an old file can never silently reset things it never saved.
    if(magic==0x50525133){
        if(in.readInt()!=kBins) return false;
        for(auto* a:{&refStereo,&refMid,&refSide}) for(int i=0;i<kBins;++i) (*a)[(size_t)i].store(in.readFloat());
        lowHz.store(in.readFloat());highHz.store(in.readFloat());maxCorrectionDb.store(in.readFloat());smoothingOctaves.store(in.readFloat());
        hasReference.store(true); applyMatch(); return true;
    }
    if(magic!=0x50515338 && magic!=0x50515337 && magic!=0x50515336 && magic!=0x50515335 && magic!=0x50515334 && magic!=0x50515333 && magic!=0x50515332) return false; // unknown/corrupt state: ignore rather than risk misreading garbage into the DSP
    bool hasManualTarget = (magic==0x50515338);
    bool hasTrim = (magic==0x50515338 || magic==0x50515337);
    bool hasManualEq = (magic==0x50515338 || magic==0x50515337 || magic==0x50515336);
    bool hasWidthStage = (magic==0x50515338 || magic==0x50515337 || magic==0x50515336 || magic==0x50515335);
    bool hadOldEnableFlags = (magic==0x50515333); // v3 wrote 3 bools we no longer use; skip them so the rest of the stream stays aligned
    stereoMatch.store(in.readFloat());midMatch.store(in.readFloat());sideMatch.store(in.readFloat());
    lowHz.store(in.readFloat());highHz.store(in.readFloat());maxCorrectionDb.store(in.readFloat());smoothingOctaves.store(in.readFloat());
    widthAmount.store(in.readFloat());widthDepth.store(in.readFloat());
    widthMode.store((WidthMode)in.readInt());
    if(hasWidthStage) widthPostEq.store(in.readBool()); else widthPostEq.store(false);
    if(hadOldEnableFlags){ in.readBool(); in.readBool(); in.readBool(); }
    if(hasTrim){ inputTrimDb.store(in.readFloat()); outputTrimDb.store(in.readFloat()); } else { inputTrimDb.store(0.f); outputTrimDb.store(0.f); }
    bool hr=in.readBool();
    if(hr) for(auto* a:{&refStereo,&refMid,&refSide}) for(int i=0;i<kBins;++i) (*a)[(size_t)i].store(in.readFloat());
    hasReference.store(hr);
    for(auto& mb:manualBands){ mb.active.store(false); mb.target.store(ManualTarget::Stereo); } // clear before loading, in case an older/smaller save is loaded
    if(hasManualEq){
        int savedCount=in.readInt();
        for(int i=0;i<savedCount;++i){
            bool active=in.readBool(); auto type=(ManualType)in.readInt(); float f=in.readFloat(), g=in.readFloat(), q=in.readFloat();
            // Older saves (pre-target) have no target field on disk; those bands default to Stereo,
            // which is exactly how they behaved before this field existed.
            ManualTarget target = hasManualTarget ? (ManualTarget)in.readInt() : ManualTarget::Stereo;
            if(i<kMaxManualBands && active){ auto& mb=manualBands[(size_t)i]; mb.type.store(type); mb.freq.store(f); mb.gainDb.store(g); mb.q.store(q); mb.target.store(target); mb.active.store(true); }
        }
    }
    manualDirty.store(true);
    // FIX: these are display-only now (see header) and were being forced back to "true" on every
    // state load regardless of what the user last had checked. Leave them as whatever they already
    // are - since they're not written/read in this state block, that's simply their pre-load value.
    applyMatch();
    return true;
}
juce::AudioProcessorEditor* PQAudioProcessor::createEditor(){return new PQAudioProcessorEditor(*this);}

// Required by JUCE's plugin client wrapper (VST3/AU/etc.) to know which
// AudioProcessor subclass to instantiate. Without this, the plugin compiles
// fine but fails at link time with "unresolved external symbol createPluginFilter".
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PQAudioProcessor();
}
