#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace atomic
{
//==============================================================================
/** Phase 86: what a project file says about where its audio lives.

    Until now it said everything there is to say: an absolute path, because
    `SampleEvent::path` is absolute and the writer wrote it out verbatim. That
    is correct exactly as long as the folder never moves.

    The moment it does — a rename, a copy to another machine, "send me the
    project" — every block on the timeline points at nothing, the wave draws as
    missing, and the only repair is re-importing each file by hand. A DAW whose
    projects cannot leave the machine they were made on is not portable in any
    sense that matters.

    So the document now stores a path *relative to the project file* whenever
    one exists, and an absolute path otherwise. Two rules keep that honest:

    - **The model never stores a relative path.** `SampleEvent::path` stays
      absolute. Relative is a property of the DOCUMENT, produced on the way out
      and consumed on the way in — a `juce::File ("media\\kick.wav")` resolved
      against an implicit "current directory" is a bug waiting for the first
      `setCurrentWorkingDirectory`.

    - **A relative path is only written if it resolves back to the same file.**
      Not "if it looks reasonable", not "if the prefix matches": the writer
      computes the candidate, runs it through the same resolver the reader uses,
      and writes it only when the answer equals the file it started from. A
      different drive, a symlink, a `..` chain that a re-parse would land
      somewhere else — all of them fail that test and fall back to absolute,
      which is always correct and only less portable.

    Everything here is a plain function over paths, so the interesting decisions
    can be asserted without a project, a window or an audio device.
*/
namespace samplepath
{
    /** A path that is not there, expressed as the file it wanted.

        Empty input answers empty — "no audio" is not "missing audio", and the
        difference is what the block draws. */
    juce::File asFile (const juce::String& absolutePath);

    /** The shortest correct path to `absolutePath` from the project's folder.

        `projectFile` may be empty (a session that has never been saved), in
        which case the answer is the absolute path unchanged: there is no folder
        to be relative to, and inventing one would put the document's meaning at
        the mercy of whatever directory the app was launched from.

        The result is verified by round-tripping it through `resolve()`, so a
        caller never has to think about drive letters, `..` chains or what
        Windows does with `D:\\a\\..\\..\\E:\\b`.
    */
    juce::String toStored (const juce::String& absolutePath, const juce::File& projectFile);

    /** The inverse: what a document's `path` attribute means. No disk access at
        all, so this is the same answer on the reader's thread as on the
        writer's — which is what makes the round-trip check above meaningful.

        An absolute stored path (every project written before v27) is returned
        untouched, so those files keep opening exactly as they always did. */
    juce::File resolve (const juce::String& storedPath, const juce::File& projectFile);

    /** `resolve()`, then look for the same file name beside the project.

        This is the "the folder was copied but the paths were absolute" rescue:
        an old project moved to a new machine has `C:\\Users\\someone\\Music\\
        kick.wav` in it and a `media\\kick.wav` sitting right next to the
        `.atomic`. Looking there costs one directory listing per missing file
        and turns "open it and re-import everything" into "open it".

        Returns an invalid File when nothing was found; callers treat that as
        still missing. */
    juce::File resolveExisting (const juce::String& storedPath, const juce::File& projectFile);

    /** Every file name reachable under `folder`, mapped to its first hit.

        Keys are lower-cased because Windows is case-insensitive and a project
        that moved between a case-preserving and a case-insensitive medium runs
        into that difference immediately. Depth is capped so pointing this at
        `C:\\` cannot turn the UI into a disk scan. */
    struct FolderIndex
    {
        int files = 0;
        int truncatedAtDepth = 0;      // how many names were dropped for depth
        std::vector<std::pair<juce::String, juce::File>> byName;   // lower-case name

        const juce::File* find (const juce::String& fileName) const;
    };

    FolderIndex indexFolder (const juce::File& folder, int maxDepth = 6);

    /** One `SampleEvent`'s worth of "this block cannot find its audio". */
    struct Missing
    {
        juce::String path;      // what the document said
        juce::String name;      // what the block is called
    };

    /** Which of these paths are gone. Order is preserved, ids are not: the
        caller keeps its own pairing. */
    std::vector<juce::String> collectMissing (const std::vector<juce::String>& paths);

    /** The replacements a user's chosen folder offers.

        One entry per input path, in the same order. An invalid File means "that
        file name is not in there either" — a relink that silently drops what it
        could not fix is how a user ends up believing a project was repaired.

        Matching is by file name only, which is a deliberate limit: matching by
        duration or content hash would resolve two files called `kick.wav` into
        each other's places, and being wrong about which audio plays is worse
        than being obviously incomplete. */
    std::vector<juce::File> relinkFrom (const std::vector<juce::String>& missingPaths,
                                       const juce::File& folder);

} // namespace samplepath
} // namespace atomic
