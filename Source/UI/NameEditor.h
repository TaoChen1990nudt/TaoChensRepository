#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace atomic
{

//==============================================================================
/**
    P79: one line of text, edited where it is written.

    Every name in this app is a word printed on a row — the layer list, a synth
    card, a group frame's row. The way to change one is therefore to type over
    it, not to open a modal that asks "new name:" and leaves the row you meant
    ambiguous until you press OK. The editor is placed exactly on the rectangle
    the row uses to DRAW that name, so the text does not move while you type it.

    One component for all three panels on purpose. Three copies of "TextEditor
    configured to look like a label" is three places for the font, the commit
    keys and the empty-name rule to drift apart — and the drift would be
    invisible until a name looked different in one panel than another.

    Contract:
      - begin() with the CURRENT name; the caret selects all of it, because the
        overwhelmingly common intent is "replace this".
      - Enter, or clicking away, commits.
      - Escape commits nothing.
      - An empty (or whitespace-only) result commits nothing. A nameless row is
        a row you cannot find again; the model refuses it too, but a cancel here
        means the panel never even asks.

    Visibility belongs to this class, not to the host: the panels add it with
    addChildComponent (hidden) so an idle panel has one fewer thing drawing, and
    begin()/close() show and hide the whole box. Showing only the inner
    TextEditor — which is what happened first — paints nothing, because a child
    of a hidden parent is never drawn; the data still changed, so no model-level
    test could see it.
*/
class NameEditor final : public juce::Component
{
public:
    NameEditor();
    ~NameEditor() override;

    /** Called with the new text when the user commits one that differs from
        what the editor was opened with. Never called for a cancel, an empty
        result, or an unchanged word. */
    std::function<void (const juce::String&)> onCommit;

    /** Called after the editor has closed, whether it committed or not. The
        host uses it to forget which row it was renaming — without it, "is
        anything being renamed" would keep answering yes after an Escape. */
    std::function<void()> onClosed;

    /** Opens on `targetInParent` — a rectangle in the PARENT's coordinates,
        normally the one the row draws its name in.

        `designFontHeight` is in design units like every font in this app
        (ui::sp() is applied inside); `textColour` comes from the row so the
        editor matches the alpha the row was using. */
    void begin (juce::Rectangle<int> targetInParent,
                const juce::String& currentName,
                float designFontHeight,
                juce::Colour textColour);

    /** Closes the editor. `commit` false is a cancel — used when the thing
        being renamed went away (an undo, a deleted layer) while it was open. */
    void close (bool commit);

    /** Moves the open editor onto a new rectangle, keeping the typed text. A
        row can move under the box (a lane unfolded above it, the list scrolled)
        and the box has to follow the word rather than the row it started on. */
    void reposition (juce::Rectangle<int> targetInParent);

    bool isEditing() const noexcept { return editing; }

    /** The name the editor was opened with, so a caller can tell "committed the
        same word" from "committed a new one" without keeping a copy. */
    const juce::String& getInitialName() const noexcept { return initialName; }

    /** For the self-test: type into the real editor, then commit it. Tests that
        fake the keystrokes would be testing JUCE, not this component. */
    juce::TextEditor& getEditor() noexcept { return editor; }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void commitNow();
    void applyTarget (juce::Rectangle<int> targetInParent);

    juce::TextEditor editor;
    juce::String     initialName;

    bool editing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NameEditor)
};

} // namespace atomic
