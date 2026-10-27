#pragma once

#include <juce_core/juce_core.h>
#include <vector>

namespace atomic
{

//==============================================================================
/** What a note's own curve actually modulates.

    Phase 8: the curve used to have exactly one job — scale the note's level.
    Now the note carries a selector, so the same 0..1 curve can drive level, pan
    or pitch. It is stored on the note (by value, like the curve) so a copy
    carries its target with it; a copy that silently reverted to "level" would
    sound different from the original for no visible reason.

    Kept in the model header, not the UI: the audio thread has to name the same
    three things the menu does. */
enum class NoteModTarget
{
    level = 0,   // 0..1 multiplies the note's gain
    pan   = 1,   // 0..1 sweeps the note's pan, 0.5 = centre
    pitch = 2    // 1.0 = no bend, 0.0 = one octave down (a glide)
};

/** ASCII key used in the project file. Chinese labels live in the UI so the
    model never needs a font. */
inline const char* noteModTargetKey (NoteModTarget target) noexcept
{
    switch (target)
    {
        case NoteModTarget::pan:   return "pan";
        case NoteModTarget::pitch: return "pitch";
        case NoteModTarget::level:
        default:                   return "level";
    }
}

inline NoteModTarget noteModTargetFromKey (const juce::String& key) noexcept
{
    if (key == "pan")   return NoteModTarget::pan;
    if (key == "pitch") return NoteModTarget::pitch;

    return NoteModTarget::level;
}

//==============================================================================
/**
    One breakpoint of a note-level envelope.

    `time` is NORMALISED across the note's length: 0.0 is the note's start,
    1.0 is its end. That is the whole trick — because time is normalised, the
    curve travels with the note when it moves and stretches with it when it is
    resized, without ever touching the envelope data.
*/
struct EnvelopePoint
{
    float time  = 0.0f;   // 0..1
    float value = 1.0f;   // 0..1
};

//==============================================================================
/**
    A per-note automation curve. Lives inside NoteEvent by value.

    Deliberately a value type, not a shared_ptr: copying a note must copy its
    automation, and there must be no way to accidentally end up with two notes
    sharing one curve.
*/
class NoteEnvelope
{
public:
    static constexpr int maxPoints = 24;

    NoteEnvelope();

    static NoteEnvelope makeADSR();
    static NoteEnvelope makeFadeOut();
    static NoteEnvelope makeFlat();

    /** Phase 49: a flat, DISABLED curve — the default for a fresh sample clip.

        A note's default is an ADSR because a synth note without a shape sounds
        wrong; a clip's default must be "the file, untouched", which here is a
        flat 1.0 line that is switched off. Same class, different factory, so
        the editor and the renderer cannot invent two different idle states. */
    static NoteEnvelope makeIdle();

    bool isEnabled() const noexcept  { return enabled; }
    void setEnabled (bool e) noexcept { enabled = e; }

    const std::vector<EnvelopePoint>& getPoints() const noexcept { return points; }

    /** Interpolated value at normalised time t. Clamped outside [0, 1]. */
    float valueAt (float t) const noexcept;

    /** Returns the index of the new point, or -1 when the envelope is full. */
    int  addPoint (float time, float value);

    /** Removes a point. Always keeps at least two points. */
    void removePoint (int index);

    /** Moves a point; the first and last points are pinned in time so the
        curve always spans the whole note. */
    void movePoint (int index, float time, float value);

    void resetToDefault();

    /** Replaces the curve wholesale (project load).

        Goes through the same constraints as the editor instead of assigning the
        vector directly, so a hand-edited or truncated file cannot produce a
        curve whose first/last points are not pinned to 0 and 1 — which would
        make the note's envelope stop spanning the note.

        The enabled flag is deliberately NOT touched: it is serialised
        separately, and a loader that silently re-enabled every envelope would
        make a saved "off" state impossible to restore. */
    void loadPoints (std::vector<EnvelopePoint> restoredPoints);

    /** Shape comparison that ignores identity — used by the self-test to prove
        a duplicated note carries an identical curve. */
    bool hasSameShapeAs (const NoteEnvelope& other) const noexcept;

private:
    /** Tag for building an empty envelope without re-entering the default
        constructor (makeADSR must not recurse through resetToDefault). */
    struct EmptyTag {};
    explicit NoteEnvelope (EmptyTag) {}

    void constrainPoint (int index);
    void sortPoints();

    std::vector<EnvelopePoint> points;
    bool enabled = true;

    JUCE_LEAK_DETECTOR (NoteEnvelope)
};

} // namespace atomic
