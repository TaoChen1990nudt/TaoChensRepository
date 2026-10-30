#include "PluginHost.h"

#include <algorithm>

#if JUCE_MSVC
 #include <excpt.h>     // EXCEPTION_EXECUTE_HANDLER for the __try around a plugin
#endif

#include "Util/Text.h"

namespace atomic
{

namespace
{
    /** One bundle, guarded. A plugin that faults while its class list is being
        read is one plugin missing from the library, not a dead editor.

        SEH rather than try/catch: an access violation is not a C++ exception.
        The function deliberately has no local object with a destructor, which
        is what MSVC requires of a __try block (C2712). */
    void scanOneBundleSafely (juce::VST3PluginFormat& format,
                              juce::OwnedArray<juce::PluginDescription>& results,
                              const juce::String& path)
    {
       #if JUCE_MSVC
        __try
        {
            format.findAllTypesForFile (results, path);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // Leave whatever was collected before the fault.
        }
       #else
        format.findAllTypesForFile (results, path);
       #endif
    }

    /** Reads the line-per-plugin file the scan writes and appends to `found`.
        `#` lines are the cache's own bookkeeping (the search-path signature) and
        are skipped — a path containing `&` would not survive an XML parse. */
    void readLibraryFile (const juce::File& file, std::vector<PluginEntry>& found)
    {
        if (! file.existsAsFile())
            return;

        juce::StringArray lines;
        lines.addLines (file.loadFileAsString());

        for (int i = lines.size(); --i >= 0;)
            if (lines[i].trim().isEmpty() || lines[i].trimStart().startsWithChar ('#'))
                lines.remove (i);

        if (lines.isEmpty())
            return;

        juce::XmlDocument document ("<PLUGINS>\n" + lines.joinIntoString ("\n") + "\n</PLUGINS>");

        // JUCE 8 hands back a smart pointer here, not a raw one.
        if (auto xml = document.getDocumentElement())
        {
            juce::StringArray seen;

            for (auto* child : xml->getChildIterator())
            {
                juce::PluginDescription description;

                if (! description.loadFromXml (*child))
                    continue;

                const auto key = description.createIdentifierString();

                if (seen.contains (key))
                    continue;

                seen.add (key);

                PluginEntry entry;
                entry.description  = description;
                entry.isInstrument = description.isInstrument;
                found.push_back (std::move (entry));
            }
        }
    }
}

//==============================================================================
PluginHost::PluginHost()
{
    // Only the formats that were compiled in (VST3 for this build). AU/VST2 are
    // switched off in CMake on purpose.
    formatManager.addDefaultFormats();
}

PluginHost::~PluginHost()
{
    // The audio callback may still be running when the host is torn down; make
    // sure it cannot be inside a plugin while they are destroyed.
    const juce::ScopedLock sl (lock);
    instances.clear();
}

//==============================================================================
void PluginHost::addSearchPath (const juce::File& directory)
{
    if (directory.isDirectory())
        extraSearchPaths.addIfNotAlreadyThere (directory);
}

void PluginHost::clearExtraSearchPaths()
{
    extraSearchPaths = juce::FileSearchPath();
}

juce::FileSearchPath PluginHost::getSearchPaths() const
{
    juce::FileSearchPath paths (extraSearchPaths);

    // The platform's own VST3 folders, so a machine with plugins installed
    // behaves the way its owner expects without any configuration.
    juce::VST3PluginFormat vst3;
    const auto defaults = vst3.getDefaultLocationsToSearch();

    for (int i = 0; i < defaults.getNumPaths(); ++i)
        paths.addIfNotAlreadyThere (defaults[i]);

    return paths;
}

void PluginHost::collectBundles (const juce::File& root, juce::Array<juce::File>& out)
{
    if (! root.isDirectory())
        return;

    // A .vst3 on Windows and Linux is a DIRECTORY, so this has to ask for
    // directories as well as files or it finds nothing at all.
    const auto candidates = root.findChildFiles (juce::File::findFilesAndDirectories
                                                   | juce::File::ignoreHiddenFiles,
                                                 true, "*.vst3");

    for (const auto& candidate : candidates)
    {
        if (candidate.getFileName().startsWithChar ('.'))
            continue;

        // A bundle CONTAINS a file that also ends in .vst3
        // (Contents/x86_64-win/Foo.vst3). Recursing finds that inner binary too
        // and the same plugin is then listed twice, at two paths. Anything
        // inside a bundle is not a plugin in its own right.
        bool insideBundle = false;

        for (auto parent = candidate.getParentDirectory();
             parent.getFullPathName().isNotEmpty();
             parent = parent.getParentDirectory())
        {
            if (parent.hasFileExtension ("vst3"))
            {
                insideBundle = true;
                break;
            }

            if (parent == parent.getParentDirectory())
                break;          // drive root
        }

        if (insideBundle)
            continue;

        bool alreadyListed = false;

        for (const auto& existing : out)
            if (existing == candidate)
                alreadyListed = true;

        if (! alreadyListed)
            out.add (candidate);
    }
}

bool PluginHost::looksLikeInstrument (const juce::PluginDescription& description)
{
    // VST3 reports this in its class info, and JUCE copies it across. Falls
    // back to "no audio input" for plugins that leave the flag alone.
    return description.isInstrument;
}

juce::File PluginHost::getCacheFile() const
{
    if (extraPathsOnly)
        return {};

    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
             .getChildFile ("AtomicDAW")
             .getChildFile ("plugin-library.txt");
}

int PluginHost::scan (bool forceRescan)
{
    const auto paths = extraPathsOnly ? extraSearchPaths : getSearchPaths();

    // Signature of the search paths, so moving a plugin folder invalidates the
    // cache instead of leaving stale entries nobody can explain.
    const auto signature = "# " + paths.toString();

    const auto cache = getCacheFile();

    if (! forceRescan && cache != juce::File() && cache.existsAsFile())
    {
        if (cache.loadFileAsString().startsWith (signature))
        {
            std::vector<PluginEntry> cached;
            readLibraryFile (cache, cached);

            if (! cached.empty())
            {
                std::sort (cached.begin(), cached.end(), [] (const PluginEntry& a, const PluginEntry& b)
                {
                    if (a.isInstrument != b.isInstrument)
                        return a.isInstrument;

                    return a.getDisplayName().compareIgnoreCase (b.getDisplayName()) < 0;
                });

                library = std::move (cached);
                ++libraryVersion;
                scanned = true;
                scanSummary = juce::String ((int) library.size()) + utf8 (" 个插件 · ")
                                + juce::String (countInstruments()) + utf8 (" 乐器 / ")
                                + juce::String (countEffects()) + utf8 (" 效果器")
                                + utf8 ("（缓存）");

                if (onLibraryChanged != nullptr)
                    onLibraryChanged();

                return (int) library.size();
            }
        }
    }

    const auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getNonexistentChildFile ("atomic-scan-", ".txt", false);

    bool ranInChild = false;
    bool childStarted = false;

    if (const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        exe.existsAsFile())
    {
        juce::StringArray command;
        command.add (exe.getFullPathName());
        command.add ("--plugin-scan-child");
        command.add (temp.getFullPathName());

        for (int i = 0; i < paths.getNumPaths(); ++i)
            command.add (paths[i].getFullPathName());

        juce::ChildProcess child;
        childStarted = child.start (command);

        if (childStarted)
        {
            ranInChild = child.waitForProcessToFinish (scanTimeoutMs);

            if (! ranInChild)
            {
                child.kill();
                ranInChild = true;
            }
        }
    }

    std::vector<PluginEntry> found;
    readLibraryFile (temp, found);

    // No child process available (unusual, but a stripped-down build should
    // still work): scan here and accept that a bad plugin can end the session.
    if (found.empty() && ! childStarted)
    {
        scanPathsToFile (paths, temp);
        found.clear();
        readLibraryFile (temp, found);
    }

    if (cache != juce::File() && ! found.empty())
    {
        cache.getParentDirectory().createDirectory();
        cache.replaceWithText (signature + "\n" + temp.loadFileAsString());
    }

    temp.deleteFile();

    std::sort (found.begin(), found.end(), [] (const PluginEntry& a, const PluginEntry& b)
    {
        if (a.isInstrument != b.isInstrument)
            return a.isInstrument;              // instruments first

        return a.getDisplayName().compareIgnoreCase (b.getDisplayName()) < 0;
    });

    library = std::move (found);
    ++libraryVersion;
    scanned = true;

    scanSummary = juce::String ((int) library.size()) + utf8 (" 个插件 · ")
                    + juce::String (countInstruments()) + utf8 (" 乐器 / ")
                    + juce::String (countEffects()) + utf8 (" 效果器");

    if (onLibraryChanged != nullptr)
        onLibraryChanged();

    return (int) library.size();
}

void PluginHost::scanPathsToFile (const juce::FileSearchPath& paths, const juce::File& outFile)
{
    outFile.deleteFile();

    juce::Array<juce::File> bundles;

    for (int i = 0; i < paths.getNumPaths(); ++i)
        collectBundles (paths[i], bundles);

    juce::VST3PluginFormat vst3;
    juce::StringArray        seen;

    for (const auto& bundle : bundles)
    {
        juce::OwnedArray<juce::PluginDescription> types;
        scanOneBundleSafely (vst3, types, bundle.getFullPathName());

        juce::StringArray lines;

        for (const auto* type : types)
        {
            if (type == nullptr)
                continue;

            // The same plugin can be reachable through two search paths.
            const auto key = type->createIdentifierString();

            if (seen.contains (key))
                continue;

            seen.add (key);

            if (auto xml = std::unique_ptr<juce::XmlElement> (type->createXml()))
                lines.add (xml->toString (juce::XmlElement::TextFormat().singleLine()));
        }

        // Written per bundle, not at the end: if the next plugin kills the
        // process, everything found so far is already on disk.
        if (! lines.isEmpty())
            outFile.appendText (lines.joinIntoString ("\n") + "\n", false, false, "\n");
    }
}

int PluginHost::countInstruments() const
{
    int n = 0;

    for (const auto& entry : library)
        if (entry.isInstrument)
            ++n;

    return n;
}

int PluginHost::countEffects() const
{
    return (int) library.size() - countInstruments();
}

//==============================================================================
int PluginHost::load (const juce::PluginDescription& description, juce::String* errorOut)
{
    // Prepare at the largest block we will ever hand a plugin. Buffers are
    // sized once and plugins are allowed to assume that; the renderer chunks
    // longer windows itself.
    const int    blockSize   = getRenderBlockSize();
    const double sampleRate  = playSampleRate;

    juce::String error;
    auto plugin = formatManager.createPluginInstance (description, sampleRate, blockSize, error);

    if (plugin == nullptr)
    {
        if (errorOut != nullptr)
            *errorOut = error.isNotEmpty() ? error : utf8 ("无法创建插件实例");

        return 0;
    }

    const bool instrument = looksLikeInstrument (description);

    plugin->enableAllBuses();
    plugin->setPlayConfigDetails (instrument ? 0 : 2, 2, sampleRate, blockSize);
    plugin->prepareToPlay (sampleRate, blockSize);

    const int id = nextInstanceId++;

    {
        const juce::ScopedLock sl (lock);

        Instance instance;
        instance.id           = id;
        instance.description  = description;
        instance.isInstrument = instrument;
        instance.plugin       = std::move (plugin);

        instances.push_back (std::move (instance));
    }

    if (onInstancesChanged != nullptr)
        onInstancesChanged();

    return id;
}

void PluginHost::unload (int instanceId)
{
    {
        const juce::ScopedLock sl (lock);

        instances.erase (std::remove_if (instances.begin(), instances.end(),
                                         [instanceId] (const Instance& i) { return i.id == instanceId; }),
                         instances.end());
    }

    if (onInstancesChanged != nullptr)
        onInstancesChanged();
}

void PluginHost::unloadAll()
{
    {
        const juce::ScopedLock sl (lock);
        instances.clear();
    }

    if (onInstancesChanged != nullptr)
        onInstancesChanged();
}

int PluginHost::getNumInstances() const
{
    const juce::ScopedLock sl (lock);
    return (int) instances.size();
}

bool PluginHost::isLoaded (int instanceId) const
{
    const juce::ScopedLock sl (lock);

    for (const auto& instance : instances)
        if (instance.id == instanceId)
            return true;

    return false;
}

juce::String PluginHost::getName (int instanceId) const
{
    const juce::ScopedLock sl (lock);

    for (const auto& instance : instances)
        if (instance.id == instanceId)
            return instance.description.name;

    return {};
}

bool PluginHost::isInstrument (int instanceId) const
{
    const juce::ScopedLock sl (lock);

    for (const auto& instance : instances)
        if (instance.id == instanceId)
            return instance.isInstrument;

    return false;
}

int PluginHost::getBlockSizeOf (int instanceId) const
{
    juce::ignoreUnused (instanceId);
    return getRenderBlockSize();
}

int PluginHost::getLatencySamples (int instanceId) const
{
    if (instanceId <= 0)
        return 0;

    const juce::ScopedLock sl (lock);

    for (const auto& instance : instances)
        if (instance.id == instanceId && instance.plugin != nullptr)
            return juce::jmax (0, instance.plugin->getLatencySamples());

    return 0;
}

std::vector<int> PluginHost::getInstanceIds() const
{
    std::vector<int> ids;

    const juce::ScopedLock sl (lock);

    for (const auto& instance : instances)
        ids.push_back (instance.id);

    return ids;
}

int PluginHost::countInstancesOf (const juce::PluginDescription& description) const
{
    const auto key = description.createIdentifierString();
    int n = 0;

    const juce::ScopedLock sl (lock);

    for (const auto& instance : instances)
        if (instance.description.createIdentifierString() == key)
            ++n;

    return n;
}

juce::StringArray PluginHost::getLoadedSummary() const
{
    juce::StringArray names;
    juce::Array<int>  counts;

    {
        const juce::ScopedLock sl (lock);

        for (const auto& instance : instances)
        {
            const auto index = names.indexOf (instance.description.name);

            if (index < 0)
            {
                names.add (instance.description.name);
                counts.add (1);
            }
            else
            {
                counts.set (index, counts[index] + 1);
            }
        }
    }

    juce::StringArray lines;

    for (int i = 0; i < names.size(); ++i)
        lines.add (names[i] + "  ·  " + juce::String (counts[i]) + utf8 (" 个实例"));

    return lines;
}

//==============================================================================
void PluginHost::setPlayConfig (double newSampleRate, int newBlockSize)
{
    playSampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    playBlockSize  = juce::jmax (64, newBlockSize);

    const int blockSize = getRenderBlockSize();

    const juce::ScopedLock sl (lock);

    for (auto& instance : instances)
    {
        if (instance.plugin == nullptr)
            continue;

        instance.plugin->setRateAndBufferSizeDetails (playSampleRate, blockSize);
        instance.plugin->prepareToPlay (playSampleRate, blockSize);
    }
}

std::unique_ptr<PluginHost> PluginHost::createOfflineClone (double sampleRate, int blockSize) const
{
    struct Copy { int id; juce::PluginDescription description; bool isInstrument; };

    std::vector<Copy> copies;

    {
        const juce::ScopedLock sl (lock);

        for (const auto& instance : instances)
            copies.push_back ({ instance.id, instance.description, instance.isInstrument });
    }

    if (copies.empty())
        return nullptr;

    auto clone = std::make_unique<PluginHost>();
    clone->playSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    clone->playBlockSize  = juce::jmax (64, blockSize);

    const int preparedBlock = clone->getRenderBlockSize();

    for (const auto& copy : copies)
    {
        juce::String error;
        auto plugin = clone->formatManager.createPluginInstance (copy.description,
                                                                 clone->playSampleRate,
                                                                 preparedBlock, error);

        if (plugin == nullptr)
            continue;       // a plugin that refuses a second instance is skipped

        plugin->enableAllBuses();
        plugin->setPlayConfigDetails (copy.isInstrument ? 0 : 2, 2, clone->playSampleRate, preparedBlock);
        plugin->prepareToPlay (clone->playSampleRate, preparedBlock);

        Instance instance;
        instance.id           = copy.id;        // SAME id: the schedule keeps meaning
        instance.description  = copy.description;
        instance.isInstrument = copy.isInstrument;
        instance.plugin       = std::move (plugin);

        clone->instances.push_back (std::move (instance));
        clone->nextInstanceId = juce::jmax (clone->nextInstanceId, copy.id + 1);
    }

    if (clone->instances.empty())
        return nullptr;

    return clone;
}

//==============================================================================
juce::AudioPluginInstance* PluginHost::getProcessorForEditor (int instanceId) const
{
    const juce::ScopedLock sl (lock);

    for (const auto& instance : instances)
        if (instance.id == instanceId)
            return instance.plugin.get();

    return nullptr;
}

juce::AudioPluginInstance* PluginHost::getInstanceWhileLocked (int instanceId) const
{
    if (instanceId <= 0)
        return nullptr;

    for (const auto& instance : instances)
        if (instance.id == instanceId)
            return instance.plugin.get();

    return nullptr;
}

void PluginHost::resetAllInstancesWhileLocked() const
{
    for (const auto& instance : instances)
        if (instance.plugin != nullptr)
            instance.plugin->reset();
}

} // namespace atomic
