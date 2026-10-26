#pragma once

#include <juce_core/juce_core.h>

#include <array>

#include "Util/Text.h"

namespace atomic::typing
{

//==============================================================================
/**
    Phase 39: the typing keyboard as a piano, mapped the way FL Studio maps it.

    Two octaves, four rows, and the black keys sit exactly where the black keys
    sit on a keyboard you can see — that is the whole trick, and it is why the
    letters look shuffled. FL Studio's own documentation describes it as "white
    notes of the lower octave (C4-B4) span from Z to M, with the black notes
    ranging from S to J; the white notes of the upper octave (C5-B5) span from Q
    to U, with the black notes ranging from 2 to 7". This table is that sentence
    written as data, and it is the ONLY place the mapping exists: the key
    handler, the settings diagram and the self-test all read it from here.

    The layout is deliberately not "one octave on one row": E-F and B-C have no
    black key between them, so the letter gaps in the black rows ARE the piano
    pattern. Transposing this table by hand into a switch statement is how the
    mapping and the picture end up disagreeing.
*/
enum class Row
{
    lowerWhite,
    lowerBlack,
    upperWhite,
    upperBlack
};

struct KeyBinding
{
    /** The keycap character, lower case. JUCE reports the text character of a
        letter key in lower case unless Shift is held; comparisons fold case. */
    juce::juce_wchar key      = 0;

    /** Semitones above the layout's base note. */
    int              semitone = 0;

    Row              row      = Row::lowerWhite;
};

/** Z is middle C. Two octaves of 12 keys each. */
inline constexpr int numKeys    = 24;
inline constexpr int lowestNote = 60;

inline const std::array<KeyBinding, numKeys>& bindings()
{
    static const std::array<KeyBinding, numKeys> table { {
        //-- Lower octave: white keys on the Z row, black keys in the gaps of
        //   the row above. Nothing between E and F, nothing between B and C.
        { 'z',  0, Row::lowerWhite },
        { 's',  1, Row::lowerBlack },
        { 'x',  2, Row::lowerWhite },
        { 'd',  3, Row::lowerBlack },
        { 'c',  4, Row::lowerWhite },
        { 'v',  5, Row::lowerWhite },
        { 'g',  6, Row::lowerBlack },
        { 'b',  7, Row::lowerWhite },
        { 'h',  8, Row::lowerBlack },
        { 'n',  9, Row::lowerWhite },
        { 'j', 10, Row::lowerBlack },
        { 'm', 11, Row::lowerWhite },

        //-- Upper octave: the Q row, with the number row filling the blacks.
        { 'q', 12, Row::upperWhite },
        { '2', 13, Row::upperBlack },
        { 'w', 14, Row::upperWhite },
        { '3', 15, Row::upperBlack },
        { 'e', 16, Row::upperWhite },
        { 'r', 17, Row::upperWhite },
        { '5', 18, Row::upperBlack },
        { 't', 19, Row::upperWhite },
        { '6', 20, Row::upperBlack },
        { 'y', 21, Row::upperWhite },
        { '7', 22, Row::upperBlack },
        { 'u', 23, Row::upperWhite }
    } };

    return table;
}

/** Semitones above the base note, or -1 when the character is not a piano key.

    `octaveShift` is in octaves and is applied by the caller, not here: the
    table describes FL Studio's layout, and shifting the layout is a separate
    decision (see MainComponent's octave keys). */
inline int semitoneForCharacter (juce::juce_wchar character)
{
    if (character == 0)
        return -1;

    const auto folded = juce::CharacterFunctions::toLowerCase (character);

    for (const auto& binding : bindings())
        if (binding.key == folded)
            return binding.semitone;

    return -1;
}

/** The same lookup for a JUCE key code. Letters arrive with keyCode == their
    ASCII upper-case character, so folding is what makes 'z' and 'Z' agree. */
inline int semitoneForKeyCode (int keyCode)
{
    if (keyCode <= 0 || keyCode > 0x10ffff)
        return -1;

    return semitoneForCharacter ((juce::juce_wchar) keyCode);
}

/** The MIDI note a key plays, or -1. Clamped to the MIDI range so a wild octave
    shift cannot produce a note the engine would have to reject. */
inline int noteForKeyCode (int keyCode, int octaveShift = 0)
{
    const int semitone = semitoneForKeyCode (keyCode);

    if (semitone < 0)
        return -1;

    const int note = lowestNote + semitone + 12 * octaveShift;
    return juce::isPositiveAndBelow (note, 128) ? note : -1;
}

/** The keycaps of one row, in playing order, with the skips a piano has.
    Built from the table so the diagram cannot drift from the mapping. */
inline juce::String rowKeys (Row row)
{
    juce::String result;

    for (const auto& binding : bindings())
    {
        if (binding.row != row)
            continue;

        if (result.isNotEmpty())
            result += " ";

        result += juce::String::charToString (binding.key).toUpperCase();
    }

    return result;
}

/** The notes of one row, in the same order, e.g. "C4 D4 E4 F4 G4 A4 B4". */
inline juce::String rowNotes (Row row)
{
    juce::String result;

    for (const auto& binding : bindings())
    {
        if (binding.row != row)
            continue;

        if (result.isNotEmpty())
            result += " ";

        result += atomic::noteName (lowestNote + binding.semitone);
    }

    return result;
}

} // namespace atomic::typing
