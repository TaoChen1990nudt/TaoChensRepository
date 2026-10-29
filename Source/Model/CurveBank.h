#pragma once

#include <juce_core/juce_core.h>

#include <utility>
#include <vector>

#include "Util/Text.h"

namespace atomic
{

//==============================================================================
/** One curve in an object's bank: a shape, a name, and somewhere to go.

    Phase 82. Until now an object's curves were a FIXED set, and the set was the
    enum: a note had one curve plus a selector saying what it drove, a clip had
    exactly two members, a group had three resident lanes. Every one of those is
    a design where "which curve drives which parameter" has exactly one answer
    per parameter, and there is no way to say "keep this shape, park it, try the
    other one" — or "I want a level curve AND a pitch curve on this note".

    A slot answers both questions at once, and keeps them separable:

      * `curve` is the shape, and it belongs to the object (by value, like every
        curve in this project — move the object, the shape moves with it).
      * `target` is where it points, or -1 for NOWHERE. Nowhere is a real
        answer, not a missing one: the curve stays, stays drawn, and drives
        nothing until the user points it somewhere. Deleting and un-routing are
        different operations because they are different decisions.
      * `pluginParam` rides along because a plugin parameter is an index into
        one vendor's list (Phase 24's argument) — the address has to travel with
        the target, never on its own.
*/
template <typename CurveType>
struct CurveSlot
{
    int          id          = 0;
    juce::String name;
    int          target      = -1;   // -1 = 空：画着，但不驱动任何东西
    int          pluginParam = -1;
    CurveType    curve;

    bool isRouted() const noexcept { return target >= 0; }
};

//==============================================================================
/**
    An object's curves: an ordered list, one of them on screen, one target each.

    The three rules this class exists to make uniform:

    1. **A target belongs to at most one curve.** Two curves on one parameter is
       two answers to "what is the level here", and the audio thread would have
       to combine them — which is not what the box draws. So `route()` takes the
       target away from whoever had it. (The plugin target is the exception that
       proves the rule: parameter 3 and parameter 17 of the same synth are
       different destinations, so they are told apart by `pluginParam`.)

    2. **The list is never empty.** Every reader in the engine dereferences the
       active curve without checking, and a note with no curve at all would be a
       note whose level is undefined. `remove()` refuses the last one.

    3. **Ids, not indices.** The box keeps a curve selected while curves are
       added and deleted above it; an index would silently re-point the editor
       at somebody else's shape.

    A value type, not a shared_ptr, for the same reason the curves themselves
    are: copying a note must copy its automation, and there must be no way to
    end up with two notes sharing one curve.
*/
template <typename CurveType>
class CurveBank
{
public:
    using Slot = CurveSlot<CurveType>;

    /** A ceiling, not a target: the box lists these in a menu, and a menu of
        sixteen curves is a menu nobody can tell apart. */
    static constexpr int maxCurves = 8;

    CurveBank() = default;

    /** Builds a bank that already holds its first curve, which is what every
        object wants: an empty bank would be a state with no valid "active". */
    static CurveBank withOne (CurveType initial, juce::String curveName, int target,
                              int pluginParam = -1)
    {
        CurveBank bank;
        const int id = bank.add (std::move (initial), std::move (curveName));

        if (id > 0)
            bank.route (id, target, pluginParam);

        return bank;
    }

    int  getNumCurves() const noexcept { return (int) slots.size(); }
    bool isEmpty() const noexcept      { return slots.empty(); }

    const std::vector<Slot>& getCurves() const noexcept { return slots; }
    std::vector<Slot>&       getCurves()       noexcept { return slots; }

    Slot* find (int curveId) noexcept
    {
        for (auto& s : slots)
            if (s.id == curveId)
                return &s;

        return nullptr;
    }

    const Slot* find (int curveId) const noexcept
    {
        for (const auto& s : slots)
            if (s.id == curveId)
                return &s;

        return nullptr;
    }

    /** The curve driving `target` (and, for the plugin target, that particular
        parameter), or null when nobody drives it. At most one by `route()`'s
        exclusivity rule — this is a lookup, not a search for the loudest. */
    Slot* findRoutedTo (int target, int pluginParam = -1) noexcept
    {
        if (target < 0)
            return nullptr;

        for (auto& s : slots)
            if (s.target == target && s.pluginParam == pluginParam)
                return &s;

        return nullptr;
    }

    const Slot* findRoutedTo (int target, int pluginParam = -1) const noexcept
    {
        if (target < 0)
            return nullptr;

        for (const auto& s : slots)
            if (s.target == target && s.pluginParam == pluginParam)
                return &s;

        return nullptr;
    }

    bool isTargetDriven (int target, int pluginParam = -1) const noexcept
    {
        return findRoutedTo (target, pluginParam) != nullptr;
    }

    //-- Which curve the box shows -------------------------------------------
    int getActiveId() const noexcept { return activeId; }

    void setActiveId (int curveId) noexcept
    {
        if (find (curveId) != nullptr)
            activeId = curveId;
    }

    Slot& getActive() noexcept
    {
        if (auto* s = find (activeId))
            return *s;

        return slots.front();      // 不变量：bank 永远至少一条
    }

    const Slot& getActive() const noexcept
    {
        if (auto* s = find (activeId))
            return *s;

        return slots.front();
    }

    //-- Editing -------------------------------------------------------------
    /** Adds a curve holding `initial`'s shape. Returns its id, or -1 when full.

        The new curve starts UNROUTED: it is a scratch shape until the user says
        where it goes. Routing it on creation would silently steal the target
        from whatever was driving it, which is a side effect nobody asked for
        when they clicked "新建曲线". */
    int add (CurveType initial, juce::String curveName = {})
    {
        if ((int) slots.size() >= maxCurves)
            return -1;

        Slot slot;
        slot.id    = nextId++;
        slot.name  = curveName.isNotEmpty()
                       ? curveName
                       : utf8 ("曲线 ") + juce::String (nextCurveNumber++);
        slot.curve = std::move (initial);

        slots.push_back (std::move (slot));

        if (activeId < 0)
            activeId = slots.back().id;

        return slots.back().id;
    }

    /** Adds a curve under a GIVEN id, for a project load.

        The caller owns uniqueness. This exists because the file remembers which
        curve was on screen, and that memory is an id — so the ids in a loaded
        bank cannot be silently renumbered by the order the file happened to be
        read in. `nextId` is pushed past whatever the file used, so a curve the
        user creates after loading cannot collide with one they already had. */
    int addWithId (int curveId, CurveType initial, juce::String curveName = {})
    {
        if ((int) slots.size() >= maxCurves)
            return -1;

        Slot slot;
        slot.id    = curveId > 0 ? curveId : nextId;
        slot.name  = curveName.isNotEmpty()
                       ? curveName
                       : utf8 ("曲线 ") + juce::String (nextCurveNumber++);
        slot.curve = std::move (initial);

        nextId = juce::jmax (nextId, slot.id + 1);

        slots.push_back (std::move (slot));

        if (activeId < 0)
            activeId = slots.back().id;

        return slots.back().id;
    }

    /** Deletes a curve. Refuses the last one: an object always has a shape. */
    bool remove (int curveId)
    {
        if ((int) slots.size() <= 1)
            return false;

        for (size_t i = 0; i < slots.size(); ++i)
        {
            if (slots[i].id != curveId)
                continue;

            const bool wasActive = (slots[i].id == activeId);

            slots.erase (slots.begin() + (std::ptrdiff_t) i);

            if (wasActive)
                activeId = slots[juce::jmin (i, slots.size() - 1)].id;

            return true;
        }

        return false;
    }

    /** Points `curveId` at `target`, taking it away from whoever had it.
        `target < 0` parks the curve: it stays, it just drives nothing. */
    void route (int curveId, int target, int pluginParam = -1)
    {
        auto* self = find (curveId);

        if (self == nullptr)
            return;

        const int wantedParam = target >= 0 ? pluginParam : -1;

        if (target >= 0)
        {
            for (auto& s : slots)
                if (s.id != curveId && s.target == target && s.pluginParam == wantedParam)
                {
                    s.target      = -1;
                    s.pluginParam = -1;
                }
        }

        self->target      = target;
        self->pluginParam = wantedParam;
    }

    /** Empties the bank for a project load. Callers must add at least one. */
    void clear()
    {
        slots.clear();
        activeId = -1;
        nextId   = 1;
    }

private:
    std::vector<Slot> slots;
    int activeId         = -1;
    int nextId           = 1;
    int nextCurveNumber  = 2;    // 第一条由宿主命名（"音量"/"声相"…），新造的从 2 起
};

} // namespace atomic
