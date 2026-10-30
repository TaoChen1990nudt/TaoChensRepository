#include "Recovery.h"

#include <algorithm>

namespace atomic
{
namespace recovery
{

namespace
{
    // FNV-1a over the full path. A plain file name would collide the moment two
    // projects share one ("song.atomic" in two folders is the normal case, not
    // the exotic one), and a collision here means the second project silently
    // overwrites the work the first one was about to recover.
    juce::String tagFor (const juce::File& projectFile)
    {
        const auto path = projectFile.getFullPathName();

        juce::uint32 hash = 2166136261u;

        for (auto character : path)
            hash = (hash ^ (juce::uint32) character) * 16777619u;

        return juce::String::toHexString ((juce::int64) hash).paddedLeft ('0', 8);
    }

    juce::String rootTag()
    {
        // Must match what project::toXmlText writes; asserted in the self-test
        // against a real document rather than trusted here.
        return "AtomicDAW";
    }

    std::unique_ptr<juce::XmlElement> parseSession (const juce::File& file)
    {
        if (! file.existsAsFile() || file.getSize() <= 0)
            return nullptr;

        auto document = juce::parseXML (file.loadFileAsString());

        if (document == nullptr || ! document->hasTagName (rootTag()))
            return nullptr;

        return document;
    }
} // namespace

juce::File getFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
             .getChildFile ("AtomicDAW")
             .getChildFile ("Recovery");
}

juce::File getFileFor (const juce::File& projectFile)
{
    return getFolder().getChildFile (projectFile == juce::File()
                                       ? juce::String ("untitled") + fileExtension
                                       : projectFile.getFileNameWithoutExtension()
                                           + "-" + tagFor (projectFile) + fileExtension);
}

Decision inspect (const juce::File& recoveryFile, const juce::File& projectFile)
{
    if (! recoveryFile.existsAsFile())
        return Decision::none;

    // Parsed, not merely non-empty: a truncated or foreign file must never turn
    // into a dialog offering to restore it.
    if (parseSession (recoveryFile) == nullptr)
        return Decision::unusable;

    if (! projectFile.existsAsFile())
        return Decision::offer;

    // A tie counts as stale. The copy is only worth showing when it is strictly
    // newer than the file it would replace; on a tie, disturbing the user costs
    // more than the edit it might save.
    if (recoveryFile.getLastModificationTime() <= projectFile.getLastModificationTime())
        return Decision::stale;

    return Decision::offer;
}

bool write (const juce::File& recoveryFile, const juce::String& xml,
            const juce::File& projectFile, juce::String& error)
{
    auto document = juce::parseXML (xml);

    if (document == nullptr || ! document->hasTagName (rootTag()))
    {
        error = "not a session document";
        return false;
    }

    document->setAttribute ("recoveryOf", projectFile.getFullPathName());
    document->setAttribute ("recoveredAt", juce::Time::getCurrentTime().toISO8601 (true));

    const auto folder = recoveryFile.getParentDirectory();

    if (! folder.isDirectory() && ! folder.createDirectory().wasOk())
    {
        error = "cannot create " + folder.getFullPathName();
        return false;
    }

    if (! recoveryFile.replaceWithText (document->toString()))
    {
        error = "cannot write " + recoveryFile.getFullPathName();
        return false;
    }

    return true;
}

juce::String readProjectPath (const juce::File& recoveryFile)
{
    if (auto document = parseSession (recoveryFile))
        return document->getStringAttribute ("recoveryOf");

    return {};
}

std::vector<Entry> listIn (const juce::File& folder)
{
    std::vector<Entry> entries;

    if (! folder.isDirectory())
        return entries;

    for (const auto& file : folder.findChildFiles (juce::File::findFiles, false,
                                                   juce::String ("*") + fileExtension))
    {
        Entry entry;
        entry.file    = file;
        entry.savedAt = file.getLastModificationTime();
        entry.bytes   = file.getSize();

        if (auto document = parseSession (file))
            entry.projectPath = document->getStringAttribute ("recoveryOf");

        if (entry.projectPath.isNotEmpty())
            entry.projectFile = juce::File (entry.projectPath);

        entries.push_back (std::move (entry));
    }

    // Newest first: the startup prompt only ever talks about the head of this
    // list, and "which one was I working on" is answered by time, not by name.
    std::sort (entries.begin(), entries.end(), [] (const Entry& a, const Entry& b)
    {
        return a.savedAt > b.savedAt;
    });

    return entries;
}

std::vector<Entry> list()
{
    return listIn (getFolder());
}

void discard (const juce::File& recoveryFile)
{
    if (recoveryFile.existsAsFile())
        recoveryFile.deleteFile();
}

} // namespace recovery
} // namespace atomic
