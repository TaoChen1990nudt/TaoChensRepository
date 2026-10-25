#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

#include "Util/Text.h"

namespace atomic::ui
{

//==============================================================================
/**
    Phase 20: the right-click menu every numeric control shares.

    A fader, a pan, a tempo field and a group trim are all "a number you drag",
    and until now none of them answered a right click at all. What a numeric
    control is *for* has an obvious answer at that moment — put it back where it
    started — so every one of them now offers 复位, spelled the same way
    everywhere so the gesture is learned once.

    `addExtra`/`onExtra` exist because three of these controls share their pixel
    with an owner that has more to say (a layer's automation, a group's mount).
    Rather than invent a second right-click meaning for those rows, the shared
    menu grows extra items that open the owner's own menu. Item ids handed to
    `onExtra` are >= 10; 1 is reserved for 复位.
*/
inline void showValueMenu (juce::Component* owner,
                           juce::Point<int> screenPos,
                           const juce::String& title,
                           const juce::String& valueText,
                           std::function<void()> onReset,
                           std::function<void (juce::PopupMenu&)> addExtra = {},
                           std::function<void (int)> onExtra = {})
{
    juce::PopupMenu menu;
    menu.addSectionHeader (title + utf8 (" · ") + valueText);
    menu.addItem (1, utf8 ("复位"));

    if (addExtra != nullptr)
    {
        menu.addSeparator();
        addExtra (menu);
    }

    juce::Component::SafePointer<juce::Component> safe (owner);

    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetScreenArea (juce::Rectangle<int> (screenPos.x, screenPos.y,
                                                                         1, 1)),
                        [safe, onReset, onExtra] (int result)
                        {
                            if (safe == nullptr || result <= 0)
                                return;

                            if (result == 1)
                            {
                                if (onReset != nullptr)
                                    onReset();

                                return;
                            }

                            if (onExtra != nullptr)
                                onExtra (result);
                        });
}

} // namespace atomic::ui
