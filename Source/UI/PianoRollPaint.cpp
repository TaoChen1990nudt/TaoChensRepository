#include "PianoRollComponent.h"
#include "PianoRollInternals.h"

#include "Util/Metrics.h"
#include "Util/SharedRules.h"

#include "Media/SampleLibrary.h"
#include "Util/Text.h"
#include "Util/Theme.h"

#include <limits>
#include <map>
#include <set>

namespace atomic
{
using namespace roll;


void PianoRollComponent::paint (juce::Graphics& g)
{
    g.fillAll (kRollBackground);

    const auto area = getGridArea();

    g.setColour (kGridBackground);
    g.fillRect (area);

    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (area.toNearestInt());

        drawPitchRows (g);
        drawTimeGrid (g);
        drawGridLines (g);
        drawGroupBands (g);       // Phase 16: a frame around each group
        drawSamples (g);          // Phase 17: audio blocks sit under the notes
        drawNotes (g);
        drawGroupFrames (g);      // Phase 57: the frame's outline, ON the members
        drawGroupLabels (g);      // ... and their name pills in front
        drawFocusHighlights (g);  // the focused synth's notes, on top of all
        drawSpotlightExitChip (g); // Phase 29: and the way back out of the veil
        drawMarquee (g);
        drawDropPreview (g);      // Phase 17: where a dragged sample would land
        drawPlayhead (g);
    }

    drawToolbar (g);
    drawRuler (g);
    drawKeyboard (g);
    drawScrollBar (g);

    // The inline note automation box floats above the notes and is clipped to
    // the grid: it is anchored under a note, so it must not creep over the
    // keyboard or the ruler when that note sits near an edge.
    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (area.toNearestInt());
        drawNoteAutomationBox (g);
        drawGroupAutomationBox (g);   // Phase 21: the same, for a group
        drawSampleAutomationBox (g);  // Phase 49: and for a clip's own curve
    }

    // Seams between the panes.
    g.setColour (kOutline);
    g.drawVerticalLine (keyboardWidth, 0.0f, (float) getHeight());
    g.drawHorizontalLine (toolbarHeight, 0.0f, (float) getWidth());
    g.drawHorizontalLine (toolbarHeight + rulerHeight, 0.0f, (float) getWidth());
    g.drawHorizontalLine (getHeight() - scrollbarHeight, 0.0f, (float) getWidth());
}


void PianoRollComponent::drawToolbar (juce::Graphics& g)
{
    auto bar = getToolbarArea();

    g.setColour (kRulerBackground);
    g.fillRect (bar);

    // Phase 8: icons instead of words. A DAW tool strip is read at a glance
    // while the eye is on the notes, and four two-character labels need a
    // second look every time.
    // Phase 51: the knife is the fifth tool — a mode, exactly like the pencil.
    const Tool tools[5] { Tool::pencil, Tool::eraser, Tool::select, Tool::hand, Tool::knife };

    const auto tint = theme::accent;

    for (int i = 0; i < 5; ++i)
    {
        const bool active  = tool == tools[i];
        const bool hovered = hoveredTool == i;
        const auto bounds  = getToolButtonBounds (i).toFloat();

        g.setColour (active ? tint.withAlpha (0.22f)
                            : juce::Colours::white.withAlpha (hovered ? 0.12f : 0.05f));
        g.fillRoundedRectangle (bounds, ui::pf (4));

        if (active)
        {
            g.setColour (tint.withAlpha (0.85f));
            g.drawRoundedRectangle (bounds.reduced (0.5f), ui::pf (4), 1.0f);
        }

        drawToolIcon (g, i, bounds, active ? juce::Colours::white : kText);
    }

    //-- Copy-state button -----------------------------------------------------
    // Not a mode: an action with a lit "loaded" state. It says "画笔状态" on it
    // because a dropper icon alone reads as "pick colour" to everyone who has
    // used a paint program.
    {
        const auto bounds  = getToolButtonBounds (5).toFloat();
        const bool hovered = hoveredTool == 5;
        const bool loaded  = hasPaintState;

        g.setColour (loaded ? tint.withAlpha (0.22f)
                            : juce::Colours::white.withAlpha (hovered ? 0.12f : 0.05f));
        g.fillRoundedRectangle (bounds, ui::pf (4));

        g.setColour (loaded ? tint.withAlpha (0.85f)
                            : juce::Colours::white.withAlpha (hovered ? 0.20f : 0.10f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), ui::pf (4), 1.0f);

        drawToolIcon (g, 5, bounds.withTrimmedLeft (ui::pf (6)).withWidth (ui::pf (18)),
                      loaded ? juce::Colours::white : kText);

        g.setColour (loaded ? juce::Colours::white : kText);
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
        g.drawText (utf8 ("状态"),
                    bounds.withTrimmedLeft (ui::pf (24)), juce::Justification::centredLeft, false);
    }

    //-- Phase 16: group button ----------------------------------------------
    // An action, not a mode, exactly like the dropper above it. It stays lit
    // while the selection is inside one group, because that is when clicking it
    // again is the "pick the whole group up" gesture.
    {
        const auto bounds  = getToolButtonBounds (6).toFloat();
        const bool hovered = hoveredTool == 6;
        const bool inGroup = getSelectedGroupId() >= 0;
        const bool enabled = hasAnySelection();

        g.setColour (inGroup ? tint.withAlpha (0.22f)
                             : juce::Colours::white.withAlpha (hovered && enabled ? 0.12f : 0.05f));
        g.fillRoundedRectangle (bounds, ui::pf (4));

        g.setColour (inGroup ? tint.withAlpha (0.85f)
                             : juce::Colours::white.withAlpha (enabled ? 0.20f : 0.08f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), ui::pf (4), 1.0f);

        drawToolIcon (g, 6, bounds.withTrimmedLeft (ui::pf (6)).withWidth (ui::pf (18)),
                      inGroup ? juce::Colours::white : kText.withAlpha (enabled ? 1.0f : 0.4f));

        g.setColour (inGroup ? juce::Colours::white : kText.withAlpha (enabled ? 1.0f : 0.45f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
        g.drawText (utf8 ("编组"),
                    bounds.withTrimmedLeft (ui::pf (24)), juce::Justification::centredLeft, false);
    }

    //-- Phase 20: ungroup ----------------------------------------------------
    // Same shape as 编组, dimmed until there is a selection — the pair reads as
    // one control with two directions, which is exactly what it is. Phase 21
    // put it directly right of 编组, ahead of the magnet.
    {
        const auto bounds  = getToolButtonBounds (7).toFloat();
        const bool hovered = hoveredTool == 7;
        const bool enabled = hasAnySelection();
        const bool grouped = getSelectedGroupId() >= 0;

        g.setColour (juce::Colours::white.withAlpha (hovered && enabled ? 0.12f : 0.05f));
        g.fillRoundedRectangle (bounds, ui::pf (4));

        g.setColour (juce::Colours::white.withAlpha (enabled ? 0.20f : 0.08f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), ui::pf (4), 1.0f);

        drawToolIcon (g, 7, bounds.withTrimmedLeft (ui::pf (6)).withWidth (ui::pf (18)),
                      kText.withAlpha (enabled ? 1.0f : 0.4f));

        g.setColour (kText.withAlpha (enabled && grouped ? 1.0f : (enabled ? 0.72f : 0.45f)));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
        g.drawText (utf8 ("解组"),
                    bounds.withTrimmedLeft (ui::pf (24)), juce::Justification::centredLeft, false);
    }

    //-- Phase 18: snap toggle ------------------------------------------------
    // A toggle, not an action: it stays lit for as long as the grid is on, and
    // it switches the AUTOMATION window's grid too. One switch, one answer —
    // two independent snap flags is how a project ends up with notes on the
    // grid and curves half a tick off it.
    {
        const auto bounds  = getToolButtonBounds (8).toFloat();
        const bool hovered = hoveredTool == 8;
        const bool on      = snapEnabled;

        g.setColour (on ? tint.withAlpha (0.22f)
                        : juce::Colours::white.withAlpha (hovered ? 0.12f : 0.05f));
        g.fillRoundedRectangle (bounds, ui::pf (4));

        g.setColour (on ? tint.withAlpha (0.85f)
                        : juce::Colours::white.withAlpha (0.20f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), ui::pf (4), 1.0f);

        // Phase 20: a magnet instead of the 3x3 dot grid. "Snap" is a thing
        // every DAW draws as a magnet, and a dot grid reads as "grid view".
        drawToolIcon (g, 7, bounds.withTrimmedLeft (ui::pf (7)).withWidth (ui::pf (18)),
                      on ? juce::Colours::white : kText.withAlpha (0.75f));

        g.setColour (on ? juce::Colours::white : kText.withAlpha (0.75f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
        g.drawText (utf8 ("吸附"),
                    bounds.withTrimmedLeft (ui::pf (26)), juce::Justification::centredLeft, false);
    }

    // A short reminder of what the current tool does, so the mode is never a
    // mystery after a click. With a paint state loaded, saying so matters more
    // than describing the tool: every click from now on stamps that note.
    const char* hints[4] { "单击空白处画音符 · 双击音符编辑包络",
                           "单击音符删除",
                           "拖动音符移动 · 空白处框选",
                           "拖动平移视图 · 滚轮上下 · Ctrl+滚轮或拖标尺缩放" };

    const auto hintArea = bar.withTrimmedLeft (getToolButtonBounds (numToolButtons - 1).getRight()
                                                 + ui::px (14));

    if (statusText.isNotEmpty())
    {
        // Phase 22: copy/paste are silent verbs. The tool hint steps aside for
        // a moment so the strip can say what actually happened.
        g.setColour (tint);
        g.setFont (boldFont (10.0f));
        g.drawText (statusText, hintArea, juce::Justification::centredLeft, true);
    }
    else if (hasPaintState)
    {
        g.setColour (tint.withAlpha (0.85f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.0f))));
        g.drawText (utf8 ("画笔状态：")
                      + juce::String (paintState.lengthBeats, 2) + utf8 (" 拍 · vel ")
                      + juce::String (paintState.velocity, 2)
                      + (paintState.envelope().isEnabled() ? utf8 (" · 带包络") : juce::String())
                      + (paintState.outputSlotOverride >= 0
                           ? utf8 (" · 越级 ") + rack.getSlotName (paintState.outputSlotOverride)
                           : juce::String())
                      + utf8 ("   （再点一次清除）"),
                    hintArea, juce::Justification::centredLeft, true);
    }
    else
    {
        for (int i = 0; i < 4; ++i)
            if (tools[i] == tool)
            {
                // Phase 16: with a selection, the strip also says what it is —
                // the count and whether it is already one group. That is the
                // readout that tells you whether the 编组 button will do
                // anything at all.
                juce::String text (utf8 (hints[i]));

                if (! selection.empty())
                {
                    const int groupId = getSelectedGroupId();

                    text << utf8 ("   ·   ") << juce::String ((int) selection.size())
                         << utf8 (" 个音符");

                    if (groupId >= 0)
                        text << utf8 (" · 已编组");
                }

                g.setColour (juce::Colours::white.withAlpha (0.38f));
                g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.0f))));
                g.drawText (text, hintArea, juce::Justification::centredLeft, true);
            }
    }

    // Scroll position readout: the timeline now grows, so "how long is it" is a
    // number that changes while you work and has to be readable.
    g.setColour (kText.withAlpha (0.7f));
    g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.0f))));
    g.drawText (juce::String (scrollBeats, 1) + " / " + juce::String (timelineBeats, 0) + " beat",
                bar.withTrimmedRight (12), juce::Justification::centredRight, false);
}


void PianoRollComponent::drawScrollBar (juce::Graphics& g)
{
    auto area = getScrollBarArea();

    g.setColour (kRulerBackground);
    g.fillRect (area);

    const auto track = area.reduced (2).toFloat();
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillRoundedRectangle (track, track.getHeight() * 0.5f);

    const auto thumb = getScrollThumb();
    g.setColour (theme::accent.withAlpha (dragMode == DragMode::scrollThumb ? 0.75f : 0.45f));
    g.fillRoundedRectangle (thumb, thumb.getHeight() * 0.5f);
}


void PianoRollComponent::drawPitchRows (juce::Graphics& g)
{
    const auto area = getGridArea();

    const int topPitch    = contentYToPitch (scrollY);
    const int bottomPitch = contentYToPitch (scrollY + area.getHeight());

    for (int pitch = bottomPitch; pitch <= topPitch; ++pitch)
    {
        if (! isBlackKey (pitch))
            continue;

        g.setColour (kBlackRowColour);
        g.fillRect (area.getX(), screenYForPitch (pitch), area.getWidth(), keyHeight);
    }
}


void PianoRollComponent::drawTimeGrid (juce::Graphics& g)
{
    // A second is a musical quantity once a tempo exists: at 120 BPM it is two
    // beats, at 60 BPM one, at 180 BPM three. Drawing it means a tempo change
    // is immediately visible in the grid instead of only being audible.
    const double beatsPerSecond = bpm / 60.0;
    const double secondsPerBeat = 1.0 / beatsPerSecond;

    if (secondsPerBeat <= 1.0e-6)
        return;

    // Below about 20 px a second the lines would just muddy the 1/16 grid.
    const double pixelsPerSecond = secondsPerBeat * (double) beatWidth;

    if (pixelsPerSecond < 20.0)
        return;

    const auto area = getGridArea();
    const double visibleBeats = (double) area.getWidth() / beatWidth;

    const int firstSecond = (int) std::floor (scrollBeats / beatsPerSecond);
    const int lastSecond  = (int) std::ceil ((scrollBeats + visibleBeats) / beatsPerSecond);

    g.setColour (theme::accent.withAlpha (0.075f));

    for (int second = firstSecond; second <= lastSecond; ++second)
    {
        const double beat = (double) second * beatsPerSecond;

        if (beat < 0.0 || beat > timelineBeats)
            continue;

        g.drawVerticalLine ((int) std::floor (screenXForBeat (beat)),
                            area.getY(), area.getBottom());
    }
}


void PianoRollComponent::drawGridLines (juce::Graphics& g)
{
    const auto area = getGridArea();

    // Phase 87: the loop, seen from the lanes.
    //
    // The strip in the ruler is where the loop is EDITED; this is where it is
    // READ. Without it, "which bars am I repeating" is a question you answer by
    // looking at a nine-pixel bar and counting. Drawn under the lines and under
    // everything else, at an alpha that tints the lanes without lying about the
    // colour of a clip.
    if (loopRegion.isUsable())
    {
        const auto left  = juce::jmax (area.getX(), screenXForBeat (loopRegion.startBeat));
        const auto right = juce::jmin (area.getRight(), screenXForBeat (loopRegion.endBeat));

        if (right > left)
        {
            g.setColour (theme::accent.withAlpha (0.08f));
            g.fillRect (juce::Rectangle<float> (left, area.getY(), right - left, area.getHeight()));

            // Two edges, because a tint alone is ambiguous next to a group
            // bracket: "does the loop start here or does the group?". The lines
            // are the same answer the strip above gives, drawn down the lanes.
            g.setColour (theme::accent.withAlpha (0.22f));
            g.drawVerticalLine ((int) std::floor (left), area.getY(), area.getBottom());
            g.drawVerticalLine ((int) std::floor (right), area.getY(), area.getBottom());
        }
    }

    // Phase 68: the list already reflects the current step, so "skip the thin
    // lines when they are too close together" is a property of the geometry
    // rather than a check the painter has to remember.
    std::vector<double> lines;
    collectGridLines (scrollBeats, scrollBeats + (double) area.getWidth() / beatWidth, lines);

    for (double beat : lines)
    {
        const float x = std::floor (screenXForBeat (beat)) + 0.5f;

        g.setColour (juce::Colours::white.withAlpha (gridLineAlpha (beat, beatsPerBar)));
        g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
    }
}


void PianoRollComponent::drawNotes (juce::Graphics& g)
{
    const auto area = getGridArea();
    const int activeIndex = stack.getActiveLayerIndex();

    // The playhead's position through a note is the same everywhere; computing
    // it here means the drawing code has no transport state of its own.
    auto progressOf = [this] (const NoteEvent& note)
    {
        if (! transportActive)
            return -1.0;

        return envelopeProgress (note, playheadBeat);
    };

    // --- Pass 1: every visible NON-active layer, at 30% ----------------------
    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        if (i == activeIndex)
            continue;

        auto* layer = stack.getLayer (i);

        if (! isLayerShown (i))
            continue;

        for (const auto& note : layer->getNotes())
        {
            if (! getNoteBounds (note).intersects (area))
                continue;

            drawNote (g, note, layer->getColour(), ghostAlphaValue, false, false, progressOf (note));
        }
    }

    // --- Pass 2: the active layer, fully opaque, drawn last so it sits on top -
    // The eye button does not hide this one: it is the sheet being edited, and
    // an editor that can hide its own contents is a bug, not a feature.
    if (auto* active = stack.getActiveLayer())
    {
        for (const auto& note : active->getNotes())
        {
            if (! getNoteBounds (note).intersects (area))
                continue;

            drawNote (g, note, active->getColour(), solidAlpha,
                      isSelected (note.id), note.id == hoveredNoteId, progressOf (note));
        }
    }

    // --- Pass 3: there isn't one any more -----------------------------------
    // Phase 30 removed the spotlight's 46% black veil. Dimming the whole sheet
    // to answer "which notes are on this synth" made the sheet unusable for
    // everything else — you could not read, select or edit anything until you
    // found a way out. The focused notes are now marked POSITIVELY instead
    // (a tinted wash, a ring and a tab in the slot's colour, drawn in
    // drawFocusHighlights); nothing on the grid gets darker, so nothing becomes
    // unreadable.
}


//==============================================================================
//  Phase 16: group marks
//==============================================================================
void PianoRollComponent::drawGroupBands (juce::Graphics& g)
{
    const int activeIndex = stack.getActiveLayerIndex();

    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        auto* layer = stack.getLayer (i);

        if (! isLayerShown (i) || layer->getGroups().empty())
            continue;

        const bool  onActive = (i == activeIndex);
        const float baseAlpha = onActive ? 1.0f : ghostAlphaValue + 0.25f;

        // Phase 58: one band per APPEARANCE. A group reused four times is four
        // rectangles on the sheet and one definition behind them.
        for (const auto& group : layer->getGroups())
            for (int which : layer->getGroupOccurrences (group.getId()))
            {
                // groupFrameOf, not getGroupContentBounds: the box the self-test
                // reads and the box that is painted have to be the same call, or
                // "the frame sits on the block" becomes a claim about a function
                // nobody draws with.
                const auto content = groupFrameOf (i, group.getId(), which);

                if (content.isEmpty())
                    continue;

                const bool focused = isGroupFocused (i, group.getId(), which);
                const auto colour  = group.getColour();

                // Phase 57: the band is the members' box, not the members' box
                // plus a margin — see drawGroupFrames for the outline it shares.
                const auto frame   = content;

                g.setColour (colour.withAlpha (baseAlpha * (focused ? 0.10f : 0.05f)));
                g.fillRoundedRectangle (frame, ui::pf (4));
            }
    }
}


void PianoRollComponent::drawGroupFrames (juce::Graphics& g)
{
    const int activeIndex = stack.getActiveLayerIndex();

    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        auto* layer = stack.getLayer (i);

        if (! isLayerShown (i) || layer->getGroups().empty())
            continue;

        const bool  onActive = (i == activeIndex);
        const float baseAlpha = onActive ? 1.0f : ghostAlphaValue + 0.25f;

        for (const auto& group : layer->getGroups())
            for (int which : layer->getGroupOccurrences (group.getId()))
            {
                const auto content = groupFrameOf (i, group.getId(), which);

                if (content.isEmpty())
                    continue;

                const bool focused = isGroupFocused (i, group.getId(), which);
                const bool editing = (group.getId() == editingGroupId);
                const auto colour  = group.getColour();

                g.setColour (colour.withAlpha (baseAlpha * (editing ? 1.0f
                                                                  : (focused ? 0.85f : 0.34f))));

                // Half a pixel in: a 1px stroke on the exact edge is drawn ON the
                // clip's own border and reads as "the clip got thicker" rather
                // than "this is the group". Reduced by the stroke's own
                // half-width, the two lines touch without merging — no gap, no
                // double line.
                const float stroke = editing ? ui::pf (1.8f)
                                             : (focused ? ui::pf (1.4f) : ui::pf (1.0f));
                auto box = content.reduced (stroke * 0.5f);

                if (box.getWidth() <= 0.0f || box.getHeight() <= 0.0f)
                    box = content;      // one-pixel member: the frame is all there is

                g.drawRoundedRectangle (box, ui::pf (4), stroke);

                //-- Phase 73: the dashed ring -------------------------------------
                // An open group gets a second, dashed frame just outside the
                // solid one. The difference between "this is one object" and
                // "you are inside it" is then a SHAPE, not a shade — a thicker
                // line alone reads as "hovered", which is already taken.
                //
                // A ring rather than four corner brackets: the name tag sits on
                // the frame's top-left corner and would hide exactly one of
                // them, so the group that most needs the mark (a short one,
                // whose pill covers most of its width) would lose it.
                if (editing)
                {
                    const auto ring = box.expanded (ui::pf (3.0f));
                    const float dashes[] { ui::pf (4.0f), ui::pf (3.5f) };
                    const float thick = ui::pf (1.4f);

                    g.setColour (colour.withAlpha (baseAlpha * 0.85f));

                    g.drawDashedLine ({ ring.getTopLeft(),    ring.getTopRight() }, dashes, 2, thick);
                    g.drawDashedLine ({ ring.getBottomLeft(), ring.getBottomRight() }, dashes, 2, thick);
                    g.drawDashedLine ({ ring.getTopLeft(),    ring.getBottomLeft() }, dashes, 2, thick);
                    g.drawDashedLine ({ ring.getTopRight(),   ring.getBottomRight() }, dashes, 2, thick);
                }
            }
    }
}


void PianoRollComponent::drawGroupLabels (juce::Graphics& g)
{
    const int activeIndex = stack.getActiveLayerIndex();

    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        auto* layer = stack.getLayer (i);

        if (! isLayerShown (i) || layer->getGroups().empty())
            continue;

        const bool onActive = (i == activeIndex);

        // Phase 58: a pill per appearance — each copy is its own grab area, so
        // each copy has its own name tag.
        for (const auto& group : layer->getGroups())
            for (int which : layer->getGroupOccurrences (group.getId()))
        {
            const auto pill = getGroupLabelBounds (i, group.getId(), which);

            if (pill.isEmpty())
                continue;

            const bool focused = isGroupFocused (i, group.getId(), which);
            const bool hovered = (hoveredGroupId == group.getId() && hoveredGroupLayer == i
                                    && hoveredGroupOccurrence == which);

            // Phase 73: the open group's name tag is filled solid and ringed, so
            // "which group am I inside" is answered by the label as well as by
            // the corner brackets. Needed most when the group is small: the
            // brackets of a two-note group are two pixels apart.
            const bool editing = (group.getId() == editingGroupId);
            const auto colour  = group.getColour();

            const float alpha = onActive ? 1.0f : 0.55f;

            // Phase 35: a muted group is drawn muted. The pill keeps its colour
            // but loses most of its fill, because "this group is silent" has to
            // be visible on the thing that names it — a mute whose only witness
            // is a row in a side panel is a mute you will debug twice.
            const float pillAlpha = group.isMuted() ? alpha * 0.45f : alpha;

            // Focused and hovered pills are filled; the rest are outlined so a
            // screen with six groups is not six shouting tabs.
            if (editing || focused || hovered)
            {
                g.setColour (colour.withAlpha (pillAlpha * (editing ? 1.0f
                                                                  : (focused ? 0.92f : 0.75f))));
                g.fillRoundedRectangle (pill, ui::pf (4));

                if (editing)
                {
                    g.setColour (theme::surface1.withAlpha (pillAlpha * 0.8f));
                    g.drawRoundedRectangle (pill, ui::pf (4), ui::pf (1.2f));
                }
            }
            else
            {
                g.setColour (colour.withAlpha (pillAlpha * 0.14f));
                g.fillRoundedRectangle (pill, ui::pf (4));
                g.setColour (colour.withAlpha (pillAlpha * 0.55f));
                g.drawRoundedRectangle (pill, ui::pf (4), 1.0f);
            }

            const juce::String text = groupLabelText (*layer, group);

            g.setColour (editing || focused || hovered ? theme::surface1
                                                       : colour.withAlpha (pillAlpha));
            g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));
            g.drawText (text, pill.reduced (ui::pf (5), 0.0f).withTrimmedRight (ui::pf (13)),
                        juce::Justification::centredLeft, false);

            //-- Phase 21: the curve handle ----------------------------------
            // The note's tail button is a small chevron on a dark square; this
            // is the same glyph in the same place, so "the thing at the right
            // end opens a curve" is learned once. The first version drew a tiny
            // curve icon here and at 12 px it read as the letter A.
            {
                const auto handle = getGroupAutomationHandleBounds (i, group.getId(), which);
                const bool open   = (expandedGroupId == group.getId()
                                       && expandedGroupLayerIndex == i
                                       && expandedGroupOccurrence == which);
                const bool hot    = hoveredGroupHandle
                                      && hoveredHandleGroupId == group.getId()
                                      && hoveredHandleLayer == i;

                // Phase 66: absent when the tag is too narrow to hold it.
                if (! handle.isEmpty())
                {
                    g.setColour (open ? colour.brighter (0.8f).withAlpha (alpha * 0.55f)
                                      : juce::Colours::black.withAlpha (alpha * 0.30f));
                    g.fillRoundedRectangle (handle, ui::pf (2.5f));

                    juce::Path chevron;
                    const float cx = handle.getCentreX();
                    const float cy = handle.getCentreY() + (open ? ui::pf (1.2f) : 0.0f);

                    chevron.startNewSubPath (cx - ui::pf (2.8f), cy - ui::pf (1.4f));
                    chevron.lineTo (cx, cy + ui::pf (1.8f));
                    chevron.lineTo (cx + ui::pf (2.8f), cy - ui::pf (1.4f));

                    g.setColour (juce::Colours::white.withAlpha (open || hot ? 0.95f : 0.7f));
                    g.strokePath (chevron, juce::PathStrokeType (ui::pf (1.2f),
                                                                 juce::PathStrokeType::curved,
                                                                 juce::PathStrokeType::rounded));
                }
            }

            // A mount marker: the pill's bottom edge is underlined in the
            // synth's own colour, so "this group plays on B" is visible without
            // opening anything. An underline rather than a dot because a filled
            // pill has no room for a dot that reads.
            if (! group.followsLayerOutput())
            {
                const auto* slot = rack.findSlot (group.getOutputSlotId());

                g.setColour ((slot != nullptr ? slot->colour : theme::record)
                                .withAlpha (alpha * 0.95f));
                g.fillRoundedRectangle (pill.withTrimmedTop (pill.getHeight() - ui::pf (2.5f))
                                            .withTrimmedLeft (ui::pf (4))
                                            .withTrimmedRight (ui::pf (15)),
                                        ui::pf (1.2));
            }
        }
    }
}


void PianoRollComponent::drawFocusHighlights (juce::Graphics& g)
{
    if (focusSlotId < 0)
        return;

    const auto* slot = rack.findSlot (focusSlotId);

    if (slot == nullptr)
        return;

    const auto ring = slot->colour;

    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        auto* layer = stack.getLayer (i);

        if (! isLayerShown (i))
            continue;

        const bool onActive = (i == stack.getActiveLayerIndex());

        for (const auto& note : layer->getNotes())
        {
            if (routing::resolve (*layer, note, rack).slotId != focusSlotId)
                continue;

            const auto bounds = getNoteBounds (note);

            if (! bounds.intersects (getGridArea()))
                continue;

            // Phase 30: the emphasis is POSITIVE now that the veil is gone — a
            // tinted wash in the slot's colour instead of "everything else got
            // darker". A wash is read against the note's own colour, so it works
            // on all three layers and at any velocity, which a brightness tweak
            // on the body did not.
            g.setColour (ring.withAlpha (onActive ? 0.34f : 0.20f));
            g.fillRoundedRectangle (bounds.reduced (0.5f), 3.0f);

            // A soft halo, then a crisp edge. Two rings rather than a fatter
            // one: a single 4 px outline eats a 14 px note.
            g.setColour (ring.withAlpha (onActive ? 0.22f : 0.12f));
            g.drawRoundedRectangle (bounds.expanded (ui::pf (1.6)), ui::pf (4), ui::pf (2.0));

            // Phase 29: a SELECTED note keeps its white ring. The crisp edge
            // here sits exactly on top of it, and in a spotlit view that meant
            // "selected" was invisible — which is the one thing you are about to
            // act on. The halo and the tab still say which synth it plays on.
            const bool selectedNote = onActive && isSelected (note.id);

            if (! selectedNote)
            {
                g.setColour (ring.withAlpha (onActive ? 0.92f : 0.50f));
                g.drawRoundedRectangle (bounds.reduced (0.5f), 3.0f, ui::pf (1.2));
            }

            // A 2 px tab on the left edge: the same mark at any zoom level, and
            // it survives the note being narrower than its own outline.
            g.fillRoundedRectangle (juce::Rectangle<float> (bounds.getX() - ui::pf (1),
                                                            bounds.getY() + ui::pf (1),
                                                            ui::pf (2.5),
                                                            juce::jmax (ui::pf (3), bounds.getHeight() - ui::pf (2))),
                                    ui::pf (1.2));
        }
    }
}


//==============================================================================
//  Phase 29: the way out of the spotlight
//==============================================================================
juce::Rectangle<int> PianoRollComponent::getSpotlightExitChipBounds() const
{
    if (focusSlotId < 0)
        return {};

    const auto area = getGridArea().toNearestInt().reduced (ui::px (8), ui::px (8));
    const int  w    = juce::jmin (area.getWidth(), ui::px (176));
    const int  h    = ui::px (24);

    // Bottom-right, not top-right (Phase 30). The top-right corner is where
    // clips and the higher melody notes live — the chip was covering the thing
    // it was meant to help you look at, and eating the clicks that landed on it.
    return juce::Rectangle<int> (juce::jmax (area.getX(), area.getRight() - w),
                                 juce::jmax (area.getY(), area.getBottom() - h), w, h);
}


void PianoRollComponent::drawSpotlightExitChip (juce::Graphics& g)
{
    const auto chip = getSpotlightExitChipBounds();

    if (chip.isEmpty())
        return;

    const auto box = chip.toFloat();

    // Dark and opaque: it is drawn on top of a 46% black veil, and a translucent
    // chip on a veil is exactly the thing the user cannot find.
    g.setColour (theme::surface3);
    g.fillRoundedRectangle (box, ui::pf (4));

    g.setColour (kPlayheadColour.withAlpha (0.80f));
    g.drawRoundedRectangle (box.reduced (0.5f), ui::pf (4), 1.0f);

    const juce::String name = rack.findSlot (focusSlotId) != nullptr
                                ? rack.getSlotName (focusSlotId)
                                : juce::String (focusSlotId);

    g.setColour (juce::Colours::white.withAlpha (0.94f));
    g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));

    // Phase 30: the chip no longer sits on a veil, so it has to name the exit
    // gestures that exist — the empty grid is one of them now.
    g.drawText (utf8 ("高亮 ") + name + utf8 ("   ·   点空白或 Esc 退出"),
                chip.withTrimmedLeft (ui::px (9)).withTrimmedRight (ui::px (6)),
                juce::Justification::centredLeft, true);
}


void PianoRollComponent::drawNote (juce::Graphics& g, const NoteEvent& note, juce::Colour colour,
                                   float alpha, bool selected, bool hovered, double progress)
{
    auto bounds = getNoteBounds (note);

    // Velocity modulates brightness, not opacity — opacity is reserved for
    // the ghost / solid distinction and must stay unambiguous.
    const auto tinted = colour.withMultipliedBrightness (0.78f + 0.35f * note.velocity);
    const bool sounding = progress >= 0.0;

    // Phase 22: a selected note is a brighter note. The outline alone was too
    // quiet a difference on a sheet with twenty notes on it.
    g.setColour ((selected ? tinted.brighter (0.30f) : tinted).withAlpha (alpha * 0.88f));
    g.fillRoundedRectangle (bounds, 3.0f);

    // Top highlight edge: makes stacked sheets readable even at 30%.
    g.setColour (tinted.brighter (0.55f).withAlpha (alpha * 0.95f));
    g.fillRoundedRectangle (bounds.withHeight (2.5f), 1.4f);

    g.setColour (tinted.darker (0.7f).withAlpha (alpha));
    g.drawRoundedRectangle (bounds.reduced (0.5f), 3.0f, 1.0f);

    // A notch in the target synth's colour: this note is bypassing its layer.
    // Same colours as the rack cards, so the destination is identifiable at a
    // glance without opening the inspector.
    if (note.outputSlotOverride >= 0 && bounds.getWidth() >= 14.0f)
    {
        const auto* target = rack.findSlot (note.outputSlotOverride);
        const auto badgeColour = target != nullptr ? target->colour : theme::record;

        juce::Path notch;
        notch.addTriangle (bounds.getRight() - 10.0f, bounds.getY() + 1.0f,
                           bounds.getRight() - 1.5f, bounds.getY() + 1.0f,
                           bounds.getRight() - 1.5f, bounds.getY() + 9.5f);

        g.setColour (badgeColour.withAlpha (alpha));
        g.fillPath (notch);
    }

    //-- The note's own automation, inside the block -------------------------
    // This is the visual proof of the binding: move or copy the block and the
    // curve moves with it. While the playhead is inside the note, the curve is
    // the thing being *played*, so it shows how far it has got.
    if (note.envelope().isEnabled() && note.envelope().getPoints().size() >= 2
         && bounds.getWidth() >= 10.0f)
    {
        const auto inner = bounds.reduced (1.5f, 2.0f);

        auto xForTime   = [&inner] (float t) { return inner.getX() + t * inner.getWidth(); };
        auto yForValue  = [&inner] (float v) { return inner.getBottom() - v * inner.getHeight(); };

        auto buildCurve = [&] (float from, float to)
        {
            juce::Path path;

            const int steps = juce::jlimit (2, 96, (int) (inner.getWidth() * (to - from) / 2.5f));

            for (int i = 0; i < steps; ++i)
            {
                const float t = from + (to - from) * (float) i / (float) (steps - 1);
                const float x = xForTime (t);
                const float y = yForValue (note.envelope().valueAt (t));

                if (i == 0) path.startNewSubPath (x, y);
                else        path.lineTo (x, y);
            }

            return path;
        };

        // Whole curve, dim: what the note will do.
        g.setColour (juce::Colours::white.withAlpha (alpha * (sounding ? 0.30f : 0.55f)));
        g.strokePath (buildCurve (0.0f, 1.0f), juce::PathStrokeType (1.0f));

        if (sounding)
        {
            const float p = (float) progress;

            // Area already covered by the envelope, filled at low alpha: the
            // "progress bar" of a single note.
            if (p > 0.001f)
            {
                auto filled = buildCurve (0.0f, p);
                filled.lineTo (xForTime (p), inner.getBottom());
                filled.lineTo (xForTime (0.0f), inner.getBottom());
                filled.closeSubPath();

                g.setColour (tinted.brighter (0.85f).withAlpha (alpha * 0.22f));
                g.fillPath (filled);

                // The part of the curve that has already played, at full
                // strength — this is the line "moving" under the playhead.
                g.setColour (juce::Colours::white.withAlpha (alpha * 0.95f));
                g.strokePath (buildCurve (0.0f, p), juce::PathStrokeType (1.6f));
            }

            // The head: a dot riding the curve at exactly the level the engine
            // is applying to this note right now.
            const float v  = note.envelope().valueAt (p);
            const float cx = xForTime (p);
            const float cy = yForValue (v);

            g.setColour (tinted.brighter (0.9f).withAlpha (alpha * 0.35f));
            g.fillEllipse (cx - 4.5f, cy - 4.5f, 9.0f, 9.0f);

            g.setColour (juce::Colours::white.withAlpha (alpha * 0.95f));
            g.fillEllipse (cx - 2.0f, cy - 2.0f, 4.0f, 4.0f);
        }
    }

    if (sounding)
    {
        // A lit edge marks the note as currently sounding, so its outline reads
        // at a glance even on a busy sheet.
        g.setColour (tinted.brighter (0.9f).withAlpha (alpha * 0.75f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 3.0f, 1.4f);
    }

    //-- Phase 8: the "v" handle at the tail ---------------------------------
    // Only the active layer gets one. A ghost note is click-through by design,
    // so offering it a button would break the one rule the roll never breaks.
    if (alpha >= 0.9f)
    {
        const auto handle = getNoteAutomationHandleBounds (note);

        if (! handle.isEmpty())
        {
            const bool open = note.id == expandedNoteId && expandedLayerIndex == stack.getActiveLayerIndex();

            g.setColour (open ? tinted.brighter (0.8f).withAlpha (0.55f)
                              : juce::Colours::black.withAlpha (0.30f));
            g.fillRoundedRectangle (handle, 2.5f);

            juce::Path chevron;
            const float cx = handle.getCentreX();
            const float cy = handle.getCentreY() + (open ? 1.5f : 0.0f);

            chevron.startNewSubPath (cx - 2.8f, cy - 1.4f);
            chevron.lineTo (cx, cy + 1.8f);
            chevron.lineTo (cx + 2.8f, cy - 1.4f);

            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.strokePath (chevron, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved,
                                                         juce::PathStrokeType::rounded));
        }
    }

    if (hovered && ! selected)
    {
        g.setColour (juce::Colours::white.withAlpha (0.45f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 3.0f, 1.2f);
    }

    //-- Phase 22: selection has to win at a glance --------------------------
    // Phase 8 marked a selected note with a 1.6 px outline, which on a busy
    // sheet was indistinguishable from the hover ring — "did I select that
    // one?" is not a question the editor should make you ask twice, because
    // the answer decides what the next drag moves. Three signals now: a
    // brighter body (above), a 2 px white ring, and four corner brackets
    // outside the block.
    if (selected)
    {
        g.setColour (juce::Colours::white.withAlpha (0.96f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 3.0f, 2.0f);

        if (bounds.getWidth() >= 12.0f && bounds.getHeight() >= 6.0f)
        {
            const float arm = 3.0f;
            const auto  box = bounds.expanded (1.5f);
            juce::Path brackets;

            brackets.startNewSubPath (box.getX(), box.getY() + arm);
            brackets.lineTo (box.getX(), box.getY());
            brackets.lineTo (box.getX() + arm, box.getY());

            brackets.startNewSubPath (box.getRight() - arm, box.getY());
            brackets.lineTo (box.getRight(), box.getY());
            brackets.lineTo (box.getRight(), box.getY() + arm);

            brackets.startNewSubPath (box.getRight(), box.getBottom() - arm);
            brackets.lineTo (box.getRight(), box.getBottom());
            brackets.lineTo (box.getRight() - arm, box.getBottom());

            brackets.startNewSubPath (box.getX() + arm, box.getBottom());
            brackets.lineTo (box.getX(), box.getBottom());
            brackets.lineTo (box.getX(), box.getY() + box.getHeight() - arm);

            g.setColour (juce::Colours::white.withAlpha (0.82f));
            g.strokePath (brackets, juce::PathStrokeType (1.4f, juce::PathStrokeType::mitered,
                                                          juce::PathStrokeType::butt));
        }
    }
}


void PianoRollComponent::drawMarquee (juce::Graphics& g)
{
    if (dragMode != DragMode::marquee)
        return;

    auto r = juce::Rectangle<float> (marqueeStart, marqueeCurrent);

    g.setColour (juce::Colours::white.withAlpha (0.10f));
    g.fillRect (r);

    g.setColour (juce::Colours::white.withAlpha (0.55f));
    g.drawRect (r, 1.0f);
}


void PianoRollComponent::drawPlayhead (juce::Graphics& g)
{
    if (playheadBeat < 0.0)
        return;

    const auto area = getGridArea();
    const float x = screenXForBeat (playheadBeat);

    if (x < area.getX() || x > area.getRight())
        return;

    // Drawn as a path rather than drawVerticalLine: at 60 fps the playhead
    // moves about 1.3 px per frame, and an integer-snapped line makes that
    // read as a series of jumps. A 1.2 px path lands on sub-pixels, so the
    // motion looks continuous.
    juce::Path glow;
    glow.addRectangle (x - 2.5f, area.getY(), 5.0f, area.getHeight());
    g.setColour (kPlayheadColour.withAlpha (0.12f));
    g.fillPath (glow);

    juce::Path body;
    body.addRectangle (x - 0.6f, area.getY(), 1.2f, area.getHeight());
    g.setColour (kPlayheadColour.withAlpha (0.90f));
    g.fillPath (body);
}


void PianoRollComponent::drawRuler (juce::Graphics& g)
{
    auto ruler = getRulerArea();

    g.setColour (kRulerBackground);
    g.fillRect (ruler);

    //-- Corner readout: how long a bar is at the current tempo ---------------
    // The beat grid deliberately does not move when the tempo changes, so this
    // is the number that shows the timeline has been re-timed.
    {
        // The corner cell sits beside the ruler, not above it: drawing it at 0,0
        // painted over the tool strip and hid the first tool button.
        const auto corner = getMeterCornerArea();

        g.setColour (kRulerBackground);
        g.fillRect (corner);

        const double barSeconds = (double) beatsPerBar * (60.0 / juce::jmax (1.0, bpm));

        g.setColour (juce::Colours::white.withAlpha (0.32f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (7.5f))));

        // Phase 91: the numerator IS the metre, so it is drawn instead of the
        // word "1 小节" — and clicking it is how it changes. A control nobody
        // can see is a control nobody has.
        g.drawText (juce::String (beatsPerBar) + "/4", ui::px (8), corner.getY() + ui::px (3),
                    ui::px (60), ui::px (14), juce::Justification::centredLeft, false);

        g.setColour (theme::accent.withAlpha (0.90f));
        g.setFont (boldFont (9.5f));
        g.drawText (juce::String (barSeconds, 2) + " s", ui::px (8), corner.getY() + ui::px (18),
                    ui::px (70), ui::px (16), juce::Justification::centredLeft, false);
    }

    auto formatTime = [] (double seconds)
    {
        const int    minutes = (int) (seconds / 60.0);
        const double rest    = seconds - (double) minutes * 60.0;

        return juce::String (minutes) + ":"
                 + juce::String (rest, 1).paddedLeft ('0', 4);
    };

    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (ruler);

        //-- One-second ticks: the wall-clock scale under the musical one -----
        const double secondsPerBeat = 60.0 / juce::jmax (1.0, bpm);
        const double beatsPerSecond = 1.0 / secondsPerBeat;
        const double pixelsPerSecond = secondsPerBeat * (double) beatWidth;

        if (pixelsPerSecond >= 20.0)
        {
            const int firstSecond = (int) std::floor (scrollBeats / beatsPerSecond);
            const int lastSecond  = (int) std::ceil ((scrollBeats + (double) ruler.getWidth() / beatWidth)
                                                       / beatsPerSecond);

            g.setColour (theme::accent.withAlpha (0.30f));

            for (int second = firstSecond; second <= lastSecond; ++second)
            {
                const double beat = (double) second * beatsPerSecond;

                if (beat < 0.0 || beat > timelineBeats)
                    continue;

                const int x = (int) std::floor (screenXForBeat (beat));

                g.drawVerticalLine (x, (float) ruler.getBottom() - ui::pf (5),
                                    (float) ruler.getBottom());
            }
        }

        //-- Bars, with their wall-clock position underneath -------------------
        g.setFont (boldFont (10.5f));

        // Phase 91: a bar is the PROJECT's bar, not four beats. Read once into a
        // local so the three uses below cannot disagree with each other.
        const double barLength = (double) juce::jmax (1, beatsPerBar);

        const int firstBar = (int) std::floor (scrollBeats / barLength);
        const int lastBar  = (int) std::ceil ((scrollBeats + (double) ruler.getWidth() / beatWidth)
                                                 / barLength);

        for (int bar = firstBar; bar <= lastBar; ++bar)
        {
            const double beat = (double) bar * barLength;

            if (beat < 0.0 || beat > timelineBeats)
                continue;

            const float x = std::floor (screenXForBeat (beat)) + 0.5f;

            g.setColour (juce::Colours::white.withAlpha (0.22f));
            g.drawVerticalLine ((int) x, (float) ruler.getBottom() - ui::pf (9),
                                (float) ruler.getBottom());

            g.setColour (kText);
            g.drawText (juce::String (bar + 1),
                        juce::Rectangle<int> ((int) x + ui::px (4), ruler.getY() + ui::px (2),
                                              ui::px (46), ui::px (16)),
                        juce::Justification::centredLeft, false);

            // Changing the tempo rewrites this row — the beat grid itself must
            // not move, so the seconds are what tells you the tempo changed.
            g.setColour (theme::accent.withAlpha (0.45f));
            g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (8.5f))));
            g.drawText (formatTime (beat * secondsPerBeat),
                        juce::Rectangle<int> ((int) x + ui::px (4), ruler.getY() + ui::px (19),
                                              ui::px (64), ui::px (15)),
                        juce::Justification::centredLeft, false);
            g.setFont (boldFont (10.5f));
        }
    }

    //-- Phase 87: the loop -------------------------------------------------
    // Over the ticks and under the playhead: the loop is part of the time axis,
    // and the playhead must still read as being on top of it. Only the ends are
    // clamped to the strip — a region that runs past the edge of the screen is
    // drawn to the edge rather than dropped, because "the loop ends somewhere
    // over there" is information.
    {
        const auto bar = getLoopBarArea();

        // Nothing at all while the loop is off.
        //
        // A hollow bracket was the first version, and it put a ghost of bars
        // 0-4 on every empty project — a region nobody had made, drawn on every
        // still, saying "something is arranged here". Off means off: the strip
        // still answers a double click (that is how it comes back), and a drag
        // still draws a new one.
        if (! bar.isEmpty() && loopRegion.isUsable())
        {
            const auto left  = juce::jlimit ((float) bar.getX(), (float) bar.getRight(),
                                             screenXForBeat (loopRegion.startBeat));
            const auto right = juce::jlimit ((float) bar.getX(), (float) bar.getRight(),
                                             screenXForBeat (loopRegion.endBeat));

            if (right > left)
            {
                const juce::Rectangle<float> span (left, (float) bar.getY(),
                                                   right - left, (float) bar.getHeight());

                g.setColour (theme::accent.withAlpha (0.22f));
                g.fillRoundedRectangle (span, ui::pf (2.0f));

                g.setColour (theme::accent.withAlpha (0.85f));
                g.drawRoundedRectangle (span.reduced (0.5f), ui::pf (2.0f), ui::pf (1.0f));

                // Two grips, so "this end" is something you can see before you
                // have to hit it.
                g.fillRect (juce::Rectangle<float> (left, span.getY(),
                                                    ui::pf (2.0f), span.getHeight()));
                g.fillRect (juce::Rectangle<float> (right - ui::pf (2.0f), span.getY(),
                                                    ui::pf (2.0f), span.getHeight()));

            }
        }
    }

    // Playhead marker in the ruler, drawn on top of the bar numbers.
    if (playheadBeat >= 0.0)
    {
        const float x = screenXForBeat (playheadBeat);

        if (x >= ruler.getX() && x <= ruler.getRight())
        {
            juce::Path body;
            body.addRectangle (x - 0.6f, (float) ruler.getY(), 1.2f, (float) ruler.getHeight());

            g.setColour (kPlayheadColour);
            g.fillPath (body);

            juce::Path head;
            head.addTriangle (x - ui::pf (6), (float) ruler.getY(),
                              x + ui::pf (6), (float) ruler.getY(),
                              x, (float) ruler.getY() + ui::pf (9));
            g.fillPath (head);
        }
    }

    g.setColour (kOutline);
    g.drawHorizontalLine (ruler.getBottom() - 1, (float) ruler.getX(), (float) ruler.getRight());
}


void PianoRollComponent::drawKeyboard (juce::Graphics& g)
{
    auto keys = getKeyboardArea();

    g.setColour (theme::surface2);
    g.fillRect (keys);

    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (keys);

        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));

        const int topPitch    = contentYToPitch (scrollY);
        const int bottomPitch = contentYToPitch (scrollY + (float) keys.getHeight());

        for (int pitch = bottomPitch; pitch <= topPitch; ++pitch)
        {
            const float y = screenYForPitch (pitch);
            const bool  black = isBlackKey (pitch);

            auto row = juce::Rectangle<float> ((float) keys.getX(), y,
                                               black ? (float) keys.getWidth() * 0.58f : (float) keys.getWidth(),
                                               keyHeight);

            g.setColour (black ? kKeyBlack : kKeyWhite);
            g.fillRect (row);

            // Phase 22: the key under the pointer is lit the same colour the
            // notes use for "you are working on this", so a key press and a
            // note selection read as the same kind of thing.
            if (pitch == pressedKeyPitch)
            {
                g.setColour (theme::accent.withAlpha (black ? 0.55f : 0.40f));
                g.fillRect (row);

                g.setColour (theme::accent);
                g.drawRect (row, 1.2f);
            }

            if (! black)
            {
                g.setColour (juce::Colours::black.withAlpha (0.55f));
                g.drawText (noteName (pitch),
                            keys.withY ((int) y).withHeight ((int) keyHeight).reduced (ui::px (5), 0),
                            juce::Justification::centredRight, false);
            }

            g.setColour (juce::Colours::black.withAlpha (black ? 0.35f : 0.12f));
            g.drawHorizontalLine ((int) (y + keyHeight) - 1, (float) keys.getX(), (float) keys.getRight());
        }
    }

    g.setColour (kOutline);
    g.drawVerticalLine (keys.getRight() - 1, (float) keys.getY(), (float) keys.getBottom());
}


//==============================================================================
//  Phase 8: tool icons
//==============================================================================
void PianoRollComponent::drawToolIcon (juce::Graphics& g, int toolIndex,
                                       juce::Rectangle<float> area, juce::Colour colour)
{
    // Every icon is drawn inside a 16x16 box centred on the button. (Callers
    // used to pre-shrink the button with reduced(), which turns inside out once
    // the inset passes half the height — the icons ended up hanging off the
    // bottom edge.)
    const auto box = juce::Rectangle<float> (16.0f, 16.0f).withCentre (area.getCentre());
    const auto c   = box.getCentre();

    g.setColour (colour);

    switch (toolIndex)
    {
        case 0:     // pencil
        {
            juce::Path body;
            body.startNewSubPath (box.getX() + 5.0f, box.getBottom() - 5.0f);
            body.lineTo (box.getRight() - 3.0f, box.getY() + 3.0f);

            g.strokePath (body, juce::PathStrokeType (3.2f, juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));

            // The sharpened tip, meeting the body at 45 degrees.
            juce::Path tip;
            tip.addTriangle (box.getX() + 1.8f, box.getBottom() - 1.8f,
                             box.getX() + 3.2f, box.getBottom() - 6.6f,
                             box.getX() + 6.6f, box.getBottom() - 3.2f);
            g.fillPath (tip);
            break;
        }

        case 1:     // eraser: a tilted block with the rubber end marked
        {
            juce::Path block;
            block.addRoundedRectangle (-5.5f, -3.2f, 11.0f, 6.4f, 1.6f);
            block.applyTransform (juce::AffineTransform::rotation (-0.55f).translated (c));

            g.strokePath (block, juce::PathStrokeType (1.6f));

            juce::Path divider;
            divider.startNewSubPath (-1.2f, -3.2f);
            divider.lineTo (-1.2f, 3.2f);
            divider.applyTransform (juce::AffineTransform::rotation (-0.55f).translated (c));

            g.strokePath (divider, juce::PathStrokeType (1.2f));
            break;
        }

        case 2:     // select: the classic arrow cursor
        {
            juce::Path arrow;
            arrow.startNewSubPath (box.getX() + 4.0f, box.getY() + 2.0f);
            arrow.lineTo (box.getX() + 4.0f, box.getBottom() - 3.0f);
            arrow.lineTo (box.getX() + 7.0f, box.getBottom() - 5.6f);
            arrow.lineTo (box.getX() + 9.0f, box.getBottom() - 0.5f);
            arrow.lineTo (box.getX() + 11.2f, box.getBottom() - 1.6f);
            arrow.lineTo (box.getX() + 9.1f, box.getBottom() - 6.4f);
            arrow.lineTo (box.getX() + 12.5f, box.getBottom() - 6.6f);
            arrow.closeSubPath();

            g.fillPath (arrow);
            break;
        }

        case 4:     // Phase 51: knife — a utility blade: body, snap grooves, tip
        {
            // First version was a slim filled triangle plus a handle stroke,
            // and at 16 px it read as one thin diagonal line — indistinguishable
            // from noise, let alone from the pencil. A utility knife reads
            // instead: the eraser's rounded-rect body (one family look), a
            // triangular blade sticking out of the working end, and the snap
            // grooves across the blade. The eraser has no tip; the pencil has
            // no grooves; this has both.
            const auto knife = juce::AffineTransform::rotation (-0.55f).translated (c);

            juce::Path body;
            body.addRoundedRectangle (-5.6f, -2.2f, 9.2f, 4.4f, 1.4f);
            body.applyTransform (knife);

            g.strokePath (body, juce::PathStrokeType (1.6f));

            juce::Path blade;
            blade.addTriangle (11.0f, 0.0f, 4.6f, -2.2f, 4.6f, 2.2f);
            blade.applyTransform (knife);

            g.fillPath (blade);

            juce::Path grooves;
            grooves.startNewSubPath (6.4f, -1.5f);
            grooves.lineTo (6.4f, 1.5f);
            grooves.startNewSubPath (8.1f, -1.1f);
            grooves.lineTo (8.1f, 1.9f);
            grooves.applyTransform (knife);

            g.strokePath (grooves, juce::PathStrokeType (1.0f));
            break;
        }

        case 5:     // copy note state: a dropper, with a drop of what it picked up
        {
            juce::Path pipette;
            pipette.startNewSubPath (box.getRight() - 3.0f, box.getY() + 2.0f);
            pipette.lineTo (box.getX() + 5.0f, box.getBottom() - 8.0f);
            pipette.lineTo (box.getX() + 2.0f, box.getBottom() - 5.0f);
            pipette.closeSubPath();

            g.fillPath (pipette);

            // The bulb at the top, and the drop beneath — the two things that
            // tell a dropper apart from a pencil at 16 px.
            g.fillEllipse (box.getRight() - 6.0f, box.getY() + 0.5f, 5.5f, 5.5f);

            juce::Path drop;
            drop.startNewSubPath (box.getX() + 2.5f, box.getBottom() - 4.5f);
            drop.quadraticTo (box.getX() - 1.5f, box.getBottom() - 1.0f,
                              box.getX() + 2.5f, box.getBottom() - 0.5f);
            drop.quadraticTo (box.getX() + 6.5f, box.getBottom() - 1.0f,
                              box.getX() + 2.5f, box.getBottom() - 4.5f);
            g.fillPath (drop);
            break;
        }

        case 6:     // group: three blocks behind a bracket
        {
            g.fillRoundedRectangle (box.getX() + 4.5f, box.getY() + 3.5f, 7.5f, 3.0f, 1.0f);
            g.fillRoundedRectangle (box.getX() + 4.5f, box.getY() + 8.0f, 7.5f, 3.0f, 1.0f);

            // The bracket says "these are one object"; without it the two bars
            // read as a pause icon, which is the one thing it must not be.
            juce::Path bracket;
            bracket.startNewSubPath (box.getX() + 2.5f, box.getY() + 2.0f);
            bracket.lineTo (box.getX() + 1.0f, box.getY() + 2.0f);
            bracket.lineTo (box.getX() + 1.0f, box.getBottom() - 2.0f);
            bracket.lineTo (box.getX() + 2.5f, box.getBottom() - 2.0f);
            bracket.startNewSubPath (box.getRight() - 2.5f, box.getY() + 2.0f);
            bracket.lineTo (box.getRight() - 1.0f, box.getY() + 2.0f);
            bracket.lineTo (box.getRight() - 1.0f, box.getBottom() - 2.0f);
            bracket.lineTo (box.getRight() - 2.5f, box.getBottom() - 2.0f);

            g.strokePath (bracket, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved,
                                                          juce::PathStrokeType::rounded));
            break;
        }

        case 7:     // ungroup: the group mark, with the brackets coming off
        {
            g.fillRoundedRectangle (box.getX() + 5.0f, box.getY() + 3.5f, 6.5f, 3.0f, 1.0f);
            g.fillRoundedRectangle (box.getX() + 5.0f, box.getY() + 9.5f, 6.5f, 3.0f, 1.0f);

            // Broken brackets, open at the middle and at both ends: the pair
            // that held these two bars together is being taken away, which is
            // the one thing that distinguishes this from the 编组 button.
            juce::Path left;
            left.startNewSubPath (box.getX() + 3.2f, box.getY() + 2.0f);
            left.lineTo (box.getX() + 1.4f, box.getY() + 2.0f);
            left.lineTo (box.getX() + 1.4f, box.getY() + 6.4f);
            left.startNewSubPath (box.getX() + 1.4f, box.getY() + 9.6f);
            left.lineTo (box.getX() + 1.4f, box.getY() + 14.0f);
            left.lineTo (box.getX() + 3.2f, box.getY() + 14.0f);

            juce::Path right;
            right.startNewSubPath (box.getRight() - 3.2f, box.getY() + 2.0f);
            right.lineTo (box.getRight() - 1.4f, box.getY() + 2.0f);
            right.lineTo (box.getRight() - 1.4f, box.getY() + 6.4f);
            right.startNewSubPath (box.getRight() - 1.4f, box.getY() + 9.6f);
            right.lineTo (box.getRight() - 1.4f, box.getY() + 14.0f);
            right.lineTo (box.getRight() - 3.2f, box.getY() + 14.0f);

            g.strokePath (left, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::butt));
            g.strokePath (right, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::butt));
            break;
        }

        case 8:     // snap: a small horseshoe magnet, poles up
        {
            // Drawn as a path, not as a filled shape: the U is one stroke of
            // constant width, so it stays a magnet at 16 px instead of turning
            // into a blob. Right leg -> bottom arc -> left leg, because JUCE
            // measures arc angles clockwise from 12 o'clock and the path has to
            // travel the way the stroke expects.
            //
            // The first version drew the legs at 3.0 px with pole faces only
            // one pixel wider, and the whole thing read as the letter U. The
            // legs are now thin (2.6) and the pole faces flare well past them
            // with a gap down the middle, which is the silhouette everyone
            // recognises. The legs start below the poles rather than behind
            // them, so neither shape eats the other.
            const float leftLeg  = box.getX() + 4.5f;
            const float rightLeg = box.getX() + 11.5f;
            const float poleTop  = box.getY() + 1.3f;
            const float poleH    = 3.4f;

            juce::Path horseshoe;
            horseshoe.startNewSubPath (rightLeg, poleTop + poleH);
            horseshoe.lineTo (rightLeg, box.getY() + 9.2f);
            horseshoe.addCentredArc (box.getX() + 8.0f, box.getY() + 9.2f, 3.5f, 3.5f, 0.0f,
                                     juce::MathConstants<float>::pi * 0.5f,
                                     juce::MathConstants<float>::pi * 1.5f, false);
            horseshoe.lineTo (leftLeg, poleTop + poleH);

            g.strokePath (horseshoe, juce::PathStrokeType (2.6f, juce::PathStrokeType::mitered,
                                                           juce::PathStrokeType::butt));

            g.fillRoundedRectangle (juce::Rectangle<float> (leftLeg - 2.6f, poleTop, 5.2f, poleH), 0.8f);
            g.fillRoundedRectangle (juce::Rectangle<float> (rightLeg - 2.6f, poleTop, 5.2f, poleH), 0.8f);
            break;
        }

        case 3:     // move view: a four-way cross
        default:
        {
            const float arm = 5.0f;

            g.drawLine (c.x - arm, c.y, c.x + arm, c.y, 1.5f);
            g.drawLine (c.x, c.y - arm, c.x, c.y + arm, 1.5f);

            auto head = [&g, c] (float dx, float dy)
            {
                juce::Path p;
                p.addTriangle (c.x + dx * 7.5f, c.y + dy * 7.5f,
                               c.x + dx * 4.0f - dy * 2.6f, c.y + dy * 4.0f - dx * 2.6f,
                               c.x + dx * 4.0f + dy * 2.6f, c.y + dy * 4.0f + dx * 2.6f);
                g.fillPath (p);
            };

            head (0.0f, -1.0f);
            head (0.0f,  1.0f);
            head (-1.0f, 0.0f);
            head ( 1.0f, 0.0f);
            break;
        }
    }
}
} // namespace atomic
