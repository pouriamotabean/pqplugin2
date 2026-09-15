#pragma once
#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <vector>
#include <memory>

class PQAnalysisThread; // defined in PluginProcessor.cpp - see PQAudioProcessor::friend declaration below

class PQAudioProcessor : public juce::AudioProcessor
{
    // FIX (B1 - FFT on the audio thread): analyzeAndUpdate()'s ~16k-point FFT used to run straight
    // inside processBlock() every kHopSize samples - a real chunk of work dropped into the audio
    // callback with nothing between it and an xrun on a loaded/slower machine. PQAnalysisThread (see
    // .cpp) now owns that work on its own juce::Thread; processBlock() only ever does the cheap part
    // (copying this block's raw mid/side samples into a lock-free FIFO and pinging the thread), never
    // touches fftMid/fftSide/fftPos/hopCounter itself anymore, and never calls analyzeAndUpdate()
    // directly. Friended so that thread can drive analyzeAndUpdate() and the fftMid/fftSide/fftPos/
    // hopCounter state exactly as processBlock used to, just from a different thread.
    friend class PQAnalysisThread;
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
        // Low Cut / High Cut only: the actual roll-off steepness in dB/octave - see kCutSlopeSteps.
        // Scroll-wheel on a cut node cycles through these (see PluginEditor::mouseWheelMove) instead
        // of adjusting q, since q's "resonance bump" reading was never what people meant by "steeper
        // slope" on a cut filter.
        std::atomic<int> slopeOrder{12};
    };
    // The slope steps a Low/High Cut node cycles through via mouse-wheel, up to 96dB/oct - steep
    // enough to read as a near-vertical wall for any practical audio purpose (each doubling past
    // 48 buys diminishing audible return but costs another cascaded stage of CPU/phase).
    static constexpr std::array<int,8> kCutSlopeSteps{6,12,18,24,36,48,72,96};
    // A cut's slope is built from a cascade of up to this many simple stages (1-pole = 6dB/oct,
    // RBJ 2-pole = 12dB/oct) chained together - see rebuildManualCoefficients(). 96dB/oct needs 8
    // two-pole stages, which is the largest case.
    static constexpr int kMaxCutStages = 8;
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
    // Sets a Low/High Cut band's roll-off steepness (see ManualBand::slopeOrder above). Ignored -
    // has no effect on the DSP - for any other band type.
    void setManualBandSlope(int index,int slopeOrderDbPerOct);
    // Changing a band's target mid-playback moves it between completely separate filter states
    // (L/R vs mono Mid vs mono Side - see manualStateL/R/Mid/Side below), so its old state is reset
    // to silence at the moment of the switch rather than carrying stale history into the new path,
    // which is what would otherwise cause a click/pop.
    void setManualBandTarget(int index,ManualTarget target);

    PQAudioProcessor(); ~PQAudioProcessor() override;
    void prepareToPlay(double,int) override; void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&,juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override; bool hasEditor() const override{return true;}
    const juce::String getName() const override{return "PQ";} bool acceptsMidi() const override{return false;} bool producesMidi() const override{return false;} bool isMidiEffect() const override{return false;} double getTailLengthSeconds() const override{return 0;}
    int getNumPrograms() override{return 1;} int getCurrentProgram() override{return 0;} void setCurrentProgram(int) override{} const juce::String getProgramName(int) override{return{};} void changeProgramName(int,const juce::String&) override{}
    void getStateInformation(juce::MemoryBlock&) override; void setStateInformation(const void*,int) override;
    void captureReference(); void clearReference(); bool saveReference(const juce::File&); bool loadReference(const juce::File&); void applyMatch();
    // I2: a reference curve on its own, separate from the full preset (no manual EQ, width, or match
    // amounts) - for handing a captured target to someone else, or between your own projects, without
    // dragging every other setting along with it. Own file format/extension (.pqmatch) so it can never
    // be confused with or accidentally opened as a full .pqref preset.
    bool exportReferenceOnly(const juce::File&);
    bool importReferenceOnly(const juce::File&);

    std::atomic<float> stereoMatch{0},midMatch{0},sideMatch{0};
    std::atomic<float> lowHz{20},highHz{20000},maxCorrectionDb{6},smoothingOctaves{0.35f};
    std::atomic<float> widthAmount{0},widthDepth{1};
    // Mono Maker: 0-100%, mapped log-scale to a 20Hz-20kHz cutoff for a fixed-slope (24dB/oct) cut
    // on the Side signal above... below that cutoff, i.e. a highpass on Side. At 0% the cutoff sits
    // at 20Hz (no audible effect); at 100% it sits at 20kHz, which removes essentially all Side
    // content - the whole signal goes mono. Applied at the very end of the chain, recomputed
    // directly from the final L/R, so it works identically regardless of the Width PRE/POST setting
    // (see processBlock) - it doesn't need to know where any Side content it's cutting came from.
    std::atomic<float> monoMakerAmount{0.f};
    std::atomic<WidthMode> widthMode{WidthMode::MicroShift};
    // Input/output trim, in dB, applied at the very start / very end of the chain. Each is user-
    // draggable from its own vertical meter in the editor. matchGain() is the "MATCH GAIN" button:
    // it reads the current (already-trimmed) input/output RMS meters and adjusts outputTrimDb so the
    // processed output sits at the same average level as the input - a quick way to A/B the tonal
    // change (EQ, width) without the comparison being biased by a loudness difference.
    std::atomic<float> inputTrimDb{0.f}, outputTrimDb{0.f};
    // I3: which pair of meters matchGain() reads. Peak (default) neutralizes a peak-level change the
    // EQ/width introduced - the most direct "make it fair to A/B" comparison. Rms instead matches
    // average perceived loudness, which is what most people actually mean by "same volume" when the
    // material isn't peaky (e.g. already-limited masters where the peak barely moves either way).
    enum class GainMatchMode { Peak, Rms };
    std::atomic<GainMatchMode> gainMatchMode{GainMatchMode::Peak};
    void matchGain();
    // Whether the mono-widener runs before the EQ correction (so the analyzer/match "sees" the
    // widened signal) or after it (so widening is the last thing applied to the final output).
    std::atomic<bool> widthPostEq{false};
    // FIX (bug #1/#2 - "solo stereo has input but no output" / analyzer lines behaving oddly):
    // these three used to ALSO gate the audio itself (muting Mid/Side to isolate a band), which is
    // exactly what broke Stereo: it has no signal of its own, it only corrects the already-recombined
    // L/R - so muting Mid+Side to "solo" Stereo left it correcting silence. These are now PURELY a
    // display concern: which of the three curves (live analyzer + captured reference) is drawn on
    // the chart. The audio path no longer reads these at all - see processBlock(). Default OFF so
    // the analyzer opens clean; each one is switched on independently as its curve is wanted.
    std::atomic<bool> stereoOn{false}, midOn{false}, sideOn{false};
    // True bypass: skips Mid/Side/Stereo correction, manual EQ, and Width entirely (see
    // processBlock()) so the output is the raw (trim-adjusted) input - the quick A/B every engineer
    // reaches for first. Deliberately NOT persisted in getStateInformation()/loadReference() - same
    // treatment as stereoOn/midOn/sideOn above, since it's a transient monitoring toggle, not a
    // tonal setting worth saving into a preset. The analyzer and input/output meters keep running
    // normally while bypassed, so you can still watch the match curve while listening dry.
    std::atomic<bool> bypassed{false};
    std::atomic<bool> hasReference{false};
    // I1: CAPTURE now averages over a short window instead of grabbing one instantaneous frame, so a
    // single loud transient or a quiet gap doesn't skew the reference. capturingReference/
    // captureProgress are read by the editor (to disable the button and show a percentage);
    // captureDurationSec is fixed (not user-exposed yet) but kept as a named atomic rather than a
    // magic number in case that changes later.
    std::atomic<bool> capturingReference{false};
    std::atomic<float> captureProgress{0.f}; // 0..1
    std::atomic<float> captureDurationSec{5.f};
    std::array<std::atomic<float>,kBins> stereoCurve{},midCurve{},sideCurve{};
    // FIX (crash-risk #2): reference arrays are now atomic (were plain float[] before) because the
    // UI thread writes them on Capture/Load/state-restore while the audio thread reads them every
    // FFT hop. Plain shared floats like that is a data race (undefined behaviour in C++, and in
    // practice can produce clicks/torn values). Atomics make every read/write well-defined, no locks
    // needed since these are single float reads/writes.
    std::array<std::atomic<float>,kBins> refStereo{},refMid{},refSide{};
    // The auto-match correction gain per band, in dB - i.e. exactly the EQ curve buildCorrection()
    // computed from the reference (output-minus-input, in dB, is literally what an EQ gain curve
    // is). Public so the editor can draw it directly as the "delta" overlay - what the automatic
    // matching is actually adding/removing - without needing a second live analyzer on the
    // processed signal. FIX (B1): atomic because buildCorrection() runs on the background analysis
    // thread while rebuildCoefficients() reads it on the audio thread and the editor reads it for
    // painting - three readers/writers across two threads, plain floats would be a data race.
    std::array<std::atomic<float>,kBands> corrStereo{},corrMid{},corrSide{};
    // The kBands correction-band centre frequencies (log-spaced 20Hz-20kHz) - public so the editor
    // can place corrStereo/corrMid/corrSide's points at the right x position on the chart.
    std::array<float,kBands> bandHz{};
    std::atomic<float> inputRmsDb{-90}, outputRmsDb{-90};
    // Peak-hold meters (instantaneous peak, held then released at a slow fixed dB/sec rate - see
    // processBlock). Separate from the RMS meters above: matchGain() below uses these, the RMS pair
    // is only for the meter bar's average-level fill.
    std::atomic<float> inputPeakDb{-90}, outputPeakDb{-90};
    std::atomic<uint64_t> generation{0};
    // FIX (B2): true whenever something has changed since the last successful save or load - set
    // from every manual-EQ mutator above, matchGain(), captureReference()/clearReference(), and (from
    // the editor) every slider/combo bound directly to a processor atomic that isn't behind one of
    // those setters. Cleared back to false inside saveReference() and applyStateBlock() (so both a
    // manual Load and a host session restore count as a fresh "clean" baseline). PresetPanel checks
    // this before an implicit load (list.onChange) to warn about unsaved changes.
    std::atomic<bool> presetDirty{false};

private:
    // FIX (thread safety, part of B1): was a plain double, read from processBlock() (audio thread)
    // while only ever written from prepareToPlay() (message/setup thread) - already a data race
    // before the background analysis thread existed. Now also read by that new thread. Atomic makes
    // every read/write well-defined without a lock (single float-sized value, no torn reads).
    std::atomic<double> sr{44100.0}; juce::dsp::FFT fft{kFFTOrder}; juce::dsp::WindowingFunction<float> window{kFFTSize,juce::dsp::WindowingFunction<float>::hann};
    // FIX (analyzer latency): these are now circular history buffers instead of "fill once then
    // reset to zero" blocks, so analyzeAndUpdate() can run every kHopSize samples using the last
    // kFFTSize samples of history (75% overlap) instead of waiting a full kFFTSize samples between
    // updates. fftPos is the write cursor (wraps continuously); hopCounter times the analysis calls.
    std::array<float,kFFTSize> fftMid{},fftSide{}; int fftPos=0; int hopCounter=0;
    std::array<float,kBins> liveStereo{},liveMid{},liveSide{};
    // I1: power-domain accumulators for the averaging capture - only ever touched by the analysis
    // thread (same ownership as fftMid/fftSide/liveStereo above), reset the moment that thread
    // notices capturingReference flip from false to true (see captureWasActive in analyzeAndUpdate()).
    std::array<double,kBins> captureAccumStereo{},captureAccumMid{},captureAccumSide{};
    int captureFrameCount=0; double captureElapsedSec=0.0; bool captureWasActive=false;
    std::array<Coeff,kBands> stereoCoeff{},midCoeff{},sideCoeff{};
    struct State{float z1=0,z2=0;}; std::array<State,kBands> stStereoL{},stStereoR{},stMid{},stSide{};
    // FIX (widener too subtle): the delay line used to be a fixed 64-sample array, which is under
    // 1.5ms at typical sample rates - far too short for Haas/decorrelation to be audible. It's now
    // sized from the sample rate (up to 50ms) in prepareToPlay, and each width mode reads it at a
    // musically meaningful delay time in milliseconds rather than a fixed sample count.
    std::vector<float> widthDelay; int widthWriteIdx=0; int widthBufSize=0;
    float prevMono=0;
    std::atomic<bool> dirty{true};

    // Manual EQ DSP: up to kMaxCutStages coefficient sets per possible band (Bell/Shelf/Notch only
    // ever use stage 0; Low/High Cut use as many stages as their slope needs - see
    // rebuildManualCoefficients()), plus one state per signal path it could be routed to. Only the
    // state matching the band's current target is ever advanced in processBlock(); the others sit
    // idle so switching target doesn't mix histories together.
    std::array<std::array<Coeff,kMaxCutStages>,kMaxManualBands> manualCoeff{};
    std::array<int,kMaxManualBands> manualStageCount{};
    std::array<std::array<State,kMaxCutStages>,kMaxManualBands> manualStateL{},manualStateR{},manualStateMid{},manualStateSide{};
    // Mono Maker's own fixed-slope filter (2 cascaded 2-pole stages = 24dB/oct) - single channel
    // since it runs on the Side signal alone, rebuilt in rebuildManualCoefficients() alongside the
    // manual EQ bands whenever monoMakerAmount changes (same manualDirty flag).
    std::array<Coeff,2> monoMakerCoeff{}; std::array<State,2> monoMakerState{};
    void rebuildManualCoefficients();
    static Coeff makeManualCoeff(ManualType,float freqHz,float gainDb,float q,double sampleRate);
    // Single first-order (6dB/oct) high-pass/low-pass stage, used as a building block for the odd
    // slope steps (currently just 18) that a whole number of 2-pole (12dB/oct) stages can't hit
    // exactly on their own.
    static Coeff make1PoleCut(bool highpass,float hz,double sampleRate);

    static float logFreq(float); static float interp(const std::array<float,kBins>&,float);
    static float interpAtomic(const std::array<std::atomic<float>,kBins>&,float);
    void analyzeAndUpdate(); void buildCorrection(std::array<std::atomic<float>,kBands>&,const std::array<std::atomic<float>,kBins>&,const std::array<float,kBins>&,float);
    void rebuildCoefficients(); static Coeff peak(float,float,float,float); static float process(const Coeff&,State&,float);

    // FIX (B1): lock-free single-producer/single-consumer handoff from the audio thread to
    // PQAnalysisThread. processBlock() writes this block's raw mid/side samples into fifoMid/
    // fifoSide (sized generously - a fraction of a second - so the analysis thread falling briefly
    // behind under load just delays the on-screen curve slightly, it never blocks or drops audio);
    // the thread drains them at its own pace. If the FIFO is ever completely full (analysis thread
    // starved), prepareToWrite silently reports less room than requested and the newest samples for
    // that block are skipped - a fine tradeoff for a visual/match analyzer, never touches the audio
    // path itself.
    juce::AbstractFifo analysisFifo{1<<16};
    std::vector<float> fifoMid, fifoSide;
    // Per-block scratch, written only by the audio thread while it still holds each sample locally
    // (right where fftMid/fftSide used to be written directly) - sized once in prepareToPlay().
    std::vector<float> blockMid, blockSide;
    std::unique_ptr<PQAnalysisThread> analysisThread;
    // Shared by setStateInformation() (host session restore) AND saveReference/loadReference (the
    // on-disk .pqref "preset" file - see item 6): a preset is now just the full plugin state, so
    // both paths funnel through the exact same parser/writer. Understands two on-disk formats:
    //   - legacy "PRQ3" (0x50525133): old reference-only .pqref files from before presets existed.
    //     Only the reference curves + low/high/cap/smoothing are read; everything else already
    //     loaded (manual EQ, width, trims, match amounts...) is left completely untouched, so an old
    //     file never wipes out settings it never knew about.
    //   - current "PQS8" (0x50515338) and its older PQS-prefixed ancestors: full processor state.
    // Returns false (without side effects beyond what was already read) if the magic is unrecognised.
    bool applyStateBlock(const void* data, int size);
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PQAudioProcessor)
};
