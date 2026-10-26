#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace atomic
{

class MixerPanel;

//==============================================================================
/**
    The mixer as its own desktop window.

    Phase 8 moved the console out of the shell: eight strips plus a rack do not
    fit under a piano roll without stealing the height the roll needs, and a
    mixer is something you open, look at, and close again.

    The window does NOT own the panel. MixerPanel is a member of MainComponent,
    because the engine, the meters and the undo history all point at it; handing
    ownership to a document window would mean a closed window destroys state
    that the rest of the app still refers to.
*/
class MixerWindow final : public juce::DocumentWindow
{
public:
    explicit MixerWindow (MixerPanel& panelToShow);
    ~MixerWindow() override;

    /** The close button hides the window instead of killing the mixer, so the
        faders are exactly where they were the next time it opens. */
    std::function<void()> onCloseRequest;

    /** Phase 43: 空格 and the computer keyboard have to keep working while the
        console has focus.

        Same hole Phase 42 plugged for a plugin's editor: this is a top-level
        window, so its keys never reach the main component's parent chain. The
        shell owns the key map, so it owns the listener — the window only carries
        it, on itself AND on the panel it hosts. */
    void setKeyForwarder (juce::KeyListener* listener);
    bool hasKeyForwarder() const noexcept { return keyForwarder != nullptr; }

    void closeButtonPressed() override;

private:
    MixerPanel& panel;
    juce::KeyListener* keyForwarder = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerWindow)
};

} // namespace atomic
