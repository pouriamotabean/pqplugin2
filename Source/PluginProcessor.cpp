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
    stStereoL.fill({});stStereoR.fill({});stMid.fill({});stSide.fill({}); manualStateL.fill({}); manualStateR.fill({});
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
    const SoloBand soloNow = solo.load();
    const float widthAmt=juce::jlimit(0.f,1.f,widthAmount.load());
    const float widthDep=widthDepth.load();
    const WidthMode wMode=widthMode.load();
    const bool widthPost=widthPostEq.load();
    if(manualDirty.exchange(false)) rebuildManualCoefficients();
    for(int i=0;i<n;++i){
        float L=b.getSample(0,i), R=trueMono?L:b.getSample(1,i); inSum += .5f*(L*L+R*R);

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
            side*=widthAmt*widthDep; Lx=monoIn+side; Rx=monoIn-side;
        };

        if(!widthPost) applyWidth(L,R);

        float m=.5f*(L+R), s=.5f*(L-R);
        // Analyzer/reference matching always sees the raw, pre-correction M/S content, regardless of
        // solo state - soloing a band to listen to it never affects what the analyzer measures.
        fftMid[(size_t)fftPos]=m; fftSide[(size_t)fftPos]=s;

        if(soloNow==SoloBand::Mid){
            // FIX (real solo): play the raw mid content only, on both channels, so it can be
            // auditioned before any EQ correction is applied.
            L=m; R=m;
        } else if(soloNow==SoloBand::Side){
            // FIX (real solo): play the raw side (difference) content only.
            L=s; R=-s;
        } else {
            // Normal path (also used when "STEREO" is selected, since that just means "no band is
            // isolated - hear the full corrected mix").
            if(dirty.exchange(false)) rebuildCoefficients();
            for(int k=0;k<kBands;++k) m=process(midCoeff[k],stMid[k],m);
            for(int k=0;k<kBands;++k) s=process(sideCoeff[k],stSide[k],s);
            L=m+s; R=m-s;
            for(int k=0;k<kBands;++k){L=process(stereoCoeff[k],stStereoL[k],L); R=process(stereoCoeff[k],stStereoR[k],R);}
            if(widthPost) applyWidth(L,R);
            // Manual, mouse-placed EQ bands: the final stage, applied identically (same
            // coefficients, independent state) to both channels so it never introduces width.
            for(int mb=0;mb<kMaxManualBands;++mb){
                if(!manualBands[(size_t)mb].active.load()) continue;
                L=process(manualCoeff[(size_t)mb],manualStateL[(size_t)mb],L);
                R=process(manualCoeff[(size_t)mb],manualStateR[(size_t)mb],R);
            }
        }
        b.setSample(0,i,L); if(ch>1)b.setSample(1,i,R); outSum += .5f*(L*L+R*R);
        // FIX (analyzer latency): advance the circular history buffer every sample, but only run the
        // (expensive) analysis every kHopSize samples - a 75% overlap - instead of once per full
        // kFFTSize block, so the on-screen curve updates ~4x more often with much lower perceived lag.
        fftPos=(fftPos+1)%kFFTSize;
        if(++hopCounter>=kHopSize){hopCounter=0; analyzeAndUpdate();}
    }
    inputRmsDb.store(juce::Decibels::gainToDecibels(std::sqrt(inSum/juce::jmax(1,n))+1e-9f)); outputRmsDb.store(juce::Decibels::gainToDecibels(std::sqrt(outSum/juce::jmax(1,n))+1e-9f));
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
        for(int i=0;i<kBins;++i){
            float t=i/float(kBins-1);
            float tLo = (i==0) ? t : 0.5f*(t + (i-1)/float(kBins-1));
            float tHi = (i==kBins-1) ? t : 0.5f*(t + (i+1)/float(kBins-1));
            int binLo = juce::jlimit(1,kFFTSize/2-1,(int)std::floor(hzAt(tLo)*kFFTSize/sr));
            int binHi = juce::jlimit(binLo,kFFTSize/2-1,(int)std::ceil(hzAt(tHi)*kFFTSize/sr));
            double sumPow=0.0; int count=0;
            for(int bIdx=binLo;bIdx<=binHi;++bIdx){ float re=buf[2*bIdx],im=buf[2*bIdx+1]; sumPow += (double)(re*re+im*im); ++count; }
            float mag = count>0 ? (float)std::sqrt(sumPow/count) : 0.f;
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
int PQAudioProcessor::addManualBand(ManualType type,float freq,float gainDb,float q){
    for(int i=0;i<kMaxManualBands;++i){ auto& mb=manualBands[(size_t)i];
        if(!mb.active.load()){
            mb.type.store(type); mb.freq.store(juce::jlimit(20.f,20000.f,freq)); mb.gainDb.store(juce::jlimit(-24.f,24.f,gainDb)); mb.q.store(juce::jlimit(0.1f,18.f,q));
            mb.active.store(true); manualDirty.store(true); return i;
        }
    }
    return -1; // no free slot (kMaxManualBands reached)
}
void PQAudioProcessor::removeManualBand(int index){ if(index<0||index>=kMaxManualBands)return; manualBands[(size_t)index].active.store(false); manualDirty.store(true); }
void PQAudioProcessor::setManualBand(int index,ManualType type,float freq,float gainDb,float q){ if(index<0||index>=kMaxManualBands)return; auto& mb=manualBands[(size_t)index]; mb.type.store(type); mb.freq.store(juce::jlimit(20.f,20000.f,freq)); mb.gainDb.store(juce::jlimit(-24.f,24.f,gainDb)); mb.q.store(juce::jlimit(0.1f,18.f,q)); manualDirty.store(true); }
void PQAudioProcessor::setManualBandType(int index,ManualType type){ if(index<0||index>=kMaxManualBands)return; manualBands[(size_t)index].type.store(type); manualDirty.store(true); }
void PQAudioProcessor::setManualBandFreqGain(int index,float freq,float gainDb){ if(index<0||index>=kMaxManualBands)return; auto& mb=manualBands[(size_t)index]; mb.freq.store(juce::jlimit(20.f,20000.f,freq)); mb.gainDb.store(juce::jlimit(-24.f,24.f,gainDb)); manualDirty.store(true); }
void PQAudioProcessor::setManualBandQ(int index,float q){ if(index<0||index>=kMaxManualBands)return; manualBands[(size_t)index].q.store(juce::jlimit(0.1f,18.f,q)); manualDirty.store(true); }

void PQAudioProcessor::rebuildCoefficients(){if(sr<=0)return;for(int i=0;i<kBands;++i){stereoCoeff[i]=peak(bandHz[i],corrStereo[i],.8f,(float)sr);midCoeff[i]=peak(bandHz[i],corrMid[i],.8f,(float)sr);sideCoeff[i]=peak(bandHz[i],corrSide[i],.8f,(float)sr);}generation.fetch_add(1);}
void PQAudioProcessor::captureReference(){for(int i=0;i<kBins;++i){refStereo[i].store(stereoCurve[i].load());refMid[i].store(midCurve[i].load());refSide[i].store(sideCurve[i].load());}hasReference.store(true);applyMatch();}
void PQAudioProcessor::clearReference(){hasReference.store(false);corrStereo.fill(0);corrMid.fill(0);corrSide.fill(0);dirty.store(true);}
void PQAudioProcessor::applyMatch(){if(!hasReference.load())return;buildCorrection(corrMid,refMid,liveMid,midMatch.load());buildCorrection(corrSide,refSide,liveSide,sideMatch.load());buildCorrection(corrStereo,refStereo,liveStereo,stereoMatch.load());dirty.store(true);}

bool PQAudioProcessor::saveReference(const juce::File& f){
    juce::MemoryOutputStream o; o.writeInt(0x50525133); o.writeInt(kBins);
    for(auto* a:{&refStereo,&refMid,&refSide}) for(int i=0;i<kBins;++i) o.writeFloat((*a)[(size_t)i].load());
    o.writeFloat(lowHz.load());o.writeFloat(highHz.load());o.writeFloat(maxCorrectionDb.load());o.writeFloat(smoothingOctaves.load());
    return f.replaceWithData(o.getData(),o.getDataSize());
}
bool PQAudioProcessor::loadReference(const juce::File& f){
    juce::MemoryBlock mb; if(!f.loadFileAsData(mb))return false;
    juce::MemoryInputStream in(mb,false);
    if(in.readInt()!=0x50525133||in.readInt()!=kBins)return false; // FIX: bumped magic since on-disk layout changed (atomic-per-float write order is unchanged, but this guards against loading a v1.2 .pqref file with a different internal format silently)
    for(auto* a:{&refStereo,&refMid,&refSide}) for(int i=0;i<kBins;++i) (*a)[(size_t)i].store(in.readFloat());
    lowHz.store(in.readFloat());highHz.store(in.readFloat());maxCorrectionDb.store(in.readFloat());smoothingOctaves.store(in.readFloat());
    hasReference.store(true);applyMatch();return true;
}

void PQAudioProcessor::getStateInformation(juce::MemoryBlock& d){
    juce::MemoryOutputStream o(d,true);
    o.writeInt(0x50515336); // FIX: version bumped (was 0x50515335) to add the manual EQ bands
    for(float v:{stereoMatch.load(),midMatch.load(),sideMatch.load(),lowHz.load(),highHz.load(),maxCorrectionDb.load(),smoothingOctaves.load(),widthAmount.load(),widthDepth.load()})o.writeFloat(v);
    o.writeInt((int)widthMode.load());
    o.writeBool(widthPostEq.load());
    o.writeBool(hasReference.load());
    if(hasReference.load()) for(auto* a:{&refStereo,&refMid,&refSide}) for(int i=0;i<kBins;++i) o.writeFloat((*a)[(size_t)i].load());
    o.writeInt(kMaxManualBands);
    for(auto& mb:manualBands){ o.writeBool(mb.active.load()); o.writeInt((int)mb.type.load()); o.writeFloat(mb.freq.load()); o.writeFloat(mb.gainDb.load()); o.writeFloat(mb.q.load()); }
}
void PQAudioProcessor::setStateInformation(const void* data,int size){
    juce::MemoryInputStream in(data,(size_t)size,false);
    auto magic=in.readInt();
    if(magic!=0x50515336 && magic!=0x50515335 && magic!=0x50515334 && magic!=0x50515333 && magic!=0x50515332) return; // unknown/corrupt state: ignore rather than risk misreading garbage into the DSP
    bool hasManualEq = (magic==0x50515336);
    bool hasWidthStage = (magic==0x50515336 || magic==0x50515335);
    bool hadOldEnableFlags = (magic==0x50515333); // v3 wrote 3 bools we no longer use; skip them so the rest of the stream stays aligned
    stereoMatch.store(in.readFloat());midMatch.store(in.readFloat());sideMatch.store(in.readFloat());
    lowHz.store(in.readFloat());highHz.store(in.readFloat());maxCorrectionDb.store(in.readFloat());smoothingOctaves.store(in.readFloat());
    widthAmount.store(in.readFloat());widthDepth.store(in.readFloat());
    widthMode.store((WidthMode)in.readInt());
    if(hasWidthStage) widthPostEq.store(in.readBool()); else widthPostEq.store(false);
    if(hadOldEnableFlags){ in.readBool(); in.readBool(); in.readBool(); }
    bool hr=in.readBool();
    if(hr) for(auto* a:{&refStereo,&refMid,&refSide}) for(int i=0;i<kBins;++i) (*a)[(size_t)i].store(in.readFloat());
    hasReference.store(hr);
    for(auto& mb:manualBands) mb.active.store(false); // clear before loading, in case an older/smaller save is loaded
    if(hasManualEq){
        int savedCount=in.readInt();
        for(int i=0;i<savedCount;++i){
            bool active=in.readBool(); auto type=(ManualType)in.readInt(); float f=in.readFloat(), g=in.readFloat(), q=in.readFloat();
            if(i<kMaxManualBands && active){ auto& mb=manualBands[(size_t)i]; mb.type.store(type); mb.freq.store(f); mb.gainDb.store(g); mb.q.store(q); mb.active.store(true); }
        }
    }
    manualDirty.store(true);
    solo.store(SoloBand::None);
    applyMatch();
}
juce::AudioProcessorEditor* PQAudioProcessor::createEditor(){return new PQAudioProcessorEditor(*this);}

// Required by JUCE's plugin client wrapper (VST3/AU/etc.) to know which
// AudioProcessor subclass to instantiate. Without this, the plugin compiles
// fine but fails at link time with "unresolved external symbol createPluginFilter".
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PQAudioProcessor();
}
