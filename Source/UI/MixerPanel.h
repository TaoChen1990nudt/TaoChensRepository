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
*/
class MixerPanel final : public juce::Component,
                         public LayerStack::Listener,
                         public RackModel::Listener,
                         public MixerModel::Listener
{
public:
    static constexpr int headerHeight = ui::px (24);
    static constexpr int rackWidth    = ui::px (336);

    /** Phase 18: 1..8 channels plus the master. The ceiling is the engine's bus
        count; a project can grow and shrink inside it. */
    int getStripCount() const noexcept { return mixer.getChannelCount() + 1; }

    MixerPanel (LayerStack& layerStack, RackModel& rackModel, MixerModel& mixerModel);
    ~MixerPanel() override;

    /** 0..1 peak for channel 0..7 (console channels) and 8 (master). */
    std::function<float (int channel)> levelSource;

    /** The host owns the plugin library, so it owns the VST3 submenu. */
    std::function<void (int channel, int slot, juce::Point<int> screenPos)> onInsertPluginRequested;

    /** "插件管理器…" in the same menu. */
    std::function<void()> onOpenPluginManager;

    /** Phase 12: right-click on a strip asks the host for this channel's menu:
        automation, and now "reset / delete this strip". The strip cannot build
        it itself — automation belongs to the session model, not to this view. */
    std::function<void (int channel, juce::Point<int> screenPos)> onChannelMenuRequested;

    /** Phase 18: wipes a strip back to factory — inserts cleared, fader at
        unity, pan centred, mute and solo off. Routing is NOT touched: "reset
        this strip" is not "unplug the instrument". */
    void resetStrip (int channel);

    /** Phase 18: removes the LAST strip from the console (shrinking the count)
        after unplugging whatever was still routed into it. */
    void deleteLastStrip();

    /** Phase 12: click an insert to open its editor (the plugin's own window for
        a VST3, the built-in one for the native effects). */
    std::function<void (int channel, int slot)> onInsertOpenRequested;

    void updateMeters();

    /** Inserts the rack edits. 0..7 = channel, 8 = master. */
    void setSelectedChannel (int channel);
    int  getSelectedChannel() const noexcept { return selectedChannel; }

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

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
    void mixerChanged() override  { repaint(); }

private:
    enum class Part { none, strip, fader, pan, mute, solo,
                      insertType, insertParam, insertPower };

    juce::Rectangle<int> getHeaderArea() const;

    /** "＋ / －" in the header: the console grows and shrinks from there. */
    juce::Rectangle<int> getAddChannelBounds() const;
    juce::Rectangle<int> getRemoveChannelBounds() const;
    juce::Rectangle<int> getStripsArea() const;
    juce::Rectangle<int> getRackArea() const;

    juce::Rectangle<int> getStripBounds (int channel) const;
    juce::Rectangle<int> getFaderBounds (int channel) const;
    juce::Rectangle<int> getPanBounds (int channel) const;
    juce::Rectangle<int> getMuteBounds (int channel) const;
    juce::Rectangle<int> getSoloBounds (int channel) const;
    juce::Rectangle<int> getMeterBounds (int channel) const;

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

    void showInsertMenu (int slot, juce::Point<int> screenAnchor);

    void drawStrip (juce::Graphics&, int channel);
    void drawRack (juce::Graphics&);

    LayerStack& stack;
    RackModel&  rack;
    MixerModel& mixer;

    int  selectedChannel = 0;
    Part dragPart = Part::none;
    int  dragChannel = -1;
    int  dragSlot = -1;

    Part hoveredPart = Part::none;
    int  hoveredChannel = -1;
    int  hoveredSlot = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerPanel)
};

} // namespace atomic
