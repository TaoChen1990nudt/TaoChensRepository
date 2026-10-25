#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <memory>
#include <vector>

#include "Model/Gain.h"

namespace atomic
{

//==============================================================================
/** One plugin found on disk. `isInstrument` decides which half of the plugin
    manager it is listed in, and whether the rack will offer it. */
struct PluginEntry
{
    juce::PluginDescription description;
    bool                    isInstrument = false;

    /** "Vendor — Name", falling back to the bare name. */
    juce::String getDisplayName() const
    {
        const auto& d = description;

        return d.manufacturerName.isEmpty() || d.manufacturerName == d.name
                 ? d.name
                 : d.manufacturerName + " — " + d.name;
    }
};

//==============================================================================
/**
    Owns every VST3 instance in the session.

    Why this is a separate object and not a few pointers on the rack:

    - **One lock, one place.** The audio thread processes plugins, the message
      thread creates, prepares and destroys them. The only synchronisation point
      is `getLock()`. The audio thread uses a TRY lock and skips the plugin
      stage when it fails, so a plugin that is being added mid-playback can
      never stall the sound card; the message thread uses a blocking lock,
      which is fine because it is not realtime.
    - **Lifetime is explicit.** Instances are addressed by a stable id, never by
      pointer. A layer, a note override and a mixer insert all store an int;
      deleting a plugin simply makes that id resolve to nothing, which every
      call site already has to handle (a saved project can name a plugin that is
      no longer installed).
    - **Offline bouncing gets its own instances.** Two threads processing one
      plugin state machine at the same time is a data race inside someone
      else's code, which is not a bug you can fix. `createOfflineClone()` builds
      a second set with the same ids, so the bounce is bit-for-bit the same
      graph and the live playback is untouched.
*/
class PluginHost
{
public:
    PluginHost();
    ~PluginHost();

    //-- Library (what is installed) -----------------------------------------
    void addSearchPath (const juce::File& directory);
    void clearExtraSearchPaths();
    juce::FileSearchPath getSearchPaths() const;

    /** Blocks; message thread only. Returns how many plugins were found.

        Runs the actual scan in a CHILD PROCESS of this executable. JUCE 8's VST3
        format loads every bundle into the calling process to read its class
        list (the `moduleinfo.json` fast path only covers newer bundles), so a
        single misbehaving plugin takes the editor down with it — measured on
        this machine, where u-he Diva and Hive fault while being inspected. A
        child process turns that into "one plugin missing from the list".

        The result is cached on disk, keyed by the search paths: scanning 222
        plugins takes about two and a half minutes, and paying that on every
        launch would make the feature unusable. `forceRescan` is what the plugin
        manager's 重新扫描 button uses. */
    int scan (bool forceRescan = false);

    /** Where the scan result is cached. Empty when the scan is restricted to the
        extra paths — the headless harness must not overwrite the real cache. */
    juce::File getCacheFile() const;

    /** The scan itself: no session, no child process, no GUI. Static because
        the child is a bare executable. Appends one `PLUGIN` XML element per
        plugin (one per line) as it goes, so a scan that dies halfway still
        leaves everything it had already found. */
    static void scanPathsToFile (const juce::FileSearchPath& paths, const juce::File& outFile);

    /** How long the parent waits before killing a stuck child. */
    static constexpr int scanTimeoutMs = 5 * 60 * 1000;

    /** When true, scan() looks at the extra paths only and ignores the system
        VST3 folders. The headless harness sets it so taking a screenshot does
        not enumerate (or inspect) every plugin the user has installed — that
        can take minutes and is not what the screenshot is about. */
    void setScanExtraPathsOnly (bool shouldRestrict) noexcept { extraPathsOnly = shouldRestrict; }
    bool isScanExtraPathsOnly() const noexcept                { return extraPathsOnly; }

    const std::vector<PluginEntry>& getLibrary() const noexcept { return library; }

    int countInstruments() const;
    int countEffects() const;

    bool hasScanned() const noexcept { return scanned; }
    juce::String getScanSummary() const { return scanSummary; }
    int getLibraryVersion() const noexcept { return libraryVersion; }

    //-- Instances (what is loaded) -------------------------------------------
    /** Creates and prepares one instance. Returns 0 on failure. Message thread.

        @param errorOut filled with the format's complaint when it fails. */
    int load (const juce::PluginDescription&, juce::String* errorOut = nullptr);

    void unload (int instanceId);
    void unloadAll();

    int  getNumInstances() const;
    bool isLoaded (int instanceId) const;

    juce::String getName (int instanceId) const;
    bool         isInstrument (int instanceId) const;
    int          getBlockSizeOf (int instanceId) const;

    /** Every loaded instance id, in creation order. Used to work out which
        plugins nothing points at any more. */
    std::vector<int> getInstanceIds() const;

    /** How many instances of this exact plugin are loaded. The plugin manager
        prints it next to the name so "already in the rack" is visible. */
    int countInstancesOf (const juce::PluginDescription&) const;

    /** Lines like "Synth — 3 个实例" for the plugin manager. */
    juce::StringArray getLoadedSummary() const;

    //-- Audio configuration --------------------------------------------------
    void   setPlayConfig (double newSampleRate, int newBlockSize);
    double getSampleRate() const noexcept { return playSampleRate; }
    int    getBlockSize()  const noexcept { return playBlockSize; }

    /** The largest buffer a plugin has been prepared for. The renderer chunks
        anything longer than this itself, so a 1-second offline window cannot
        hand a plugin a buffer it never sized for. */
    static constexpr int maxBlockSize = 4096;
    int getRenderBlockSize() const noexcept { return juce::jmax (maxBlockSize, playBlockSize); }

    /** Same plugins, same instance ids, its own state. Null if nothing loaded. */
    std::unique_ptr<PluginHost> createOfflineClone (double sampleRate, int blockSize) const;

    //-- Audio thread ---------------------------------------------------------
    /** Hold this for a whole plugin stage of a block. Only ever try-lock it on
        the audio thread. */
    juce::CriticalSection& getLock() noexcept { return lock; }
    const juce::CriticalSection& getLock() const noexcept { return lock; }

    /** Only valid while the caller holds getLock(). */
    juce::AudioPluginInstance* getInstanceWhileLocked (int instanceId) const;

    /** Phase 12: the instance an editor window may be built on.

        Message thread only. The pointer stays valid until the instance is
        unloaded, and unloading happens on the message thread too, so a window
        that closes itself when its plugin goes away never dereferences a dead
        processor. Never hand this to the audio thread. */
    juce::AudioPluginInstance* getProcessorForEditor (int instanceId) const;

    /** Audio thread, lock held: drops every note a hosted instrument is holding
        and clears every effect's tail. Called when the transport stops or
        wraps, because a note-off that was never sent leaves a synth droning
        forever. */
    void resetAllInstancesWhileLocked() const;

    //-- Notification ---------------------------------------------------------
    std::function<void()> onLibraryChanged;
    std::function<void()> onInstancesChanged;

private:
    struct Instance
    {
        int                                        id = 0;
        juce::PluginDescription                    description;
        bool                                       isInstrument = false;
        std::unique_ptr<juce::AudioPluginInstance> plugin;
    };

    void rebuildSearchPaths();
    static void collectBundles (const juce::File& root, juce::Array<juce::File>& out);
    static bool looksLikeInstrument (const juce::PluginDescription&);

    juce::AudioPluginFormatManager formatManager;
    juce::FileSearchPath           extraSearchPaths;

    std::vector<PluginEntry> library;
    std::vector<Instance>    instances;

    mutable juce::CriticalSection lock;

    double playSampleRate = 44100.0;
    int    playBlockSize  = 512;

    int nextInstanceId = 1;
    int libraryVersion = 0;
    bool scanned = false;
    bool extraPathsOnly = false;
    juce::String scanSummary;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginHost)
};

} // namespace atomic
