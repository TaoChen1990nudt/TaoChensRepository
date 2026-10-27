#pragma once

#include <utility>
#include <vector>

#include "Audio/PlaybackEngine.h"

namespace atomic::scope
{

//==============================================================================
/**
    Phase 50: "render THIS to a block" — what the scope is, and what comes back.

    The bounce itself is `bounce::renderToFile`, the same function the export menu
    calls. This module only answers "which part of the schedule is the user
    pointing at", because that is the only new question: a group, a layer and a
    selection are three ways of naming a subset of one frozen schedule.

    Deliberately a filter over a PUBLISHED schedule rather than a second
    publisher. Publishing again from a hand-built LayerStack would be a second
    mixer: routing, group trims, both faders, every curve would have to be
    re-derived, and the frozen pack would eventually disagree with what the user
    hears. Filtering cannot disagree — it removes rows and keeps everything else.
*/
struct Scope
{
    enum class Kind
    {
        selection,   // noteIds / sampleIds below
        group,       // groupId below
        layer        // layerIndex, everything on it
    };

    Kind kind       = Kind::layer;
    int  layerIndex = -1;
    int  groupId    = -1;
    std::vector<int> noteIds;
    std::vector<int> sampleIds;
};

struct ExtractResult
{
    PublishedSchedule schedule;
    double startSeconds  = 0.0;   // where the kept content begins
    double lengthSeconds = 0.0;   // how much of the timeline it covers
    bool   empty         = true;  // nothing matched the scope
};

/** The kept rows, with `contentBeats` moved to the real end of what is left.

    Times are NOT rebased. The renderer can start at any second, so the honest
    answer to "the first note is at beat 20" is "start reading at 10 s", not
    "subtract 10 s from every note and every automation point" — the second one
    has to shift curves too, and a missed curve silently changes the mix. */
ExtractResult extract (const PublishedSchedule& full, const Scope& scope);

//==============================================================================
/** Phase 50: where a freshly bounced block can land.

    `occupied` is every note and sample block on the target layer, as
    (start, end) in beats; `contentEnd` is the end of that layer's content. The
    search is limited to that span on purpose: appending after the last note is
    not "a place without notes", it is silently making the song longer, and the
    user asked to be told when there is no room. */
struct FreeBeat
{
    bool   found = false;
    double beat  = 0.0;
};

FreeBeat findFreeBeat (std::vector<std::pair<double, double>> occupied,
                       double lengthBeats,
                       double contentEnd);

//==============================================================================
/** Phase 50: puts a bounced file on a layer as a mounted block.

    A free function, not a MainComponent method, for the reason every other
    model edit is one: the window should not own the arithmetic, and a test that
    has to drive a window to check "did the block land" ends up asserting the
    window instead of the block. The host keeps only the parts that need it —
    which file, which layer, the message box and the undo step.
*/
struct InsertResult
{
    bool   ok     = false;
    int    sampleId = -1;
    double beat   = 0.0;
    juce::String message;      // empty on success; why it failed otherwise
};

InsertResult insertSampleBlock (LayerStack& stack, RackModel& rack, int layerIndex,
                                const juce::File& file, double bpm,
                                const juce::String& displayName,
                                int laneSpan = 4, int pitch = 60);

} // namespace atomic::scope
