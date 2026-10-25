#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Util/Metrics.h"
#include <functional>

#include "Model/HardwareRack.h"
#include "Model/LayerModel.h"

namespace atomic
{

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

    std::function<NoteEnvelope*()> envelopeSource;
    juce::Colour accent { 0xff4fd1c5 };
    double playheadProgress = -1.0;

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
    juce::Rectangle<int> getCloseBounds() const;
    juce::Rectangle<int> getPresetBounds (int index) const;
    juce::Rectangle<int> getOutputBounds() const;
    void applyPreset (int index);
    void showOutputMenu();

    LayerStack& stack;
    RackModel&  rack;
    EnvelopeEditor editor;

    int layerIndex = -1;
    int noteId     = -1;
    double playheadBeat = -1.0;

    int  hoveredPreset = -1;
    bool hoveringClose = false;
    bool hoveringOutput = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoteEnvelopePanel)
};

} // namespace atomic
