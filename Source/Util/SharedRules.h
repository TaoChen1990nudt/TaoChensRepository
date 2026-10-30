#pragma once

//==============================================================================
/** Rules that the product code and the self-test must agree on, in one place.

    Everything here used to live in an anonymous namespace inside
    MainComponent.cpp, which made it unreachable from SelfTest.cpp — and the
    alternative was copy-pasting the rule into two files, which is how "the
    test measures one thing and the product does another" bugs are born.
    (Menu id bands are the P43 lesson: the same rule lived in one place until
    a second consumer appeared.)

    Contents, by kind:
      * transport decisions  — stopDecision / spaceStartsPlayback
      * transport glyph geometry — transportIconBox / kTransportGlyph /
        playGlyph / stopGlyph (the self-test measures the same geometry the
        paint routine fills)
      * menu id bands — kSlotAutoParamBase / kSlotAutoTargetBase
*/

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>

namespace atomic
{

//-- Transport decisions --------------------------------------------------------

/** Phase 33: what a STOP press means, as a value.

    `stop` says whether the press does anything at all, `beat` where the
    playhead lands when it does. Two cases, both of them the user's:

      * something is running  -> end it and return to where THIS RUN started
        (Phase 51: the caller passes the beat playTransport() recorded);
      * nothing is running    -> do NOTHING. The playhead is also how you
        choose where playback starts, so a stop press while stopped must not
        throw away the bar you just clicked.

    P32 removed "back to where this run started" because with a hardcoded
    zero it made the stop button a second play-from-here. Phase 51 brings
    the run's own start back at the user's request: stop is now "put it back
    where I started it", and rewind-to-top stays with Return and the ruler.
    The default of 0.0 keeps every caller that has no run to remember
    honest — a stop with no previous start behaves like the old rule. */
struct StopDecision
{
    bool   stop = false;
    double beat = 0.0;
};

inline StopDecision stopDecision (bool engaged, double playStartBeat = 0.0) noexcept
{
    return engaged ? StopDecision { true, juce::jmax (0.0, playStartBeat) }
                   : StopDecision { false, 0.0 };
}

/** Phase 33: the space bar is a PLAY/STOP toggle again. Pressed while idle
    it starts playback where the playhead is; pressed while rolling it is a
    stop. Return is the other verb: it always rewinds, which is the one way
    left to get back to the top without touching the ruler. */
inline bool spaceStartsPlayback (bool engaged) noexcept { return ! engaged; }

//-- Musical time ---------------------------------------------------------------

// Phase 91: bar arithmetic moved to Model/Meter.h, together with the metre it
// needs. It could not stay here: these two functions were the only readers of a
// constant that is now a project value, and a Util header cannot see a Model
// one. "复制到后一小节" and the ruler both include Meter.h instead.

//-- Drag payloads --------------------------------------------------------------

/** Phase 52 / 53: "this drag is a card from the rack, and here is which one".

    The rack starts the drag and the roll accepts it, so the spelling lives in
    one place both can read: a string built in one file and parsed in another
    is exactly how the two ends drift apart. A file drag is a plain absolute
    path, which is why the card payload needs a prefix at all — without one,
    `C:\\loops\\kick.wav` and a slot id would arrive through the same channel
    and only the receiver could tell them apart, by guessing.

    TWO prefixes, not one id, because the same card means different things on
    the other end: a sampler hands over the FILE it holds (drop on empty grid =
    a new block, drop on a block = that block now plays this file), while an
    instrument hands over its MOUNT (drop on a note = that note plays on it).
    The receiver must know which question it is being asked, and asking the
    rack afterwards would be a second lookup that can fail wildly (the slot may
    have been removed mid-drag). */
namespace dragpayload
{
    inline juce::String describe (const char* prefix, int slotId)
    {
        return juce::String (prefix) + juce::String (slotId);
    }

    /** -1 when `text` does not start with `prefix` or carries no usable id. */
    inline int idOf (const juce::String& text, const char* prefix)
    {
        if (! text.startsWith (prefix))
            return -1;

        const int id = text.fromFirstOccurrenceOf (":", false, false).getIntValue();

        return id > 0 ? id : -1;
    }
}

constexpr const char* kSamplerDragPrefix    = "atomic-sampler:";
constexpr const char* kInstrumentDragPrefix = "atomic-instrument:";
constexpr const char* kGroupDragPrefix      = "atomic-group:";

inline juce::String samplerDragDescription (int slotId)
{
    return dragpayload::describe (kSamplerDragPrefix, slotId);
}

/** -1 when `text` is not a sampler drag at all (a file path, say). */
inline int samplerDragSlotId (const juce::String& text)
{
    return dragpayload::idOf (text, kSamplerDragPrefix);
}

/** Phase 53: a card carrying an instrument rather than a file. */
inline juce::String instrumentDragDescription (int slotId)
{
    return dragpayload::describe (kInstrumentDragPrefix, slotId);
}

inline int instrumentDragSlotId (const juce::String& text)
{
    return dragpayload::idOf (text, kInstrumentDragPrefix);
}

/** Phase 58: a row dragged out of the group list.

    TWO numbers, unlike a card, because groups live inside a layer and their
    ids are only unique there: "group 2" exists on every sheet. A payload that
    carried just the id would make the panel's row land on whichever layer
    happened to be active, which is the same class of bug the roll avoids by
    addressing groups as (layerIndex, groupId) everywhere else.

    It stays text (not a pointer, not a NoteGroup*) because the drop happens
    after the drag, and a group can be deleted mid-drag — the receiver has to
    look the target up again, and it should be able to fail. */
inline juce::String groupDragDescription (int layerIndex, int groupId)
{
    return juce::String (kGroupDragPrefix) + juce::String (layerIndex)
             + ":" + juce::String (groupId);
}

/** False when `text` is not a group drag at all (a path, a card). The
    out-parameters are left untouched on failure, so a caller cannot act on a
    half-parsed payload. */
inline bool groupDragTarget (const juce::String& text, int& layerIndex, int& groupId)
{
    if (! text.startsWith (kGroupDragPrefix))
        return false;

    // "atomic-group:0:3" -> rest "0:3"
    const auto rest      = text.fromFirstOccurrenceOf (":", false, false);
    const auto layerText = rest.upToFirstOccurrenceOf (":", false, false);
    const auto groupText = rest.fromFirstOccurrenceOf (":", false, false);

    // containsOnly is vacuously true for an empty string, so the emptiness is
    // checked separately — otherwise "atomic-group::" would parse as (0, 0).
    if (layerText.isEmpty() || groupText.isEmpty()
         || ! layerText.containsOnly ("0123456789")
         || ! groupText.containsOnly ("0123456789"))
        return false;

    const int parsedLayer = layerText.getIntValue();
    const int parsedGroup = groupText.getIntValue();

    if (parsedLayer < 0 || parsedGroup < 0)
        return false;

    layerIndex = parsedLayer;
    groupId    = parsedGroup;

    return true;
}

//-- Transport glyph geometry ---------------------------------------------------

/** The square a transport glyph is drawn into.

    Derived from the button, never from a fixed inset. Phase 13 grew the
    transport buttons to 36 px and the old `y + 12 … bottom - 12` play
    triangle collapsed into a 12 x 4 sliver, while the pause bars stayed
    anchored to the same fixed offset and sank to the bottom of the plate.
    Both were the same mistake: a glyph has to scale with its button and
    sit on its centre. */
inline juce::Rectangle<float> transportIconBox (juce::Rectangle<float> button, float fraction)
{
    const float side = button.getHeight() * fraction;

    return juce::Rectangle<float> (side, side).withCentre (button.getCentre());
}

/** Play and stop share one glyph size so the two read as a set. */
constexpr float kTransportGlyph = 0.46f;

/** The two glyphs, defined once so the self-test measures the same
    geometry the paint routine fills. */

inline juce::Path playGlyph (juce::Rectangle<float> button)
{
    const auto  icon  = transportIconBox (button, kTransportGlyph);
    const float width = icon.getWidth() * 0.88f;

    // Nudged right by a tenth of its own width: a right-pointing arrow
    // carries its mass on the left, so a literally centred one reads as
    // sitting off-centre inside the plate.
    const float left = icon.getCentreX() - width * 0.5f + width * 0.10f;

    juce::Path triangle;
    triangle.addTriangle (left, icon.getY(),
                          left, icon.getBottom(),
                          left + width, icon.getCentreY());
    return triangle;
}

inline juce::Rectangle<float> stopGlyph (juce::Rectangle<float> button)
{
    const auto  icon = transportIconBox (button, kTransportGlyph);
    const float side = icon.getWidth() * 0.86f;

    return juce::Rectangle<float> (side, side).withCentre (icon.getCentre());
}

//-- Menu id bands --------------------------------------------------------------

/** Phase 25: id base for the rack card's "发送到混音器通道" submenu. The
    submenu lists up to 127 strips, and 21 + channel would have walked right
    through the automation / delete ids of the menu that hosts it. */
constexpr int kSendToChannelBase = 1000;

/** Phase 42: inside a synth's 创建自动化 submenu, the parameters of its
    hosted instrument start here.

    10000, not 100: the rack card's own menu already spends 10..13, 20, 30..41,
    200 + n (the VST3 instrument list) and 1000 + channel (the sends). A
    plugin with 200 parameters would have walked straight through the VST3
    list at 100, and the click would have replaced the synth. */
constexpr int kSlotAutoParamBase = 10000;

/** Phase 43: 声相 / 静音 / 独奏 live in their OWN band, not at idBase + 3.

    `idBase + 3` was the obvious choice and it was wrong within one build: the
    rack card passes idBase = 30, and 33/34/35 are already 高亮 and its
    neighbours — so choosing "创建声相自动化" on a card would have hit the
    spotlight toggle instead. Every band gets counted before it is used; this
    one is counted here. */
constexpr int kSlotAutoTargetBase = 20000;

} // namespace atomic
