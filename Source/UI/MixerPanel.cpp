#include "MixerPanel.h"

#include "Util/Metrics.h"

#include "UI/MixerLook.h"
#include "Util/Text.h"
#include "Util/ValueMenu.h"

namespace atomic
{

namespace
{
    const juce::Colour kBackground { 0xff101114 };
    const juce::Colour kPanel      { 0xff15161b };
    const juce::Colour kStrip      { 0xff191b21 };
    const juce::Colour kOutline    { 0xff2c2e37 };
    const juce::Colour kText       { 0xff9aa0ad };
    const juce::Colour kAccent     { 0xff4fd1c5 };
    const juce::Colour kMute       { 0xffffb020 };
    const juce::Colour kSolo       { 0xff5ecf9a };
    const juce::Colour kMaster     { 0xffe2e8f0 };

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
    }

    mixer.notify();
    repaint();
}

void MixerPanel::deleteLastStrip()
{
    const int count = mixer.getChannelCount();

    if (count <= MixerModel::minChannels)
        return;

    const int lastChannel = count - 1;

    // Unplug first, then shrink. A synth pointing at a channel that no longer
    // exists would fall through to the direct bus while the UI still claimed it
    // was on a strip.
    for (int i = 0; i < rack.getNumSlots(); ++i)
        if (auto* slot = rack.getSlot (i); slot != nullptr && slot->mixerChannel == lastChannel)
            rack.setMixerChannel (slot->id, -1);

    mixer.clearChannelInserts (lastChannel);
    mixer.removeChannel();

    setSelectedChannel (juce::jmin (selectedChannel, mixer.getChannelCount()));
    repaint();
}

juce::Rectangle<int> MixerPanel::getStripsArea() const
{
    return getLocalBounds().withTrimmedTop (headerHeight)
                           .withTrimmedRight (rackWidth);
}

juce::Rectangle<int> MixerPanel::getRackArea() const
{
    return getLocalBounds().withTrimmedTop (headerHeight)
                           .removeFromRight (rackWidth);
}

juce::Rectangle<int> MixerPanel::getStripBounds (int channel) const
{
    const auto area = getStripsArea();
    const float width = (float) area.getWidth() / (float) getStripCount();

    return { area.getX() + juce::roundToInt (width * (float) channel), area.getY(),
             juce::roundToInt (width) - 2, area.getHeight() };
}

juce::Rectangle<int> MixerPanel::getFaderBounds (int channel) const
{
    auto r = getStripBounds (channel).reduced (ui::px (6), ui::px (2));
    r.removeFromTop (ui::px (16));       // name
    r.removeFromTop (ui::px (15));       // pan
    r.removeFromBottom (ui::px (18));    // mute / solo
    r.removeFromBottom (ui::px (13));    // dB readout
    r.removeFromRight (ui::px (10));     // meter

    return r;
}

juce::Rectangle<int> MixerPanel::getPanBounds (int channel) const
{
    auto r = getStripBounds (channel).reduced (ui::px (8), ui::px (2));
    r.removeFromTop (ui::px (16));

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

juce::Rectangle<int> MixerPanel::getMeterBounds (int channel) const
{
    auto r = getStripBounds (channel).reduced (ui::px (6), ui::px (2));
    r.removeFromTop (ui::px (16));
    r.removeFromTop (ui::px (15));
    r.removeFromBottom (ui::px (18));
    r.removeFromBottom (ui::px (13));

    return r.removeFromRight (ui::px (6));
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
    for (int ch = 0; ch < getStripCount(); ++ch)
        if (getStripBounds (ch).contains (position))
            return ch;

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

    return juce::Colour (0xff3a3d47);
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

void MixerPanel::setSelectedChannel (int channel)
{
    const int clamped = juce::jlimit (0, MixerModel::masterChannel, channel);

    if (selectedChannel == clamped)
        return;

    selectedChannel = clamped;
    repaint();
}

//==============================================================================
void MixerPanel::updateMeters()
{
    if (levelSource == nullptr)
        return;

    for (int ch = 0; ch < getStripCount(); ++ch)
        repaint (getMeterBounds (ch).expanded (2));
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
        g.drawText (juce::String (mixer.getChannelCount()) + utf8 (" 个通道 + 主输出 · 通道收到的是音源 · 右键通道 → 菜单"),
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

        drawStepButton (getRemoveChannelBounds(), "-",
                        mixer.getChannelCount() > MixerModel::minChannels);
        drawStepButton (getAddChannelBounds(), "+",
                        mixer.getChannelCount() < MixerModel::maxChannels);

        g.setColour (kOutline);
        g.drawHorizontalLine (header.getBottom() - 1, 0.0f, (float) getWidth());
    }

    for (int ch = 0; ch < getStripCount(); ++ch)
        drawStrip (g, ch);

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

    g.setColour (master ? juce::Colour (0xff1b1e25) : kStrip);
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
    g.drawText (getChannelName (channel), name.withTrimmedLeft (ui::pf (8)),
                juce::Justification::centredLeft, false);

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
            g.fillRoundedRectangle (fader.getCentreX() - ui::pf (9), thumbY - ui::pf (5),
                                    ui::pf (18), ui::pf (10), ui::pf (2.5));
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.drawHorizontalLine ((int) thumbY, fader.getCentreX() - ui::pf (9),
                                  fader.getCentreX() + ui::pf (9));
        }
        else
        {
            g.setColour (juce::Colours::white.withAlpha (0.18f));
            g.drawRoundedRectangle (fader.getCentreX() - ui::pf (9), thumbY - ui::pf (5),
                                    ui::pf (18), ui::pf (10), ui::pf (2.5), 1.0f);
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

        // A channel fed by nothing has no level to report: "0.0 dB" would read
        // as a real setting the user could trust.
        const bool fed = live || master;
        const juce::String readout = fed ? dbText (getChannelLevelDb (channel))
                                         : juce::String (utf8 ("—"));

        g.setColour (! fed ? juce::Colours::white.withAlpha (0.22f)
                           : readout.startsWith (utf8 ("-∞"))
                               ? juce::Colours::white.withAlpha (0.35f) : kText);
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));
        g.drawText (readout, r, juce::Justification::centred, false);
    }

    //-- Mute / Solo ----------------------------------------------------------
    for (int i = 0; i < 2; ++i)
    {
        const bool solo = i == 1;
        auto r = (solo ? getSoloBounds (channel) : getMuteBounds (channel)).toFloat();
        const bool on = solo ? isChannelSoloed (channel) : isChannelMuted (channel);
        const bool hot = hoveredChannel == channel
                           && hoveredPart == (solo ? Part::solo : Part::mute);

        g.setColour (on ? (solo ? kSolo : kMute) : juce::Colours::white.withAlpha (hot ? 0.16f : 0.07f));
        g.fillRoundedRectangle (r, ui::pf (2.5));

        g.setColour (on ? juce::Colours::black.withAlpha (0.8f)
                        : juce::Colours::white.withAlpha (hot ? 0.7f : 0.45f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f)).withStyle ("Bold")));
        g.drawText (solo ? "S" : "M", r, juce::Justification::centred, false);
    }

    //-- Meter ----------------------------------------------------------------
    {
        auto meter = getMeterBounds (channel).toFloat();
        const float level = levelSource != nullptr ? levelSource (channel) : 0.0f;

        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (meter, ui::pf (1.5));

        const float fill = juce::jlimit (0.0f, 1.0f, level);
        if (fill > 0.001f)
        {
            const float h = meter.getHeight() * fill;
            g.setColour (fill > 0.92f ? juce::Colour (0xffff5f6d) : colour.withAlpha (0.9f));
            g.fillRoundedRectangle (meter.withHeight (h).withY (meter.getBottom() - h), ui::pf (1.5));
        }
    }
}

//==============================================================================
void MixerPanel::drawRack (juce::Graphics& g)
{
    auto area = getRackArea();

    g.setColour (juce::Colour (0xff0e0f13));
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

        g.setColour (slot % 2 == 0 ? juce::Colour (0xff14161c) : juce::Colour (0xff111318));
        g.fillRoundedRectangle (row.toFloat(), ui::pf (3));

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
        g.fillRoundedRectangle (type, ui::pf (2.5));

        g.setColour (active ? juce::Colours::white.withAlpha (on ? 0.9f : 0.45f)
                            : juce::Colours::white.withAlpha (0.30f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
        g.drawText (fx::displayName (insert), type, juce::Justification::centred, true);

        // Parameter bar. A VST3 has its own parameter set, so the bar is
        // replaced by a neutral line rather than a control that does nothing.
        auto param = getInsertParamBounds (slot).toFloat();

        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (param.withHeight (ui::pf (5)).withY (param.getCentreY() - ui::pf (2.5)),
                                ui::pf (2.5));

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
                                        .withWidth (param.getWidth() * juce::jlimit (0.0f, 1.0f, insert.p1)),
                                    ui::pf (2.5));

            g.setColour (juce::Colours::white.withAlpha (0.35f));
            g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (8.0f))));
            g.drawText (fx::paramName (insert.type),
                        param.withTrimmedRight (-1.0f)
                             .translated (0.0f, -(float) ui::px (9)),
                        juce::Justification::centredLeft, false);
        }

        // Power dot.
        auto power = getInsertPowerBounds (slot).toFloat();
        g.setColour (on ? juce::Colour (0xff5ecf9a) : juce::Colours::white.withAlpha (active ? 0.3f : 0.12f));
        g.fillEllipse (power);
    }

    g.setColour (juce::Colours::white.withAlpha (0.28f));
    g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (8.5f))));
    g.drawText (utf8 ("单击 / 右键插槽 → 菜单（含打开界面） · 拖动参数条 · 圆点旁通"),
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

            repaint();
        }
        else if (getRemoveChannelBounds().contains (pos))
        {
            deleteLastStrip();
        }

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

            // Phase 20: on the fader or the pan, the numeric control's own menu
            // comes first — 复位 is the verb a right click on a knob is for. The
            // channel menu is one item away, and the rest of the strip still
            // opens it directly.
            const bool onFader = getFaderBounds (channel).expanded (8, 0).contains (pos);
            const bool onPan   = getPanBounds (channel).expanded (0, 4).contains (pos);

            if (onFader || onPan)
            {
                const auto screenPos = e.getScreenPosition();
                const int  ch        = channel;

                ui::showValueMenu (this, screenPos,
                                   getChannelName (channel) + (onFader ? utf8 (" · 推子")
                                                                       : utf8 (" · 声相")),
                                   onFader ? mixerlook::dbText (getChannelLevelDb (channel)) + " dB"
                                           : mixerlook::panText (getChannelPan (channel)),
                                   [this, ch, onFader]
                                   {
                                       if (onFader)
                                           setChannelLevelDb (ch, 0.0f);
                                       else
                                           setChannelPan (ch, 0.0f);

                                       repaint();
                                   },
                                   [] (juce::PopupMenu& m)
                                   {
                                       m.addItem (10, utf8 ("通道菜单…"));
                                   },
                                   [this, ch, screenPos] (int result)
                                   {
                                       if (result == 10 && onChannelMenuRequested != nullptr)
                                           onChannelMenuRequested (ch, screenPos);
                                   });

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
                // Phase 18: a left click opens the menu too, and the menu has
                // "打开效果器界面" in it. One gesture, one place to look; opening
                // straight into an editor meant the slot's other verbs (swap,
                // remove, VST3…) were only discoverable by right-clicking.
                showInsertMenu (slot, localPointToGlobal (getInsertRowBounds (slot).getBottomLeft()));
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
                mixer.setInsertParam (selectedChannel, slot, p1, insert.p2);
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

    if (getMuteBounds (channel).contains (pos))
    {
        toggleMute (channel);
        return;
    }

    if (getSoloBounds (channel).contains (pos))
    {
        toggleSolo (channel);
        return;
    }

    if (getFaderBounds (channel).expanded (8, 0).contains (pos))
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
    if (dragPart == Part::insertParam && dragSlot >= 0)
    {
        const auto param = getInsertParamBounds (dragSlot).toFloat();
        const auto& insert = mixer.getInsert (selectedChannel, dragSlot);
        const float p1 = juce::jlimit (0.0f, 1.0f,
                                       (e.position.x - param.getX()) / juce::jmax (1.0f, param.getWidth()));
        mixer.setInsertParam (selectedChannel, dragSlot, p1, insert.p2);
        return;
    }

    if (dragChannel < 0)
        return;

    if (dragPart == Part::fader)
        setChannelLevelDb (dragChannel, dbFromY (dragChannel, e.position.y));
    else if (dragPart == Part::pan)
        setChannelPan (dragChannel, panFromX (dragChannel, e.position.x));
}

void MixerPanel::mouseUp (const juce::MouseEvent&)
{
    dragPart = Part::none;
    dragChannel = -1;
    dragSlot = -1;
    repaint();
}

void MixerPanel::mouseMove (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();

    Part part = Part::none;
    int channel = -1;
    int slot = -1;

    if (getRackArea().contains (pos))
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
            if (getMuteBounds (channel).contains (pos))      part = Part::mute;
            else if (getSoloBounds (channel).contains (pos)) part = Part::solo;
            else if (getFaderBounds (channel).expanded (8, 0).contains (pos)) part = Part::fader;
            else if (getPanBounds (channel).expanded (0, 4).contains (pos))   part = Part::pan;
            else                                             part = Part::strip;
        }
    }

    setMouseCursor (part == Part::fader || part == Part::pan || part == Part::insertParam
                        ? juce::MouseCursor::DraggingHandCursor
                        : (part == Part::none ? juce::MouseCursor::NormalCursor
                                              : juce::MouseCursor::PointingHandCursor));

    if (part != hoveredPart || channel != hoveredChannel || slot != hoveredSlot)
    {
        hoveredPart = part;
        hoveredChannel = channel;
        hoveredSlot = slot;
        repaint();
    }
}

void MixerPanel::mouseExit (const juce::MouseEvent&)
{
    if (hoveredPart != Part::none)
    {
        hoveredPart = Part::none;
        hoveredChannel = -1;
        hoveredSlot = -1;
        repaint();
    }
}

//==============================================================================
void MixerPanel::showInsertMenu (int slot, juce::Point<int> screenAnchor)
{
    const auto& insert = mixer.getInsert (selectedChannel, slot);

    juce::PopupMenu menu;
    menu.addSectionHeader (utf8 ("插槽 ") + juce::String (slot + 1) + " · "
                             + getChannelName (selectedChannel));
    menu.addItem (1, utf8 ("空（移除）"), true, insert.type == fx::Type::none);

    for (int type = 1; type < fx::numBuiltInTypes(); ++type)
        menu.addItem (type + 1, fx::typeName ((fx::Type) type), true,
                      (int) insert.type == type);

    menu.addSeparator();
    menu.addItem (102, utf8 ("打开效果器界面"), insert.type != fx::Type::none);
    menu.addItem (100, utf8 ("VST3 效果器…"), onInsertPluginRequested != nullptr,
                  insert.type == fx::Type::vst);
    menu.addItem (101, utf8 ("插件管理器…"), onOpenPluginManager != nullptr);

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

                            safe->mixer.setInsertType (channel, slot, fx::typeFromInt (result - 1));
                            safe->repaint();
                        });
}

} // namespace atomic
