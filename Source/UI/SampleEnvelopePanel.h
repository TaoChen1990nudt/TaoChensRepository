#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Util/Metrics.h"
#include "Util/Theme.h"

#include <functional>

#include "Model/HardwareRack.h"
#include "Model/LayerModel.h"

#include "UI/NoteEnvelopePanel.h"   // for EnvelopeEditor

namespace atomic
{

//==============================================================================
/**
    Phase 78: the clip's own envelope window — the note panel's twin.

    Why a window at all, when Phase 49 already gave a clip a floating curve box:
    the box is a strip that hugs the block, which is right for a one-line tweak
    while arranging, and wrong for everything else. It cannot show the identity
    of what you are editing, it cannot say which of the two curves you have, and
    it disappears the moment you click elsewhere. Double-clicking a note opens a
    real panel; double-clicking a block should land somewhere at least as
    capable, or "open the block" means two different things depending on which
    kind of object you happened to hit.

    What this panel owns:

    - **Which curve.** A clip has two (volume, pan, Phase 50) and they do not
      share a neutral point — volume idles at 1.0, pan at 0.5. The switch writes
      `SampleEvent::automationTarget`, the same field the floating box and the
      project file use, so the two editors never disagree about "the active
      curve".

    - **The curve itself**, through `EnvelopeEditor` — the very same component
      the note panel runs, bound to `SampleEvent::curveFor(target)`. Not a copy,
      not a mirror: adding a point here edits the model the renderer reads.

    - **The block's output**, through `SampleEvent::outputSlotId`, resolved by
      `routing::resolveSample` — the same resolver the engine uses. A menu that
      offered a slot the engine would ignore is worse than no menu.

    The floating box (P49) stays, and stays the chevron's job: a strip for a
    quick edit on the timeline, this window for "show me what this clip is and
    let me shape it". Opening one closes the other — two editors claiming to
    show the same curve at the same time is how users learn not to trust either.
*/
class SampleEnvelopePanel final : public juce::Component,
                                  public LayerStack::Listener,
                                  public RackModel::Listener
{
public:
    static constexpr int panelWidth  = ui::px (452);
    static constexpr int panelHeight = ui::px (314);

    SampleEnvelopePanel (LayerStack& layerStackToUse, RackModel& rackToUse);
    ~SampleEnvelopePanel() override;

    /** Addresses the block by (layerIndex, sampleId) — never by pointer: the
        vector of clips reallocates on every add/remove, and the user can delete
        the block this panel is editing while it is open. */
    void openFor (int newLayerIndex, int newSampleId);
    void close();

    bool isOpen() const noexcept               { return sampleId >= 0; }
    int  getEditedSampleId() const noexcept    { return sampleId; }
    int  getEditedLayerIndex() const noexcept  { return layerIndex; }

    /** Which curve is being edited: 0 = volume, 1 = pan. This is the ordinal of
        `SampleAutomationTarget`, and writing it goes through the same path the
        two pills in the header use — one entry point, so a test cannot exercise
        a switch the user does not have. */
    int  getTargetOrdinal() const noexcept;
    void setTargetOrdinal (int ordinal);

    /** 0 = ADSR, 1 = fade out, 2 = flat. The same three the note panel offers,
        except that "flat" means 0.5 when the target is pan: a pan curve
        flattened to 1.0 would be hard right, which is not what the word says. */
    void applyPreset (int index);

    EnvelopeEditor& getEditor() noexcept       { return editor; }

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
    SampleEvent* getSample() const;

    juce::Rectangle<int> getCloseBounds() const;
    juce::Rectangle<int> getPresetBounds (int index) const;
    juce::Rectangle<int> getOutputBounds() const;
    juce::Rectangle<int> getTargetBounds (int ordinal) const;

    void showOutputMenu();

    /** The caption pill inside the editor: which of the two curves this is. */
    void updateCaption();

    LayerStack& stack;
    RackModel&  rack;

    EnvelopeEditor editor;

    int layerIndex = -1;
    int sampleId   = -1;

    int  hoveredPreset = -1;
    int  hoveredTarget = -1;
    bool hoveringClose = false;
    bool hoveringOutput = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SampleEnvelopePanel)
};

} // namespace atomic
