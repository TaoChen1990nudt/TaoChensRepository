/*
    A deliberately tiny VST3 effect: it halves whatever arrives.

    The exact 0.5 is the point. The self-test routes a channel through this
    insert and checks that the RMS drops to half — a number it can assert on
    without knowing anything about the plugin's internals.
*/

#include <juce_audio_processors/juce_audio_processors.h>

namespace
{
    constexpr float fixedGain = 0.5f;
}

//==============================================================================
class AtomicTestFX final : public juce::AudioProcessor
{
public:
    AtomicTestFX()
        : juce::AudioProcessor (BusesProperties().withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                                                .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
    }

    const juce::String getName() const override          { return "Atomic Test FX"; }
    bool acceptsMidi() const override                    { return false; }
    bool producesMidi() const override                   { return false; }
    bool isMidiEffect() const override                   { return false; }
    double getTailLengthSeconds() const override         { return 0.0; }

    int getNumPrograms() override                        { return 1; }
    int getCurrentProgram() override                     { return 0; }
    void setCurrentProgram (int) override                {}
    const juce::String getProgramName (int) override     { return "Half"; }
    void changeProgramName (int, const juce::String&) override {}

    bool hasEditor() const override                      { return false; }
    juce::AudioProcessorEditor* createEditor() override  { return nullptr; }

    void prepareToPlay (double, int) override            {}
    void releaseResources() override                     {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            buffer.applyGain (ch, 0, buffer.getNumSamples(), fixedGain);
    }

    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AtomicTestFX)
};

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AtomicTestFX();
}
