/*  PianoRollInternals.h  —  Phase 85

    The helpers more than one piano-roll translation unit needs: the
    palette aliases, the menu id bands, the curve-list menu builders and
    the small text/glyph functions the automation boxes share.

    They used to sit in anonymous namespaces at the top of the one
    13,000-line .cpp, so anything sharing them had to live in that file.

    Every definition here is `inline`: the linker merges them, so the
    eleven Tus that include this header share one copy of each function,
    and a file that pulls in the header but uses only half of it pays
    nothing at run time.
*/
#pragma once

#include "PianoRollComponent.h"

#include "Util/Metrics.h"
#include "Util/Text.h"
#include "Util/Theme.h"

namespace atomic
{
namespace roll
{


    inline const juce::Colour& kRollBackground = theme::surface0;
    inline const juce::Colour kGridBackground  = theme::surface2;
    inline const juce::Colour kBlackRowColour  = theme::bgApp;
    inline const juce::Colour kRulerBackground = theme::surface2;
    inline const juce::Colour kKeyWhite        = theme::ink;
    inline const juce::Colour kKeyBlack        = theme::surface4;
    inline const juce::Colour& kOutline        = theme::surface4;
    inline const juce::Colour& kText           = theme::inkMuted;
    inline const juce::Colour& kPlayheadColour = theme::record;

    /** Phase 95: how much of itself a member of a take keeps when it is not the
        one you hear.

        The sheet draws every take, because choosing between them is looking at
        them — but a pass that makes no sound must not look like the pass that
        does. Dimming is the same answer the panel gives the row and the frame
        gives the pill, at the same weight, so the three views cannot disagree
        about which take is the comp.

        One function, three call sites (notes, blocks, pills): the number is a
        visual rule, not three coincidences. */
    inline constexpr float kInactiveTakeAlpha = 0.45f;

    inline float takeFadeOf (const Layer& layer, int groupId)
    {
        if (groupId < 0)
            return 1.0f;

        const auto* group = layer.findGroup (groupId);

        if (group == nullptr || ! group->isTake() || group->isActiveTake())
            return 1.0f;

        return kInactiveTakeAlpha;
    }

    /** DESIGN pixels: a note has one of these bands at each end, and dragging
        inside it changes the note's length instead of moving it. Both ends,
        because a note you cannot shorten from the front is a note you have to
        delete and redraw. */
    constexpr float kResizeHandleWidth = 6.0f;

    /** The tail's automation button. Phase 19: it moved inboard — it used to
        cover the right resize band, so the only way to lengthen a note was to
        miss the very thing the note offers. Now it stops short of the band with
        clear air on both sides. */
    constexpr float kNoteHandleWidth = 11.0f;

    /** Phase 25: the air between the button and the resize band on the TAIL
        side only. It used to be the full gap, which left the button sitting
        oddly far from the end it belongs to; 1 px still separates the two so a
        click on the band is never a click on the button, and the band keeps its
        whole width for stretching. */
    constexpr float kNoteHandleTailAir = 1.0f;

    /** Body left over once both resize bands and the button are subtracted.
        Under this the button is not drawn at all: a note that short has no room
        for a button that is not also a resize edge. */
    constexpr float kNoteHandleMinBody = 6.0f;

    //-- Phase 27: the clip menu's id ranges ----------------------------------
    /** One menu, three kinds of answer. Ranges instead of "result - 100", so a
        click that lands in the wrong kind cannot silently become a route. */
    constexpr int kClipMenuUnmount = 6;
    constexpr int kClipMenuNotices = 2999;   // disabled "there are none" rows
    constexpr int kClipMountBase   = 3000;   // + sampler slot id
    constexpr int kClipMenuUngroup = 3999;
    constexpr int kClipMenuNewGroup = 3998;   // Phase 35: make one, containing this
    constexpr int kClipGroupBase   = 4000;   // + group id
    /** Phase 31: + (int) StretchMode. A block of four, well clear of the mount
        and group ranges above it. */
    constexpr int kClipStretchBase = 5000;
    /** Phase 50: reverse. Its own id band — the stretch range is
        checked as `>= 5000 && < 5008`, so anything appended to that family has
        to start far enough away that the range test cannot swallow it. */
    constexpr int kClipMenuReverse = 6001;

    inline juce::Font boldFont (float height)
    {
        return juce::Font (juce::FontOptions{}.withHeight (atomic::ui::sp (height)).withStyle ("Bold"));
    }

    /** Phase 35: the words on a group's pill, built once.

        Two functions draw or MEASURE this pill (getGroupLabelBounds and
        drawGroupLabels), and they had already drifted: the measurement counted
        a group's clips ("+ 2") while the drawing omitted them, so a group of
        clips got a pill wider than its text for no visible reason. One builder
        means the mute marker below cannot repeat that mistake — a pill whose
        width was computed from different words than it draws is how a label
        ends up clipped at the right edge. */
    inline juce::String groupLabelText (const Layer& layer, const NoteGroup& group)
    {
        const int notes = layer.countNotesInGroup (group.getId());
        const int clips = layer.countSamplesInGroup (group.getId());

        juce::String text = group.getName() + utf8 (" · ") + juce::String (notes);

        // A count that only reported notes would read as "the clip is not
        // really in there" (Phase 27).
        if (clips > 0)
            text << " + " + juce::String (clips);

        if (group.isMuted())
            text << utf8 (" · 静音");

        return text;
    }

    /** The label the inline box's menu shows. The model stores ASCII keys, so
        this is the only place the Chinese names exist. */
    inline juce::String modTargetLabel (NoteModTarget target)
    {
        switch (target)
        {
            case NoteModTarget::pan:   return utf8 ("声相");
            case NoteModTarget::pitch: return utf8 ("音高");
            case NoteModTarget::level:
            default:                   return utf8 ("音量");
        }
    }

    inline juce::String modTargetHint (NoteModTarget target)
    {
        switch (target)
        {
            case NoteModTarget::pan:   return utf8 ("0.5 居中，两端为左右");
            case NoteModTarget::pitch: return utf8 ("1.0 不弯，0 低一个八度");
            case NoteModTarget::level:
            default:                   return utf8 ("0 静音，1 原始音量");
        }
    }

    /** Phase 24: menu ids at or above this one mean "parameter N of the hosted
        synth", with N = id - kPluginParamItemBase. Well clear of the handful of
        small fixed ids the built-in targets and the menu verbs use, so a growing
        parameter list can never collide with them. */
    constexpr int kPluginParamItemBase = 1000;

    /** How many of a plugin's parameters the menu will list. See
        addPluginParamSubmenu() for why this is not "all of them". */
    constexpr int kMaxListedPluginParams = 64;

    //-- Phase 82: the two dropdowns -----------------------------------------
    /** Ids for the LEFT dropdown ("which curve"). Each box builds its own menu,
        so these only have to avoid each other inside one menu — keeping them
        shared anyway means reading the third box is reading something already
        known. */
    constexpr int kCurveItemBase   = 200;   // + index → activate that curve
    constexpr int kCurveMenuNew    = 290;
    constexpr int kCurveMenuDelete = 291;
    constexpr int kCurveMenuReset  = 292;   // flatten and park what is open

    /** Ids for the RIGHT dropdown ("where it goes"). `none` is 空: the curve
        keeps its shape and drives nothing. */
    constexpr int kRouteMenuNone = 1;
    constexpr int kRouteItemBase = 10;      // + the target's own ordinal

    /** One row of a box's curve list, in a form the menu builder can read. */
    struct CurveMenuItem
    {
        int          id;
        juce::String name;
        juce::String route;      // empty = 空
    };

    /** The "which curve" half of the left dropdown. Shared by all three boxes
        so the three lists cannot drift apart — same words, same order, same
        verbs at the bottom. Returns nothing: the caller maps `result` back to
        an id through `items`. */
    inline void addCurveItems (juce::PopupMenu& menu, const std::vector<CurveMenuItem>& items,
                        int activeId)
    {
        menu.addSectionHeader (utf8 ("曲线"));

        for (int i = 0; i < (int) items.size(); ++i)
        {
            const auto& item = items[(size_t) i];

            juce::String text = item.name.isNotEmpty() ? item.name
                                                       : utf8 ("曲线 ") + juce::String (i + 1);

            // The route is shown next to the name because the list's whole job
            // is answering "which of these is the one on pan?" without opening
            // five of them in turn.
            text << "   ·   " << (item.route.isEmpty() ? utf8 ("空") : "→ " + item.route);

            menu.addItem (kCurveItemBase + i, text, true, item.id == activeId);
        }
    }

    inline void addCurveVerbs (juce::PopupMenu& menu, bool canDelete)
    {
        menu.addSeparator();
        menu.addItem (kCurveMenuNew, utf8 ("新建曲线…"));
        menu.addItem (kCurveMenuDelete, utf8 ("删除这条曲线"), canDelete);
    }

    /** What a note's curve is pointed at, in words. `curveTargetPlugin` is a
        fourth value that is NOT a NoteModTarget, so casting it would name the
        wrong thing — this is the only place allowed to translate it. */
    inline juce::String noteRouteLabel (const CurveSlot<NoteEnvelope>& slot)
    {
        if (slot.target < 0)
            return {};

        if (slot.target == NoteEvent::curveTargetPlugin)
            return utf8 ("音源参数");

        return modTargetLabel ((NoteModTarget) slot.target);
    }

    inline juce::String sampleRouteLabel (const CurveSlot<NoteEnvelope>& slot)
    {
        if (slot.target < 0)
            return {};

        return slot.target == (int) SampleAutomationTarget::pan ? utf8 ("声相") : utf8 ("音量");
    }

    inline juce::String groupRouteLabel (const CurveSlot<AutomationLane>& slot)
    {
        if (slot.target < 0)
            return {};

        switch ((GroupAutomationTarget) slot.target)
        {
            case GroupAutomationTarget::pan:    return utf8 ("组声相");
            case GroupAutomationTarget::plugin: return utf8 ("音源参数");
            case GroupAutomationTarget::level:
            default:                            return utf8 ("组音量");
        }
    }

    /** A knob: ring plus pointer. The universal "this is a synth parameter, not
        a fader of ours" glyph, used wherever a curve drives a plugin. */
    inline void drawKnobGlyph (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
    {
        g.setColour (colour);

        const auto ring = area.reduced (1.0f);
        g.drawEllipse (ring, 1.2f);

        const auto  centre = ring.getCentre();
        const float radius = juce::jmin (ring.getWidth(), ring.getHeight()) * 0.5f;

        juce::Path needle;
        needle.startNewSubPath (centre);
        needle.lineTo (centre.x + 0.18f * radius, centre.y - 0.80f * radius);

        g.strokePath (needle, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
    }

    /** The value a lane "resets to": unity for a level, centre for a pan, and
        centre for a plugin parameter — not zero. An emptied curve must not also
        mean "mute", or cutting a curve would silence the thing it belonged to;
        and 1.0 as a plugin default would slam a cutoff wide open the first time
        anyone cut its lane. */
    inline float neutralCurveValue (int target, bool groupTarget)
    {
        if (groupTarget)
            return (target == (int) GroupAutomationTarget::pan
                      || target == (int) GroupAutomationTarget::plugin) ? 0.5f : 1.0f;

        return target == (int) NoteModTarget::pan ? 0.5f : 1.0f;
    }

    /** Phase 24: "组 A" -> "组 A 2" (then 3, 4...) until it is free.

        A pasted group needs its own name because the group list is a list of
        names; two rows both called 组 A is a list you cannot read. Keeping the
        original as the prefix is the useful half of the information: it says
        where the copy came from. */
    inline juce::String uniqueGroupName (const Layer& layer, const juce::String& base)
    {
        const auto taken = [&layer] (const juce::String& candidate)
        {
            for (const auto& group : layer.getGroups())
                if (group.getName() == candidate)
                    return true;

            return false;
        };

        if (! taken (base))
            return base;

        for (int suffix = 2; suffix < 1000; ++suffix)
        {
            const auto candidate = base + " " + juce::String (suffix);

            if (! taken (candidate))
                return candidate;
        }

        return base;
    }

    // Note naming lives in Util/Text.h so the panel and the roll agree.

    /** Phase 74: "add to the selection" as the rest of the app spells it. Shift
        and Ctrl both mean it here, the same pair the clip hit test accepts — a
        group grab must not require a different modifier from a plain note. */
    inline bool isAdditive (juce::ModifierKeys mods) noexcept
    {
        return mods.isShiftDown() || mods.isCommandDown();
    }


//==============================================================================
//  Phase 82: one row, two buttons
//==============================================================================
    /** Below this width a cell carries a glyph instead of its own name: the
        same rule the one-button row already had, because a 1/16 note is 12 px
        wide and "音量" is not 12 px of anything. One predicate, used by the
        paint and by the painter of the narrow variant, so the two cannot end
        up disagreeing about where the word stops and the shape starts. */
    inline bool curveCellFitsText (juce::Rectangle<float> cell)
    {
        return cell.getWidth() >= 34.0f && cell.getHeight() >= ui::pf (14);
    }
} // namespace roll
} // namespace atomic
