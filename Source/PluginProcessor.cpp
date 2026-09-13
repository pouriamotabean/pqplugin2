#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace { constexpr float kFloor=-90.f; constexpr float kMaxCap=12.f; }

PQAudioProcessor::PQAudioProcessor():AudioProcessor(BusesProperties().withInput("Input",juce::AudioChannelSet::stereo(),true).withOutput("Output",juce::AudioChannelSet::stereo(),true))
{
    for(auto& x:stereoCurve)x.store(kFloor); for(auto& x:midCurve)x.store(kFloor); for(auto& x:sideCurve)x.store(kFloor);
    for(auto& x:refStereo)x.store(kFloor); for(auto& x:refMid)x.store(kFloor); for(auto& x:refSide)x.store(kFloor);
}

void PQAudioProcessor::prepareToPlay(double sampleRate,int samplesPerBlock){
    juce::ignoreUnused(samplesPerBlock); sr=sampleRate; fftPos=0; widthWrite=0; prevMono=0; std::fill(std::begin(widthDelay),std::end(widthDelay),0.f);
    for(int i=0;i<kBands;++i){float t=i/float(kBands-1); bandHz[i]=std::exp(std::log(20.f)+t*(std::log(20000.f)-std::log(20.f)));}
    stStereoL.fill({});stStereoR.fill({});stMid.fill({});stSide.fill({}); dirty.store(true); rebuildCoefficients();
}

bool PQAudioProcessor::isBusesLayoutSupported(const BusesLayout& l) const { auto in=l.getMainInputChannelSet(),out=l.getMainOutputChannelSet(); return (in==juce::AudioChannelSet::mono()||in==juce::AudioChannelSet::stereo()) && out==juce::AudioChannelSet::stereo(); }
float PQAudioProcessor::logFreq(float f){return std::log10(juce::jlimit(20.f,20000.f,f));}
float PQAudioProcessor::interp(const std::array<float,kBins>& a,float hz){float t=(logFreq(hz)-logFreq(20.f))/(logFreq(20000.f)-logFreq(20.f));float p=t*(kBins-1);int i=juce::jlimit(0,kBins-2,(int)std::floor(p));float f=p-i;return a[(size_t)i]+(a[(size_t)i+1]-a[(size_t)i])*f;}
// Same log-frequency interpolation as interp(), but reads from an atomic array (used for the
// reference curves, which can be written by the UI thread at any time).
float PQAudioProcessor::interpAtomic(const std::array<std::atomic<float>,kBins>& a,float hz){float t=(logFreq(hz)-logFreq(20.f))/(logFreq(20000.f)-logFreq(20.f));float p=t*(kBins-1);int i=juce::jlimit(0,kBins-2,(int)std::floor(p));float f=p-i;float a0=a[(size_t)i].load(),a1=a[(size_t)i+1].load();return a0+(a1-a0)*f;}

void PQAudioProcessor::processBlock(juce::AudioBuffer<float>& b,juce::MidiBuffer&){
    juce::ScopedNoDenormals nd; const int ch=b.getNumChannels(), n=b.getNumSamples(); if(ch==0)return;
    float inSum=0,outSum=0;
    const SoloBand soloNow = solo.load();
    const float widthAmt=juce::jlimit(0.f,1.f,widthAmount.load());
    const float widthDep=widthDepth.load();
    const WidthMode wMode=widthMode.load();
    const bool widthPost=widthPostEq.load();
    for(int i=0;i<n;++i){
        float L=b.getSample(0,i), R=ch>1?b.getSample(1,i):L; inSum += .5f*(L*L+R*R);

        // FIX: the widener used to only run when ch==1 (a literal mono buffer), but a DAW mixer
        // channel almost always presents 2 channels even for mono source material (dual-mono), so
        // that branch effectively never fired. It now drives off the current mono content
        // (average of L/R when stereo) instead, so it actually works in normal use.
        auto applyWidth=[&](float& Lx,float& Rx){
            if(widthAmt<=0.0001f) return;
            float monoIn = ch>1 ? 0.5f*(Lx+Rx) : Lx;
            float delayed=widthDelay[widthWrite]; widthDelay[widthWrite]=monoIn; widthWrite=(widthWrite+1)&63;
            float side=0.f;
            switch(wMode){
                case WidthMode::MicroShift: side=(monoIn-delayed)*0.5f; break;
                case WidthMode::Haas: side=monoIn-delayed; break;
                case WidthMode::Decorrelated: { float d=widthDelay[(widthWrite+17)&63]; side=(monoIn-d)*0.7071f; break; }
            }
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
        }
        b.setSample(0,i,L); if(ch>1)b.setSample(1,i,R); else b.setSample(1,i,R); outSum += .5f*(L*L+R*R);
        if(++fftPos>=kFFTSize){fftPos=0; analyzeAndUpdate();}
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
        for(int i=0;i<kFFTSize;++i)buf[2*i]=src[i]; window.multiplyWithWindowingTable(buf.data(),kFFTSize); fft.performRealOnlyForwardTransform(buf.data());
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
    o.writeInt(0x50515335); // FIX: version bumped (was 0x50515334) to add the width pre/post-EQ flag
    for(float v:{stereoMatch.load(),midMatch.load(),sideMatch.load(),lowHz.load(),highHz.load(),maxCorrectionDb.load(),smoothingOctaves.load(),widthAmount.load(),widthDepth.load()})o.writeFloat(v);
    o.writeInt((int)widthMode.load());
    o.writeBool(widthPostEq.load());
    o.writeBool(hasReference.load());
    if(hasReference.load()) for(auto* a:{&refStereo,&refMid,&refSide}) for(int i=0;i<kBins;++i) o.writeFloat((*a)[(size_t)i].load());
}
void PQAudioProcessor::setStateInformation(const void* data,int size){
    juce::MemoryInputStream in(data,(size_t)size,false);
    auto magic=in.readInt();
    if(magic!=0x50515335 && magic!=0x50515334 && magic!=0x50515333 && magic!=0x50515332) return; // unknown/corrupt state: ignore rather than risk misreading garbage into the DSP
    bool hasWidthStage = (magic==0x50515335);
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
