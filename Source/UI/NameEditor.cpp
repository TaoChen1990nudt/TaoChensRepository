#include "NameEditor.h"

#include "Util/Metrics.h"
#include "Util/Theme.h"

namespace atomic
{

//==============================================================================
NameEditor::NameEditor()
{
    setInterceptsMouseClicks (true, true);

    editor.setVisible (false);
    editor.setJustification (juce::Justification::centredLeft);
    editor.setInputRestrictions (64);
    editor.setColour (juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    editor.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    editor.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    editor.setColour (juce::TextEditor::highlightColourId, theme::accent.withAlpha (0.30f));
    editor.setColour (juce::CaretComponent::caretColourId, theme::accent);
    editor.setBorder (juce::BorderSize<int> (0));

    editor.onReturnKey = [this] { close (true); };
    editor.onEscapeKey = [this] { close (false); };

    // Clicking away is a commit, not a cancel. The user's gesture there was
    // "I am done with this row", and throwing the typing away would be the
    // program insisting on a keystroke it never asked for. Same rule the tempo
    // field follows.
    editor.onFocusLost = [this] { close (true); };

    addChildComponent (editor);
}

NameEditor::~NameEditor() = default;

//==============================================================================
void NameEditor::begin (juce::Rectangle<int> targetInParent,
                        const juce::String& currentName,
                        float designFontHeight,
                        juce::Colour textColour)
{
    initialName = currentName;
    editing     = true;

    applyTarget (targetInParent);

    editor.setFont (juce::Font (juce::FontOptions{}.withHeight (atomic::ui::sp (designFontHeight))
                                   .withStyle ("Bold")));
    editor.setColour (juce::TextEditor::textColourId, textColour);
    editor.setText (currentName, juce::dontSendNotification);

    // Ourselves first: the component is added hidden (see the header), so
    // showing the text editor inside a hidden parent shows nothing at all —
    // the exact bug this line exists to prevent, and one the model-level tests
    // could not see because the data changed either way.
    setVisible (true);
    toFront (true);

    editor.setVisible (true);
    editor.toFront (true);
    editor.selectAll();
    editor.grabKeyboardFocus();

    toFront (true);
    repaint();
}

void NameEditor::close (bool commit)
{
    if (! editing)
        return;

    // Cleared FIRST: hiding the editor fires onFocusLost, which calls straight
    // back in here. Without this the two paths re-enter each other.
    editing = false;

    if (commit)
        commitNow();

    editor.setVisible (false);
    setVisible (false);

    // The host's row is only repainted once the box is gone, or the last frame
    // would show the field's plate over the newly renamed row.
    if (auto* parent = getParentComponent())
        parent->repaint();

    repaint();

    if (onClosed != nullptr)
        onClosed();
}

void NameEditor::reposition (juce::Rectangle<int> targetInParent)
{
    if (! editing)
        return;

    applyTarget (targetInParent);
    repaint();
}

void NameEditor::applyTarget (juce::Rectangle<int> targetInParent)
{
    // Grown by 2 px on each side: the row's rectangle is sized for a label, and
    // a text field that is exactly as tall as the glyphs looks cramped the
    // moment a caret is in it.
    setBounds (targetInParent.expanded (ui::px (2), ui::px (2)));
    resized();
}

void NameEditor::commitNow()
{
    const auto typed = editor.getText().trim();

    if (typed.isEmpty())
        return;              // a nameless row is a row you cannot find again

    if (typed == initialName.trim())
        return;              // opened and closed on the same word

    if (onCommit != nullptr)
        onCommit (typed);
}

//==============================================================================
void NameEditor::paint (juce::Graphics& g)
{
    if (! editing)
        return;

    auto bounds = getLocalBounds().toFloat();

    // Opaque, not a tint: the name behind it has to disappear while the user
    // types over it. A translucent plate showed both strings at once — the old
    // name bleeding through the new one.
    g.setColour (theme::surface3);
    g.fillRoundedRectangle (bounds, ui::pf (4));

    g.setColour (theme::accent.withAlpha (0.75f));
    g.drawRoundedRectangle (bounds.reduced (0.5f), ui::pf (4), 1.0f);
}

void NameEditor::resized()
{
    editor.setBounds (getLocalBounds().reduced (ui::px (5), ui::px (1)));
}

} // namespace atomic
