#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Util/Metrics.h"
#include "Util/Theme.h"
#include <functional>

#include "Model/AutomationStack.h"
#include "Model/HardwareRack.h"
#include "Model/LayerModel.h"

namespace atomic
{

//==============================================================================
/** Phase 36: the three automation layers that decide one note's level, sampled
    onto the note's own 0..1 time base so they can be drawn in one box.

    `macro` is the layer's volume lane times the synth's level lane — two curves,
    one layer of the stack, because a note passes through both before anybody
    grouped it. `group` is the group's level lane. Neither includes the fader:
    this is a picture of the AUTOMATION, and folding a static trim into it would
    make the numbers disagree with the handles the user can see elsewhere.

    A layer with nothing live stays at the identity, so `anyLive()` false means
    "this note is not being automated by anything" and the editor can leave the
    box alone instead of drawing three flat rules at the top.
*/
struct AutomationOverlay
{
    static constexpr int steps = 64;

    std::vector<float> macro;      // steps + 1 samples, identity when not live
    std::vector<float> group;

    bool macroLive = false;
    bool groupLive = false;

    /** The note's own curve participates in LEVEL only. A curve driving pan or
        pitch is a real thing, but it does not multiply the level, and drawing it
        as if it did would be the picture lying about what the ear hears. */
    bool noteLive  = false;

    /** The group's mute, frozen as a zero. Kept separate from the curve so the
        readout can say "静音" instead of "0.00". */
    bool groupMuted = false;

    bool anyLive() const noexcept { return macroLive || groupLive || noteLive || groupMuted; }

    float macroAt (float u) const noexcept { return macroLive ? automation::sampleAt (macro, u) : 1.0f; }
    float groupAt (float u) const noexcept
    {
        if (groupMuted)
            return 0.0f;

        return groupLive ? automation::sampleAt (group, u) : 1.0f;
    }
};

//==============================================================================
/**
    The curve editor inside the note panel.

    It never caches a NoteEnvelope*. Note objects live in a std::vector, so any
    add/remove can reallocate and turn a stored pointer into a dangling one
    (duplicating a note is exactly such an event). Instead it holds a resolver
    and asks for the envelope on every access. Slow by nanoseconds, crash-proof
    by design.
*/
class EnvelopeEditor final : public juce::Component
{
public:
    EnvelopeEditor();

    void setEnvelopeSource (std::function<NoteEnvelope*()> source);
    void setAccentColour (juce::Colour colour);

    /** Phase 36: the macro and group layers, already sampled onto this note's
        time base. The editor draws them as reference lines under the note's own
        curve and multiplies all three into one result line — through
        `automation::stackLevel`, the same function the audio thread calls, so
        the picture cannot flatter the sound. */
    void setOverlay (AutomationOverlay newOverlay);

    /** Where the playhead sits inside this note, 0..1, or negative when the
        note is not currently sounding. Purely visual: it is the same
        NoteEvent::progressAt() value the piano roll draws with. */
    void setPlayheadProgress (double progress);

    /** Fired after every edit so the host can repaint the piano roll. */
    std::function<void()> onChange;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    NoteEnvelope* resolve() const;
    juce::Rectangle<float> getCurveArea() const;
    juce::Point<float> pointToScreen (const EnvelopePoint&) const;
    EnvelopePoint      screenToPoint (juce::Point<float>) const;
    int findPointAt (juce::Point<float>) const;

    /** Phase 36: one reference curve, drawn behind the note's own. */
    void drawReferenceCurve (juce::Graphics&, juce::Rectangle<float> area,
                             const std::vector<float>& curve, juce::Colour colour) const;

    /** Phase 36: the product of all three layers, which is what actually
        sounds. Draws the fill and the line, and returns the level at `u` so the
        legend can quote the same number. */
    float drawStackCurve (juce::Graphics&, juce::Rectangle<float> area,
                          const NoteEnvelope* envelope) const;

    std::function<NoteEnvelope*()> envelopeSource;
    juce::Colour accent = theme::accent;
    double playheadProgress = -1.0;
    AutomationOverlay overlay;

    int draggedIndex = -1;
    int hoveredIndex = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EnvelopeEditor)
};

//==============================================================================
/**
    Floating note inspector: identity readout, output routing and per-note
    envelope editing.

    Opened by double-clicking a note in the ACTIVE layer. The note is addressed
    by (layerIndex, noteId), never by pointer, so moving / duplicating the note
    while the panel is open cannot invalidate it.

    The "输出" row is the note-level bypass from Phase 3: it writes
    NoteEvent::outputSlotOverride, which routing::resolve() prefers over the
    layer's default. Nothing here talks to the audio engine — the engine listens
    to the model and republishes, so the change is audible on the next block.
*/
class NoteEnvelopePanel final : public juce::Component,
                                public LayerStack::Listener,
                                public RackModel::Listener
{
public:
    static constexpr int panelWidth  = ui::px (452);
    static constexpr int panelHeight = ui::px (314);

    NoteEnvelopePanel (LayerStack& layerStackToUse, RackModel& rackToUse);
    ~NoteEnvelopePanel() override;

    void openFor (int layerIndex, int noteId);
    void close();

    bool isOpen() const noexcept              { return noteId >= 0; }
    int  getEditedNoteId() const noexcept     { return noteId; }
    int  getEditedLayerIndex() const noexcept { return layerIndex; }

    /** Drives the playhead marker in the editor. Called by the transport at
        frame rate; does nothing (and repaints nothing) while the panel is shut
        or the note is not sounding. */
    void setPlayheadBeat (double beats);

    EnvelopeEditor& getEditor() noexcept      { return editor; }

    /** Phase 36: the three-layer picture this panel is currently drawing. The
        self-test reads it to prove the overlay and the renderer agree — a
        legend that quotes a different number from the one the ear hears is
        worse than no legend. */
    const AutomationOverlay& getOverlay() const noexcept { return overlay; }

    std::function<void()> onClosed;

    //-- Component ------------------------------------------------------------
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

    //-- Listeners ------------------------------------------------------------
    void layersChanged() override;
    void notesChanged() override;
    void rackChanged() override;

private:
    NoteEvent* getNote() const;

    /** Phase 36: rebuilds `overlay` from the model. Called whenever a curve
        outside this panel can have moved — a lane drag in the automation
        window, a group fader, a routing change — because the overlay is the
        only thing here that depends on more than this note. */
    void rebuildOverlay();

    juce::Rectangle<int> getCloseBounds() const;
    juce::Rectangle<int> getPresetBounds (int index) const;
    juce::Rectangle<int> getOutputBounds() const;
    void applyPreset (int index);
    void showOutputMenu();

    LayerStack& stack;
    RackModel&  rack;
    EnvelopeEditor editor;

    /** Phase 36: kept as a member, not only pushed into the editor, so the
        self-test can read the same numbers the painter drew. */
    AutomationOverlay overlay;

    int layerIndex = -1;
    int noteId     = -1;
    double playheadBeat = -1.0;

    int  hoveredPreset = -1;
    bool hoveringClose = false;
    bool hoveringOutput = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoteEnvelopePanel)
};

} // namespace atomic
