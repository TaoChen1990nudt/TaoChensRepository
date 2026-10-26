#include "MixerWindow.h"

#include "Util/Metrics.h"

#include "MixerPanel.h"
#include "Util/Text.h"

namespace atomic
{

namespace
{
    const juce::Colour kWindowBackground { 0xff14151a };
}

MixerWindow::MixerWindow (MixerPanel& panelToShow)
    : juce::DocumentWindow (utf8 ("AtomicDAW · 混音台"), kWindowBackground,
                            juce::DocumentWindow::closeButton),
      panel (panelToShow)
{
    setUsingNativeTitleBar (true);

    // Non-owned: the shell keeps the panel alive across close/open cycles.
    setContentNonOwned (&panel, false);

    setResizable (true, false);
    setResizeLimits (980, 250, 2600, 640);
    centreWithSize (atomic::ui::px (1180), atomic::ui::px (300));

    setVisible (true);
}

MixerWindow::~MixerWindow()
{
    // Must happen here: DocumentWindow would otherwise leave the panel parented
    // to a component that is about to die.
    clearContentComponent();
}

void MixerWindow::closeButtonPressed()
{
    if (onCloseRequest != nullptr)
        onCloseRequest();
}

void MixerWindow::setKeyForwarder (juce::KeyListener* listener)
{
    if (keyForwarder == listener)
        return;

    if (keyForwarder != nullptr)
    {
        removeKeyListener (keyForwarder);

        if (auto* content = getContentComponent())
            content->removeKeyListener (keyForwarder);
    }

    keyForwarder = listener;

    if (keyForwarder != nullptr)
    {
        // Both layers, for the reason spelled out in PluginEditorWindow: the
        // focus can sit on any child of the panel, and a listener that only
        // reaches the window is a listener the faders never call.
        addKeyListener (keyForwarder);

        if (auto* content = getContentComponent())
            content->addKeyListener (keyForwarder);
    }
}

} // namespace atomic
