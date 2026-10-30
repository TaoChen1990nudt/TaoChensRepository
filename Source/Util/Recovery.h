#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace atomic
{
//==============================================================================
/** Phase 84: where unsaved work waits while the process is not closing cleanly.

    The editor already knows how to describe an entire session as one string
    (`MainComponent::captureState`) and how to put one back (`restoreState`).
    Undo is built on exactly that pair. So "survive a crash" is not a new
    serialisation problem — it is a question of *when* to write that string
    somewhere and *when* to believe it again.

    Everything in here is a plain function over files, so the interesting
    decisions can be asserted without a running app.

    The one invariant the whole feature rests on:

    > **A recovery file exists if and only if this session holds edits that are
    > not in any saved project, and the process did not get to close.**

    Every rule below is chosen to keep that sentence true:

    - saving a project deletes the copy (the disk is newer now),
    - explicitly discarding changes deletes the copy (the user said no),
    - a normal close with nothing to save never had one,
    - and a crash leaves it behind, which is the only way it can survive.

    Identity, not location: the file name is derived from the project's own
    path, so two projects both called `song.atomic` in different folders never
    share one file. The single accepted collision is two never-saved windows at
    once — they are both `untitled`, and the second would overwrite the first.
    Two unsaved windows cannot be told apart by anything a file name could say,
    and inventing a random suffix per launch would make the "which one is this"
    question unanswerable after a crash instead of merely ambiguous.
*/
namespace recovery
{
    /** Same text as `project::fileExtension`, written out here so this header
        does not drag the whole model into a utility. The self-test pins the two
        together, because an extension that drifts apart would make the rescue
        file unopenable by the very code that wrote it. */
    inline constexpr const char* fileExtension = ".atomic";

    /** The folder recovery copies live in. Created on demand, never pruned by
        this module — a copy is only deleted when it has been dealt with. */
    juce::File getFolder();

    /** Where this project's unsaved work would be written.

        An empty `projectFile` (a session that has never been saved) has one
        fixed name, see the note above about the one accepted collision. */
    juce::File getFileFor (const juce::File& projectFile);

    /** One recovery copy found on disk. */
    struct Entry
    {
        juce::File   file;
        juce::File   projectFile;   // the project it claims to belong to; may not exist
        juce::String projectPath;   // that path as text, for showing the user
        juce::Time   savedAt;
        juce::int64  bytes = 0;
    };

    /** What to do with a copy on startup. Split out from the dialog because
        this is the part that can be wrong, and the dialog cannot be asserted. */
    enum class Decision
    {
        none,       // no copy: the normal case, nothing to say
        unusable,   // there is a file but it is empty — leftover junk, delete it
        stale,      // not newer than the project it belongs to: it would take
                    // the user backwards, delete it
        offer       // genuinely holds work that is not on disk anywhere
    };

    /** `projectFile` is the project the copy *claims* to belong to (from its
        `recoveryOf` attribute), not whatever the app happens to have open.

        A project that does not exist cannot be older than the copy, and that is
        the case worth rescuing most: nothing else holds it. */
    Decision inspect (const juce::File& recoveryFile, const juce::File& projectFile);

    /** Writes `xml` (a whole-session document) plus two bookkeeping attributes.

        `recoveryOf` / `recoveredAt` are added to the root element only. The
        parser reads the attributes it knows and ignores the rest, so the file
        stays a perfectly openable project — which matters, because the last
        resort for a user is to open the file by hand.

        The write itself is atomic: `File::replaceWithText` goes through JUCE's
        `TemporaryFile`, so a crash in the middle leaves the *previous* copy
        intact. Writing it any other way would make the rescue file the least
        reliable file on the machine. */
    bool write (const juce::File& recoveryFile, const juce::String& xml,
                const juce::File& projectFile, juce::String& error);

    /** The `recoveryOf` attribute of a copy, or an empty string when the file
        is missing or is not one of ours. */
    juce::String readProjectPath (const juce::File& recoveryFile);

    /** Every copy in the folder, newest first. Cheap enough to call on startup:
        a session document is kilobytes, because samples are referenced by path
        rather than embedded. */
    std::vector<Entry> list();

    /** The same scan against an arbitrary folder, which is how the self-test
        exercises it without reading or writing the user's documents. */
    std::vector<Entry> listIn (const juce::File& folder);

    /** Best effort. A copy that cannot be deleted is not a failure worth
        reporting — `inspect()` will call it stale next time anyway. */
    void discard (const juce::File& recoveryFile);

} // namespace recovery
} // namespace atomic
