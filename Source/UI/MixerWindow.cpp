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

} // namespace atomic
