#pragma once
#include <JuceHeader.h>
#include <array>
#include <atomic>

class PQAudioProcessor : public juce::AudioProcessor
{
public:
    static constexpr int kFFTOrder=13;
    static constexpr int kFFTSize=1<<kFFTOrder;
    static constexpr int kBins=1024;
    static constexpr int kBands=36;
    enum class WidthMode { MicroShift, Haas, Decorrelated };
    struct Coeff { float b0=1,b1=0,b2=0,a1=0,a2=0; };
    struct Bank { std::array<Coeff,kBands> stereo{},mid{},side{}; };

    PQAudioProcessor(); ~PQAudioProcessor() override = default;
    void prepareToPlay(double,int) override; void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&,juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override; bool hasEditor() const override{return true;}
    const juce::String getName() const override{return "PQ";} bool acceptsMidi() const override{return false;} bool producesMidi() const override{return false;} bool isMidiEffect() const override{return false;} double getTailLengthSeconds() const override{return 0;}
    int getNumPrograms() override{return 1;} int getCurrentProgram() override{return 0;} void setCurrentProgram(int) override{} const juce::String getProgramName(int) override{return{};} void changeProgramName(int,const juce::String&) override{}
    void getStateInformation(juce::MemoryBlock&) override; void setStateInformation(const void*,int) override;
    void captureReference(); void clearReference(); bool saveReference(const juce::File&); bool loadReference(const juce::File&); void applyMatch();

    std::atomic<float> stereoMatch{0},midMatch{0},sideMatch{0};
    std::atomic<float> lowHz{20},highHz{20000},maxCorrectionDb{6},smoothingOctaves{0.35f};
    std::atomic<float> widthAmount{0},widthDepth{1};
    std::atomic<WidthMode> widthMode{WidthMode::MicroShift};
    std::atomic<bool> stereoEnabled{true},midEnabled{true},sideEnabled{true},hasReference{false};
    std::array<std::atomic<float>,kBins> stereoCurve{},midCurve{},sideCurve{};
    // FIX (crash-risk #2): reference arrays are now atomic (were plain float[] before) because the
    // UI thread writes them on Capture/Load/state-restore while the audio thread reads them every
    // FFT hop. Plain shared floats like that is a data race (undefined behaviour in C++, and in
    // practice can produce clicks/torn values). Atomics make every read/write well-defined, no locks
    // needed since these are single float reads/writes.
    std::array<std::atomic<float>,kBins> refStereo{},refMid{},refSide{};
    std::array<float,kBands> bandHz{};
    std::atomic<float> inputRmsDb{-90}, outputRmsDb{-90};
    std::atomic<uint64_t> generation{0};

private:
    double sr=44100; juce::dsp::FFT fft{kFFTOrder}; juce::dsp::WindowingFunction<float> window{kFFTSize,juce::dsp::WindowingFunction<float>::hann};
    std::array<float,kFFTSize> fftMid{},fftSide{}; int fftPos=0;
    std::array<float,kBins> liveStereo{},liveMid{},liveSide{}; std::array<float,kBands> corrStereo{},corrMid{},corrSide{};
    std::array<Coeff,kBands> stereoCoeff{},midCoeff{},sideCoeff{};
    struct State{float z1=0,z2=0;}; std::array<State,kBands> stStereoL{},stStereoR{},stMid{},stSide{};
    float prevMono=0; float widthDelay[64]{}; int widthWrite=0;
    std::atomic<bool> dirty{true};

    static float logFreq(float); static float interp(const std::array<float,kBins>&,float);
    static float interpAtomic(const std::array<std::atomic<float>,kBins>&,float);
    void analyzeAndUpdate(); void buildCorrection(std::array<float,kBands>&,const std::array<std::atomic<float>,kBins>&,const std::array<float,kBins>&,float);
    void rebuildCoefficients(); static Coeff peak(float,float,float,float); static float process(const Coeff&,State&,float);
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PQAudioProcessor)
};
