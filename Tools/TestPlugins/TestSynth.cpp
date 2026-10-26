/*
    A deliberately tiny VST3 instrument.

    It exists for one reason: the self-test has to prove that the VST3 hosting
    path works end to end (scan -> instantiate -> send MIDI -> hear audio) on a
    machine that may have no third-party plugins installed at all. One sine
    oscillator and a note-on/note-off latch is all that takes.

    It is NOT part of the product. It is built only so the host has something
    real to load.
*/

#include <juce_audio_processors/juce_audio_processors.h>

namespace
{
    constexpr float outputAmplitude = 0.4f;
}

//==============================================================================
class AtomicTestSynth final : public juce::AudioProcessor
{
public:
    AtomicTestSynth()
        : juce::AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
        // Phase 24: one real parameter. "A curve drives a hosted instrument's
        // parameter" has to be checkable on a machine with no third-party
        // plugins, which means the fixture needs something to drive — and the
        // only way to prove it end to end is to make the parameter audible.
        addParameter (levelParameter = new juce::AudioParameterFloat (
                          juce::ParameterID { "level", 1 }, "Level",
                          juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f));
    }

    //-- Metadata -------------------------------------------------------------
    const juce::String getName() const override          { return "Atomic Test Synth"; }
    bool acceptsMidi() const override                    { return true; }
    bool producesMidi() const override                   { return false; }
    bool isMidiEffect() const override                   { return false; }
    double getTailLengthSeconds() const override         { return 0.0; }

    int getNumPrograms() override                        { return 1; }
    int getCurrentProgram() override                     { return 0; }
    void setCurrentProgram (int) override                {}
    const juce::String getProgramName (int) override     { return "Sine"; }
    void changeProgramName (int, const juce::String&) override {}

    bool hasEditor() const override                      { return false; }
    juce::AudioProcessorEditor* createEditor() override  { return nullptr; }

    //-- Lifecycle ------------------------------------------------------------
    void prepareToPlay (double newSampleRate, int) override
    {
        sampleRate  = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        phase       = 0.0;
        increment   = 0.0;
        amplitude   = 0.0f;
        gateOpen    = false;
    }

    void releaseResources() override {}

    //-- The whole synth ------------------------------------------------------
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        juce::ScopedNoDenormals noDenormals;

        buffer.clear();

        // The MIDI has to be consumed even when nothing is sounding, or a
        // note-on that arrives in a silent block would be lost.
        for (const auto metadata : midi)
        {
            const auto message = metadata.getMessage();

            if (message.isNoteOn())
            {
                frequency = juce::MidiMessage::getMidiNoteInHertz (message.getNoteNumber());
                increment = frequency / sampleRate;
                amplitude = outputAmplitude * message.getFloatVelocity();
                gateOpen  = true;
            }
            else if (message.isNoteOff() || message.isAllNotesOff())
            {
                gateOpen  = false;
                amplitude = 0.0f;
            }
        }

        if (! gateOpen || increment <= 0.0)
            return;

        // Read per block, not at note-on: a parameter that only took effect on
        // the next note could not demonstrate automation at all.
        const float level = levelParameter != nullptr ? levelParameter->get() : 1.0f;

        auto* left  = buffer.getWritePointer (0);
        auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const auto sample = (float) std::sin (phase * juce::MathConstants<double>::twoPi)
                                  * amplitude * level;

            left[i] = sample;

            if (right != nullptr)
                right[i] = sample;

            phase += increment;

            if (phase >= 1.0)
                phase -= 1.0;
        }
    }

    //-- State ----------------------------------------------------------------
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

private:
    juce::AudioParameterFloat* levelParameter = nullptr;

    double sampleRate = 44100.0;
    double phase      = 0.0;
    double increment  = 0.0;
    double frequency  = 440.0;
    float  amplitude  = 0.0f;
    bool   gateOpen   = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AtomicTestSynth)
};

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AtomicTestSynth();
}
