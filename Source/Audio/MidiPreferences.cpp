#include "MidiPreferences.h"

namespace atomic
{
namespace
{
    constexpr int kMaxRememberedDevices = 64;

    void appendDevice (MidiPreferences& preferences, const MidiDeviceBinding& binding)
    {
        if (binding.identifier.isEmpty() || preferences.devices.size() >= kMaxRememberedDevices)
            return;

        for (const auto& existing : preferences.devices)
            if (existing.identifier == binding.identifier)
                return;                 // one port, one memory

        preferences.devices.push_back (binding);
    }
} // namespace

//==============================================================================
juce::File getDefaultMidiPreferencesFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
             .getChildFile ("AtomicDAW")
             .getChildFile ("midi-settings.xml");
}

MidiPreferences loadMidiPreferences (const juce::File& file)
{
    MidiPreferences result;             // factory state is the answer to every bad file

    if (! file.existsAsFile() || file.getSize() <= 0)
        return result;

    auto document = juce::parseXML (file.loadFileAsString());

    if (document == nullptr || ! document->hasTagName ("MidiPreferences")
        || document->getIntAttribute ("version", -1) != 1)
        return result;

    if (const auto* sources = document->getChildByName ("Sources"))
    {
        result.sources.typingKeyboard     = sources->getIntAttribute ("typing", 1) != 0;
        result.sources.externalController = sources->getIntAttribute ("controller", 0) != 0;
    }

    if (const auto* typing = document->getChildByName ("Typing"))
        result.typingOctave = juce::jlimit (-4, 4, typing->getIntAttribute ("octave", 0));

    if (const auto* devices = document->getChildByName ("Devices"))
        for (int i = 0; i < devices->getNumChildElements(); ++i)
        {
            const auto* element = devices->getChildElement (i);

            if (element == nullptr || ! element->hasTagName ("Device"))
                continue;

            appendDevice (result, { element->getStringAttribute ("identifier"),
                                    element->getStringAttribute ("name") });
        }

    return result;
}

bool saveMidiPreferences (const juce::File& file, const MidiPreferences& preferences)
{
    if (file == juce::File())
        return false;

    juce::XmlElement document ("MidiPreferences");
    document.setAttribute ("version", 1);

    auto* sources = document.createNewChildElement ("Sources");
    sources->setAttribute ("typing", preferences.sources.typingKeyboard ? 1 : 0);
    sources->setAttribute ("controller", preferences.sources.externalController ? 1 : 0);

    auto* typing = document.createNewChildElement ("Typing");
    typing->setAttribute ("octave", juce::jlimit (-4, 4, preferences.typingOctave));

    auto* devices = document.createNewChildElement ("Devices");

    for (const auto& binding : preferences.devices)
    {
        if (binding.identifier.isEmpty())
            continue;

        auto* element = devices->createNewChildElement ("Device");
        element->setAttribute ("identifier", binding.identifier);
        element->setAttribute ("name", binding.name);
    }

    const auto parent = file.getParentDirectory();

    if (! parent.isDirectory() && ! parent.createDirectory().wasOk())
        return false;

    // Same atomic replace the project and recovery copy use: a crash halfway
    // through a write must leave the last good memory in place.
    return file.replaceWithText (document.toString(), false);
}

} // namespace atomic
