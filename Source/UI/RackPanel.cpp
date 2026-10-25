#include "RackPanel.h"

#include "Util/Metrics.h"

#include "UI/MixerLook.h"
#include "Util/Text.h"
#include "Util/ValueMenu.h"

namespace atomic
{

namespace
{
    const juce::Colour kPanelBackground { 0xff14151a };
    const juce::Colour kCardBackground  { 0xff1b1d24 };
    const juce::Colour kCardHover       { 0xff212430 };
    const juce::Colour kCardBorder      { 0xff2b2e39 };
    const juce::Colour kDivider         { 0xff262832 };
    const juce::Colour kMutedText       { 0xff8b91a0 };
    const juce::Colour kLaneHeader      { 0xff171922 };
    const juce::Colour kSoloColour      { 0xffe0b13c };
    const juce::Colour kMuteColour      { 0xffff6b6b };
    /** The routing highlight. Same teal as the app's accent, so "this is the
        thing you are pointing at" reads the same everywhere. */
    const juce::Colour kFocusAccent     { 0xff4fd1c5 };

    juce::Font boldFont (float height)
    {
        return juce::Font (juce::FontOptions{}.withHeight (atomic::ui::sp (height)).withStyle ("Bold"));
    }
}

//==============================================================================
RackPanel::RackPanel (RackModel& rackToUse)
    : rack (rackToUse)
{
    setOpaque (true);
    rack.addListener (this);

    syncLaneEditors();
}

RackPanel::~RackPanel()
{
    rack.removeListener (this);
}

//==============================================================================
bool RackPanel::isExpanded (int index) const
{
    auto* slot = rack.getSlot (index);
    return slot != nullptr && slot->automationExpanded && ! slot->collapsed;
}

bool RackPanel::isCollapsed (int index) const
{
    auto* slot = rack.getSlot (index);
    return slot != nullptr && slot->collapsed;
}

int RackPanel::cardHeightAt (int index) const
{
    // A folded card keeps the gap but loses everything else: the lane stays
    // "expanded" in the model so unfolding restores the curve untouched.
    if (isCollapsed (index))
        return collapsedHeight;

    return cardHeight + (isExpanded (index) ? laneHeight : 0);
}

int RackPanel::getContentHeight() const
{
    int total = 0;

    for (int i = 0; i < rack.getNumSlots(); ++i)
        total += cardHeightAt (i) + cardGap;

    return total;
}

int RackPanel::getViewHeight() const
{
    return juce::jmax (0, getHeight() - headerHeight - footerHeight);
}

juce::Rectangle<int> RackPanel::getViewArea() const
{
    return { 0, headerHeight, getWidth(), getViewHeight() };
}

void RackPanel::setScrollY (int pixels)
{
    scrollY = (float) pixels;
    clampScroll();
    updateLayout();
    repaint();
}

void RackPanel::clampScroll()
{
    const int excess = juce::jmax (0, getContentHeight() - getViewHeight());
    scrollY = (float) juce::jlimit (0, excess, (int) scrollY);
}

int RackPanel::cardTop (int index) const
{
    int y = headerHeight - (int) scrollY;

    for (int i = 0; i < index; ++i)
        y += cardHeightAt (i) + cardGap;

    return y;
}

juce::Rectangle<int> RackPanel::getCardBounds (int index) const
{
    return { ui::px (12), cardTop (index), getWidth() - ui::px (24),
             isCollapsed (index) ? collapsedHeight : cardHeight };
}

juce::Rectangle<int> RackPanel::getCollapseBounds (int index) const
{
    auto card = getCardBounds (index);

    // Immediately left of the S / M / X cluster, and identically placed whether
    // the card is open or folded: a fold control that moves when you use it is
    // a fold control you have to hunt for the second time.
    return { card.getRight() - ui::px (122), card.getY() + ui::px (8), ui::px (18), ui::px (18) };
}

juce::Rectangle<int> RackPanel::getWaveformBounds (int index) const
{
    auto card = getCardBounds (index);
    return { card.getX() + ui::px (12), card.getY() + ui::px (40), ui::px (84), ui::px (18) };
}

juce::Rectangle<int> RackPanel::getSoloBounds (int index) const
{
    auto card = getCardBounds (index);
    return { card.getRight() - ui::px (78), card.getY() + ui::px (7), ui::px (20), ui::px (20) };
}

juce::Rectangle<int> RackPanel::getMuteBounds (int index) const
{
    auto card = getCardBounds (index);
    return { card.getRight() - ui::px (54), card.getY() + ui::px (7), ui::px (20), ui::px (20) };
}

juce::Rectangle<int> RackPanel::getCloseBounds (int index) const
{
    auto card = getCardBounds (index);
    return { card.getRight() - ui::px (30), card.getY() + ui::px (7), ui::px (20), ui::px (20) };
}

juce::Rectangle<int> RackPanel::getVolumeBounds (int index) const
{
    auto card = getCardBounds (index);
    return { card.getX() + ui::px (12), card.getY() + ui::px (72), ui::px (112), ui::px (7) };
}

juce::Rectangle<int> RackPanel::getPanBounds (int index) const
{
    auto card = getCardBounds (index);
    return { card.getX() + ui::px (166), card.getY() + ui::px (72),
             juce::jmax (ui::px (40), card.getWidth() - ui::px (178)), ui::px (7) };
}

juce::Rectangle<int> RackPanel::getSendBounds (int index) const
{
    auto card = getCardBounds (index);
    return { card.getX() + ui::px (12), card.getY() + ui::px (106),
             card.getWidth() - ui::px (24), ui::px (16) };
}

juce::Rectangle<int> RackPanel::getAddButtonBounds() const
{
    return { ui::px (12), getHeight() - footerHeight + ui::px (14),
             juce::jmax (0, getWidth() - ui::px (24)), ui::px (32) };
}

juce::Rectangle<int> RackPanel::getLaneCanvasBounds (int index) const
{
    auto card = getCardBounds (index);
    return { ui::px (14), card.getY() + cardHeight + ui::px (22),
             getWidth() - ui::px (28), laneHeight - ui::px (28) };
}

juce::Rectangle<int> RackPanel::getLaneTargetBounds (int index, int targetIndex) const
{
    auto card = getCardBounds (index);
    return { card.getX() + ui::px (12) + targetIndex * ui::px (44),
             card.getY() + cardHeight + ui::px (3), ui::px (40), ui::px (15) };
}

juce::Rectangle<int> RackPanel::getLaneEnableBounds (int index) const
{
    auto card = getCardBounds (index);
    return { getWidth() - ui::px (58), card.getY() + cardHeight + ui::px (3),
             ui::px (44), ui::px (15) };
}

//==============================================================================
void RackPanel::paint (juce::Graphics& g)
{
    g.fillAll (kPanelBackground);

    //-- Header ---------------------------------------------------------------
    g.setColour (juce::Colours::white.withAlpha (0.92f));
    g.setFont (boldFont (13.5f));
    g.drawText (utf8 ("音源机架"), ui::px (14), ui::px (10), ui::px (140), ui::px (20),
                juce::Justification::centredLeft, false);

    if (focusActive)
    {
        g.setColour (kFocusAccent.withAlpha (0.95f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));
        g.drawText (focusSlotId >= 0 ? (utf8 ("高亮 · ") + rack.getSlotName (focusSlotId)
                                        + utf8 (" · 单击切换"))
                                     : utf8 ("没有选中音源"),
                    ui::px (14), ui::px (27), getWidth() - ui::px (24), ui::px (14),
                    juce::Justification::centredLeft, true);
    }
    else
    {
        g.setColour (juce::Colours::white.withAlpha (0.28f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));
        g.drawText ("RACK MIX" + separatorDot() + juce::String (rack.getNumSlots()),
                    ui::px (14), ui::px (27), ui::px (180), ui::px (14),
                    juce::Justification::centredLeft, false);
    }

    g.setColour (kDivider);
    g.drawHorizontalLine (headerHeight - 1, 0.0f, (float) getWidth());

    //-- Cards ----------------------------------------------------------------
    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (getViewArea());

        for (int i = 0; i < rack.getNumSlots(); ++i)
        {
            if (! getCardBounds (i).intersects (getViewArea()))
                continue;

            drawCard (g, i);

            if (isExpanded (i))
                drawLaneHeader (g, i);
        }
    }

    //-- Scroll indicator -----------------------------------------------------
    {
        const int excess = getContentHeight() - getViewHeight();

        if (excess > 0)
        {
            const auto view = getViewArea();
            const float trackTop    = (float) view.getY();
            const float trackHeight = (float) view.getHeight();
            const float thumbHeight = juce::jmax (ui::pf (18),
                                                  trackHeight * (float) getViewHeight()
                                                    / (float) juce::jmax (1, getContentHeight()));
            const float thumbTop = trackTop + (trackHeight - thumbHeight) * (scrollY / (float) excess);

            g.setColour (juce::Colours::white.withAlpha (0.10f));
            g.fillRoundedRectangle (juce::Rectangle<float> ((float) getWidth() - ui::pf (4), thumbTop,
                                                            ui::pf (3), thumbHeight), ui::pf (1.5f));
        }
    }

    if (rack.getNumSlots() == 0)
    {
        g.setColour (kMutedText.withAlpha (0.6f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (11.5f))));
        g.drawFittedText (utf8 ("机架为空\n所有音符都不会发声\n点下方「＋ 添加音源」"),
                          getLocalBounds().withTrimmedTop (headerHeight).withTrimmedBottom (footerHeight),
                          juce::Justification::centred, 3);
    }

    //-- Footer ---------------------------------------------------------------
    if (getHeight() > 200)
    {
        const auto button = getAddButtonBounds().toFloat();

        g.setColour (juce::Colours::white.withAlpha (hoveringAdd ? 0.08f : 0.04f));
        g.fillRoundedRectangle (button, ui::pf (6));
        g.setColour (juce::Colours::white.withAlpha (hoveringAdd ? 0.22f : 0.12f));
        g.drawRoundedRectangle (button, ui::pf (6), 1.0f);

        g.setColour (juce::Colours::white.withAlpha (0.60f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (11.5f))));
        g.drawText (utf8 ("＋  添加音源"), button.toNearestInt(), juce::Justification::centred, false);
    }

    g.setColour (kDivider);
    g.drawVerticalLine (0, 0.0f, (float) getHeight());
}

//==============================================================================
void RackPanel::drawCard (juce::Graphics& g, int index)
{
    auto* slot = rack.getSlot (index);

    if (slot == nullptr)
        return;

    const auto card = getCardBounds (index).toFloat();
    const bool hovered = (index == hoveredCard);
    const juce::Colour accent = slot->muted ? juce::Colour (0xff5a5f6d) : slot->colour;

    g.setColour (hovered ? kCardHover : kCardBackground);
    g.fillRoundedRectangle (card, ui::pf (8));

    g.setColour (kCardBorder);
    g.drawRoundedRectangle (card, ui::pf (8), 1.0f);

    // Left edge in the synth's own colour — the same colour the routed notes
    // use for their override badge, so the mapping is readable at a glance.
    g.setColour (accent);
    g.fillRoundedRectangle (juce::Rectangle<float> (card.getX() + ui::pf (1),
                                                   card.getY() + ui::pf (9), ui::pf (3),
                                                   card.getHeight() - ui::pf (18)),
                            ui::pf (1.5));

    const int cardX = (int) card.getX();
    const int cardY = (int) card.getY();
    const int cardW = (int) card.getWidth();

    //-- Phase 17: the folded card --------------------------------------------
    // One line: what this synth is called. The fader and the routing chips come
    // back the moment it is unfolded, so nothing is lost — this is a way to see
    // past the rack, not a different rack.
    if (slot->collapsed)
    {
        mixerlook::drawCaret (g, getCollapseBounds (index).toFloat(), false,
                              juce::Colours::white.withAlpha (hovered ? 0.75f : 0.42f));

        g.setColour (accent);
        g.fillEllipse (juce::Rectangle<float> (ui::pf (9), ui::pf (9))
                         .withCentre ({ card.getX() + ui::pf (22), card.getCentreY() }));

        g.setColour (juce::Colours::white.withAlpha (slot->muted ? 0.40f : 0.92f));
        g.setFont (juce::Font (juce::FontOptions{}.withHeight (atomic::ui::sp (12.5f))
                                 .withStyle ("Bold")));
        g.drawText (slot->name,
                    juce::Rectangle<int> (cardX + ui::px (36), cardY,
                                          cardW - ui::px (130), (int) card.getHeight()),
                    juce::Justification::centredLeft, true);

        g.setColour (juce::Colours::white.withAlpha (0.30f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
        g.drawText (mixerlook::dbText (slot->levelDb) + " dB",
                    juce::Rectangle<int> (cardX, cardY, cardW - ui::px (30), (int) card.getHeight()),
                    juce::Justification::centredRight, false);
        return;
    }

    //-- Name -----------------------------------------------------------------
    g.setColour (accent);
    g.fillEllipse ((float) cardX + ui::pf (12), (float) cardY + ui::pf (11),
                   ui::pf (8), ui::pf (8));

    g.setColour (juce::Colours::white.withAlpha (slot->muted ? 0.45f : 0.95f));
    g.setFont (boldFont (13.0f));
    g.drawText (slot->name,
                juce::Rectangle<int> (cardX + ui::px (26), cardY + ui::px (6),
                                      cardW - ui::px (134), ui::px (18)),
                juce::Justification::centredLeft, false);

    //-- S / M / close ---------------------------------------------------------
    // The fold caret leads the cluster. Without it an open card has no way to
    // become a one-line title bar, which is half the point of folding.
    mixerlook::drawCaret (g, getCollapseBounds (index).toFloat(), true,
                          juce::Colours::white.withAlpha (hovered ? 0.70f : 0.34f));

    mixerlook::drawBadge (g, getSoloBounds (index).toFloat(), "S", slot->soloed, kSoloColour,
                          hovered && hoveredPart == Part::solo);
    mixerlook::drawBadge (g, getMuteBounds (index).toFloat(), "M", slot->muted, kMuteColour,
                          hovered && hoveredPart == Part::mute);

    {
        auto close = getCloseBounds (index).toFloat();

        if (index == hoveredCard && hoveredPart == Part::close && rack.getNumSlots() > 1)
        {
            g.setColour (juce::Colours::white.withAlpha (0.10f));
            g.fillRoundedRectangle (close, ui::pf (5));
        }

        const float alpha = (rack.getNumSlots() > 1)
                              ? (hovered && hoveredPart == Part::close ? 0.9f : 0.35f)
                              : 0.12f;

        g.setColour (juce::Colours::white.withAlpha (alpha));
        g.drawLine (close.getX() + ui::pf (7), close.getY() + ui::pf (7),
                    close.getRight() - ui::pf (7), close.getBottom() - ui::pf (7), ui::pf (1.3));
        g.drawLine (close.getRight() - ui::pf (7), close.getY() + ui::pf (7),
                    close.getX() + ui::pf (7), close.getBottom() - ui::pf (7), ui::pf (1.3));
    }

    //-- Routing focus ---------------------------------------------------------
    // A second outline, outside the normal border: the card a selected note is
    // currently playing on. Drawn before the contents so the glow never covers
    // a control.
    if (focusActive && focusSlotId == slot->id)
    {
        g.setColour (accent.withAlpha (0.22f));
        g.drawRoundedRectangle (card.expanded (ui::pf (3)), ui::pf (10), ui::pf (3));
        g.setColour (accent.withAlpha (0.95f));
        g.drawRoundedRectangle (card.reduced (ui::pf (0.5)), ui::pf (7.5), ui::pf (1.8));
    }

    //-- Meter -----------------------------------------------------------------
    {
        const float level = levelSource != nullptr ? levelSource (slot->id) : 0.0f;
        mixerlook::drawMeter (g, juce::Rectangle<float> ((float) cardX + ui::pf (12),
                                                         (float) cardY + ui::pf (30),
                                                         (float) cardW - ui::pf (24), ui::pf (4)),
                              level, accent);
    }

    //-- Source pill -----------------------------------------------------------
    // A built-in synth shows its oscillator. A hosted VST3 shows "VST3", because
    // once a plugin makes the sound the waveform is not a setting any more.
    {
        auto pill = getWaveformBounds (index).toFloat();

        const bool hot = hovered && hoveredPart == Part::waveform && ! slot->isPlugin();

        g.setColour (juce::Colours::white.withAlpha (hot ? 0.11f : 0.055f));
        g.fillRoundedRectangle (pill, ui::pf (5));
        g.setColour (accent.withAlpha (hot ? 0.60f : 0.35f));
        g.drawRoundedRectangle (pill, ui::pf (5), 1.0f);

        if (slot->isPlugin())
        {
            g.setColour (accent.withAlpha (slot->pluginId > 0 ? 0.95f : 0.4f));
            g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
            g.drawText (slot->pluginId > 0 ? juce::String ("VST3") + utf8 (" · 插件")
                                           : juce::String ("VST3") + utf8 (" · 未载入"),
                        pill.reduced (ui::pf (7), 0.0f), juce::Justification::centredLeft, true);
        }
        else
        {
            g.setColour (accent.withAlpha (0.95f));
            g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
            g.drawText (waveformName (slot->waveform),
                        pill.reduced (ui::pf (7), 0.0f), juce::Justification::centredLeft, false);
        }
    }

    //-- Routing readout -------------------------------------------------------
    {
        const int notes     = noteCountSource     != nullptr ? noteCountSource (slot->id)     : 0;
        const int overrides = overrideCountSource != nullptr ? overrideCountSource (slot->id) : 0;

        g.setColour (kMutedText.withAlpha (0.85f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
        g.drawText (juce::String (notes) + " notes",
                    juce::Rectangle<int> (cardX + ui::px (102), cardY + ui::px (40),
                                          cardW - ui::px (116), ui::px (18)),
                    juce::Justification::centredLeft, false);

        if (overrides > 0)
        {
            g.setColour (accent);
            g.setFont (juce::Font (juce::FontOptions{}.withHeight (atomic::ui::sp (9.0f)).withStyle ("Bold")));
            g.drawText (juce::String (overrides) + utf8 (" 越级"),
                        juce::Rectangle<int> (cardX + ui::px (102), cardY + ui::px (40),
                                              cardW - ui::px (116), ui::px (18)),
                        juce::Justification::centredRight, false);
        }
    }

    //-- Volume ----------------------------------------------------------------
    {
        const auto fader = getVolumeBounds (index).toFloat();
        mixerlook::drawFader (g, fader, mixerlook::normFromDb (slot->levelDb), accent,
                              hovered && hoveredPart == Part::volume);

        g.setColour (juce::Colours::white.withAlpha (0.50f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (8.5f))));
        g.drawText (mixerlook::dbText (slot->levelDb),
                    juce::Rectangle<int> (cardX + ui::px (128), cardY + ui::px (66),
                                          ui::px (34), ui::px (15)),
                    juce::Justification::centredLeft, false);
    }

    //-- Pan -------------------------------------------------------------------
    {
        const auto pan = getPanBounds (index).toFloat();
        mixerlook::drawPan (g, pan, slot->pan, accent, hovered && hoveredPart == Part::pan);
    }

    //-- Automation readout ----------------------------------------------------
    // Phase 14: the 自动化 badge is gone. This line is a status, not a control:
    // a dot plus the parameter the unfolded lane below is showing, or a hint
    // pointing at the context menu when there is no curve yet.
    {
        const bool hasCurve = slot->getAutomation().isEnabled();
        const auto line = juce::Rectangle<int> (cardX + ui::px (12), cardY + ui::px (88),
                                                cardW - ui::px (24), ui::px (16));

        if (hasCurve)
        {
            g.setColour (accent);
            g.fillEllipse ((float) line.getX(), (float) line.getCentreY() - ui::pf (2),
                           ui::pf (4), ui::pf (4));
        }

        g.setColour (kMutedText.withAlpha (hasCurve ? 0.85f : 0.7f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));
        g.drawText (hasCurve ? juce::String ("AUTO") + separatorDot()
                                 + (slot->automationTarget == SlotAutomationTarget::filter
                                      ? utf8 ("滤波器") : utf8 ("音量"))
                             : utf8 ("右键：创建自动化"),
                    line.withTrimmedLeft (ui::px (9)),
                    juce::Justification::centredLeft, false);
    }

    //-- Console send ----------------------------------------------------------
    // Phase 11: the synth is what a mixer channel receives, so the card has to
    // say where it goes. "→ 主输出" is the default and means "no channel at all".
    {
        const auto chipRect = getSendBounds (index);
        const auto chip     = chipRect.toFloat();
        const bool hot      = hovered && hoveredPart == Part::send;
        const bool sent     = slot->sendsToMixer();

        g.setColour (juce::Colours::white.withAlpha (hot ? 0.10f : 0.04f));
        g.fillRoundedRectangle (chip, ui::pf (4));

        g.setColour (sent ? accent.withAlpha (hot ? 0.75f : 0.5f)
                          : juce::Colours::white.withAlpha (hot ? 0.2f : 0.1f));
        g.drawRoundedRectangle (chip, ui::pf (4), 1.0f);

        // 9.5 px, not 8.5: 混音器通道 is four dense glyphs and at 8.5 they
        // collapse into a smear. The caret is drawn rather than typed, because
        // the font's triangle glyph is a dot at this size.
        g.setColour (sent ? accent : kMutedText.withAlpha (0.8f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));
        g.drawText (sent ? (utf8 ("→ 混音器通道 ") + juce::String (slot->mixerChannel + 1))
                         : utf8 ("→ 主输出 · 不经混音器"),
                    chipRect.reduced (ui::px (7), 0).withTrimmedRight (ui::px (12)),
                    juce::Justification::centredLeft, false);

        mixerlook::drawCaret (g, chip.withTrimmedLeft (chip.getWidth() - ui::pf (18))
                                       .reduced (ui::pf (4), ui::pf (4)),
                              true, sent ? accent.withAlpha (0.85f)
                                         : kMutedText.withAlpha (0.7f));
    }
}

//==============================================================================
void RackPanel::drawLaneHeader (juce::Graphics& g, int index)
{
    auto* slot = rack.getSlot (index);

    if (slot == nullptr)
        return;

    auto card = getCardBounds (index);
    const auto strip = juce::Rectangle<float> ((float) card.getX() - ui::pf (2),
                                               (float) (card.getY() + cardHeight),
                                               (float) card.getWidth() + ui::pf (4),
                                               (float) laneHeight - ui::pf (4));

    g.setColour (kLaneHeader);
    g.fillRoundedRectangle (strip, ui::pf (5));

    const auto accent = slot->colour;
    const bool enabled = slot->getAutomation().isEnabled();
    const auto mouse = getMouseXYRelative();

    // Live readout of the automated parameter at the playhead.
    {
        const double probeBeat = playheadBeat >= 0.0 ? playheadBeat : 0.0;
        const float  value     = slot->getAutomation().valueAtBeat (probeBeat);

        const juce::String readout = slot->automationTarget == SlotAutomationTarget::filter
                                       ? juce::String (juce::roundToInt (value * 100.0f)) + "%"
                                       : mixerlook::dbText (mixerlook::faderBottomDb
                                                              + value * mixerlook::faderRangeDb);

        g.setColour (juce::Colours::white.withAlpha (enabled ? 0.48f : 0.26f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
        g.drawText (readout,
                    juce::Rectangle<int> (card.getX() + ui::px (104),
                                          card.getY() + cardHeight + ui::px (3),
                                          ui::px (100), ui::px (15)),
                    juce::Justification::centredLeft, false);
    }

    const char* names[2] { "FILTER", "LEVEL" };
    const SlotAutomationTarget targets[2] { SlotAutomationTarget::filter,
                                            SlotAutomationTarget::level };

    for (int t = 0; t < 2; ++t)
    {
        const bool selected = (slot->automationTarget == targets[t]);
        mixerlook::drawBadge (g, getLaneTargetBounds (index, t).toFloat(), names[t],
                              selected, accent, getLaneTargetBounds (index, t).contains (mouse));
    }

    mixerlook::drawBadge (g, getLaneEnableBounds (index).toFloat(),
                          enabled ? "ON" : "OFF", enabled,
                          enabled ? accent : juce::Colours::white,
                          getLaneEnableBounds (index).contains (mouse));
}

//==============================================================================
void RackPanel::syncLaneEditors()
{
    // Compare by slot ID, not by count: deleting Synth A and adding Synth C
    // leaves the count unchanged but repoints every editor. The ID is the
    // identity the whole rack is built on, so it is what the editors watch.
    std::vector<int> ids;

    for (int i = 0; i < rack.getNumSlots(); ++i)
        ids.push_back (rack.getSlotIdAt (i));

    if (ids != laneSlotIds)
    {
        laneSlotIds = ids;
        laneEditors.clear();

        for (int slotId : ids)
        {
            auto editor = std::make_unique<AutomationLaneComponent>();

            editor->setLaneSource ([this, slotId]() -> AutomationLane*
            {
                auto* slot = rack.findSlot (slotId);
                return slot != nullptr ? &slot->getAutomation() : nullptr;
            });

            editor->onChange = [this] { rack.touch(); };

            addChildComponent (*editor);
            laneEditors.push_back (std::move (editor));
        }
    }

    updateLayout();
}

void RackPanel::updateLayout()
{
    for (int i = 0; i < (int) laneEditors.size(); ++i)
    {
        auto* slot = rack.getSlot (i);
        auto& editor = *laneEditors[(size_t) i];

        if (slot == nullptr || ! slot->automationExpanded)
        {
            editor.setVisible (false);
            continue;
        }

        editor.setAccentColour (slot->colour);
        editor.setTimeline (timeline::defaultLengthBeats);
        editor.setBounds (getLaneCanvasBounds (i));
        editor.setVisible (true);
    }
}

void RackPanel::setPlayheadBeat (double beats)
{
    playheadBeat = beats;

    for (int i = 0; i < (int) laneEditors.size(); ++i)
    {
        laneEditors[(size_t) i]->setPlayheadBeat (beats);

        if (isExpanded (i))
            repaint (juce::Rectangle<int> (12, cardTop (i) + cardHeight, getWidth() - 24, 22));
    }
}

//==============================================================================
void RackPanel::updateMeters()
{
    repaint();
}

void RackPanel::setRoutingFocus (int slotId, bool hasSelection)
{
    const int  newFocus  = hasSelection ? slotId : -1;
    const bool newActive = hasSelection;

    if (newFocus == focusSlotId && newActive == focusActive)
        return;

    focusSlotId = newFocus;
    focusActive = newActive;
    repaint();
}

void RackPanel::rackChanged()
{
    // A deleted card can leave the hover index pointing at nothing.
    hoveredCard = -1;
    hoveredPart = Part::none;

    syncLaneEditors();
    repaint();
}

void RackPanel::resized()
{
    updateLayout();
}

//==============================================================================
void RackPanel::mouseDown (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();

    if (getHeight() > 200 && getAddButtonBounds().contains (pos))
    {
        // The host knows what plugins exist; the panel only knows what is
        // already racked. Falls back to a built-in synth when nobody is
        // listening, so the button never becomes dead.
        if (onAddSlotRequested != nullptr)
            onAddSlotRequested (e.getScreenPosition());
        else
            rack.addSynth();

        return;
    }

    // A card scrolled under the header or the footer is not clickable through
    // them: the list area is the only place the rack answers the mouse.
    if (! getViewArea().contains (pos))
        return;

    for (int i = 0; i < rack.getNumSlots(); ++i)
    {
        auto* slot = rack.getSlot (i);

        if (slot == nullptr)
            continue;

        const bool inCard = getCardBounds (i).contains (pos);
        const bool inLane = isExpanded (i)
                              && juce::Rectangle<int> (getCardBounds (i).getX(),
                                                       getCardBounds (i).getY() + cardHeight,
                                                       getCardBounds (i).getWidth(),
                                                       laneHeight).contains (pos);

        if (! inCard && ! inLane)
            continue;

        if (e.mods.isPopupMenu())
        {
            // Phase 20: on a fader, "reset" is the first thing you want; the
            // card's menu is one click behind it.
            const bool onVolume = inCard && getVolumeBounds (i).expanded (0, 6).contains (pos);
            const bool onPan    = inCard && getPanBounds (i).expanded (0, 6).contains (pos);

            if (onVolume || onPan)
            {
                const auto screenPos = e.getScreenPosition();
                const int  slotId    = slot->id;

                ui::showValueMenu (this, screenPos,
                                   slot->name + (onVolume ? utf8 (" · 音量") : utf8 (" · 声相")),
                                   onVolume ? mixerlook::dbText (slot->levelDb) + " dB"
                                            : mixerlook::panText (slot->pan),
                                   [this, slotId, onVolume]
                                   {
                                       if (onVolume)
                                           rack.setLevelDb (slotId, 0.0f);   // unity
                                       else
                                           rack.setPan (slotId, 0.0f);       // centre
                                   },
                                   [] (juce::PopupMenu& m)
                                   {
                                       m.addItem (10, utf8 ("音源菜单…"));
                                   },
                                   [this, slotId, screenPos] (int result)
                                   {
                                       if (result == 10 && onCardMenuRequested != nullptr)
                                           onCardMenuRequested (slotId, screenPos);
                                   });

                return;
            }

            if (onCardMenuRequested != nullptr)
                onCardMenuRequested (slot->id, e.getScreenPosition());

            return;
        }

        if (inCard)
        {
            //-- Phase 17: fold / unfold --------------------------------------
            // Before every other control, because a folded card has no others.
            if (getCollapseBounds (i).contains (pos))
            {
                rack.setCollapsed (slot->id, ! slot->collapsed);
                return;
            }

            if (slot->collapsed)
            {
                // The title bar is still the focus target: folding a card is
                // how you make room, not how you stop inspecting a synth.
                if (onCardFocused != nullptr)
                    onCardFocused (slot->id);

                return;
            }

            //-- Controls first -----------------------------------------------
            // A click on S / M / the fader must never be read as "inspect this
            // synth". (It used to be the other way round: the re-route check
            // came before every control and swallowed those clicks.)
            if (getCloseBounds (i).contains (pos))
            {
                if (rack.getNumSlots() > 1)
                    rack.removeSlot (slot->id);

                return;
            }

            if (getSoloBounds (i).contains (pos))
            {
                rack.setSoloed (slot->id, ! slot->soloed);
                return;
            }

            if (getMuteBounds (i).contains (pos))
            {
                rack.setMuted (slot->id, ! slot->muted);
                return;
            }

            if (getWaveformBounds (i).contains (pos))
            {
                // Hosted instruments have no oscillator to cycle through.
                if (! slot->isPlugin())
                    rack.setWaveform (slot->id, nextWaveform (slot->waveform));

                return;
            }

            if (getSendBounds (i).contains (pos))
            {
                if (onCardMenuRequested != nullptr)
                    onCardMenuRequested (slot->id, e.getScreenPosition());

                return;
            }

            if (getVolumeBounds (i).expanded (0, 6).contains (pos))
            {
                dragPart = Part::volume;
                dragCard = i;
                mouseDrag (e);
                return;
            }

            if (getPanBounds (i).expanded (0, 6).contains (pos))
            {
                dragPart = Part::pan;
                dragCard = i;
                mouseDrag (e);
                return;
            }

            //-- Phase 16: the card body picks the synth to inspect -----------
            // "Select a synth, see everything that plays on it" is the new
            // reading of this click. It is deliberately NOT destructive: now
            // that a group can be mounted on a synth, a stray click that moved
            // twenty notes would be a very expensive accident.
            //
            // Alt-click keeps the old meaning (mount the selected notes here),
            // and the card's context menu offers it in words for anyone who
            // does not know the modifier.
            if (e.mods.isAltDown())
            {
                if (onSlotChosen != nullptr)
                    onSlotChosen (slot->id);

                return;
            }

            if (onCardFocused != nullptr)
                onCardFocused (slot->id);

            return;
        }

        //-- Lane strip -------------------------------------------------------
        if (getLaneTargetBounds (i, 0).contains (pos))
        {
            rack.setAutomationTarget (slot->id, SlotAutomationTarget::filter);
            return;
        }

        if (getLaneTargetBounds (i, 1).contains (pos))
        {
            rack.setAutomationTarget (slot->id, SlotAutomationTarget::level);
            return;
        }

        if (getLaneEnableBounds (i).contains (pos))
        {
            slot->getAutomation().setEnabled (! slot->getAutomation().isEnabled());
            rack.touch();
            return;
        }
    }

    // Nothing was hit: clicking the empty part of the rack clears the spotlight
    // rather than leaving a highlight nobody can turn off.
    if (onCardFocused != nullptr)
        onCardFocused (-1);
}

void RackPanel::mouseDrag (const juce::MouseEvent& e)
{
    if (dragPart == Part::none || dragCard < 0)
        return;

    auto* slot = rack.getSlot (dragCard);

    if (slot == nullptr)
        return;

    if (dragPart == Part::volume)
    {
        const auto bounds = getVolumeBounds (dragCard);

        if (bounds.getWidth() > 0)
        {
            const float norm = juce::jlimit (0.0f, 1.0f,
                                             (float) (e.getPosition().x - bounds.getX()) / (float) bounds.getWidth());
            rack.setLevelDb (slot->id, mixerlook::dbFromNorm (norm));
        }
    }
    else if (dragPart == Part::pan)
    {
        const auto bounds = getPanBounds (dragCard);

        if (bounds.getWidth() > 0)
        {
            const float norm = juce::jlimit (0.0f, 1.0f,
                                             (float) (e.getPosition().x - bounds.getX()) / (float) bounds.getWidth());
            rack.setPan (slot->id, norm * 2.0f - 1.0f);
        }
    }
}

void RackPanel::mouseUp (const juce::MouseEvent&)
{
    dragPart = Part::none;
    dragCard = -1;
}

void RackPanel::mouseMove (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();

    int  newCard = -1;
    Part newPart = Part::none;

    for (int i = 0; i < rack.getNumSlots(); ++i)
    {
        if (! getViewArea().contains (pos) || ! getCardBounds (i).contains (pos))
            continue;

        newCard = i;

        if (getCollapseBounds (i).contains (pos))    newPart = Part::collapse;
        else if (isCollapsed (i))                    newPart = Part::body;
        else if (getSoloBounds (i).contains (pos))        newPart = Part::solo;
        else if (getMuteBounds (i).contains (pos))   newPart = Part::mute;
        else if (getCloseBounds (i).contains (pos))  newPart = Part::close;
        else if (getWaveformBounds (i).contains (pos)) newPart = Part::waveform;
        else if (getSendBounds (i).contains (pos))   newPart = Part::send;
        else if (getVolumeBounds (i).expanded (0, 6).contains (pos)) newPart = Part::volume;
        else if (getPanBounds (i).expanded (0, 6).contains (pos))    newPart = Part::pan;
        else newPart = Part::body;

        break;
    }

    const bool newAdd = getAddButtonBounds().contains (pos) && getHeight() > 200;

    setMouseCursor ((newPart == Part::volume || newPart == Part::pan)
                      ? juce::MouseCursor::LeftRightResizeCursor
                      : (newPart == Part::none || newPart == Part::body)
                          ? (newAdd ? juce::MouseCursor::PointingHandCursor
                                    : juce::MouseCursor::NormalCursor)
                          : juce::MouseCursor::PointingHandCursor);

    if (newCard != hoveredCard || newPart != hoveredPart || newAdd != hoveringAdd)
    {
        hoveredCard = newCard;
        hoveredPart = newPart;
        hoveringAdd = newAdd;
        repaint();
    }
}

void RackPanel::mouseExit (const juce::MouseEvent&)
{
    if (hoveredCard != -1 || hoveredPart != Part::none || hoveringAdd)
    {
        hoveredCard = -1;
        hoveredPart = Part::none;
        hoveringAdd = false;
        repaint();
    }
}

void RackPanel::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    if (getContentHeight() <= getViewHeight())
        return;

    const float delta = (wheel.deltaY != 0.0f ? wheel.deltaY : -wheel.deltaX)
                          * (float) getViewHeight() * 0.9f;

    if (delta != 0.0f)
        setScrollY ((int) (scrollY - delta));
}

} // namespace atomic
