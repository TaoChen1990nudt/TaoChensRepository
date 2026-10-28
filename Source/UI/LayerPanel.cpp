#include "LayerPanel.h"

#include "Util/Metrics.h"

#include "Model/Routing.h"
#include "UI/MixerLook.h"
#include "Util/Text.h"
#include "Util/Theme.h"
#include "Util/ValueMenu.h"

namespace atomic
{

namespace
{
    const juce::Colour& kPanelBackground = theme::surface1;
    const juce::Colour& kDivider        = theme::hairline;
    const juce::Colour& kLaneHeader     = theme::surface2;

    const juce::Colour& kSoloColour     = theme::accent;   // P48 11.2：独奏统一 accent（原黄）
    const juce::Colour& kMuteColour     = theme::warn;      // P48 11.1：静音统一黄色（原红）

    juce::Font boldFont (float height)
    {
        return juce::Font (juce::FontOptions{}.withHeight (atomic::ui::sp (height)).withStyle ("Bold"));
    }
}

//==============================================================================
LayerPanel::LayerPanel (LayerStack& layerStackToUse, RackModel& rackToUse)
    : stack (layerStackToUse),
      rack (rackToUse)
{
    setOpaque (true);
    stack.addListener (this);
    rack.addListener (this);

    syncLaneEditors();
}

LayerPanel::~LayerPanel()
{
    stack.removeListener (this);
    rack.removeListener (this);
}

//==============================================================================
bool LayerPanel::isExpanded (int index) const
{
    auto* layer = stack.getLayer (index);
    return layer != nullptr && layer->isAutomationExpanded() && ! layer->isCollapsed();
}

bool LayerPanel::isCollapsed (int index) const
{
    auto* layer = stack.getLayer (index);
    return layer != nullptr && layer->isCollapsed();
}

int LayerPanel::rowHeightAt (int index) const
{
    // A folded row is a title bar and nothing else: no lane, no fader row. The
    // lane stays "expanded" in the model, so unfolding the layer brings the
    // curve back exactly as it was.
    if (isCollapsed (index))
        return collapsedHeight;

    return rowHeight + (isExpanded (index) ? laneHeight : 0);
}

int LayerPanel::getContentHeight() const
{
    int total = 0;

    for (int i = 0; i < stack.getNumLayers(); ++i)
        total += rowHeightAt (i);

    return total;
}

int LayerPanel::getViewHeight() const
{
    return juce::jmax (0, getHeight() - headerHeight - footerHeight);
}

juce::Rectangle<int> LayerPanel::getViewArea() const
{
    return { 0, headerHeight, getWidth(), getViewHeight() };
}

void LayerPanel::setScrollY (int pixels)
{
    scrollY = (float) pixels;
    clampScroll();
    updateLayout();
    repaint();
}

void LayerPanel::clampScroll()
{
    const int excess = juce::jmax (0, getContentHeight() - getViewHeight());
    scrollY = (float) juce::jlimit (0, excess, (int) scrollY);
}

juce::Rectangle<int> LayerPanel::getBodyBounds (int index) const
{
    int y = headerHeight - (int) scrollY;

    for (int i = 0; i < index; ++i)
        y += rowHeightAt (i);

    return { 0, y, getWidth(), isCollapsed (index) ? collapsedHeight : rowHeight };
}

juce::Rectangle<int> LayerPanel::getRowBounds (int index) const
{
    return getBodyBounds (index).withHeight (rowHeightAt (index));
}

juce::Rectangle<int> LayerPanel::getEyeBounds (int index) const
{
    auto body = getBodyBounds (index);
    return { getWidth() - ui::px (32), body.getY() + ui::px (9), ui::px (20), ui::px (20) };
}

juce::Rectangle<int> LayerPanel::getMuteBounds (int index) const
{
    auto body = getBodyBounds (index);
    return { getWidth() - ui::px (56), body.getY() + ui::px (9), ui::px (20), ui::px (20) };
}

juce::Rectangle<int> LayerPanel::getSoloBounds (int index) const
{
    auto body = getBodyBounds (index);
    return { getWidth() - ui::px (80), body.getY() + ui::px (9), ui::px (20), ui::px (20) };
}

juce::Rectangle<int> LayerPanel::getOutputBounds (int index) const
{
    // 102, not 112: slot names are short ("Synth A" needs ~36 of the 56 design
    // units left after the dot and the caret), and the 10 units it gives up are
    // what lets the reset pill beside it use a legible font.
    auto body = getBodyBounds (index);
    return { ui::px (34), body.getY() + ui::px (39), ui::px (102), ui::px (17) };
}

juce::Rectangle<int> LayerPanel::getVolumeBounds (int index) const
{
    auto body = getBodyBounds (index);
    return { ui::px (34), body.getY() + ui::px (60), ui::px (118), ui::px (7) };
}

juce::Rectangle<int> LayerPanel::getPanBounds (int index) const
{
    auto body = getBodyBounds (index);
    return { ui::px (194), body.getY() + ui::px (60), ui::px (78), ui::px (7) };
}

juce::Rectangle<int> LayerPanel::getResetRoutingBounds (int index) const
{
    // Phase 14 removed the 自动化 badge that used to sit here, so the pill
    // moves into the gap instead of leaving a hole: it still stops short of
    // the pan readout, which is right-aligned at the end of this line.
    //
    // 52 units wide, hard against the pan fader at 194. At 42 the pill could
    // only fit "复位N" by dropping to 8.5 pt, which is under the floor where
    // CJK stops resolving — the two characters smeared into each other.
    auto body = getBodyBounds (index);
    return { ui::px (140), body.getY() + ui::px (39), ui::px (52), ui::px (17) };
}

int LayerPanel::getOverrideCount (int index) const
{
    const auto* layer = stack.getLayer (index);

    if (layer == nullptr)
        return 0;

    int n = 0;

    for (const auto& note : layer->getNotes())
        if (note.outputSlotOverride >= 0)
            ++n;

    return n;
}

juce::Rectangle<int> LayerPanel::getCollapseBounds (int index) const
{
    // Right-aligned against the S / M / eye cluster: the name has the whole
    // left half, and the fold control never moves as the name changes length.
    auto body = getBodyBounds (index);

    return { getWidth() - ui::px (104), body.getY() + ui::px (5), ui::px (20), ui::px (18) };
}

juce::Rectangle<int> LayerPanel::getAddButtonBounds() const
{
    return { ui::px (12), getHeight() - footerHeight + ui::px (14),
             juce::jmax (0, getWidth() - ui::px (24)), ui::px (32) };
}

juce::Rectangle<int> LayerPanel::getVisibilityToggleBounds() const
{
    // Phase 70: a lone sheet has no ghosts, so "all visible" and "only this one"
    // are the same state and the eye has nothing to toggle. Drawing it anyway
    // produced a button whose two positions looked identical — it read as a
    // broken control, not as "there is nothing to hide".
    if (stack.getNumLayers() <= 1)
        return {};

    // Right end of the title bar, vertically between the two header lines:
    // the title occupies the left, so this is the only free corner up here.
    return { getWidth() - ui::px (14) - ui::px (20), ui::px (16), ui::px (20), ui::px (20) };
}

bool LayerPanel::areAllLayersVisible() const
{
    // Phase 70: every layer, the active one included.
    //
    // The old version skipped it on the grounds that the sheet being edited is
    // always drawn, so its flag says nothing. That is true of the DRAWING, but
    // not of the STATE the eye is reporting — and skipping it made the answer
    // "true" for a one-layer project no matter what, which is what turned the
    // toggle into a no-op. The flag is still written for the active layer (see
    // toggleAllLayerVisibility) for the same reason it always was.
    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        const auto* layer = stack.getLayer (i);

        if (layer != nullptr && ! layer->isVisible())
            return false;
    }

    return true;
}

void LayerPanel::toggleAllLayerVisibility()
{
    // "All" is the target when the ghosts are currently hidden, and closing the
    // ghosts is the target otherwise. The active sheet is written as visible
    // too: it is drawn either way, and leaving it false would make the next
    // switch to another sheet silently hide the one you just left.
    const bool showAll = ! areAllLayersVisible();

    for (int i = 0; i < stack.getNumLayers(); ++i)
        stack.setLayerVisible (i, showAll || i == stack.getActiveLayerIndex());
}

juce::Rectangle<int> LayerPanel::getLaneCanvasBounds (int index) const
{
    auto body = getBodyBounds (index);
    return { ui::px (10), body.getY() + rowHeight + ui::px (22),
             getWidth() - ui::px (20), laneHeight - ui::px (28) };
}

juce::Rectangle<int> LayerPanel::getLaneTargetBounds (int index, int targetIndex) const
{
    auto body = getBodyBounds (index);
    return { ui::px (14) + targetIndex * ui::px (44), body.getY() + rowHeight + ui::px (3),
             ui::px (40), ui::px (15) };
}

juce::Rectangle<int> LayerPanel::getLaneEnableBounds (int index) const
{
    auto body = getBodyBounds (index);
    return { getWidth() - ui::px (58), body.getY() + rowHeight + ui::px (3),
             ui::px (44), ui::px (15) };
}

//==============================================================================
void LayerPanel::paint (juce::Graphics& g)
{
    g.fillAll (kPanelBackground);

    //-- Header ---------------------------------------------------------------
    // Phase 50: the two type roles of DESIGN.md §5 — title (11 pt bold, +0.5
    // tracking) over section (8.5 pt bold, +0.8, upper-case). The old pair was
    // 13.5 / 9.5 with no tracking at all, which is why the hierarchy had to be
    // carried by a 4 pt size jump.
    g.setColour (juce::Colours::white.withAlpha (0.92f));
    g.setFont (ui::title (11.0f));
    g.drawText (utf8 ("图层与混音"), ui::px (14), ui::px (10), ui::px (140), ui::px (20),
                juce::Justification::centredLeft, false);

    g.setColour (juce::Colours::white.withAlpha (0.30f));
    g.setFont (ui::section (8.5f));
    g.drawText ("LAYER MIX" + separatorDot() + juce::String (stack.getNumLayers()),
                ui::px (14), ui::px (27), ui::px (180), ui::px (14),
                juce::Justification::centredLeft, false);

    g.setColour (kDivider);
    g.drawHorizontalLine (headerHeight - 1, 0.0f, (float) getWidth());

    //-- Phase 65: ghost switch ----------------------------------------------
    // The same eye the rows use, in the one place that speaks for all of them.
    // Open = every sheet is showing as a ghost; struck through = only the sheet
    // being edited is on screen.
    {
        const auto eyeBox = getVisibilityToggleBounds();
        const bool all    = areAllLayersVisible();

        // Empty on a one-layer project: there is nothing to toggle (see
        // getVisibilityToggleBounds).
        if (! eyeBox.isEmpty())
        {
            if (hoveredVisibilityToggle)
            {
                g.setColour (juce::Colours::white.withAlpha (0.07f));
                g.fillRoundedRectangle (eyeBox.toFloat(), ui::pf (5));
            }

            drawEye (g, eyeBox.toFloat().reduced (ui::pf (4)),
                     juce::Colours::white.withAlpha (hoveredVisibilityToggle ? 0.95f
                                                                            : (all ? 0.72f : 0.42f)),
                     all);
        }
    }

    //-- Rows -----------------------------------------------------------------
    // Clipped to the list area: a row scrolled past the header must not paint
    // over it, and one scrolled past the footer must not land on the button.
    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (getViewArea());

        for (int i = 0; i < stack.getNumLayers(); ++i)
        {
            if (! getRowBounds (i).intersects (getViewArea()))
                continue;

            drawRow (g, i);
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

    //-- Footer ---------------------------------------------------------------
    if (getHeight() > 200)
    {
        const auto button = getAddButtonBounds().toFloat();

        g.setColour (juce::Colours::white.withAlpha (0.04f));
        g.fillRoundedRectangle (button, ui::pf (6));
        g.setColour (juce::Colours::white.withAlpha (0.12f));
        g.drawRoundedRectangle (button, ui::pf (6), 1.0f);

        g.setColour (juce::Colours::white.withAlpha (0.55f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (11.5f))));
        g.drawText (utf8 ("＋  新建图层"), button.toNearestInt(), juce::Justification::centred, false);
    }

    g.setColour (kDivider);
    g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());
}

//==============================================================================
void LayerPanel::drawRow (juce::Graphics& g, int index)
{
    auto* layer = stack.getLayer (index);

    if (layer == nullptr)
        return;

    const auto body      = getBodyBounds (index).toFloat();
    const bool isActive  = (index == stack.getActiveLayerIndex());
    const bool ghost     = layer->isVisible();   // Phase 20: "draw me as a ghost"
    const bool soloed    = layer->isSoloed();
    const bool muted     = layer->isMuted();
    const bool expanded  = layer->isAutomationExpanded();
    const auto colour    = layer->getColour();
    const bool hovered   = (hoveredRow == index);

    //-- Phase 16: does this sheet feed the synth the roll is spotlighting? ---
    const bool attached = isLayerAttachedToFocus (index);
    const auto* focusedSlot = focusSlotId >= 0 ? rack.findSlot (focusSlotId) : nullptr;
    const auto focusColour  = focusedSlot != nullptr ? focusedSlot->colour
                                                     : theme::accent;

    juce::Rectangle<float> plate (body.getX() + ui::pf (6), body.getY() + ui::pf (3),
                                  body.getWidth() - ui::pf (12), body.getHeight() - ui::pf (6));

    if (isActive)
    {
        g.setColour (colour.withAlpha (0.13f));
        g.fillRoundedRectangle (plate, 6.0f);
    }
    else if (attached)
    {
        // A tinted plate instead of the active layer's brighter one: the row is
        // not the editable sheet, it is one of the sheets feeding this synth.
        g.setColour (focusColour.withAlpha (0.09f));
        g.fillRoundedRectangle (plate, 6.0f);
    }
    else if (hovered)
    {
        g.setColour (juce::Colours::white.withAlpha (0.035f));
        g.fillRoundedRectangle (plate, 6.0f);
    }

    if (attached)
    {
        // The accent bar sits at the panel's edge, outside the layer's own bar,
        // so "active" and "on the focused synth" can both be true and both be
        // visible at once.
        g.setColour (focusColour.withAlpha (isActive ? 0.55f : 0.85f));
        g.fillRoundedRectangle (juce::Rectangle<float> (ui::pf (1.5), body.getY() + ui::pf (10),
                                                       ui::pf (2.5), body.getHeight() - ui::pf (20)),
                                ui::pf (1.2));
    }

    if (isActive)
    {
        g.setColour (colour);
        g.fillRoundedRectangle (juce::Rectangle<float> (ui::pf (4), body.getY() + ui::pf (12),
                                                       ui::pf (3), body.getHeight() - ui::pf (24)),
                                ui::pf (1.5));
    }

    const int rowY = (int) body.getY();

    // The folded row shares the same name alpha, so it is computed once, above
    // both branches rather than inside the expanded one.
    // The ACTIVE row is never dimmed for a closed eye: the eye hides ghosts, it
    // does not hide the sheet being edited, so the row would lie about it.
    const float nameAlpha = muted ? 0.38f
                                 : (isActive ? 0.96f : (ghost ? 0.74f : 0.42f));

    //-- Phase 17: the folded row ---------------------------------------------
    // A title bar and nothing else. Everything below it is a control the user
    // asked to stop looking at — but the colour, the name and "how much is on
    // this sheet" stay, because that is the part you scan a stack by.
    if (layer->isCollapsed())
    {
        mixerlook::drawCaret (g, getCollapseBounds (index).toFloat(), false,
                              juce::Colours::white.withAlpha (hovered ? 0.75f : 0.42f));

        g.setColour (isActive || ghost ? colour : colour.withAlpha (0.42f));
        g.fillRoundedRectangle (juce::Rectangle<float> (ui::pf (16), body.getY() + ui::pf (10),
                                                        ui::pf (10), ui::pf (10)), 3.0f);

        g.setColour (juce::Colours::white.withAlpha (nameAlpha));
        g.setFont (boldFont (12.0f));
        g.drawText (layer->getName(),
                    juce::Rectangle<int> (ui::px (34), rowY + ui::px (7),
                                          getWidth() - ui::px (190), ui::px (16)),
                    juce::Justification::centredLeft, false);

        g.setColour (juce::Colours::white.withAlpha (ghost ? 0.30f : 0.20f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
        g.drawText (juce::String ((int) layer->getNotes().size()) + " notes",
                    juce::Rectangle<int> (ui::px (0), rowY + ui::px (8),
                                          getWidth() - ui::px (112), ui::px (14)),
                    juce::Justification::centredRight, false);
        return;
    }

    //-- Colour chip ----------------------------------------------------------
    const auto chip = juce::Rectangle<float> (ui::pf (16), body.getY() + ui::pf (11),
                                              ui::pf (11), ui::pf (11));
    g.setColour (isActive || ghost ? colour : colour.withAlpha (0.42f));
    g.fillRoundedRectangle (chip, 3.5f);

    //-- Name -----------------------------------------------------------------
    g.setColour (juce::Colours::white.withAlpha (nameAlpha));
    g.setFont (boldFont (13.0f));
    g.drawText (layer->getName(),
                juce::Rectangle<int> (ui::px (34), rowY + ui::px (5),
                                      getWidth() - ui::px (124), ui::px (17)),
                juce::Justification::centredLeft, false);

    mixerlook::drawCaret (g, getCollapseBounds (index).toFloat(), true,
                          juce::Colours::white.withAlpha (hovered ? 0.70f : 0.34f));

    //-- Sub readout ----------------------------------------------------------
    juce::String sub = juce::String ((int) layer->getNotes().size()) + " notes"
                         + separatorDot() + mixerlook::dbText (layer->getLevelDb()) + " dB";

    if (attached && focusedSlot != nullptr)
        sub << separatorDot() << utf8 ("挂载 ") << focusedSlot->name;

    g.setColour (juce::Colours::white.withAlpha (ghost ? 0.30f : 0.20f));
    g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));
    g.drawText (sub,
                juce::Rectangle<int> (ui::px (34), rowY + ui::px (23),
                                      getWidth() - ui::px (124), ui::px (13)),
                juce::Justification::centredLeft, false);

    //-- Output dropdown ------------------------------------------------------
    drawOutputPill (g, index, hovered && hoveredPart == Part::output);

    //-- Note routing reset ----------------------------------------------------
    // Only drawn when there is something to undo: a permanently visible button
    // that usually does nothing teaches people to ignore it.
    {
        const int overrides = getOverrideCount (index);

        if (overrides > 0)
        {
            const auto pill = getResetRoutingBounds (index).toFloat();
            const bool hot  = hovered && hoveredPart == Part::resetRouting;

            g.setColour (colour.withAlpha (hot ? 0.30f : 0.16f));
            g.fillRoundedRectangle (pill, 4.0f);
            g.setColour (colour.withAlpha (hot ? 0.85f : 0.45f));
            g.drawRoundedRectangle (pill, 4.0f, 1.0f);

            // A circular arrow: the universally understood "put it back".
            const auto knob = juce::Rectangle<float> (ui::pf (7), ui::pf (7))
                                .withCentre ({ pill.getX() + ui::pf (9.5), pill.getCentreY() });

            juce::Path arc;
            arc.addCentredArc (knob.getCentreX(), knob.getCentreY(), knob.getWidth() * 0.5f,
                               knob.getHeight() * 0.5f, 0.0f, -2.4f, 2.4f, true);

            g.setColour (colour.withAlpha (hot ? 0.95f : 0.7f));
            g.strokePath (arc, juce::PathStrokeType (ui::pf (1.3), juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));

            juce::Path tip;
            tip.addTriangle (knob.getRight() - ui::pf (0.5), knob.getY() - ui::pf (0.5),
                             knob.getRight() + ui::pf (3), knob.getCentreY() + ui::pf (1),
                             knob.getRight() - ui::pf (1), knob.getCentreY() + ui::pf (3.5));
            g.fillPath (tip);

            g.setColour (juce::Colours::white.withAlpha (hot ? 0.92f : 0.66f));
            g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));
            g.drawText (utf8 ("复位") + juce::String (overrides),
                        pill.withTrimmedLeft (ui::pf (16)), juce::Justification::centredLeft, false);
        }
    }

    //-- Volume fader ---------------------------------------------------------
    {
        const auto fader = getVolumeBounds (index).toFloat();
        mixerlook::drawFader (g, fader, mixerlook::normFromDb (layer->getLevelDb()),
                              colour, hovered && hoveredPart == Part::volume);

        g.setColour (juce::Colours::white.withAlpha (0.52f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (8.5f))));
        g.drawText (mixerlook::dbText (layer->getLevelDb()),
                    juce::Rectangle<int> (ui::px (155), rowY + ui::px (54), ui::px (34), ui::px (16)),
                    juce::Justification::centredLeft, false);
    }

    //-- Pan ------------------------------------------------------------------
    {
        const auto pan = getPanBounds (index).toFloat();
        mixerlook::drawPan (g, pan, layer->getPan(), colour,
                            hovered && hoveredPart == Part::pan);

        g.setColour (juce::Colours::white.withAlpha (0.40f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (8.5f))));
        g.drawText (mixerlook::panText (layer->getPan()),
                    juce::Rectangle<int> (ui::px (194), rowY + ui::px (44), ui::px (78), ui::px (13)),
                    juce::Justification::centredRight, false);
    }

    //-- S / M / eye ----------------------------------------------------------
    mixerlook::drawBadge (g, getSoloBounds (index).toFloat(), "S", soloed, kSoloColour,
                          hovered && hoveredPart == Part::solo);
    mixerlook::drawBadge (g, getMuteBounds (index).toFloat(), "M", muted, kMuteColour,
                          hovered && hoveredPart == Part::mute);

    {
        auto eye = getEyeBounds (index).toFloat();

        if (hovered && hoveredPart == Part::eye)
        {
            g.setColour (juce::Colours::white.withAlpha (0.07f));
            g.fillRoundedRectangle (eye, 5.0f);
        }

        // Phase 20: the active sheet is always drawn, so its eye reads as open
        // whatever the ghost flag says. Drawing a closed eye on the row you are
        // editing would be the panel telling you the opposite of what the
        // canvas is doing; clicking it still sets the flag for when this row
        // becomes a ghost.
        const bool eyeOn = isActive || ghost;

        drawEye (g, eye.reduced (ui::pf (4)),
                 eyeOn ? colour.brighter (0.2f) : juce::Colours::white.withAlpha (0.30f), eyeOn);
    }

    //-- Meter ----------------------------------------------------------------
    {
        const float level = levelSource != nullptr ? levelSource (index) : 0.0f;
        mixerlook::drawMeter (g, juce::Rectangle<float> (ui::pf (34), body.getY() + ui::pf (70),
                                                         body.getWidth() - ui::pf (68), ui::pf (2)),
                              level, muted ? juce::Colours::grey : colour);
    }

    //-- Lane header ----------------------------------------------------------
    if (expanded)
        drawLaneHeader (g, index);
}

//==============================================================================
void LayerPanel::drawLaneHeader (juce::Graphics& g, int index)
{
    auto* layer = stack.getLayer (index);

    if (layer == nullptr)
        return;

    auto body = getBodyBounds (index);
    const auto strip = juce::Rectangle<float> (ui::pf (10), (float) (body.getY() + rowHeight),
                                               (float) getWidth() - ui::pf (20),
                                               (float) laneHeight - ui::pf (4));

    g.setColour (kLaneHeader);
    g.fillRoundedRectangle (strip, 5.0f);

    const auto colour = layer->getColour();
    const auto& lane = layer->getAutomation();
    const bool enabled = lane.isEnabled();
    const auto mouse = getMouseXYRelative();

    // The value the lane is asking for right now. A curve without a number is
    // decoration; with one it is a control.
    {
        const double probeBeat = playheadBeat >= 0.0 ? playheadBeat : 0.0;
        const float  value     = lane.valueAtBeat (probeBeat);

        const juce::String readout = layer->getAutomationTarget() == LayerAutomationTarget::volume
                                       ? juce::String (juce::roundToInt (value * 100.0f)) + "%"
                                       : mixerlook::panText (value * 2.0f - 1.0f);

        g.setColour (juce::Colours::white.withAlpha (enabled ? 0.48f : 0.26f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
        g.drawText (readout,
                    juce::Rectangle<int> (ui::px (104), body.getY() + rowHeight + ui::px (3),
                                          ui::px (120), ui::px (15)),
                    juce::Justification::centredLeft, false);
    }

    // Target selector: volume or pan. One lane, two possible destinations —
    // the alternative (two lanes per layer) triples the panel height.
    const char* names[2] { "VOL", "PAN" };
    const LayerAutomationTarget targets[2] { LayerAutomationTarget::volume,
                                             LayerAutomationTarget::pan };

    for (int t = 0; t < 2; ++t)
    {
        const bool selected = (layer->getAutomationTarget() == targets[t]);
        const bool hoveredHere = getLaneTargetBounds (index, t).contains (mouse);

        mixerlook::drawBadge (g, getLaneTargetBounds (index, t).toFloat(), names[t],
                              selected, colour, hoveredHere);
    }

    const bool hoveredEnable = getLaneEnableBounds (index).contains (mouse);

    mixerlook::drawBadge (g, getLaneEnableBounds (index).toFloat(),
                          enabled ? "ON" : "OFF", enabled,
                          enabled ? colour : juce::Colours::white, hoveredEnable);
}

//==============================================================================
void LayerPanel::drawOutputPill (juce::Graphics& g, int index, bool hovered) const
{
    auto* layer = stack.getLayer (index);

    if (layer == nullptr)
        return;

    // Resolved, not configured: if the configured synth was deleted this shows
    // the synth that will actually play, which is the only useful answer.
    const auto route = routing::resolve (*layer, NoteEvent{}, rack);
    const auto slotColour = rack.getSlotColour (route.slotId);
    const auto slotName = rack.getSlotName (route.slotId);

    auto pill = getOutputBounds (index).toFloat();

    g.setColour (juce::Colours::white.withAlpha (hovered ? 0.10f : 0.05f));
    g.fillRoundedRectangle (pill, 5.0f);

    g.setColour (slotColour.withAlpha (hovered ? 0.70f : 0.38f));
    g.drawRoundedRectangle (pill, 5.0f, 1.0f);

    g.setColour (route.isValid() ? slotColour : juce::Colours::grey);
    g.fillEllipse (juce::Rectangle<float> (ui::pf (5), ui::pf (5))
                     .withCentre ({ pill.getX() + ui::pf (9), pill.getCentreY() }));

    g.setColour (juce::Colours::white.withAlpha (hovered ? 0.92f : 0.70f));
    g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
    g.drawText (route.isValid() ? slotName : juce::String::fromUTF8 ("\xe6\x97\xa0\xe6\xba\x90"),
                pill.reduced (ui::pf (18), 0.0f).withTrimmedRight (ui::pf (8)),
                juce::Justification::centredLeft, false);

    juce::Path caret;
    const float cx = pill.getRight() - ui::pf (9);
    const float cy = pill.getCentreY() - ui::pf (1);
    caret.addTriangle (cx - ui::pf (3), cy - ui::pf (1.5), cx + ui::pf (3), cy - ui::pf (1.5),
                       cx, cy + ui::pf (2.5));

    g.setColour (juce::Colours::white.withAlpha (hovered ? 0.80f : 0.40f));
    g.fillPath (caret);
}

//==============================================================================
void LayerPanel::drawEye (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour colour, bool open) const
{
    juce::Path outline;
    outline.startNewSubPath (r.getX(), r.getCentreY());
    outline.quadraticTo (r.getCentreX(), r.getY(), r.getRight(), r.getCentreY());
    outline.quadraticTo (r.getCentreX(), r.getBottom(), r.getX(), r.getCentreY());

    g.setColour (colour);
    g.strokePath (outline, juce::PathStrokeType (1.2f));

    if (open)
    {
        g.fillEllipse (juce::Rectangle<float> (3.0f, 3.0f).withCentre (r.getCentre()));
    }
    else
    {
        g.drawLine (r.getX() + 1.0f, r.getBottom() - 1.0f, r.getRight() - 1.0f, r.getY() + 1.0f, 1.2f);
    }
}

//==============================================================================
void LayerPanel::showOutputMenu (int index)
{
    auto* layer = stack.getLayer (index);

    if (layer == nullptr || rack.getNumSlots() == 0)
        return;

    const int currentId = routing::layerSlotId (*layer, rack);

    juce::PopupMenu menu;
    menu.addSectionHeader (utf8 ("图层默认输出"));

    for (int i = 0; i < rack.getNumSlots(); ++i)
        if (auto* slot = rack.getSlot (i))
        {
            // Phase 27: a sheet of notes needs something that plays notes. A
            // sampler slot holds one audio file and has no oscillator, so it is
            // not a legal default output — see routing::layerSlotId, which
            // skips it for exactly the same reason.
            if (slot->isSampler())
                continue;

            menu.addItem (100 + slot->id, slot->name, true, slot->id == currentId);
        }

    juce::Component::SafePointer<LayerPanel> safeThis (this);

    menu.showMenuAsync (juce::PopupMenu::Options()
                          .withTargetComponent (this)
                          .withTargetScreenArea (localAreaToGlobal (getOutputBounds (index))),
                        [safeThis, index] (int result)
                        {
                            if (safeThis == nullptr || result < 100)
                                return;

                            safeThis->stack.setLayerOutputSlot (index, result - 100);
                        });
}

//==============================================================================
void LayerPanel::syncLaneEditors()
{
    const int numLayers = stack.getNumLayers();

    if ((int) laneEditors.size() != numLayers)
    {
        laneEditors.clear();

        for (int i = 0; i < numLayers; ++i)
        {
            auto editor = std::make_unique<AutomationLaneComponent>();

            // Resolver, not a pointer: layers can be removed under us.
            editor->setLaneSource ([this, i]() -> AutomationLane*
            {
                auto* layer = stack.getLayer (i);
                return layer != nullptr ? &layer->getAutomation() : nullptr;
            });

            editor->onChange = [this] { stack.touchLayers(); };

            addChildComponent (*editor);
            laneEditors.push_back (std::move (editor));
        }
    }

    updateLayout();
}

void LayerPanel::updateLayout()
{
    for (int i = 0; i < (int) laneEditors.size(); ++i)
    {
        auto* layer = stack.getLayer (i);
        auto& editor = *laneEditors[(size_t) i];

        if (layer == nullptr || ! layer->isAutomationExpanded())
        {
            editor.setVisible (false);
            continue;
        }

        editor.setAccentColour (layer->getColour());
        editor.setTimeline (timeline::defaultLengthBeats);
        editor.setBounds (getLaneCanvasBounds (i));
        editor.setVisible (true);
    }
}

//==============================================================================
void LayerPanel::setFocusSlotId (int slotId)
{
    if (focusSlotId == slotId)
        return;

    focusSlotId = slotId;
    repaint();
}

bool LayerPanel::isLayerAttachedToFocus (int index) const
{
    const auto* layer = const_cast<LayerPanel*> (this)->stack.getLayer (index);

    if (layer == nullptr || focusSlotId < 0)
        return false;

    // One rule, the engine's: a sheet is "on" this synth when at least one of
    // its notes resolves there. That folds in the layer default, a group mount
    // and a single note-level override without inventing a second priority
    // order — which is exactly the kind of drift this codebase avoids.
    for (const auto& note : layer->getNotes())
        if (routing::resolve (*layer, note, rack).slotId == focusSlotId)
            return true;

    return false;
}

void LayerPanel::setPlayheadBeat (double beats)
{
    playheadBeat = beats;

    for (int i = 0; i < (int) laneEditors.size(); ++i)
    {
        laneEditors[(size_t) i]->setPlayheadBeat (beats);

        // Only the lane header carries a value readout; repainting the whole
        // panel at 60 Hz would be rude to the CPU for no reason.
        if (isExpanded (i))
        {
            const auto body = getBodyBounds (i);
            repaint (juce::Rectangle<int> (10, body.getY() + rowHeight, getWidth() - 20, 22));
        }
    }
}

void LayerPanel::layersChanged()
{
    syncLaneEditors();
    repaint();
}

void LayerPanel::resized()
{
    updateLayout();
}

//==============================================================================
void LayerPanel::mouseDown (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();

    if (getHeight() > 200 && getAddButtonBounds().contains (pos))
    {
        const int index = stack.getNumLayers();
        const int newIndex = stack.addLayer ("Layer " + juce::String (index + 1),
                                             palette::nextLayerColour (index));

        // A new layer gets an explicit default rather than "unassigned".
        stack.setLayerOutputSlot (newIndex, rack.getFirstSlotId());
        return;
    }

    // Phase 65: the title-bar eye is above the list, so it is tested before the
    // "nothing outside the view area" guard below.
    if (getVisibilityToggleBounds().contains (pos))
    {
        toggleAllLayerVisibility();
        return;
    }

    // Nothing outside the list area is a row: a row scrolled under the header
    // would otherwise still be clickable through it.
    if (! getViewArea().contains (pos))
        return;

    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        if (! getRowBounds (i).contains (pos))
            continue;

        auto* layer = stack.getLayer (i);

        if (layer == nullptr)
            return;

        if (e.mods.isPopupMenu())
        {
            // Phase 20: a right click ON a fader is about that fader. Reset it,
            // or step through to the row's own menu if the click was meant for
            // the automation side of things.
            const bool onVolume = getVolumeBounds (i).expanded (0, 6).contains (pos);
            const bool onPan    = getPanBounds (i).expanded (0, 6).contains (pos);

            if (onVolume || onPan)
            {
                const auto screenPos = e.getScreenPosition();
                const int  index     = i;

                ui::showValueMenu (this, screenPos,
                                   layer->getName() + (onVolume ? utf8 (" · 音量")
                                                                : utf8 (" · 声相")),
                                   onVolume ? mixerlook::dbText (layer->getLevelDb()) + " dB"
                                            : mixerlook::panText (layer->getPan()),
                                   [this, index, onVolume]
                                   {
                                       if (onVolume)
                                           stack.setLayerLevelDb (index, 0.0f);   // unity
                                       else
                                           stack.setLayerPan (index, 0.0f);       // centre
                                   },
                                   // Phase 21: a real sub-menu instead of an
                                   // item that opened a second menu — the
                                   // curve list appears on hover now, exactly
                                   // like the rack's VST3 sub-menu.
                                   [this, index] (juce::PopupMenu& m)
                                   {
                                       juce::PopupMenu sub;

                                       if (buildAutomationSubmenu != nullptr)
                                           buildAutomationSubmenu (sub, index);

                                       m.addSubMenu (utf8 ("自动化"), sub, true);
                                   },
                                   [this, index] (int result)
                                   {
                                       if (onAutomationChosen != nullptr)
                                           onAutomationChosen (index, result);
                                   });

                return;
            }

            stack.setActiveLayerIndex (i);

            if (onAutomationRequested != nullptr)
                onAutomationRequested (i, e.getScreenPosition());

            return;
        }

        // Phase 20: the eye is a GHOST switch, so it must not also make the row
        // active. "Show me what the pad is playing while I edit the lead" is a
        // request about a layer you are NOT on; activating it first would swap
        // the sheet out from under the click and hide what it just revealed.
        if (! layer->isCollapsed() && getEyeBounds (i).contains (pos))
        {
            stack.setLayerVisible (i, ! layer->isVisible());
            return;
        }

        stack.setActiveLayerIndex (i);

        //-- Phase 17: fold / unfold ------------------------------------------
        if (getCollapseBounds (i).contains (pos))
        {
            layer->setCollapsed (! layer->isCollapsed());
            stack.touchLayers();
            return;
        }

        if (layer->isCollapsed())
            return;                  // a title bar has no other controls

        if (getSoloBounds (i).contains (pos))
        {
            stack.setLayerSoloed (i, ! layer->isSoloed());
            return;
        }

        if (getMuteBounds (i).contains (pos))
        {
            stack.setLayerMuted (i, ! layer->isMuted());
            return;
        }

        if (getOutputBounds (i).contains (pos))
        {
            showOutputMenu (i);
            return;
        }

        if (getOverrideCount (i) > 0 && getResetRoutingBounds (i).contains (pos))
        {
            if (onResetNoteRouting != nullptr)
                onResetNoteRouting (i);

            return;
        }

        if (layer->isAutomationExpanded())
        {
            if (getLaneTargetBounds (i, 0).contains (pos))
            {
                stack.setLayerAutomationTarget (i, LayerAutomationTarget::volume);

                if (onAutomationTargetChosen != nullptr)
                    onAutomationTargetChosen (i, 0);

                return;
            }

            if (getLaneTargetBounds (i, 1).contains (pos))
            {
                stack.setLayerAutomationTarget (i, LayerAutomationTarget::pan);

                if (onAutomationTargetChosen != nullptr)
                    onAutomationTargetChosen (i, 1);

                return;
            }

            if (getLaneEnableBounds (i).contains (pos))
            {
                layer->getAutomation().setEnabled (! layer->getAutomation().isEnabled());
                stack.touchLayers();
                return;
            }
        }

        // Faders: value follows the pointer on press, then on every drag.
        if (getVolumeBounds (i).expanded (0, 6).contains (pos))
        {
            dragPart = Part::volume;
            dragRow  = i;
            mouseDrag (e);
            return;
        }

        if (getPanBounds (i).expanded (0, 6).contains (pos))
        {
            dragPart = Part::pan;
            dragRow  = i;
            mouseDrag (e);
            return;
        }

        return;
    }
}

void LayerPanel::mouseDrag (const juce::MouseEvent& e)
{
    if (dragPart == Part::none || dragRow < 0)
        return;

    if (dragPart == Part::volume)
    {
        const auto bounds = getVolumeBounds (dragRow);

        if (bounds.getWidth() > 0)
        {
            const float norm = juce::jlimit (0.0f, 1.0f,
                                             (float) (e.getPosition().x - bounds.getX()) / (float) bounds.getWidth());
            stack.setLayerLevelDb (dragRow, mixerlook::dbFromNorm (norm));
        }
    }
    else if (dragPart == Part::pan)
    {
        const auto bounds = getPanBounds (dragRow);

        if (bounds.getWidth() > 0)
        {
            const float norm = juce::jlimit (0.0f, 1.0f,
                                             (float) (e.getPosition().x - bounds.getX()) / (float) bounds.getWidth());
            stack.setLayerPan (dragRow, norm * 2.0f - 1.0f);
        }
    }
}

void LayerPanel::mouseUp (const juce::MouseEvent&)
{
    dragPart = Part::none;
    dragRow  = -1;
}

//==============================================================================
void LayerPanel::mouseMove (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();

    // Phase 65: the title-bar eye. Answered first and returned, so the row
    // hit-test below cannot claim a click that lands in the header.
    if (getVisibilityToggleBounds().contains (pos))
    {
        // Leaving a row behind: the pointer is in the header now, so the row
        // highlight has to go with it, or the last hovered row stays lit while
        // the pointer is nowhere near it.
        const bool dirty = ! hoveredVisibilityToggle
                             || hoveredRow != -1 || hoveredPart != Part::none;

        hoveredVisibilityToggle = true;
        hoveredRow  = -1;
        hoveredPart = Part::none;

        if (dirty)
            repaint();

        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        return;
    }

    if (hoveredVisibilityToggle)
    {
        hoveredVisibilityToggle = false;
        repaint();
    }

    int  newRow  = -1;
    Part newPart = Part::none;

    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        if (! getViewArea().contains (pos) || ! getRowBounds (i).contains (pos))
            continue;

        newRow = i;

        if (getCollapseBounds (i).contains (pos))  newPart = Part::collapse;
        else if (isCollapsed (i))                  newPart = Part::body;
        else if (getEyeBounds (i).contains (pos))       newPart = Part::eye;
        else if (getSoloBounds (i).contains (pos)) newPart = Part::solo;
        else if (getMuteBounds (i).contains (pos)) newPart = Part::mute;
        else if (getOutputBounds (i).contains (pos)) newPart = Part::output;
        else if (getOverrideCount (i) > 0 && getResetRoutingBounds (i).contains (pos))
            newPart = Part::resetRouting;
        else if (getVolumeBounds (i).expanded (0, 6).contains (pos)) newPart = Part::volume;
        else if (getPanBounds (i).expanded (0, 6).contains (pos))    newPart = Part::pan;
        else newPart = Part::body;

        break;
    }

    setMouseCursor ((newPart == Part::volume || newPart == Part::pan)
                      ? juce::MouseCursor::LeftRightResizeCursor
                      : (newPart == Part::none || newPart == Part::body)
                          ? juce::MouseCursor::NormalCursor
                          : juce::MouseCursor::PointingHandCursor);

    if (newRow != hoveredRow || newPart != hoveredPart)
    {
        hoveredRow  = newRow;
        hoveredPart = newPart;
        repaint();
    }
}

void LayerPanel::mouseExit (const juce::MouseEvent&)
{
    bool dirty = hoveredVisibilityToggle;

    hoveredVisibilityToggle = false;

    if (hoveredRow != -1 || hoveredPart != Part::none)
    {
        hoveredRow  = -1;
        hoveredPart = Part::none;
        dirty = true;
    }

    if (dirty)
        repaint();
}

void LayerPanel::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    if (getContentHeight() <= getViewHeight())
        return;

    // A plain wheel scrolls the list. There is no horizontal axis to steal from
    // here, so a sideways-only gesture is treated as vertical rather than
    // silently dropped.
    const float delta = (wheel.deltaY != 0.0f ? wheel.deltaY : -wheel.deltaX)
                          * (float) getViewHeight() * 0.9f;

    if (delta != 0.0f)
        setScrollY ((int) (scrollY - delta));
}

} // namespace atomic
