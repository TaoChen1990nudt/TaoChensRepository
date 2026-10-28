#include "MixerPanel.h"

#include "Util/Metrics.h"

#include "UI/MixerLook.h"
#include "Util/Text.h"
#include "Util/Theme.h"
#include "Util/ValueMenu.h"

namespace atomic
{

namespace
{
    const juce::Colour& kBackground = theme::surface0;
    const juce::Colour kPanel      = theme::surface2;
    const juce::Colour kStrip      = theme::surface2;
    const juce::Colour& kOutline   = theme::surface4;
    const juce::Colour& kText      = theme::inkMuted;
    const juce::Colour& kAccent    = theme::accent;
    const juce::Colour kMute       = theme::warn;      // 静音统一黄色（P48，11.1 bug）
    const juce::Colour& kSolo      = theme::accent;   // P48 11.2：独奏统一 accent（原绿，绿留给运行态）
    const juce::Colour kMaster     = theme::ink;

    juce::String dbText (float db)
    {
        if (db <= -47.5f)
            return utf8 ("-∞");

        return (db > 0.0f ? "+" : "") + juce::String (db, 1);
    }
}

//==============================================================================
MixerPanel::MixerPanel (LayerStack& layerStack, RackModel& rackModel, MixerModel& mixerModel)
    : stack (layerStack), rack (rackModel), mixer (mixerModel)
{
    stack.addListener (this);
    rack.addListener (this);
    mixer.addListener (this);
}

MixerPanel::~MixerPanel()
{
    mixer.removeListener (this);
    rack.removeListener (this);
    stack.removeListener (this);
}

//==============================================================================
int MixerPanel::slotIdOnChannel (int channel) const
{
    if (isMaster (channel))
        return -1;

    for (int i = 0; i < rack.getNumSlots(); ++i)
        if (const auto* slot = rack.getSlot (i); slot != nullptr && slot->mixerChannel == channel)
            return slot->id;

    return -1;
}

int MixerPanel::sourceCountOnChannel (int channel) const
{
    int count = 0;

    for (int i = 0; i < rack.getNumSlots(); ++i)
        if (const auto* slot = rack.getSlot (i); slot != nullptr && slot->mixerChannel == channel)
            ++count;

    return count;
}

//==============================================================================
juce::Rectangle<int> MixerPanel::getHeaderArea() const
{
    return getLocalBounds().withHeight (headerHeight);
}

juce::Rectangle<int> MixerPanel::getRemoveChannelBounds() const
{
    auto area = getStripsArea();

    return { area.getRight() - ui::px (46), ui::px (3), ui::px (20), headerHeight - ui::px (7) };
}

juce::Rectangle<int> MixerPanel::getAddChannelBounds() const
{
    auto area = getStripsArea();

    return { area.getRight() - ui::px (24), ui::px (3), ui::px (20), headerHeight - ui::px (7) };
}

void MixerPanel::resetStrip (int channel)
{
    if (channel < 0 || channel > MixerModel::masterChannel)
        return;

    mixer.clearChannelInserts (channel);

    if (isMaster (channel))
    {
        mixer.setMasterLevelDb (0.0f);
        mixer.setMasterPan (0.0f);
        mixer.setMasterMuted (false);
        mixer.setMasterMono (false);
    }
    else
    {
        // The fader IS the synth's, so resetting a strip means resetting the
        // instruments feeding it. Routing survives: "reset" is not "unplug".
        for (int i = 0; i < rack.getNumSlots(); ++i)
        {
            auto* slot = rack.getSlot (i);

            if (slot == nullptr || slot->mixerChannel != channel)
                continue;

            rack.setLevelDb (slot->id, 0.0f);
            rack.setPan (slot->id, 0.0f);
            rack.setMuted (slot->id, false);
            rack.setSoloed (slot->id, false);
        }

        // Phase 26: mono belongs to the strip, so "reset this strip" turns it
        // off here — otherwise a reset strip would still be folding its signal
        // and nothing on the panel would explain why.
        mixer.setChannelMono (channel, false);
    }

    mixer.notify();
    repaint();
}

bool MixerPanel::deleteStrip (int channel)
{
    const int count = mixer.getChannelCount();

    if (! mixer.canRemoveChannel() || ! juce::isPositiveAndBelow (channel, count))
        return false;

    // 1. Unplug FIRST, renumber second. Every synth whose send points at this
    //    strip becomes "not sending" (`mixerChannel = -1`): its destination is
    //    about to stop existing, and leaving the number behind would silently
    //    re-point the send at whatever slides into that slot.
    //
    //    The sends ABOVE the hole move down with the strips they feed. That is
    //    the whole cost of deleting a middle strip, and it is not optional: once
    //    the console renumbers, "channel 6" means a different hardware strip, so
    //    a send left at 6 would quietly land somewhere the user never chose.
    //    `removeChannelAt` cannot do this — the rack is not its business — which
    //    is exactly why this loop lives here and not in the model.
    for (int i = 0; i < rack.getNumSlots(); ++i)
    {
        auto* slot = rack.getSlot (i);

        if (slot == nullptr)
            continue;

        if (slot->mixerChannel == channel)
            rack.setMixerChannel (slot->id, -1);
        else if (slot->mixerChannel > channel)
            rack.setMixerChannel (slot->id, slot->mixerChannel - 1);
    }

    // 2. The strip itself: inserts, mono flag, routes above it, and the count.
    //    All of that is the model's job — `removeChannelAt` is the only place
    //    that knows a hole has to be closed rather than truncated.
    mixer.clearChannelInserts (channel);

    if (! mixer.removeChannelAt (channel))
        return false;

    // 3. Selection follows the row, not the number: if the strip that vanished
    //    was the selected one, stay on the same place in the console. The master
    //    is display slot 0 and is not part of this count, so it always survives.
    if (selectedChannel != MixerModel::masterChannel)
    {
        // Phase 70: "follows the row" is not a clamp. Removing a strip ABOVE the
        // selection slides every strip below it up one place, so the selection
        // has to move up with them — clamping alone left it on the neighbour the
        // user was not looking at, and the rack and insert editor then edited a
        // channel nobody had chosen. Removing the selected strip itself keeps
        // the index: the one below takes its place, which IS the same row.
        const int follow = channel < selectedChannel ? selectedChannel - 1 : selectedChannel;

        setSelectedChannel (juce::jlimit (0, mixer.getChannelCount() - 1, follow));
    }

    setStripScrollX (stripScrollX);
    repaint();
    return true;
}

void MixerPanel::deleteLastStrip()
{
    deleteStrip (mixer.getChannelCount() - 1);
}

juce::Rectangle<int> MixerPanel::getStripsArea() const
{
    return getLocalBounds().withTrimmedTop (headerHeight)
                           .withTrimmedRight (rackWidth);
}

int MixerPanel::getTotalStripWidth() const
{
    return stripWidth * getStripCount();
}

bool MixerPanel::isStripScrollNeeded() const
{
    return getTotalStripWidth() > getStripsArea().getWidth();
}

int MixerPanel::getMaxStripScroll() const
{
    return juce::jmax (0, getTotalStripWidth() - getStripsArea().getWidth());
}

juce::Rectangle<int> MixerPanel::getStripsViewport() const
{
    auto area = getStripsArea();

    // The scrollbar lives UNDER the strips, so it takes height from them and
    // from nothing else: the effect rack on the right keeps its full height.
    if (isStripScrollNeeded())
        area = area.withTrimmedBottom (scrollBarHeight);

    return area;
}

juce::Rectangle<int> MixerPanel::getScrollBarBounds() const
{
    if (! isStripScrollNeeded())
        return {};

    auto area = getStripsArea();

    return area.removeFromBottom (scrollBarHeight).reduced (ui::px (6), ui::px (2));
}

juce::Rectangle<int> MixerPanel::getScrollThumbBounds() const
{
    const auto track = getScrollBarBounds();

    if (track.isEmpty())
        return {};

    const int total = getTotalStripWidth();
    const int view  = getStripsArea().getWidth();
    const int max   = getMaxStripScroll();

    if (max <= 0 || total <= 0)
        return track;

    const int width = juce::jlimit (ui::px (30), track.getWidth(),
                                    juce::roundToInt ((float) track.getWidth() * (float) view / (float) total));
    const int travel = juce::jmax (0, track.getWidth() - width);
    const int x      = juce::roundToInt ((float) travel * (float) stripScrollX / (float) max);

    return { track.getX() + x, track.getY(), width, track.getHeight() };
}

void MixerPanel::setStripScrollX (int pixels)
{
    const int clamped = juce::jlimit (0, getMaxStripScroll(), pixels);

    if (clamped == stripScrollX)
        return;

    stripScrollX = clamped;
    repaint();
}

juce::Rectangle<int> MixerPanel::getRackArea() const
{
    return getLocalBounds().withTrimmedTop (headerHeight)
                           .removeFromRight (rackWidth);
}

juce::Rectangle<int> MixerPanel::getStripBounds (int channel) const
{
    const auto viewport = getStripsViewport();

    return { viewport.getX() + displaySlotFor (channel) * stripWidth - stripScrollX,
             viewport.getY(), stripWidth - ui::px (2), viewport.getHeight() };
}

juce::Rectangle<int> MixerPanel::getFaderRowBounds (int channel) const
{
    auto r = getStripBounds (channel).reduced (ui::px (6), ui::px (2));
    r.removeFromTop (ui::px (16));       // name
    r.removeFromTop (ui::px (15));       // output
    r.removeFromTop (ui::px (15));       // pan
    r.removeFromBottom (ui::px (18));    // mute / solo
    r.removeFromBottom (ui::px (13));    // dB readout

    return r;
}

int MixerPanel::faderWidthFor (int rowWidth)
{
    // Phase 27: half the row, less the gutter that separates the two controls.
    // A floor of 16 px keeps the 18 px thumb drawable on a narrow layout — the
    // thumb overhangs its track by design, so a fader narrower than the thumb
    // would look like a mistake rather than a narrow fader.
    return juce::jmax (ui::px (16), rowWidth / 2 - ui::px (2));
}

juce::Rectangle<int> MixerPanel::getFaderBounds (int channel) const
{
    auto r = getFaderRowBounds (channel);

    // Phase 27: the fader no longer owns the whole row. It takes the left half
    // and the stereo meter takes what is left, which is roughly four times the
    // width it had before — a two-bar meter squeezed into 9 px could show that
    // a signal was there, but not how much of it was on each side.
    return r.removeFromLeft (faderWidthFor (r.getWidth()));
}

juce::Rectangle<int> MixerPanel::getFaderHitBounds (int channel) const
{
    const auto fader = getFaderBounds (channel);
    const auto meter = getMeterBounds (channel);

    // The meter is carved out of the same row, to the right, with a 4 px gap.
    // Reach no further than half of it.
    const int gap    = juce::jmax (0, meter.getX() - fader.getRight());
    const int reach  = juce::jmin (ui::px (8), gap / 2);

    return fader.expanded (reach, 0);
}

bool MixerPanel::faderHitClearsMeter (int channel) const
{
    return getFaderHitBounds (channel).getRight() <= getMeterBounds (channel).getX();
}

juce::Rectangle<int> MixerPanel::getOutputBounds (int channel) const
{
    auto r = getStripBounds (channel).reduced (ui::px (6), ui::px (2));
    r.removeFromTop (ui::px (16));       // name

    return r.removeFromTop (ui::px (15));
}

juce::Rectangle<int> MixerPanel::getPanBounds (int channel) const
{
    auto r = getStripBounds (channel).reduced (ui::px (8), ui::px (2));
    r.removeFromTop (ui::px (16));
    r.removeFromTop (ui::px (15));

    return r.removeFromTop (ui::px (13));
}

juce::Rectangle<int> MixerPanel::getMuteBounds (int channel) const
{
    auto r = getStripBounds (channel).reduced (ui::px (6), ui::px (2));
    r = r.removeFromBottom (ui::px (17));

    return r.removeFromLeft (r.getWidth() / 2 - ui::px (2));
}

juce::Rectangle<int> MixerPanel::getSoloBounds (int channel) const
{
    auto r = getStripBounds (channel).reduced (ui::px (6), ui::px (2));
    r = r.removeFromBottom (ui::px (17));

    return r.removeFromRight (r.getWidth() / 2 - ui::px (2));
}

juce::Rectangle<int> MixerPanel::getMonoBounds (int channel) const
{
    auto r = getStripBounds (channel).reduced (ui::px (6), ui::px (2));
    r.removeFromBottom (ui::px (18));            // mute / solo
    auto row = r.removeFromBottom (ui::px (13)); // dB readout

    // The right end of the readout row: the only slack a 92 px strip has left.
    return row.removeFromRight (ui::px (26)).reduced (0, ui::px (1));
}

juce::Rectangle<int> MixerPanel::getMeterBounds (int channel) const
{
    auto r = getFaderRowBounds (channel);

    // Phase 27: everything the fader did not take. The two of them are derived
    // from the SAME row and the same split, so they cannot end up overlapping
    // or leaving a band of dead pixels between them.
    r.removeFromLeft (faderWidthFor (r.getWidth()) + ui::px (4));

    return r;
}

juce::Rectangle<int> MixerPanel::getMeterBarBounds (int channel, int side) const
{
    const auto meter = getMeterBounds (channel);
    const int  bar   = juce::jmax (2, (meter.getWidth() - ui::px (1)) / 2);

    // `withWidth` keeps x, `withX` keeps width — so the right bar needs both, or
    // it starts at the right place and still runs off the end of the pair.
    return side == 0 ? meter.withWidth (bar)
                     : meter.withX (meter.getRight() - bar).withWidth (bar);
}

juce::Rectangle<int> MixerPanel::getStripCloseBounds (int channel) const
{
    // The right end of the name row. Everything else on a strip is a control
    // with a value; this is the one that removes the thing the value belongs to,
    // so it sits beside the name — as far from the fader as the strip allows,
    // because a mis-aimed click there must not be able to delete a strip.
    auto row = getStripBounds (channel).reduced (ui::px (6), ui::px (2));

    return row.removeFromTop (ui::px (16)).removeFromRight (ui::px (12))
                                          .reduced (0, ui::px (1));
}

juce::Rectangle<int> MixerPanel::getInsertRowBounds (int slot) const
{
    auto area = getRackArea().reduced (ui::px (8), 0);
    return { area.getX(), area.getY() + ui::px (26) + slot * ui::px (21),
             area.getWidth(), ui::px (20) };
}

juce::Rectangle<int> MixerPanel::getInsertTypeBounds (int slot) const
{
    auto row = getInsertRowBounds (slot);
    return row.withTrimmedLeft (ui::px (18)).withWidth (ui::px (78));
}

juce::Rectangle<int> MixerPanel::getInsertParamBounds (int slot) const
{
    auto row = getInsertRowBounds (slot);
    return row.withTrimmedLeft (ui::px (100)).withTrimmedRight (ui::px (22));
}

juce::Rectangle<int> MixerPanel::getInsertPowerBounds (int slot) const
{
    auto row = getInsertRowBounds (slot);
    return row.removeFromRight (ui::px (18)).reduced (ui::px (4), ui::px (4));
}

//==============================================================================
int MixerPanel::channelAt (juce::Point<int> position) const
{
    // A strip scrolled half under the effect rack must not answer clicks that
    // landed on the rack, and one scrolled off the left edge must not answer
    // anything: the viewport is the only place a strip exists.
    if (! getStripsViewport().contains (position))
        return -1;

    for (int slot = 0; slot < getStripCount(); ++slot)
        if (getStripBounds (channelForDisplaySlot (slot)).contains (position))
            return channelForDisplaySlot (slot);

    return -1;
}

float MixerPanel::dbFromY (int channel, float y) const
{
    const auto fader = getFaderBounds (channel).toFloat();
    const float t = juce::jlimit (0.0f, 1.0f, (y - fader.getY()) / juce::jmax (1.0f, fader.getHeight()));

    return 6.0f - t * 54.0f;          // top +6 dB, bottom -48 dB
}

float MixerPanel::panFromX (int channel, float x) const
{
    const auto pan = getPanBounds (channel).toFloat();
    const float t = juce::jlimit (0.0f, 1.0f, (x - pan.getX()) / juce::jmax (1.0f, pan.getWidth()));

    return t * 2.0f - 1.0f;
}

/** The synth a channel strip is showing, or null. Never cached: the rack can be
    reordered and replaced under the console's feet. */
const SynthSlot* MixerPanel::slotOnChannel (int channel) const
{
    const int slotId = slotIdOnChannel (channel);

    return slotId >= 0 ? rack.findSlot (slotId) : nullptr;
}

float MixerPanel::getChannelLevelDb (int channel) const
{
    if (isMaster (channel))
        return mixer.getMasterLevelDb();

    if (const auto* slot = slotOnChannel (channel))
        return slot->levelDb;

    return 0.0f;                  // an unrouted channel sits at unity, unused
}

float MixerPanel::getChannelPan (int channel) const
{
    if (isMaster (channel))
        return mixer.getMasterPan();

    if (const auto* slot = slotOnChannel (channel))
        return slot->pan;

    return 0.0f;
}

bool MixerPanel::isChannelMuted (int channel) const
{
    if (isMaster (channel))
        return mixer.isMasterMuted();

    if (const auto* slot = slotOnChannel (channel))
        return slot->muted;

    return false;
}

bool MixerPanel::isChannelSoloed (int channel) const
{
    if (isMaster (channel))
        return false;

    if (const auto* slot = slotOnChannel (channel))
        return slot->soloed;

    return false;
}

juce::String MixerPanel::getChannelName (int channel) const
{
    if (isMaster (channel))
        return utf8 ("主输出");

    const int sources = sourceCountOnChannel (channel);

    if (const auto* slot = slotOnChannel (channel))
        return sources > 1 ? slot->name + " +" + juce::String (sources - 1) : slot->name;

    return utf8 ("通道 ") + juce::String (channel + 1);
}

juce::Colour MixerPanel::getChannelColour (int channel) const
{
    if (isMaster (channel))
        return kMaster;

    if (const auto* slot = slotOnChannel (channel))
        return slot->colour;

    return theme::surface4;
}

juce::String MixerPanel::getOutputName (int channel) const
{
    if (isMaster (channel))
        return utf8 ("音频输出");

    const int destination = mixer.getChannelOutput (channel);

    return destination == MixerModel::masterChannel ? utf8 ("主输出")
                                                    : getChannelName (destination);
}

void MixerPanel::setChannelLevelDb (int channel, float db)
{
    if (isMaster (channel))
    {
        mixer.setMasterLevelDb (db);
        mixer.notify();
        return;
    }

    if (const int slotId = slotIdOnChannel (channel); slotId >= 0)
        rack.setLevelDb (slotId, db);
}

void MixerPanel::setChannelPan (int channel, float pan)
{
    if (isMaster (channel))
    {
        mixer.setMasterPan (pan);
        mixer.notify();
        return;
    }

    if (const int slotId = slotIdOnChannel (channel); slotId >= 0)
        rack.setPan (slotId, pan);
}

void MixerPanel::toggleMute (int channel)
{
    if (isMaster (channel))
    {
        mixer.setMasterMuted (! mixer.isMasterMuted());
        mixer.notify();
        return;
    }

    if (const int slotId = slotIdOnChannel (channel); slotId >= 0)
        rack.setMuted (slotId, ! isChannelMuted (channel));
}

void MixerPanel::toggleSolo (int channel)
{
    if (isMaster (channel))
        return;

    if (const int slotId = slotIdOnChannel (channel); slotId >= 0)
        rack.setSoloed (slotId, ! isChannelSoloed (channel));
}

void MixerPanel::toggleMono (int channel)
{
    // Phase 25: unlike mute and solo this is NOT the synth's setting. Mute and
    // solo decide what is in the mix; mono decides how the strip is listened to,
    // so it belongs to the channel and survives a change of instrument.
    //
    // Phase 26: the master has the same button, and it means the same thing.
    // The master has no channel number, so its flag lives beside the master
    // fader in the model rather than in `channelMono`.
    if (isMaster (channel))
    {
        mixer.setMasterMono (! mixer.isMasterMono());
        return;
    }

    if (! juce::isPositiveAndBelow (channel, mixer.getChannelCount()))
        return;

    mixer.setChannelMono (channel, ! mixer.isChannelMono (channel));
}

bool MixerPanel::isChannelMono (int channel) const
{
    return isMaster (channel) ? mixer.isMasterMono()
                              : mixer.isChannelMono (channel);
}

bool MixerPanel::monoButtonHasOwnPixels() const
{
    const auto box = getMonoBounds (0);

    return ! box.isEmpty()
             && ! box.intersects (getMuteBounds (0))
             && ! box.intersects (getSoloBounds (0))
             && ! box.intersects (getFaderBounds (0))
             && ! box.intersects (getPanBounds (0))
             && ! box.intersects (getMeterBounds (0));
}

bool MixerPanel::stereoMeterHasOwnPixels() const
{
    for (const int channel : { 0, MixerModel::masterChannel })
    {
        const auto left  = getMeterBarBounds (channel, 0);
        const auto right = getMeterBarBounds (channel, 1);
        const auto pair  = getMeterBounds (channel);

        if (left.isEmpty() || right.isEmpty()
             || left.intersects (right)
             || ! pair.contains (left) || ! pair.contains (right)
             || left.intersects (getFaderBounds (channel))
             || right.intersects (getFaderBounds (channel)))
            return false;
    }

    return true;
}

bool MixerPanel::faderYieldedHalfTheRow() const
{
    const auto row = getFaderRowBounds (0);

    if (row.getWidth() < 20)
        return false;

    for (const int channel : { 0, MixerModel::masterChannel })
    {
        const auto fader = getFaderBounds (channel);
        const auto meter = getMeterBounds (channel);

        if (fader.isEmpty() || meter.isEmpty())
            return false;

        const int rowWidth = getFaderRowBounds (channel).getWidth();

        // "Half" with a gutter's worth of tolerance: the split is integer
        // arithmetic, so an odd row width cannot land exactly on 0.5.
        const double share = (double) fader.getWidth() / (double) rowWidth;

        if (share < 0.40 || share > 0.55)
            return false;

        // The meter has to have gained the space, not merely moved into it.
        if (meter.getWidth() <= fader.getWidth())
            return false;

        if (fader.intersects (meter))
            return false;

        if (! getFaderRowBounds (channel).contains (fader)
             || ! getFaderRowBounds (channel).contains (meter))
            return false;
    }

    return true;
}

juce::String MixerPanel::describeControlGeometry() const
{
    const auto describe = [this] (int channel)
    {
        return "ch" + juce::String (channel)
                 + " row " + getFaderRowBounds (channel).toString()
                 + " fader " + getFaderBounds (channel).toString()
                 + " meter " + getMeterBounds (channel).toString();
    };

    return describe (0) + " | " + describe (MixerModel::masterChannel);
}

juce::String MixerPanel::describeMeterGeometry() const
{
    const auto describe = [this] (int channel)
    {
        const auto box = getMeterBounds (channel);

        return "ch" + juce::String (channel)
                 + " strip " + getStripBounds (channel).toString()
                 + " meter " + box.toString()
                 + " L " + getMeterBarBounds (channel, 0).toString()
                 + " R " + getMeterBarBounds (channel, 1).toString()
                 + " fader " + getFaderBounds (channel).toString();
    };

    return describe (0) + " | " + describe (MixerModel::masterChannel);
}

void MixerPanel::setSelectedChannel (int channel)
{
    // Phase 70: the ceiling is the live strip count, not the array capacity.
    // Coming from a 24-channel project to an 8-channel one used to leave the
    // selection on a strip that no longer exists — the rack header then read
    // "通道 22" and the insert editor wrote into nothing. The master stays
    // selectable: it is display slot 0 and always exists.
    const int clamped = channel == MixerModel::masterChannel
                          ? MixerModel::masterChannel
                          : juce::jlimit (0, juce::jmax (0, mixer.getChannelCount() - 1), channel);

    if (selectedChannel == clamped)
        return;

    selectedChannel = clamped;
    repaint();
}

void MixerPanel::mixerChanged()
{
    // The console can shrink under the selection (a project load, or a delete).
    // The master is display slot 0 and never goes away, so only a channel
    // selection has to move.
    if (selectedChannel != MixerModel::masterChannel
         && selectedChannel >= mixer.getChannelCount())
        setSelectedChannel (mixer.getChannelCount() - 1);

    repaint();
}

//==============================================================================
void MixerPanel::updateMeters()
{
    if (levelSource == nullptr)
        return;

    for (int slot = 0; slot < getStripCount(); ++slot)
        repaint (getMeterBounds (channelForDisplaySlot (slot)).expanded (2));
}

void MixerPanel::resized()
{
    // A wider window can leave the row scrolled past its own end; a narrower
    // one may need a scrollbar that was not there before.
    stripScrollX = juce::jlimit (0, getMaxStripScroll(), stripScrollX);
    repaint();
}

//==============================================================================
void MixerPanel::paint (juce::Graphics& g)
{
    g.fillAll (kBackground);

    //-- Header ---------------------------------------------------------------
    {
        auto header = getHeaderArea();

        g.setColour (kPanel);
        g.fillRect (header);

        g.setColour (juce::Colours::white.withAlpha (0.82f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (11.5f)).withStyle ("Bold")));
        g.drawText (utf8 ("混音台"),
                    header.withTrimmedLeft (ui::px (12)).withWidth (ui::px (60)),
                    juce::Justification::centredLeft, false);

        g.setColour (juce::Colours::white.withAlpha (0.32f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));
        g.drawText (juce::String (mixer.getChannelCount()) + utf8 (" 个通道 + 主输出 · 主输出在最左 · 通道的 → 可选输出到另一个通道"),
                    header.withTrimmedLeft (ui::px (66)).withTrimmedRight (ui::px (58)),
                    juce::Justification::centredLeft, true);

        // Phase 18: the console's own strip count. "－" is dead at one channel,
        // "＋" at eight — that is the engine's bus limit, not a UI choice.
        const auto drawStepButton = [&] (juce::Rectangle<int> r, const char* glyph, bool enabled)
        {
            const auto f = r.toFloat();

            g.setColour (juce::Colours::white.withAlpha (enabled ? 0.10f : 0.04f));
            g.fillRoundedRectangle (f, ui::pf (4));
            g.setColour (juce::Colours::white.withAlpha (enabled ? 0.30f : 0.10f));
            g.drawRoundedRectangle (f.reduced (0.5f), ui::pf (4), 1.0f);

            g.setColour (juce::Colours::white.withAlpha (enabled ? 0.80f : 0.22f));
            g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (11.0f)).withStyle ("Bold")));
            g.drawText (glyph, r, juce::Justification::centred, false);
        };

        drawStepButton (getRemoveChannelBounds(), "-", mixer.canRemoveChannel());
        drawStepButton (getAddChannelBounds(), "+",
                        mixer.getChannelCount() < MixerModel::maxChannels);

        g.setColour (kOutline);
        g.drawHorizontalLine (header.getBottom() - 1, 0.0f, (float) getWidth());
    }

    //-- Strips ---------------------------------------------------------------
    // Phase 23: clipped to the viewport so a strip scrolled under the effect
    // rack cannot paint over it.
    {
        const auto viewport = getStripsViewport();

        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (viewport);

        for (int slot = 0; slot < getStripCount(); ++slot)
            drawStrip (g, channelForDisplaySlot (slot));
    }

    //-- Horizontal scrollbar ------------------------------------------------
    if (isStripScrollNeeded())
    {
        const auto track = getScrollBarBounds().toFloat();
        const auto thumb = getScrollThumbBounds().toFloat();

        // A groove, not just a floating bar: without it the thumb reads as a
        // stray highlight sitting under the mute buttons.
        g.setColour (juce::Colours::white.withAlpha (0.055f));
        g.fillRoundedRectangle (track, track.getHeight() * 0.5f);

        const bool hot = draggingScroll || hoveredScroll;
        g.setColour (juce::Colours::white.withAlpha (hot ? 0.46f : 0.28f));
        g.fillRoundedRectangle (thumb, thumb.getHeight() * 0.5f);
    }

    drawRack (g);
}

void MixerPanel::drawStrip (juce::Graphics& g, int channel)
{
    const auto bounds = getStripBounds (channel).toFloat().reduced (ui::pf (1));
    const auto colour = getChannelColour (channel);
    const bool master = isMaster (channel);
    const bool selected = selectedChannel == channel;
    // "exists" = something is actually feeding this channel. An unrouted channel
    // still exists (its inserts are editable), it just has no signal. Its knob
    // and thumb are drawn hollow so the strip reads as "empty", not "quiet".
    const bool exists = master || slotIdOnChannel (channel) >= 0;
    const bool live   = exists;

    g.setColour (master ? theme::surface3 : kStrip);
    g.fillRoundedRectangle (bounds, ui::pf (4));

    if (selected)
    {
        g.setColour (colour.withAlpha (0.55f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), ui::pf (4), 1.2f);
    }

    //-- Name -----------------------------------------------------------------
    auto name = getStripBounds (channel).reduced (ui::px (6), ui::px (2)).toFloat()
                                    .withHeight (ui::pf (15));

    g.setColour (colour.withAlpha (exists ? 0.95f : 0.3f));
    g.fillRoundedRectangle (name.getX(), name.getY() + ui::pf (6), ui::pf (3), ui::pf (9),
                            ui::pf (1.5));

    g.setColour (exists ? juce::Colours::white.withAlpha (0.88f)
                        : juce::Colours::white.withAlpha (0.28f));
    g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.0f))));
    g.drawText (getChannelName (channel),
                name.withTrimmedLeft (ui::pf (8))
                    .withTrimmedRight (master ? 0.0f : ui::pf (12)),
                juce::Justification::centredLeft, false);

    //-- Delete this strip (Phase 26) -----------------------------------------
    // Every ordinary strip carries its own "－". The master does not: it is not a
    // channel, it is where all of them end, and "delete the output" is not a
    // gesture a console offers. At the floor the button stays visible but dead —
    // hiding it would read as "this strip is special", and it is not.
    if (! master)
    {
        const auto r     = getStripCloseBounds (channel).toFloat();
        const bool armed = mixer.canRemoveChannel();
        const bool hot   = armed && hoveredChannel == channel
                                   && hoveredPart == Part::stripClose;

        g.setColour (juce::Colours::white.withAlpha (hot ? 0.18f
                                                         : (armed ? 0.05f : 0.02f)));
        g.fillRoundedRectangle (r, ui::pf (2.5f));

        g.setColour (juce::Colours::white.withAlpha (armed ? (hot ? 0.85f : 0.42f) : 0.14f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f)).withStyle ("Bold")));
        g.drawText ("-", getStripCloseBounds (channel),
                    juce::Justification::centred, false);
    }

    //-- Output routing -------------------------------------------------------
    // Phase 23: one row, click = menu. The master's row is a label, not a
    // control: it has no destination to choose, it IS the destination.
    {
        auto r = getOutputBounds (channel).toFloat();
        const bool hot = hoveredPart == Part::output && hoveredChannel == channel;
        auto text = r;

        if (! master)
        {
            text.removeFromRight (ui::pf (12));   // the caret's column

            g.setColour (juce::Colours::white.withAlpha (hot ? 0.13f : 0.05f));
            g.fillRoundedRectangle (r, ui::pf (3));
            g.setColour (juce::Colours::white.withAlpha (hot ? 0.32f : 0.14f));
            g.drawRoundedRectangle (r.reduced (0.5f), ui::pf (3), 1.0f);

            const auto caret = r.removeFromRight (ui::pf (12)).reduced (0.0f, ui::pf (4));
            mixerlook::drawCaret (g, caret, false, juce::Colours::white.withAlpha (hot ? 0.75f : 0.42f));
        }

        g.setColour (juce::Colours::white.withAlpha (master ? 0.30f : (hot ? 0.90f : 0.62f)));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
        g.drawText (utf8 ("→ ") + getOutputName (channel),
                    text.withTrimmedLeft (ui::pf (5)),
                    juce::Justification::centredLeft, true);
    }

    //-- Pan ------------------------------------------------------------------
    {
        auto pan = getPanBounds (channel).toFloat();
        const float value = getChannelPan (channel);
        const float x = pan.getCentreX() + value * pan.getWidth() * 0.5f;

        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (pan.withHeight (ui::pf (3)).withY (pan.getCentreY() - ui::pf (1.5)),
                                ui::pf (1.5));

        if (live)
        {
            g.setColour (colour.withAlpha (0.85f));
            g.fillEllipse (x - ui::pf (5), pan.getCentreY() - ui::pf (5),
                           ui::pf (10), ui::pf (10));
        }
        else
        {
            // Hollow: nothing is being panned, so nothing pretends to be a value.
            g.setColour (juce::Colours::white.withAlpha (0.20f));
            g.drawEllipse (x - ui::pf (4.5), pan.getCentreY() - ui::pf (4.5),
                           ui::pf (9), ui::pf (9), 1.0f);
        }

        g.setColour (juce::Colours::white.withAlpha (0.12f));
        g.drawVerticalLine ((int) pan.getCentreX(), pan.getY(), pan.getBottom());
    }

    //-- Fader ----------------------------------------------------------------
    {
        const auto fader = getFaderBounds (channel).toFloat();
        const float db = getChannelLevelDb (channel);
        const float t = juce::jlimit (0.0f, 1.0f, (6.0f - db) / 54.0f);
        const float thumbY = fader.getY() + t * fader.getHeight();

        g.setColour (juce::Colours::black.withAlpha (live ? 0.45f : 0.22f));
        g.fillRoundedRectangle (fader.getCentreX() - ui::pf (2), fader.getY(),
                                ui::pf (4), fader.getHeight(), ui::pf (2));

        // 0 dB mark: the one graduation that has to be there.
        const float zeroY = fader.getY() + (6.0f / 54.0f) * fader.getHeight();
        g.setColour (juce::Colours::white.withAlpha (0.25f));
        g.drawHorizontalLine ((int) zeroY, fader.getX() - ui::pf (2), fader.getRight() + ui::pf (2));

        const bool hot = hoveredPart == Part::fader && hoveredChannel == channel;

        if (live)
        {
            g.setColour (colour.withAlpha (hot ? 1.0f : 0.92f));
            g.fillRoundedRectangle (fader.getCentreX() - ui::pf (9), thumbY - ui::pf (5), ui::pf (18), ui::pf (10), ui::pf (2));
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.drawHorizontalLine ((int) thumbY, fader.getCentreX() - ui::pf (9),
                                  fader.getCentreX() + ui::pf (9));
        }
        else
        {
            g.setColour (juce::Colours::white.withAlpha (0.18f));
            g.drawRoundedRectangle (fader.getCentreX() - ui::pf (9), thumbY - ui::pf (5), ui::pf (18), ui::pf (10), ui::pf (2), 1.0f);
        }
    }

    //-- "No source" plate ----------------------------------------------------
    // An empty channel still opens its insert rack, so the strip stays — but the
    // plate has to sit ON TOP of the fader rail, otherwise the two collide into a
    // smear (which is exactly what a bare label at the rail's height looked like).
    if (! live && ! master)
    {
        const auto fader = getFaderBounds (channel);
        auto plate = fader.withHeight (ui::px (44)).withY (fader.getCentreY() - ui::px (22))
                          .expanded (ui::px (3), 0).toFloat();

        g.setColour (kStrip);
        g.fillRoundedRectangle (plate, ui::pf (4));

        g.setColour (juce::Colours::white.withAlpha (0.44f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.0f))));
        g.drawText (utf8 ("无音源"), plate.removeFromTop (ui::pf (22)),
                    juce::Justification::centredBottom, false);

        g.setColour (juce::Colours::white.withAlpha (0.30f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
        g.drawText (utf8 ("发送到此通道"), plate.withTrimmedTop (ui::pf (4)),
                    juce::Justification::centredTop, false);
    }

    //-- dB readout -----------------------------------------------------------
    {
        auto r = getStripBounds (channel).reduced (ui::px (6), ui::px (2));
        r.removeFromBottom (ui::px (18));
        r = r.removeFromBottom (ui::px (13));

        // Phase 25/26: the mono button takes the right end of this row — on the
        // master too, now that the master has one.
        r.removeFromRight (ui::px (26));

        // A channel fed by nothing has no level to report: "0.0 dB" would read
        // as a real setting the user could trust.
        const bool fed = live || master;
        const juce::String readout = fed ? dbText (getChannelLevelDb (channel))
                                         : juce::String (utf8 ("—"));

        g.setColour (! fed ? juce::Colours::white.withAlpha (0.22f)
                           : readout.startsWith (utf8 ("-∞"))
                               ? juce::Colours::white.withAlpha (0.35f) : kText);

        // Phase 50: tabular digits. A fader readout is the one number in this
        // window that moves constantly, and "-6.0" is two pixels wider than
        // "-9.8" in a proportional face — the whole column twitches while a
        // fader is dragged.
        ui::drawTabular (g, readout, r.toFloat(), ui::font (9.5f),
                         juce::Justification::centred);
    }

    //-- Mono monitoring (Phase 25 / 26) ---------------------------------------
    // Drawn for the master as well. It is the same listen control, and the one
    // place a mono check is actually performed is the end of the chain — if the
    // master had no button the user would have to fold every strip by hand to
    // hear the same thing.
    {
        auto r = getMonoBounds (channel).toFloat();
        const bool on  = isChannelMono (channel);
        const bool hot = hoveredChannel == channel && hoveredPart == Part::mono;

        g.setColour (on ? kAccent.withAlpha (0.92f)
                        : juce::Colours::white.withAlpha (hot ? 0.16f : 0.07f));
        g.fillRoundedRectangle (r, ui::pf (2));

        g.setColour (on ? juce::Colours::black.withAlpha (0.85f)
                        : juce::Colours::white.withAlpha (hot ? 0.70f : 0.45f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (8.0f)).withStyle ("Bold")));
        g.drawText ("MONO", r, juce::Justification::centred, false);
    }

    //-- Mute / Solo ----------------------------------------------------------
    for (int i = 0; i < 2; ++i)
    {
        const bool solo = i == 1;

        // Phase 70: the master has no solo. Soloing the end of the chain is not
        // a state that means anything — `toggleSolo()` already refuses it — so
        // drawing the button only produced a control that could never light and
        // a right-click menu entry that did nothing. The M beside it is real:
        // it mutes the whole mix.
        if (solo && isMaster (channel))
            continue;

        auto r = (solo ? getSoloBounds (channel) : getMuteBounds (channel)).toFloat();
        const bool on = solo ? isChannelSoloed (channel) : isChannelMuted (channel);
        const bool hot = hoveredChannel == channel
                           && hoveredPart == (solo ? Part::solo : Part::mute);

        g.setColour (on ? (solo ? kSolo : kMute) : juce::Colours::white.withAlpha (hot ? 0.16f : 0.07f));
        g.fillRoundedRectangle (r, ui::pf (2));

        g.setColour (on ? juce::Colours::black.withAlpha (0.8f)
                        : juce::Colours::white.withAlpha (hot ? 0.7f : 0.45f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f)).withStyle ("Bold")));
        g.drawText (solo ? "S" : "M", r, juce::Justification::centred, false);
    }

    //-- Stereo meter (Phase 26) ----------------------------------------------
    {
        const auto pair = getMeterBounds (channel).toFloat();

        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (pair, ui::pf (1.5));

        // Two bars, not one averaged bar. Averaging would hide exactly the case
        // the meter exists for: a strip panned hard left is silent on the right,
        // and a single bar would report it as "half loud" forever.
        for (int side = 0; side < 2; ++side)
        {
            auto bar = getMeterBarBounds (channel, side).toFloat();
            const float level = levelSource != nullptr ? levelSource (channel, side) : 0.0f;
            const float fill  = juce::jlimit (0.0f, 1.0f, level);

            if (fill <= 0.001f)
                continue;

            const float h = bar.getHeight() * fill;

            g.setColour (fill > 0.92f ? theme::record : colour.withAlpha (0.9f));
            g.fillRect (bar.withHeight (h).withY (bar.getBottom() - h));
        }
    }
}

//==============================================================================
void MixerPanel::drawRack (juce::Graphics& g)
{
    auto area = getRackArea();

    g.setColour (theme::bgApp);
    g.fillRect (area);

    g.setColour (kOutline);
    g.drawVerticalLine (area.getX(), (float) area.getY(), (float) area.getBottom());

    const auto channelColour = getChannelColour (selectedChannel);

    g.setColour (juce::Colours::white.withAlpha (0.82f));
    g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.5f)).withStyle ("Bold")));
    g.drawText (utf8 ("效果器机架"),
                area.withTrimmedLeft (ui::px (12)).withTrimmedTop (ui::px (5)).withHeight (ui::px (14)),
                juce::Justification::centredLeft, false);

    g.setColour (channelColour.withAlpha (0.9f));
    g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));
    g.drawText (getChannelName (selectedChannel) + utf8 (" · 8 插槽"),
                area.withTrimmedLeft (ui::px (90)).withTrimmedTop (ui::px (5)).withHeight (ui::px (14)),
                juce::Justification::centredLeft, false);

    for (int slot = 0; slot < MixerModel::numInserts; ++slot)
    {
        const auto row = getInsertRowBounds (slot);
        const auto& insert = mixer.getInsert (selectedChannel, slot);
        const bool active = insert.type != fx::Type::none;
        const bool on = active && insert.enabled;

        g.setColour (slot % 2 == 0 ? theme::surface2 : theme::surface1);
        g.fillRoundedRectangle (row.toFloat(), ui::pf (3));

        // Phase 43: the row being dragged says so. Without this the insert under
        // the pointer is just another row that changed colour, and a drag across
        // four slots reads as four unrelated repaints.
        if (insertDragMoved && pressedInsertSlot == slot)
        {
            g.setColour (channelColour.withAlpha (0.30f));
            g.fillRoundedRectangle (row.toFloat(), ui::pf (3));
        }

        g.setColour (juce::Colours::white.withAlpha (0.28f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
        g.drawText (juce::String (slot + 1),
                    row.withTrimmedLeft (ui::px (4)).withWidth (ui::px (14)),
                    juce::Justification::centredLeft, false);

        // Type cell: clickable, opens the effect menu.
        auto type = getInsertTypeBounds (slot).toFloat();
        const bool typeHot = hoveredPart == Part::insertType && hoveredSlot == slot;

        g.setColour (active ? channelColour.withAlpha (on ? 0.22f : 0.10f)
                            : juce::Colours::white.withAlpha (typeHot ? 0.10f : 0.04f));
        g.fillRoundedRectangle (type, ui::pf (2));

        g.setColour (active ? juce::Colours::white.withAlpha (on ? 0.9f : 0.45f)
                            : juce::Colours::white.withAlpha (0.30f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
        g.drawText (fx::displayName (insert), type, juce::Justification::centred, true);

        // Parameter bar. A VST3 has its own parameter set, so the bar is
        // replaced by a neutral line rather than a control that does nothing.
        auto param = getInsertParamBounds (slot).toFloat();

        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (param.withHeight (ui::pf (5)).withY (param.getCentreY() - ui::pf (2.5)), ui::pf (2));

        if (insert.type == fx::Type::vst)
        {
            g.setColour (juce::Colours::white.withAlpha (on ? 0.30f : 0.14f));
            g.fillRoundedRectangle (param.withHeight (ui::pf (2)).withY (param.getCentreY() - ui::pf (1)),
                                    ui::pf (1));
        }
        else if (active)
        {
            g.setColour (channelColour.withAlpha (on ? 0.85f : 0.35f));
            g.fillRoundedRectangle (param.withHeight (ui::pf (5)).withY (param.getCentreY() - ui::pf (2.5))
                                        .withWidth (param.getWidth() * juce::jlimit (0.0f, 1.0f, insert.params[(size_t) 0])), ui::pf (2));

            g.setColour (juce::Colours::white.withAlpha (0.35f));
            g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (8.0f))));
            g.drawText (fx::paramName (insert.type),
                        param.withTrimmedRight (-1.0f)
                             .translated (0.0f, -(float) ui::px (9)),
                        juce::Justification::centredLeft, false);
        }

        // Power dot.
        auto power = getInsertPowerBounds (slot).toFloat();
        g.setColour (on ? theme::accent : juce::Colours::white.withAlpha (active ? 0.3f : 0.12f));
        g.fillEllipse (power);
    }

    g.setColour (juce::Colours::white.withAlpha (0.28f));
    g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (8.5f))));
    g.drawText (utf8 ("单击/右键 → 菜单 · 拖名字换位置 · 拖参数条 · 圆点旁通"),
                getRackArea().reduced (ui::px (10), 0).removeFromBottom (ui::px (16)),
                juce::Justification::centredLeft, false);
}

//==============================================================================
void MixerPanel::mouseDown (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();

    if (getHeaderArea().contains (pos))
    {
        if (getAddChannelBounds().contains (pos))
        {
            if (mixer.addChannel())
                setSelectedChannel (juce::jmin (mixer.getChannelCount() - 1, selectedChannel));

            setStripScrollX (stripScrollX);
            repaint();
        }
        else if (getRemoveChannelBounds().contains (pos))
        {
            deleteLastStrip();
        }

        return;
    }

    //-- Phase 23: the horizontal scrollbar comes before every strip ----------
    if (isStripScrollNeeded() && getScrollBarBounds().expanded (0, ui::px (3)).contains (pos))
    {
        const auto thumb = getScrollThumbBounds();

        draggingScroll = true;

        if (thumb.contains (pos))
        {
            // Grabbed the thumb: remember where, so it does not jump under the
            // pointer the moment the drag starts.
            dragScrollGrab = pos.x - thumb.getX();
        }
        else
        {
            // Clicked the empty track: centre the thumb there, then keep
            // dragging — the same gesture a scrollbar is expected to give.
            dragScrollGrab = thumb.getWidth() / 2;
            mouseDrag (e);
        }

        repaint();
        return;
    }

    //-- Phase 12/18: right-click builds the same menus the left button does ---
    // On the rack both buttons now mean "this insert's menu" (Phase 18 — an
    // effect you can see is an effect you expect a menu for), on a strip it
    // means "this channel's menu": automation, reset, delete.
    if (e.mods.isPopupMenu())
    {
        if (getRackArea().contains (pos))
        {
            for (int slot = 0; slot < MixerModel::numInserts; ++slot)
            {
                if (getInsertRowBounds (slot).contains (pos))
                {
                    showInsertMenu (slot, localPointToGlobal (getInsertRowBounds (slot).getBottomLeft()));
                    return;
                }
            }

            return;
        }

        const int channel = channelAt (pos);

        if (channel >= 0)
        {
            setSelectedChannel (channel);

            // Phase 23: the output button answers a right click with its own
            // menu. It is a value, and on every other value in this app a right
            // click means "the menu for this value". The master has no menu of
            // its own, so there it falls through to the strip's.
            if (! isMaster (channel) && getOutputBounds (channel).contains (pos))
            {
                showOutputMenu (channel, e.getScreenPosition());
                return;
            }

            // Phase 43: the four CONTROLS answer with their own menu. It used to
            // be "复位 + a 通道 submenu" on the fader and pan, and nothing at all
            // on mute/solo (they fell through to the channel menu, so a
            // right-click on 静音 offered 删除通道条). One control, one verb:
            // what this number is for, plus whether you want a curve on it.
            const bool onFader = getFaderHitBounds (channel).contains (pos);
            const bool onPan   = getPanBounds (channel).expanded (0, 4).contains (pos);
            const bool onMute  = getMuteBounds (channel).contains (pos);
            const bool onSolo  = ! isMaster (channel) && getSoloBounds (channel).contains (pos);

            if (onFader || onPan || onMute || onSolo)
            {
                showControlMenu (channel,
                                 onFader ? StripControl::fader
                               : onPan   ? StripControl::pan
                               : onMute  ? StripControl::mute
                                         : StripControl::solo,
                                 e.getScreenPosition());
                return;
            }

            if (onChannelMenuRequested != nullptr)
                onChannelMenuRequested (channel, e.getScreenPosition());
        }

        return;
    }

    if (getRackArea().contains (pos))
    {
        for (int slot = 0; slot < MixerModel::numInserts; ++slot)
        {
            if (getInsertTypeBounds (slot).contains (pos))
            {
                // Phase 18: a click opens the menu, and the menu has "打开效果器
                // 界面" in it. One gesture, one place to look; opening straight
                // into an editor meant the slot's other verbs (swap, remove,
                // VST3…) were only discoverable by right-clicking.
                //
                // Phase 43: the press is remembered instead of acted on. Drag it
                // and the insert moves in the chain; release it where it was and
                // the menu opens, exactly as before. Acting on mouseDown would
                // have made "grab and move" impossible to distinguish from
                // "ask for the menu".
                pressedInsertSlot = slot;
                insertDragMoved   = false;
                return;
            }

            if (getInsertParamBounds (slot).expanded (0, 4).contains (pos))
            {
                const auto& insert = mixer.getInsert (selectedChannel, slot);

                // A hosted plugin's parameters belong to the plugin, not to a
                // 0..1 bar: the bar would promise a control it cannot move.
                if (insert.type == fx::Type::none || insert.type == fx::Type::vst)
                    return;

                dragPart = Part::insertParam;
                dragSlot = slot;

                const auto param = getInsertParamBounds (slot).toFloat();
                const float p1 = juce::jlimit (0.0f, 1.0f,
                                               (pos.x - param.getX()) / juce::jmax (1.0f, param.getWidth()));
                mixer.setInsertParam (selectedChannel, slot, p1, insert.params[(size_t) 1]);
                return;
            }

            if (getInsertPowerBounds (slot).expanded (3, 3).contains (pos))
            {
                const auto& insert = mixer.getInsert (selectedChannel, slot);

                if (insert.type != fx::Type::none)
                    mixer.setInsertEnabled (selectedChannel, slot, ! insert.enabled);

                return;
            }
        }

        return;
    }

    const int channel = channelAt (pos);

    if (channel < 0)
        return;

    setSelectedChannel (channel);

    // Phase 26: "－" deletes THIS strip. Tested before every value control: it
    // sits in the name row, where nothing else answers, and a click that lands
    // on it must never be interpreted as "grab the strip".
    if (! isMaster (channel) && getStripCloseBounds (channel).contains (pos))
    {
        deleteStrip (channel);
        return;
    }

    if (! isMaster (channel) && getOutputBounds (channel).contains (pos))
    {
        showOutputMenu (channel, e.getScreenPosition());
        return;
    }

    if (getMuteBounds (channel).contains (pos))
    {
        toggleMute (channel);
        return;
    }

    if (! isMaster (channel) && getSoloBounds (channel).contains (pos))
    {
        toggleSolo (channel);
        return;
    }

    // Phase 25: mono monitoring. Before the fader test, because the button sits
    // in the readout row and the fader's hit area is expanded sideways.
    if (getMonoBounds (channel).contains (pos))
    {
        toggleMono (channel);
        return;
    }

    if (getFaderHitBounds (channel).contains (pos))
    {
        dragPart = Part::fader;
        dragChannel = channel;
        setChannelLevelDb (channel, dbFromY (channel, (float) pos.y));
        return;
    }

    if (getPanBounds (channel).expanded (0, 4).contains (pos))
    {
        dragPart = Part::pan;
        dragChannel = channel;
        setChannelPan (channel, panFromX (channel, (float) pos.x));
        return;
    }
}

void MixerPanel::mouseDrag (const juce::MouseEvent& e)
{
    // Phase 43: an insert being moved. The chain is an ORDER, so dragging a row
    // onto another row has to mean "put it there", not "swap these two" — the
    // model rotates, and the row under the pointer becomes the one being
    // dragged, which is what makes a long drag feel like one gesture instead of
    // a series of swaps.
    if (pressedInsertSlot >= 0)
    {
        for (int slot = 0; slot < MixerModel::numInserts; ++slot)
        {
            if (! getInsertRowBounds (slot).contains (e.getPosition()) || slot == pressedInsertSlot)
                continue;

            if (mixer.moveInsert (selectedChannel, pressedInsertSlot, slot))
            {
                pressedInsertSlot = slot;
                insertDragMoved   = true;
                repaint();
            }

            break;
        }

        return;
    }

    if (draggingScroll)
    {
        const auto track = getScrollBarBounds();
        const auto thumb = getScrollThumbBounds();
        const int  travel = track.getWidth() - thumb.getWidth();
        const int  maxScroll = getMaxStripScroll();

        if (travel <= 0 || maxScroll <= 0)
            return;

        const int x = juce::jlimit (0, travel,
                                    juce::roundToInt (e.position.x) - track.getX() - dragScrollGrab);

        setStripScrollX (juce::roundToInt ((float) maxScroll * (float) x / (float) travel));
        return;
    }

    if (dragPart == Part::insertParam && dragSlot >= 0)
    {
        const auto param = getInsertParamBounds (dragSlot).toFloat();
        const auto& insert = mixer.getInsert (selectedChannel, dragSlot);
        const float p1 = juce::jlimit (0.0f, 1.0f,
                                       (e.position.x - param.getX()) / juce::jmax (1.0f, param.getWidth()));
        mixer.setInsertParam (selectedChannel, dragSlot, p1, insert.params[(size_t) 1]);
        return;
    }

    if (dragChannel < 0)
        return;

    if (dragPart == Part::fader)
        setChannelLevelDb (dragChannel, dbFromY (dragChannel, e.position.y));
    else if (dragPart == Part::pan)
        setChannelPan (dragChannel, panFromX (dragChannel, e.position.x));
}

void MixerPanel::mouseUp (const juce::MouseEvent& e)
{
    // Phase 43: a press on an insert label that never moved is the click it
    // always was — the menu opens here instead of on mouseDown, and this is the
    // only thing that made a drag possible in the same gesture.
    if (pressedInsertSlot >= 0)
    {
        const int  slot  = pressedInsertSlot;
        const bool moved = insertDragMoved;

        pressedInsertSlot = -1;
        insertDragMoved   = false;

        if (! moved && getInsertTypeBounds (slot).contains (e.getPosition()))
            showInsertMenu (slot, localPointToGlobal (getInsertRowBounds (slot).getBottomLeft()));

        repaint();
        return;
    }

    dragPart = Part::none;
    dragChannel = -1;
    dragSlot = -1;
    draggingScroll = false;
    repaint();
}

void MixerPanel::mouseMove (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();

    Part part = Part::none;
    int channel = -1;
    int slot = -1;

    const bool overScroll = isStripScrollNeeded()
                              && getScrollBarBounds().expanded (0, ui::px (3)).contains (pos);

    if (overScroll)
    {
        part = Part::none;                 // the bar is its own thing, not a strip
    }
    else if (getRackArea().contains (pos))
    {
        for (int i = 0; i < MixerModel::numInserts; ++i)
        {
            if (getInsertTypeBounds (i).contains (pos))
            {
                part = Part::insertType;
                slot = i;
                break;
            }

            if (getInsertParamBounds (i).expanded (0, 4).contains (pos)
                 || getInsertPowerBounds (i).expanded (3, 3).contains (pos))
            {
                part = Part::insertParam;
                slot = i;
                break;
            }
        }
    }
    else
    {
        channel = channelAt (pos);

        if (channel >= 0)
        {
            if (getOutputBounds (channel).contains (pos)
                 && ! isMaster (channel))                     part = Part::output;
            else if (! isMaster (channel) && getStripCloseBounds (channel).contains (pos))
                                                                  part = Part::stripClose;
            else if (getMuteBounds (channel).contains (pos))      part = Part::mute;
            else if (! isMaster (channel) && getSoloBounds (channel).contains (pos))
                                                                  part = Part::solo;
            else if (getMonoBounds (channel).contains (pos))      part = Part::mono;
            else if (getFaderHitBounds (channel).contains (pos)) part = Part::fader;
            else if (getPanBounds (channel).expanded (0, 4).contains (pos))   part = Part::pan;
            else                                             part = Part::strip;
        }
    }

    setMouseCursor (part == Part::fader || part == Part::pan || part == Part::insertParam
                        ? juce::MouseCursor::DraggingHandCursor
                        : (part == Part::none ? (overScroll ? juce::MouseCursor::DraggingHandCursor
                                                            : juce::MouseCursor::NormalCursor)
                                              : juce::MouseCursor::PointingHandCursor));

    if (part != hoveredPart || channel != hoveredChannel || slot != hoveredSlot
         || overScroll != hoveredScroll)
    {
        hoveredPart = part;
        hoveredChannel = channel;
        hoveredSlot = slot;
        hoveredScroll = overScroll;
        repaint();
    }
}

void MixerPanel::mouseExit (const juce::MouseEvent&)
{
    if (hoveredPart != Part::none || hoveredScroll)
    {
        hoveredPart = Part::none;
        hoveredChannel = -1;
        hoveredSlot = -1;
        hoveredScroll = false;
        repaint();
    }
}

//==============================================================================
void MixerPanel::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    // Phase 23: a wheel over the strips scrolls them sideways, because there is
    // nothing else for a wheel to do on a fader (a fader is dragged, not
    // wheeled — a wheel that moved a fader would be a trap).
    if (isStripScrollNeeded() && getStripsArea().contains (e.getPosition()))
    {
        const float delta = std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? wheel.deltaX
                                                                              : wheel.deltaY;

        if (std::abs (delta) > 1.0e-4f)
        {
            setStripScrollX (stripScrollX - juce::roundToInt (delta * (float) ui::px (120)));
            return;
        }
    }

    juce::Component::mouseWheelMove (e, wheel);
}

//==============================================================================
void MixerPanel::showOutputMenu (int channel, juce::Point<int> screenPos)
{
    if (isMaster (channel) || ! juce::isPositiveAndBelow (channel, mixer.getChannelCount()))
        return;

    const int current = mixer.getChannelOutput (channel);

    juce::PopupMenu menu;
    menu.addSectionHeader (getChannelName (channel) + utf8 (" · 输出到"));

    // The master first: it is both the default and the most common answer, and
    // a menu where the usual choice is buried under eight strips is a menu that
    // gets read every single time.
    menu.addItem (MixerModel::masterChannel + 1, utf8 ("主输出"), true,
                  current == MixerModel::masterChannel);

    menu.addSeparator();

    for (int destination = 0; destination < mixer.getChannelCount(); ++destination)
    {
        if (destination == channel)
            continue;

        // A strip that already feeds this one would close a loop, so it is
        // listed but greyed: "you cannot, and here is why" beats hiding it and
        // leaving the user wondering where the channel went.
        menu.addItem (destination + 1, getChannelName (destination),
                      ! mixer.wouldLoop (channel, destination),
                      current == destination);
    }

    juce::Component::SafePointer<MixerPanel> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetScreenArea (juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1))
                            .withMinimumWidth (ui::px (150)),
                        [safe, channel] (int result)
                        {
                            if (safe == nullptr || result <= 0)
                                return;

                            if (safe->mixer.setChannelOutput (channel, result - 1))
                                safe->repaint();
                        });
}

//==============================================================================
void MixerPanel::fillControlMenu (juce::PopupMenu& menu, int channel, StripControl control)
{
    const juce::String name = getChannelName (channel);

    juce::String title;
    juce::String value;
    bool         isSwitch = false;

    switch (control)
    {
        case StripControl::fader:
            title = name + utf8 (" · 推子");
            value = mixerlook::dbText (getChannelLevelDb (channel)) + " dB";
            break;

        case StripControl::pan:
            title = name + utf8 (" · 声相");
            value = mixerlook::panText (getChannelPan (channel));
            break;

        case StripControl::mute:
            isSwitch = true;
            title    = name + utf8 (" · 静音");
            value    = isChannelMuted (channel) ? utf8 ("已静音") : utf8 ("开");
            break;

        case StripControl::solo:
            isSwitch = true;
            title    = name + utf8 (" · 独奏");
            value    = isChannelSoloed (channel) ? utf8 ("已独奏") : utf8 ("关");
            break;
    }

    menu.addSectionHeader (title + utf8 (" · ") + value);

    // One verb: 复位 for a number, the on/off it toggles for a switch. A switch
    // that offered "复位" would be offering "turn it off", which is what the
    // item below already says in the user's own words.
    menu.addItem (stripMenuActionId,
                  isSwitch ? (control == StripControl::mute
                                ? (isChannelMuted (channel) ? utf8 ("取消静音") : utf8 ("静音"))
                                : (isChannelSoloed (channel) ? utf8 ("取消独奏") : utf8 ("独奏")))
                           : utf8 ("复位"));

    menu.addSeparator();

    // The host adds exactly one line here — "创建音量自动化" and friends — and
    // owns the ids above `stripMenuActionId`.
    if (buildStripAutomationItems != nullptr)
        buildStripAutomationItems (menu, channel, control);
}

void MixerPanel::showControlMenu (int channel, StripControl control, juce::Point<int> screenPos)
{
    juce::PopupMenu menu;
    fillControlMenu (menu, channel, control);

    juce::Component::SafePointer<MixerPanel> safe (this);
    const int ch = channel;

    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetScreenArea (juce::Rectangle<int> (screenPos.x, screenPos.y,
                                                                         1, 1))
                            .withMinimumWidth (ui::px (170)),
                        [safe, ch, control] (int result)
                        {
                            if (safe == nullptr || result <= 0)
                                return;

                            if (result == stripMenuActionId)
                            {
                                switch (control)
                                {
                                    case StripControl::fader: safe->setChannelLevelDb (ch, 0.0f); break;
                                    case StripControl::pan:   safe->setChannelPan (ch, 0.0f); break;
                                    case StripControl::mute:  safe->toggleMute (ch); break;
                                    case StripControl::solo:  safe->toggleSolo (ch); break;
                                }

                                safe->repaint();
                                return;
                            }

                            if (safe->onStripAutomationChosen != nullptr)
                                safe->onStripAutomationChosen (ch, control, result);
                        });
}

//==============================================================================
void MixerPanel::showInsertMenu (int slot, juce::Point<int> screenAnchor)
{
    const auto& insert = mixer.getInsert (selectedChannel, slot);

    juce::PopupMenu menu;
    menu.addSectionHeader (utf8 ("插槽 ") + juce::String (slot + 1) + " · "
                             + getChannelName (selectedChannel));
    menu.addItem (1, utf8 ("空（移除）"), true, insert.type == fx::Type::none);

    // Phase 46: walk the whole enum but SKIP vst — it sits in the middle
    // (value 5, frozen by old project files) and is chosen from the plugin
    // submenu below. The old `type < numBuiltInTypes()` loop would have
    // served eq/comp/reverb as vst the moment the enum grew.
    for (int type = 1; type < fx::numTypes(); ++type)
    {
        if ((fx::Type) type == fx::Type::vst)
            continue;

        menu.addItem (type + 1, fx::typeName ((fx::Type) type), true,
                      (int) insert.type == type);
    }

    menu.addSeparator();
    menu.addItem (102, utf8 ("打开效果器界面"), insert.type != fx::Type::none);
    menu.addItem (100, utf8 ("VST3 效果器…"), onInsertPluginRequested != nullptr,
                  insert.type == fx::Type::vst);
    menu.addItem (101, utf8 ("插件管理器…"), onOpenPluginManager != nullptr);

    // Phase 43: the same move the drag does, spelled out. A rack you can only
    // reorder by dragging is a rack where the feature is invisible; and this is
    // also the only way to move an insert whose neighbours are all empty, since
    // there is nothing to drag it onto that changes anything.
    menu.addSeparator();
    menu.addItem (103, utf8 ("上移一格"), slot > 0 && insert.type != fx::Type::none);
    menu.addItem (104, utf8 ("下移一格"),
                  slot + 1 < MixerModel::numInserts && insert.type != fx::Type::none);

    const int channel = selectedChannel;
    const auto screenPosition = getScreenBounds().getCentre();

    juce::Component::SafePointer<MixerPanel> safe (this);

    // Phase 18: the menu opens UNDER the slot that was clicked. Below is where
    // the eye already is (the rack grows downwards), and JUCE flips it up on
    // its own when the console sits against the bottom of the screen.
    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetScreenArea (juce::Rectangle<int> (screenAnchor.x,
                                                                         screenAnchor.y, 1, 1))
                            .withMinimumWidth (ui::px (150)),
                        [safe, channel, slot, screenPosition] (int result)
                        {
                            if (safe == nullptr || result <= 0)
                                return;

                            if (result == 102)
                            {
                                if (safe->onInsertOpenRequested != nullptr)
                                    safe->onInsertOpenRequested (channel, slot);

                                return;
                            }

                            if (result == 100)
                            {
                                if (safe->onInsertPluginRequested != nullptr)
                                    safe->onInsertPluginRequested (channel, slot, screenPosition);

                                return;
                            }

                            if (result == 101)
                            {
                                if (safe->onOpenPluginManager != nullptr)
                                    safe->onOpenPluginManager();

                                return;
                            }

                            if (result == 103 || result == 104)
                            {
                                const int to = slot + (result == 103 ? -1 : 1);

                                if (safe->mixer.moveInsert (channel, slot, to))
                                    safe->repaint();

                                return;
                            }

                            safe->mixer.setInsertType (channel, slot, fx::typeFromInt (result - 1));
                            safe->repaint();
                        });
}

} // namespace atomic
