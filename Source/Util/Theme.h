#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
/** The interface palette, in one place.

    Before this file every panel declared its own set of `kAccent` / `kDivider` /
    `kText` constants in an anonymous namespace. Seven files each spelled
    `kAccent` and all seven happened to write `#4fd1c5` — which is luck, not a
    system: nothing would have caught it if one of them had drifted. The scan
    that produced this file found 93 colour constants across `Source/`, and 40
    distinct near-black values in use at once, several of them visually
    identical (`#0d0e11` / `#0d0e12` / `#0d0e13`). Seven different blacks read
    as one black, which is why the panels had no sense of depth.

    **Step 1 of three (this file).** Every value here is the value the code
    already used, so wiring panels to these tokens cannot change a single
    pixel. The point is to give each *role* a name, not to restyle anything.
    Step 2 collapses the near-blacks onto this ladder; step 3 desaturates the
    identity colours and tightens the radii. See `DESIGN.md` at the repo root.

    Deliberately NOT here: `palette::layer1..5` and `groupPalette::group1..5`.
    Those live in `Model/LayerModel.h` because the model owns them — they are
    serialised into the project file, so moving them would put UI in the model
    layer and colour in the wrong file. The interface reads them from there.

    Naming: `atomic::palette` is taken. Hence `theme`.
*/
namespace atomic::theme
{

//-- Surfaces -------------------------------------------------------------------
/** Six steps of depth. A panel that sits further forward is a brighter surface,
    never a thicker border.

    `bgApp` is the seam between panels and the window behind them;
    `surface0` is a work canvas (the roll and the automation floor);
    `surface1` is a regular panel card; `surface2` a band inside one (lane
    headers, strips); `surface3` anything you can click (field, combo, card);
    `surface4` its hover/pressed/active state. */
inline const juce::Colour bgApp    { 0xff0d0e11 };   // step 2 target: #0a0b0e
inline const juce::Colour topBar   { 0xff16171c };
inline const juce::Colour surface0 { 0xff101114 };
inline const juce::Colour surface1 { 0xff14151a };
inline const juce::Colour surface2 { 0xff171922 };
inline const juce::Colour surface3 { 0xff1b1d24 };
inline const juce::Colour surface4 { 0xff2c2e37 };

//-- Ink ------------------------------------------------------------------------
/** Four weights of text, brightest first. Hierarchy comes from weight and
    letter-spacing as much as from these values — an 11 pt bold with +0.5
    tracking already reads as a heading against 10 pt regular body, without
    having to widen the type. */
inline const juce::Colour ink        { 0xffeef1f6 };   // primary values, timecode
inline const juce::Colour inkMuted   { 0xff9aa0ad };   // body, labels, lane names
inline const juce::Colour inkSubtle  { 0xff8b91a0 };   // secondary notes, disabled
inline const juce::Colour inkFaint   { 0xff7d8492 };   // faintest: axis marks, units

//-- Hairlines ------------------------------------------------------------------
/** One-pixel separators. `strong` is for a boundary that has to hold against
    content on both sides — a splitter, a floating panel's outer frame. */
inline const juce::Colour hairline       { 0xff262832 };   // was kDivider (5 files)
inline const juce::Colour hairlineStrong { 0xff343743 };   // was kPanelBorder

//-- Semantics ------------------------------------------------------------------
/** The only colours allowed to mean something.

    Accent is the interface's own voice: selection, focus, automation curves,
    scroll thumbs. It should stay a thin line — a 1 px edge, a 1.5 px curve —
    never a large fill, because a filled teal block competes with `palette::layer1`
    which is the same value.

    Record is red and means record. It currently also draws the playhead; that
    stays for now, but the intended split is red = recording, accent = playing. */
inline const juce::Colour accent { 0xff4fd1c5 };   // was kAccent (7 files)
inline const juce::Colour record { 0xffff5f6d };   // was kPlayhead / kPlayheadColour
inline const juce::Colour live   { 0xff5ecf9a };   // running / success
inline const juce::Colour warn   { 0xffe0b13c };   // warning / bypass / solo
inline const juce::Colour link   { 0xff7c96f5 };   // information, links

//-- Shadows --------------------------------------------------------------------
/** Only floating panels get a shadow. Static panels are tiled edge to edge, so
    any shadow they cast lands on their neighbour and reads as a dirty border. */
inline juce::Colour shadowForOverlay() { return juce::Colours::black.withAlpha (0.55f); }

} // namespace atomic::theme
