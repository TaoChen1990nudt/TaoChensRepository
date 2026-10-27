#include "ScopeRender.h"

#include "Media/SampleLibrary.h"

#include <algorithm>
#include <cmath>

namespace atomic::scope
{

namespace
{
    bool contains (const std::vector<int>& ids, int id)
    {
        return std::find (ids.begin(), ids.end(), id) != ids.end();
    }
}

//==============================================================================
ExtractResult extract (const PublishedSchedule& full, const Scope& scope)
{
    ExtractResult out;
    out.schedule = full;                 // layers, slots, groups, channels, curves
    out.schedule.notes.clear();
    out.schedule.clips.clear();

    const double secondsPerBeat = juce::jmax (1.0e-9, full.secondsPerBeat);

    double first = 0.0;
    double last  = 0.0;
    bool   any   = false;

    const auto noteWanted = [&scope] (const ScheduledNote& note)
    {
        if (scope.layerIndex >= 0 && note.layerIndex != scope.layerIndex)
            return false;

        switch (scope.kind)
        {
            case Scope::Kind::group:     return note.groupId == scope.groupId;
            case Scope::Kind::selection: return contains (scope.noteIds, note.noteId);
            case Scope::Kind::layer:
            default:                     return true;
        }
    };

    const auto clipWanted = [&scope] (const ScheduledClip& clip)
    {
        if (scope.layerIndex >= 0 && clip.layerIndex != scope.layerIndex)
            return false;

        switch (scope.kind)
        {
            case Scope::Kind::group:     return clip.groupId == scope.groupId;
            case Scope::Kind::selection: return contains (scope.sampleIds, clip.clipId);
            case Scope::Kind::layer:
            default:                     return true;
        }
    };

    for (const auto& note : full.notes)
    {
        if (! noteWanted (note))
            continue;

        out.schedule.notes.push_back (note);

        if (! any || note.startSeconds < first) first = note.startSeconds;
        if (! any || note.endSeconds   > last)  last  = note.endSeconds;
        any = true;
    }

    for (const auto& clip : full.clips)
    {
        if (! clipWanted (clip))
            continue;

        out.schedule.clips.push_back (clip);

        if (! any || clip.startSeconds < first) first = clip.startSeconds;
        if (! any || clip.endSeconds   > last)  last  = clip.endSeconds;
        any = true;
    }

    out.empty         = ! any;
    out.startSeconds  = any ? juce::jmax (0.0, first) : 0.0;
    out.lengthSeconds = any ? juce::jmax (0.0, last - out.startSeconds) : 0.0;

    // The end, expressed in the unit renderToFile measures duration in. It
    // stays absolute: the render's duration is `contentSeconds - startSeconds`,
    // which is exactly "from the first kept row to the last".
    out.schedule.contentBeats = last / secondsPerBeat;

    return out;
}

//==============================================================================
FreeBeat findFreeBeat (std::vector<std::pair<double, double>> occupied,
                       double lengthBeats,
                       double contentEnd)
{
    FreeBeat result;

    if (lengthBeats <= 0.0)
        return result;

    // Sort and merge first: overlapping notes would otherwise make the scan
    // test the same stretch twice and, worse, "find" a gap inside a chord.
    std::sort (occupied.begin(), occupied.end(),
               [] (const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<std::pair<double, double>> merged;

    for (const auto& span : occupied)
    {
        if (span.second <= span.first)
            continue;

        if (! merged.empty() && span.first <= merged.back().second + 1.0e-9)
            merged.back().second = juce::jmax (merged.back().second, span.second);
        else
            merged.push_back (span);
    }

    double cursor = 0.0;

    // An empty layer has nothing to collide with, so beat 0 is the answer — and
    // it is the ONLY case where the span matters, because `contentEnd` is 0 too.
    // Without this line a bounce onto a fresh layer would report "no room",
    // which is the one answer that is certainly wrong.
    if (merged.empty())
    {
        result.found = true;
        result.beat  = 0.0;
        return result;
    }

    for (const auto& span : merged)
    {
        if (span.first - cursor >= lengthBeats - 1.0e-9)
        {
            result.found = true;
            result.beat  = cursor;
            return result;
        }

        cursor = juce::jmax (cursor, span.second);
    }

    // The tail. Only "up to the end of existing content" counts as room: past
    // that the block would lengthen the layer, which is a different decision
    // and not one a bounce gets to make on the user's behalf.
    if (contentEnd - cursor >= lengthBeats - 1.0e-9)
    {
        result.found = true;
        result.beat  = cursor;
    }

    return result;
}

//==============================================================================
InsertResult insertSampleBlock (LayerStack& stack, RackModel& rack, int layerIndex,
                                const juce::File& file, double bpm,
                                const juce::String& displayName,
                                int laneSpan, int pitch)
{
    InsertResult out;

    auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
    {
        out.message = juce::String::fromUTF8 (u8"目标图层不在了");
        return out;
    }

    const auto loaded = loadSampleFile (file);

    if (! loaded.ok())
    {
        out.message = loaded.error;
        return out;
    }

    // The block is exactly as long as the audio it holds (Phase 31's rule).
    const double beats = SampleEvent::beatsForSeconds (loaded.buffer->lengthSeconds(),
                                                       juce::jmax (1.0, bpm));

    std::vector<std::pair<double, double>> occupied;
    double contentEnd = 0.0;

    for (const auto& note : layer->getNotes())
    {
        occupied.push_back ({ note.startBeat, note.endBeat() });
        contentEnd = juce::jmax (contentEnd, note.endBeat());
    }

    for (const auto& clip : layer->getSamples())
    {
        occupied.push_back ({ clip.startBeat, clip.endBeat() });
        contentEnd = juce::jmax (contentEnd, clip.endBeat());
    }

    const auto free = findFreeBeat (std::move (occupied), beats, contentEnd);

    if (! free.found)
    {
        out.message = juce::String::fromUTF8 (u8"这个图层里没有空位");
        return out;
    }

    SampleEvent sample;
    sample.name        = displayName;
    sample.path        = file.getFullPathName();
    sample.audio       = loaded.buffer;
    sample.startBeat   = free.beat;
    sample.lengthBeats = beats;
    sample.laneSpan    = juce::jlimit (1, SampleEvent::numPitchLanes, laneSpan);
    sample.pitch       = juce::jlimit (0, 127 - sample.laneSpan, pitch);

    // A bounced block is a normal block: it gets a sampler like a dropped file,
    // so it can be mounted elsewhere, trimmed, reversed and re-rendered.
    sample.outputSlotId = rack.addSampler (loaded.buffer, sample.path, sample.name);

    out.sampleId = layer->addSample (std::move (sample));
    out.beat     = free.beat;
    out.ok       = true;

    stack.notifyNotesChanged();

    return out;
}

} // namespace atomic::scope
