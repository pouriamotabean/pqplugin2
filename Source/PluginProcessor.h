#pragma once
#include <JuceHeader.h>
#include <array>
#include <atomic>

class PQAudioProcessor : public juce::AudioProcessor
{
public:
    // FIX (low-frequency resolution): FFT bin spacing is sr/kFFTSize, so with the old 8192-point
    // FFT each bin covered ~5.4Hz - meaning the lowest octaves (20-80Hz) were built from only a
    // handful of real measurements no matter how many log-spaced points the display asked for.
    // That's what read as a "staircase"/pixelated line at the low end. Doubling to 16384 halves
    // the bin spacing (~2.7Hz) for real extra low-end resolution. The hop is tightened from 1/4 to
    // 1/8 of the FFT size so the on-screen curve keeps updating every ~2048 samples, same as before -
    // only the analysis window got bigger, not the refresh latency.
    static constexpr int kFFTOrder=14;
    static constexpr int kFFTSize=1<<kFFTOrder;
    static constexpr int kHopSize=kFFTSize/8;
    static constexpr int kBins=1024;
    static constexpr int kBands=36;
    enum class WidthMode { MicroShift, Haas, Decorrelated };
    struct Coeff { float b0=1,b1=0,b2=0,a1=0,a2=0; };
    struct Bank { std::array<Coeff,kBands> stereo{},mid{},side{}; };

    // ---- Manual, mouse-only parametric EQ (Pro-Q style) ----------------------------------
    // This sits on top of the automatic match-EQ, on the final stereo output. It has no knobs:
    // every band is created, moved, reshaped and deleted purely with the mouse in the editor.
    enum class ManualType { Bell, LowShelf, HighShelf, LowCut, HighCut, Notch };
    // Which signal a manual band is inserted on: Stereo = final L/R (post width, as before),
    // Mid/Side = the corresponding M/S leg, before it's folded back into L/R. See processBlock().
    enum class ManualTarget { Stereo, Mid, Side };
    static constexpr int kMaxManualBands = 16;
    struct ManualBand {
        std::atomic<bool> active{false};
        std::atomic<ManualType> type{ManualType::Bell};
        std::atomic<ManualTarget> target{ManualTarget::Stereo};
        std::atomic<float> freq{1000.f};
        std::atomic<float> gainDb{0.f};
        std::atomic<float> q{0.7f};
    };
    std::array<ManualBand,kMaxManualBands> manualBands{};
    std::atomic<bool> manualDirty{true};
    // Editor calls these instead of touching manualBands directly, so the processor can flag
    // its coefficients dirty and pick the fix an empty slot for a new band.
    int addManualBand(ManualType type,float freq,float gainDb,float q,ManualTarget target=ManualTarget::Stereo);
    void removeManualBand(int index);
    void setManualBand(int index,ManualType type,float freq,float gainDb,float q);
    void setManualBandType(int index,ManualType type);
    void setManualBandFreqGain(int index,float freq,float gainDb);
    void setManualBandQ(int index,float q);
    // Changing a band's target mid-playback moves it between completely separate filter states
    // (L/R vs mono Mid vs mono Side - see manualStateL/R/Mid/Side below), so its old state is reset
    // to silence at the moment of the switch rather than carrying stale history into the new path,
    // which is what would otherwise cause a click/pop.
    void setManualBandTarget(int index,ManualTarget target);

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
    // Input/output trim, in dB, applied at the very start / very end of the chain. Each is user-
    // draggable from its own vertical meter in the editor. matchGain() is the "MATCH GAIN" button:
    // it reads the current (already-trimmed) input/output RMS meters and adjusts outputTrimDb so the
    // processed output sits at the same average level as the input - a quick way to A/B the tonal
    // change (EQ, width) without the comparison being biased by a loudness difference.
    std::atomic<float> inputTrimDb{0.f}, outputTrimDb{0.f};
    void matchGain();
    // Whether the mono-widener runs before the EQ correction (so the analyzer/match "sees" the
    // widened signal) or after it (so widening is the last thing applied to the final output).
    std::atomic<bool> widthPostEq{false};
    // FIX: replaced per-band "apply correction" toggles with a real audition Solo (mutually
    // exclusive). Match-amount sliders already control how much correction each band gets, so a
    // separate enable/disable was redundant - what was actually missing was the ability to hear one
    // band in isolation before any correction is applied.
    enum class SoloBand { None, Stereo, Mid, Side };
    std::atomic<SoloBand> solo{SoloBand::None};
    std::atomic<bool> hasReference{false};
    std::array<std::atomic<float>,kBins> stereoCurve{},midCurve{},sideCurve{};
    // FIX (crash-risk #2): reference arrays are now atomic (were plain float[] before) because the
    // UI thread writes them on Capture/Load/state-restore while the audio thread reads them every
    // FFT hop. Plain shared floats like that is a data race (undefined behaviour in C++, and in
    // practice can produce clicks/torn values). Atomics make every read/write well-defined, no locks
    // needed since these are single float reads/writes.
    std::array<std::atomic<float>,kBins> refStereo{},refMid{},refSide{};
    std::array<float,kBands> bandHz{};
    std::atomic<float> inputRmsDb{-90}, outputRmsDb{-90};
    // Peak-hold meters (instantaneous peak, held then released at a slow fixed dB/sec rate - see
    // processBlock). Separate from the RMS meters above: matchGain() below uses these, the RMS pair
    // is only for the meter bar's average-level fill.
    std::atomic<float> inputPeakDb{-90}, outputPeakDb{-90};
    std::atomic<uint64_t> generation{0};

private:
    double sr=44100; juce::dsp::FFT fft{kFFTOrder}; juce::dsp::WindowingFunction<float> window{kFFTSize,juce::dsp::WindowingFunction<float>::hann};
    // FIX (analyzer latency): these are now circular history buffers instead of "fill once then
    // reset to zero" blocks, so analyzeAndUpdate() can run every kHopSize samples using the last
    // kFFTSize samples of history (75% overlap) instead of waiting a full kFFTSize samples between
    // updates. fftPos is the write cursor (wraps continuously); hopCounter times the analysis calls.
    std::array<float,kFFTSize> fftMid{},fftSide{}; int fftPos=0; int hopCounter=0;
    std::array<float,kBins> liveStereo{},liveMid{},liveSide{}; std::array<float,kBands> corrStereo{},corrMid{},corrSide{};
    std::array<Coeff,kBands> stereoCoeff{},midCoeff{},sideCoeff{};
    struct State{float z1=0,z2=0;}; std::array<State,kBands> stStereoL{},stStereoR{},stMid{},stSide{};
    // FIX (widener too subtle): the delay line used to be a fixed 64-sample array, which is under
    // 1.5ms at typical sample rates - far too short for Haas/decorrelation to be audible. It's now
    // sized from the sample rate (up to 50ms) in prepareToPlay, and each width mode reads it at a
    // musically meaningful delay time in milliseconds rather than a fixed sample count.
    std::vector<float> widthDelay; int widthWriteIdx=0; int widthBufSize=0;
    float prevMono=0;
    std::atomic<bool> dirty{true};

    // Manual EQ DSP: one coefficient set per possible band, plus one state per signal path it could
    // be routed to. Only the state matching the band's current target is ever advanced in
    // processBlock(); the others sit idle so switching target doesn't mix histories together.
    std::array<Coeff,kMaxManualBands> manualCoeff{};
    std::array<State,kMaxManualBands> manualStateL{},manualStateR{},manualStateMid{},manualStateSide{};
    void rebuildManualCoefficients();
    static Coeff makeManualCoeff(ManualType,float freqHz,float gainDb,float q,double sampleRate);

    static float logFreq(float); static float interp(const std::array<float,kBins>&,float);
    static float interpAtomic(const std::array<std::atomic<float>,kBins>&,float);
    void analyzeAndUpdate(); void buildCorrection(std::array<float,kBands>&,const std::array<std::atomic<float>,kBins>&,const std::array<float,kBins>&,float);
    void rebuildCoefficients(); static Coeff peak(float,float,float,float); static float process(const Coeff&,State&,float);
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PQAudioProcessor)
};
