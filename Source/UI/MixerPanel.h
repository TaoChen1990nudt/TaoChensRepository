#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Util/Metrics.h"

#include <functional>

#include "Model/HardwareRack.h"
#include "Model/LayerModel.h"
#include "Model/MixerModel.h"

namespace atomic
{

//==============================================================================
/**
    The bottom mixer: eight channel strips, the master strip, and the insert
    rack for whichever strip is selected.

    Phase 11: a channel is fed by a SYNTH, not by a layer. A sheet of notes picks
    an instrument (that is the rack's job); the instrument is what a console
    channel receives, and it only receives it when someone routes it there. A
    fresh project routes nothing, so the console reads empty until the user sends
    an instrument to a channel — which is the only moment a channel strip has
    anything to say.

    So the strip fader IS the routed synth's fader. This component writes through
    RackModel::setLevelDb; it does not keep a second copy of the value. Two views
    of one number is a mixer; two numbers is a bug waiting for the first "why did
    the fader jump" report.

    The insert rack is per channel and holds eight slots; that state lives in
    MixerModel because synths do not have insert chains.

    Phase 23 changed the shape of the console, not just its contents:

      * the MASTER is display slot 0 — the signal ends where it visibly ends,
        and "删除通道条" can no longer be mistaken for "delete the output";
      * a strip is a FIXED width (see stripWidth), so adding a channel adds a
        strip instead of squeezing the eight that were already there. When the
        row does not fit, a horizontal scrollbar appears under the strips and
        the row scrolls;
      * each channel carries its own output selector, so 1+2 → 3 → master is
        something you can do with the mouse. The master has none: it feeds the
        audio device, which is not a strip.
*/
class MixerPanel final : public juce::Component,
                         public LayerStack::Listener,
                         public RackModel::Listener,
                         public MixerModel::Listener
{
public:
    static constexpr int headerHeight = ui::px (24);
    static constexpr int rackWidth    = ui::px (336);

    /** Phase 23: a strip is a FIXED width. Phase 22 and earlier divided the
        available width by the strip count, so every added channel made all the
        others narrower and the faders never sat still. 92 design px is what nine
        strips (1 master + 8 channels) get in the default window — 1080 px of
        console in a 1097 px viewport, so the shipped layout needs no scrollbar
        and the scrollbar only ever appears for a window the user shrank. */
    static constexpr int stripWidth   = ui::px (92);

    /** Shown under the strips only when they do not all fit. */
    static constexpr int scrollBarHeight = ui::px (9);

    /** Phase 18: 1..8 channels plus the master. The ceiling is the engine's bus
        count; a project can grow and shrink inside it. */
    int getStripCount() const noexcept { return mixer.getChannelCount() + 1; }

    /** Phase 25: true when the mono button sits on pixels nothing else claims.
        The self-test asks the panel instead of comparing rectangles from the
        outside — those boxes are private precisely because their layout is. */
    bool monoButtonHasOwnPixels() const;

    /** Phase 26: the two meter bars are separate, side by side, and inside the
        strip area the fader leaves for them. Asked from the outside like the
        mono check above, for the same reason. */
    bool stereoMeterHasOwnPixels() const;

    /** Phase 26: the rects the check above looks at, spelled out. A failing
        geometry check that only says "overlapping" is unfalsifiable; this is
        what the self-test prints when it fails. */
    juce::String describeMeterGeometry() const;

    /** Phase 27: the fader and the meter are cut from one row, so "the fader
        gave up half its width and the meter took it" is ONE assertion, not two
        that can half-pass. Asked from the outside, like the two above. */
    bool faderYieldedHalfTheRow() const;

    /** Phase 70: the fader's hit area must stop before the meter's left bar.
        It used to reach a flat 8 px across a 4 px gap, so the last 4 px of
        "fader" were the meter — clicking there jumped the fader to that level. */
    bool faderHitClearsMeter (int channel) const;
    juce::String describeControlGeometry() const;

    /** Phase 43: which of the four controls a menu is about. Declared up here
        because `fillControlMenu` needs it and because it is part of the panel's
        public vocabulary — the shell answers with one of these four. */
    enum class StripControl { fader, pan, mute, solo };

    /** Phase 43: builds the menu one control answers a right-click with.

        Public, and split out of showControlMenu, so the self-test can READ the
        menu instead of photographing it: "the fader's menu no longer offers
        删除通道条" is a claim about a list, and a popup cannot be opened without a
        desktop. Same reasoning as the geometry probes above. */
    void fillControlMenu (juce::PopupMenu& menu, int channel, StripControl control);

    void showControlMenu (int channel, StripControl control, juce::Point<int> screenPos);

    /** Phase 23: the master sits at display slot 0, channels follow. Strip
        bounds, hit tests and painting all go through this pair, so "where a
        strip is drawn" and "which channel it is" cannot drift apart. */
    int displaySlotFor (int channel) const noexcept
    {
        return channel == MixerModel::masterChannel ? 0 : channel + 1;
    }

    int channelForDisplaySlot (int slot) const noexcept
    {
        return slot <= 0 ? MixerModel::masterChannel : slot - 1;
    }

    /** How far the strip row is scrolled, and the furthest it may go. The
        self-test reads both to prove the scrollbar appears and moves. */
    int getStripScrollX() const noexcept { return stripScrollX; }
    int getMaxStripScroll() const;
    bool isStripScrollNeeded() const;
    int  getTotalStripWidth() const;

    MixerPanel (LayerStack& layerStack, RackModel& rackModel, MixerModel& mixerModel);
    ~MixerPanel() override;

    /** 0..1 peak per SIDE for console channels and for the master strip
        (`MixerModel::masterChannel`). Phase 26: the panel draws a two-bar meter,
        so it asks for the two sides separately — a single "loudest" number
        cannot show that a hard-panned strip is silent on one side. */
    std::function<float (int channel, int side)> levelSource;

    /** The host owns the plugin library, so it owns the VST3 submenu. */
    std::function<void (int channel, int slot, juce::Point<int> screenPos)> onInsertPluginRequested;

    /** "插件管理器…" in the same menu. */
    std::function<void()> onOpenPluginManager;

    /** Phase 12: right-click on a strip asks the host for this channel's menu:
        automation, and now "reset / delete this strip". The strip cannot build
        it itself — automation belongs to the session model, not to this view.

        Phase 43 narrowed this: the four CONTROLS (fader, pan, mute, solo) no
        longer open it. A right-click on a fader used to offer "delete this
        strip", which is a menu about the strip answering a question about a
        number. Those four now open their own menu and nothing else; the strip's
        own menu is still what the rest of the strip answers with. */
    std::function<void (int channel, juce::Point<int> screenPos)> onChannelMenuRequested;

    /** Phase 43: the single "创建…自动化" item that belongs to ONE control.

        The panel knows which control was clicked and nothing else; the host owns
        the curves, so it is the host that knows a fader means the synth's level
        lane and a master fader means the master's. Adding the item straight into
        the menu (not a submenu) is the point: one control, one verb, one line. */
    std::function<void (juce::PopupMenu&, int channel, StripControl)> buildStripAutomationItems;
    std::function<void (int channel, StripControl, int result)> onStripAutomationChosen;

    /** The id the panel keeps for itself in those menus: 复位 on a fader, 静音 /
        取消静音 on a switch. The host's automation items live above it. */
    static constexpr int stripMenuActionId = 1;

    /** Phase 18: wipes a strip back to factory — inserts cleared, fader at
        unity, pan centred, mute and solo off. Routing is NOT touched: "reset
        this strip" is not "unplug the instrument". */
    void resetStrip (int channel);

    /** Phase 26: removes THE STRIP THE USER POINTED AT, closing the gap. Every
        ordinary strip has a "－", so the console is no longer a stack you can
        only shorten from the top — deleting channel 3 out of 12 is a normal
        edit, and the channels above it slide down one number.

        Two things happen in one order, and the order is the point: first the
        synths feeding this strip are unplugged (`mixerChannel = -1`), then the
        model renumbers. Unplugging afterwards would leave a synth pointing at
        the strip that just slid into that number — a send the user never made.

        Refused at `MixerModel::deletionFloor`: the shipped eight strips are the
        console, not a starting point for dismantling it. */
    bool deleteStrip (int channel);

    /** Phase 18/26: the header "－" removes the LAST strip. Same rules. */
    void deleteLastStrip();

    /** Phase 12: click an insert to open its editor (the plugin's own window for
        a VST3, the built-in one for the native effects). */
    std::function<void (int channel, int slot)> onInsertOpenRequested;

    void updateMeters();

    /** Inserts the rack edits. 0..7 = channel, 8 = master. */
    void setSelectedChannel (int channel);
    int  getSelectedChannel() const noexcept { return selectedChannel; }

    /** Phase 23: scrolls the strip row. Clamped to [0, getMaxStripScroll()]. */
    void setStripScrollX (int pixels);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    /** The synth id feeding a channel, or -1 when nothing is routed here. The
        self-test reads this to prove the strip follows the rack, not the layer
        stack. */
    int slotIdOnChannel (int channel) const;

    /** How many synths feed a channel (0, 1, or more when the user deliberately
        summed two instruments into one bus). */
    int sourceCountOnChannel (int channel) const;

    //-- Listeners -------------------------------------------------------------
    void layersChanged() override { repaint(); }
    void rackChanged() override   { repaint(); }
    /** Phase 70: repaint, and bring the selection down with the console.

        Clamping inside `setSelectedChannel()` only helps callers that go
        through it; loading a smaller project changes the count without going
        near the selection, and everything that reads `selectedChannel` — the
        rack header, the insert editor — would then be pointed at a strip that
        no longer exists. */
    void mixerChanged() override;

private:
    enum class Part { none, strip, fader, pan, mute, solo, mono, output, stripClose,
                      insertType, insertParam, insertPower, sendTarget };

    juce::Rectangle<int> getHeaderArea() const;

    /** "＋ / －" in the header: the console grows and shrinks from there. */
    juce::Rectangle<int> getAddChannelBounds() const;
    juce::Rectangle<int> getRemoveChannelBounds() const;
    juce::Rectangle<int> getStripsArea() const;

    /** The part of the strips area the strips are actually painted into: the
        same box minus the scrollbar's height when one is needed. */
    juce::Rectangle<int> getStripsViewport() const;
    juce::Rectangle<int> getScrollBarBounds() const;
    juce::Rectangle<int> getScrollThumbBounds() const;

    juce::Rectangle<int> getRackArea() const;

    juce::Rectangle<int> getStripBounds (int channel) const;

    /** Phase 27: the band a strip gives to its fader and its meter. Both are
        cut from this one rectangle, so "the fader is half as wide" and "the
        meter is four times as wide" are one decision, not two that can drift. */
    juce::Rectangle<int> getFaderRowBounds (int channel) const;
    static int faderWidthFor (int rowWidth);

    juce::Rectangle<int> getFaderBounds (int channel) const;

    /** Phase 70: the fader's HIT area, which is wider than its paint area.

        The horizontal reach used to be a flat 8 px, but the gap between the
        fader and the meter is 4 — so the last 4 px of the "fader" was the
        meter's left bar. Clicking there jumped the fader to that level instead
        of doing nothing, and the cursor promised a drag the user had not aimed
        at. Half the gap is the widest a hit area can grow without taking a
        neighbour's pixels. */
    juce::Rectangle<int> getFaderHitBounds (int channel) const;
    juce::Rectangle<int> getPanBounds (int channel) const;
    juce::Rectangle<int> getOutputBounds (int channel) const;

    /** Phase 99: one row per send, stacked under the output row.

        Both are always drawn, even when the strip has nowhere to send: a
        console where the rows appear and disappear as you patch it is a console
        whose strip height changes under the mouse. An unused row reads as "—".
    */
    static constexpr int numSendRows = MixerModel::numSends;
    juce::Rectangle<int> getSendRowBounds (int channel, int index) const;
    juce::Rectangle<int> getMuteBounds (int channel) const;
    juce::Rectangle<int> getSoloBounds (int channel) const;

    /** Phase 25: mono monitoring, on the dB readout row. It lives there because
        that row had slack and the strip has no vertical space to spare — adding
        a row would have shortened every fader for one button.

        Phase 26: the master strip has one too. Same control, same row, same
        meaning — it is where the master's signal is being listened to, not what
        the mix is. */
    juce::Rectangle<int> getMonoBounds (int channel) const;

    /** Phase 26: the pair of bars, then one bar. `side` 0 = left, 1 = right. */
    juce::Rectangle<int> getMeterBounds (int channel) const;
    juce::Rectangle<int> getMeterBarBounds (int channel, int side) const;

    /** Phase 26: the "－" at the right end of the name row. Drawn on every
        ordinary strip; dead (and dimmed) once the console is at the floor, so
        the button that cannot act still tells the user why. */
    juce::Rectangle<int> getStripCloseBounds (int channel) const;

    juce::Rectangle<int> getInsertRowBounds (int slot) const;
    juce::Rectangle<int> getInsertTypeBounds (int slot) const;
    juce::Rectangle<int> getInsertParamBounds (int slot) const;
    juce::Rectangle<int> getInsertPowerBounds (int slot) const;

    int   channelAt (juce::Point<int> position) const;
    float dbFromY (int channel, float y) const;
    float panFromX (int channel, float x) const;

    bool isMaster (int channel) const noexcept { return channel == MixerModel::masterChannel; }
    const SynthSlot* slotOnChannel (int channel) const;
    float getChannelLevelDb (int channel) const;
    float getChannelPan (int channel) const;
    bool  isChannelMuted (int channel) const;
    bool  isChannelSoloed (int channel) const;
    juce::String getChannelName (int channel) const;
    juce::Colour getChannelColour (int channel) const;

    void setChannelLevelDb (int channel, float db);
    void setChannelPan (int channel, float pan);
    void toggleMute (int channel);
    void toggleSolo (int channel);

    /** Phase 25: flips the strip's mono monitoring. The state lives in the
        mixer model because the engine publishes it, not in the panel. */
    void toggleMono (int channel);
    bool isChannelMono (int channel) const;

    void showInsertMenu (int slot, juce::Point<int> screenAnchor);

    /** Phase 23: "this strip's output goes to…" — a plain menu built from the
        model, because the answer is a property of the console and nothing else
        in the session has an opinion about it. */
    void showOutputMenu (int channel, juce::Point<int> screenPos);

    /** The strip's destination as it should read on the button. */
    juce::String getOutputName (int channel) const;

    /** Phase 99: "this strip also feeds…" — destination, level and tap point in
        one menu, because a send is one decision with three parts and splitting
        it across three gestures is how a console ends up with sends nobody can
        find. */
    void showSendMenu (int channel, int index, juce::Point<int> screenPos);

    /** "→ 3  -6.0" as it reads on the row, or "—" for a send that is off. */
    juce::String getSendText (int channel, int index) const;
    bool isSendRowLive (int channel, int index) const;

    void drawStrip (juce::Graphics&, int channel);
    void drawRack (juce::Graphics&);

    LayerStack& stack;
    RackModel&  rack;
    MixerModel& mixer;

    int  selectedChannel = 0;
    Part dragPart = Part::none;
    int  dragChannel = -1;
    int  dragSlot = -1;

    /** Phase 43: an insert row that has been pressed but not yet decided.

        A press on the slot's label is now two gestures: drag it up or down to
        move it in the chain, or release without moving to get its menu (which is
        what a click used to do, half of it). Recording the press and waiting for
        either is the only way to tell them apart. */
    int  pressedInsertSlot = -1;
    bool insertDragMoved   = false;

    /** Phase 23: strip-row scrolling. Pixels, like every other scroll in this
        project — the scrollbar is drawn from these two numbers and nothing
        else caches them. */
    int  stripScrollX    = 0;
    bool draggingScroll  = false;
    bool hoveredScroll   = false;
    int  dragScrollGrab  = 0;      // where inside the thumb the drag started

    Part hoveredPart = Part::none;
    int  hoveredChannel = -1;
    int  hoveredSlot = -1;

    /** Phase 99: which send row the mouse is over (0..numSendRows-1, -1 = none).
        `hoveredPart` says "a send row"; this says which one, the same way
        `hoveredSlot` does for the rack. */
    int  hoveredSend = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerPanel)
};

} // namespace atomic
