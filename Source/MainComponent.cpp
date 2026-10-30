#include "MainComponent.h"

#include "Util/Metrics.h"
#include "Util/SharedRules.h"
#include "Util/Theme.h"

#include "Model/AutomationStack.h"
#include "Model/Routing.h"
#include "Model/SamplePath.h"
#include "Audio/Mp3Encoder.h"
#include "UI/MixerLook.h"
#include "Util/Text.h"

#include <algorithm>
#include <cmath>

#include <cstring>
#include <iostream>

namespace atomic
{

namespace
{
    /** Only used by the self-test's diagnostic text. */
    inline juce::String loadedInsertName8 (const MixerModel& m)
    {
        return fx::typeName (m.getInsert (2, 3).type) + " @ slot 4 / master "
                 + fx::typeName (m.getInsert (MixerModel::masterChannel, 0).type);
    }

    // Step 1 of the palette convergence: these four were the only values in the
    // file, and every one of them already matched a `theme` role. Aliased
    // rather than renamed so the 20-odd use sites below read unchanged.
    const juce::Colour& kAppBackground = theme::bgApp;
    const juce::Colour& kTopBar        = theme::topBar;
    const juce::Colour& kDivider       = theme::hairline;
    const juce::Colour& kAccent        = theme::accent;
    const juce::Colour& kPlayhead      = theme::record;

    juce::Font boldFont (float height)
    {
        return juce::Font (juce::FontOptions{}.withHeight (atomic::ui::sp (height)).withStyle ("Bold"));
    }



    /** Phase 33: what a STOP press means, as a value.

        `stop` says whether the press does anything at all, `beat` where the
        playhead lands when it does. Two cases, both of them the user's:

          * something is running  -> end it and rewind to zero;
          * nothing is running    -> do NOTHING. The playhead is also how you
            choose where playback starts, so a stop press while stopped must not
            throw away the bar you just clicked.

        It used to be "back to where this run started", which made the stop
        button a second play-from-here and left the keyboard with no way to
        start playback — see the space key below. */




    /** The background half of the bounce.

        It owns nothing from MainComponent: by the time it starts, the schedule
        has already been frozen into a shared_ptr, so the user is free to keep
        editing (or close the window) while this renders. That is also why the
        dialog is shown from a static call rather than through the component. */
    class ExportTask final : public juce::ThreadWithProgressWindow
    {
    public:
        ExportTask (std::shared_ptr<const PublishedSchedule> scheduleToRender,
                    juce::File destinationFile,
                    ExportSettings settingsToUse,
                    std::function<void()> onFinished)
            : juce::ThreadWithProgressWindow (utf8 ("正在导出音频"), true, true, 20000, utf8 ("取消")),
              schedule (std::move (scheduleToRender)),
              destination (std::move (destinationFile)),
              settings (settingsToUse),
              finishedCallback (std::move (onFinished))
        {
        }

        void run() override
        {
            setStatusMessage (utf8 ("离线渲染中…"));

            result = bounce::renderToFile (destination, *schedule, settings,
                                           bounce::exportSampleRate, 0.05,
                                           [this] (double fraction)
                                           {
                                               setProgress (fraction);
                                               return ! threadShouldExit();
                                           });
        }

        void threadComplete (bool userPressedCancel) override
        {
            if (result.ok)
            {
                const juce::String peakText = juce::String (
                    juce::Decibels::gainToDecibels (result.peak, -100.0f), 1);

                juce::AlertWindow::showMessageBoxAsync (
                    juce::MessageBoxIconType::InfoIcon,
                    utf8 ("导出完成"),
                    destination.getFileName() + "\n"
                      + juce::String (result.seconds, 2) + utf8 (" 秒 · ") + settings.describe()
                      + utf8 ("\n峰值 ") + peakText + utf8 (" dBFS")
                      + (result.peakLimited ? utf8 ("（已启用峰值保护）") : juce::String())
                      + "\n\n" + destination.getFullPathName(),
                    utf8 ("好"));
            }
            else if (! userPressedCancel)
            {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                        utf8 ("导出失败"),
                                                        result.message.isNotEmpty() ? result.message
                                                                                    : utf8 ("未知错误"),
                                                        utf8 ("好"));
            }

            if (finishedCallback != nullptr)
                finishedCallback();

            // The documented pattern for launchThread(): the window owns itself.
            delete this;
        }

    private:
        std::shared_ptr<const PublishedSchedule> schedule;
        juce::File destination;
        ExportSettings settings;
        std::function<void()> finishedCallback;
        bounce::ExportResult result;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ExportTask)
    };

    /** Phase 50: the same thread-with-progress the export uses, but it does not
        announce itself. The result of a scope render is a block appearing on the
        timeline — that IS the completion signal, and a modal "done!" in front of
        it would have to be dismissed before the user could look at what they
        asked for. */
    class ScopeRenderTask final : public juce::ThreadWithProgressWindow
    {
    public:
        ScopeRenderTask (std::shared_ptr<const PublishedSchedule> scheduleToRender,
                         juce::File destinationFile,
                         double startSeconds,
                         PluginHost* plugins,
                         std::function<void (const juce::File&, const bounce::ExportResult&)> onFinished)
            : juce::ThreadWithProgressWindow (utf8 ("正在渲染为采样"), true, true, 20000, utf8 ("取消")),
              schedule (std::move (scheduleToRender)),
              destination (std::move (destinationFile)),
              start (startSeconds),
              pluginHost (plugins),
              finishedCallback (std::move (onFinished))
        {
        }

        void run() override
        {
            setStatusMessage (utf8 ("离线渲染中…"));

            result = bounce::renderToFile (destination, *schedule, bounce::exportSampleRate, 0.05,
                                        [this] (double fraction)
                                        {
                                            setProgress (fraction);
                                            return ! threadShouldExit();
                                        },
                                        pluginHost,
                                        start);
        }

        void threadComplete (bool userPressedCancel) override
        {
            if (! userPressedCancel && finishedCallback != nullptr)
                finishedCallback (destination, result);

            delete this;
        }

    private:
        std::shared_ptr<const PublishedSchedule> schedule;
        juce::File destination;
        double start = 0.0;
        PluginHost* pluginHost = nullptr;
        std::function<void (const juce::File&, const bounce::ExportResult&)> finishedCallback;
        bounce::ExportResult result;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScopeRenderTask)
    };

    /** Fills a popup with vendor submenus.

        A machine with Arturia, u-he and Steinberg installed has 90 instruments;
        one flat scrolling column of them is a wall nobody wants to read. Ids are
        `firstId + index` into `entries`, so the caller's switch is unchanged. */
    void addPluginItems (juce::PopupMenu& menu,
                         const std::vector<juce::PluginDescription>& entries,
                         int firstId,
                         const juce::String& tickedName = {})
    {
        juce::StringArray vendors;

        auto vendorOf = [] (const juce::PluginDescription& d)
        {
            return d.manufacturerName.isNotEmpty() ? d.manufacturerName
                                                   : juce::String (utf8 ("其他"));
        };

        for (const auto& d : entries)
        {
            const auto vendor = vendorOf (d);

            if (! vendors.contains (vendor))
                vendors.add (vendor);
        }

        vendors.sort (true);

        for (const auto& vendor : vendors)
        {
            juce::PopupMenu sub;

            for (int i = 0; i < (int) entries.size(); ++i)
                if (vendorOf (entries[(size_t) i]) == vendor)
                {
                    const auto& entry = entries[(size_t) i];

                    // Phase 38: the rack's replace menu says which plugin the
                    // slot is running right now. Without a tick, "did my
                    // replacement land?" is a question about a 9px pill.
                    sub.addItem (firstId + i, entry.name, true,
                                 tickedName.isNotEmpty() && entry.name == tickedName);
                }

            menu.addSubMenu (vendor, sub);
        }
    }
}

//==============================================================================
VerticalSplitter::VerticalSplitter()
{
    setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    setRepaintsOnMouseActivity (false);
}

void VerticalSplitter::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    const bool lit = hot || dragging;

    g.setColour (kAppBackground);
    g.fillRect (r);

    const float y = r.getCentreY();

    g.setColour (lit ? kAccent.withAlpha (0.65f) : juce::Colours::white.withAlpha (0.14f));
    g.fillRect (r.getX(), y - 0.5f, r.getWidth(), 1.0f);

    // A grip in the middle even when the mouse is elsewhere: the band has to
    // look like something you can grab, and a single hairline reads as a border
    // between two panels — which is what it is, but not all it is.
    g.setColour (lit ? kAccent.withAlpha (0.9f) : juce::Colours::white.withAlpha (0.30f));

    if (lit)
    {
        for (int i = 0; i < 2; ++i)
            g.fillRect (r.getCentreX() - 14.0f, y - 2.0f + (float) i * 3.0f, 28.0f, 1.0f);
    }
    else
    {
        // Three dots, not a dash: at rest this must read as "grab me", and a
        // solid line is indistinguishable from the seam between two panes.
        for (int i = -1; i <= 1; ++i)
            g.fillRect (r.getCentreX() + (float) i * 7.0f - 2.0f, y - 1.0f, 4.0f, 2.0f);
    }
}

void VerticalSplitter::mouseDown (const juce::MouseEvent& e)
{
    dragging = true;

    // Screen coordinates, not local ones: this component MOVES while it is
    // dragged, so `e.getPosition()` measured against it would keep cancelling
    // out the very travel it is supposed to report.
    dragStartScreenY = e.getScreenY();
    lastTravel       = 0;
    repaint();

    if (onDragBegin != nullptr)
        onDragBegin();
}

void VerticalSplitter::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging)
        return;

    const int travel = e.getScreenY() - dragStartScreenY;

    if (travel == lastTravel)
        return;

    lastTravel = travel;

    if (onDragTravel != nullptr)
        onDragTravel (travel);
}

void VerticalSplitter::simulateDrag (int totalTravel, bool continueGesture)
{
    if (! continueGesture && onDragBegin != nullptr)
        onDragBegin();

    if (onDragTravel != nullptr)
        onDragTravel (totalTravel);
}

void VerticalSplitter::mouseUp (const juce::MouseEvent&)
{
    dragging = false;
    repaint();
}

void VerticalSplitter::mouseEnter (const juce::MouseEvent&)
{
    hot = true;
    repaint();
}

void VerticalSplitter::mouseExit (const juce::MouseEvent&)
{
    hot = false;
    repaint();
}

//==============================================================================
MainComponent::MainComponent()
{
    setOpaque (true);
    setSize (ui::px (1440), ui::px (820));
    setWantsKeyboardFocus (true);

    // Phase 71: the press/release witness. Registered on the Desktop rather
    // than on this component's subtree, because the console, a plugin's editor
    // and the automation window are all separate top-level windows — a gesture
    // in any of them has to bracket the same way.
    juce::Desktop::getInstance().addGlobalMouseListener (&gestureWatcher);

    // Phase 34: the question both cascade deletions ask. Installed here rather
    // than defaulted in the header so the self-test can swap in a scripted
    // answer and put the real dialog back afterwards.
    installSamplerCascadePrompt();

    addAndMakeVisible (layerPanel);
    addAndMakeVisible (pianoRoll);
    addAndMakeVisible (rackPanel);
    addAndMakeVisible (automationWindow);
    addAndMakeVisible (bpmField);
    addAndMakeVisible (splitterBar);

    // Phase 17: the two lower lists. They are added before the roll is wired
    // for drag-and-drop, but order does not matter: the container is the host,
    // and the roll only has to be a DragAndDropTarget.
    addAndMakeVisible (filePanel);
    addAndMakeVisible (groupPanel);

    splitterBar.onDragBegin = [this]
    {
        dragBaseAutomationHeight = automationPaneHeight;
    };

    splitterBar.onDragTravel = [this] (int travel)
    {
        // Dragging DOWN pushes the automation window down, so it gets shorter.
        // Absolute travel from a captured base: the band is where the pointer is
        // until the pane bottoms out, and it lets go the moment the pointer
        // comes back — no accumulated deltas to lose.
        setAutomationPaneHeight (dragBaseAutomationHeight - travel);
    };

    // Phase 21: the two side columns get the same handle. Adding the panels as
    // visible before the bands keeps the bands on top, which matters because a
    // panel that repaints itself must not draw over the thing you are dragging.
    addAndMakeVisible (leftSplitter);
    addAndMakeVisible (rightSplitter);

    leftSplitter.onDragBegin = [this]
    {
        dragBaseFileHeight = filePaneHeight;
    };

    leftSplitter.onDragTravel = [this] (int travel)
    {
        // Dragging UP makes the file tree taller, so the height is minus the
        // travel — same sign convention as the automation band, for the same
        // reason: the handle stays under the pointer.
        filePaneHeight = dragBaseFileHeight - travel;
        resized();
    };

    rightSplitter.onDragBegin = [this]
    {
        dragBaseGroupHeight = groupPaneHeight;
    };

    rightSplitter.onDragTravel = [this] (int travel)
    {
        groupPaneHeight = dragBaseGroupHeight - travel;
        resized();
    };

    // The mixer lives in its own window (Phase 8), so it is NOT a child here.
    // It is still a member: the engine, the meters and the undo snapshots all
    // talk to the panel, never to the window.
    mixerPanel.setSize (ui::px (1180), ui::px (300));

    bpmField.setBpm (audio.getBpm(), false);
    bpmField.onBpmChanged = [this] (double newBpm) { setBpm (newBpm); };

    // Phase 80: the tempo curve's only entry point is the field the user already
    // points at. The field asks; the host does the adding.
    bpmField.onShowTempoAutomation = [this] { showTempoAutomation(); };

    // The inspector floats above everything and starts hidden.
    addChildComponent (envelopePanel);
    addChildComponent (sampleEnvelopePanel);      // Phase 78

    pianoRoll.onNoteDoubleClicked = [this] (int layerIndex, int noteId)
    {
        openEnvelopePanel (layerIndex, noteId);
    };

    // Phase 78: a block's double click asks for the same kind of window the
    // note's does. Both live here, both start hidden.
    pianoRoll.onSampleDoubleClicked = [this] (int layerIndex, int sampleId)
    {
        openSampleEnvelopePanel (layerIndex, sampleId);
    };

    // Clicking the canvas dismisses the inspector, like any popover.
    pianoRoll.onCanvasMouseDown = [this]
    {
        envelopePanel.close();
        sampleEnvelopePanel.close();
    };

    // The rack reads the engine through callbacks: numbers in, pixels out.
    rackPanel.levelSource = [this] (int slotId)
    {
        return audio.getPlayback().getSlotPeak (slotId);
    };

    layerPanel.levelSource = [this] (int layerIndex)
    {
        return audio.getPlayback().getLayerPeak (layerIndex);
    };

    rackPanel.noteCountSource = [this] (int slotId)
    {
        return audio.getPlayback().countNotesRoutedTo (slotId);
    };

    rackPanel.overrideCountSource = [this] (int slotId)
    {
        return audio.getPlayback().countOverridesRoutedTo (slotId);
    };

    // The mixer owns the channel inserts and the master strip, so the engine
    // needs it before the first schedule is published.
    audio.setMixerModel (&mixer);

    // Phase 80: the tempo curve reaches the transport through the engine, the
    // same way the mixer does. Attached before the first publish below.
    audio.setTempoModel (&tempo);

    // Phase 9: the engine reaches plugin instances through this host. It is a
    // plain pointer, not an owner — the host is a member above it.
    audio.setPluginHost (&plugins);

    automationWindow.onChanged = [this]
    {
        republish();
        layerPanel.repaint();
        rackPanel.repaint();
    };

    // One zoom level, one scroll position: the automation window is not a
    // second timeline, it is the same timeline drawn 200 px lower.
    pianoRoll.onScrollChanged = [this]
    {
        automationWindow.setScrollBeats (pianoRoll.getScrollBeats());
        automationWindow.setTimeline (pianoRoll.getTimelineBeats(),
                                      (double) pianoRoll.getBeatWidth());
    };

    // Phase 12: the roll's length is the automation window's length. Scrolling
    // past beat 64 or dropping a note there widens both rulers at once.
    pianoRoll.onTimelineChanged = [this]
    {
        automationWindow.setTimeline (pianoRoll.getTimelineBeats(),
                                      (double) pianoRoll.getBeatWidth());
    };

    // Phase 87: the ruler edits the loop; the session is what owns it. The
    // roll never writes its own copy on the strength of a gesture — it reports,
    // and the host decides (undo step, dirty flag, engine).
    // Phase 91: the metre is the project's, so the roll asks rather than sets.
    pianoRoll.onMeterRequested = [this] (juce::Point<int> screenPos)
    {
        showMeterMenu (screenPos);
    };

    pianoRoll.onLoopRegionChanged = [this] (const LoopRegion& region)
    {
        setLoopRegion (region);
    };

    automationWindow.setTimeline (pianoRoll.getTimelineBeats(),
                                  (double) pianoRoll.getBeatWidth());
    automationWindow.setScrollBeats (pianoRoll.getScrollBeats());

    // Phase 14: the only way to create an automation track is a right-click
    // menu on the control. The layer row's context menu and the synth card's
    // context menu are the two entry points; the badges they replaced are gone.
    layerPanel.onAutomationRequested = [this] (int layerIndex, juce::Point<int> pos)
    {
        if (auto* layer = stack.getLayer (layerIndex))
            showAutomationMenu (AutomationOwner::layer, layer->getId(), pos);
    };

    // Phase 21: the fader's value menu hangs the same list as a sub-menu, so
    // the curve you want appears when the pointer reaches 自动化 — no second
    // click, no second menu in a different place on screen.
    layerPanel.buildAutomationSubmenu = [this] (juce::PopupMenu& menu, int layerIndex)
    {
        if (auto* layer = stack.getLayer (layerIndex))
            fillAutomationMenu (menu, AutomationOwner::layer, layer->getId());
    };

    layerPanel.onAutomationChosen = [this] (int layerIndex, int result)
    {
        if (auto* layer = stack.getLayer (layerIndex))
            applyAutomationChoice (result, AutomationOwner::layer, layer->getId());
    };

    // Picking 音量 / 声相 under an open lane has to create the row too: the
    // panel may never show a curve the window does not list.
    layerPanel.onAutomationTargetChosen = [this] (int layerIndex, int target)
    {
        if (auto* layer = stack.getLayer (layerIndex))
            ensureAutomationTrack (AutomationOwner::layer, layer->getId(), target);
    };

    //-- Phase 9: plugin slots and note/synth routing ------------------------
    rackPanel.onAddSlotRequested = [this] (juce::Point<int> pos)
    {
        showRackAddMenu (pos);
    };

    // P79: a card's name is not only the card's. Sample blocks print it too,
    // and only the host can walk every sheet to keep them saying the same word.
    rackPanel.onSlotRenamed = [this] (int slotId, juce::String newName)
    {
        renameSynthSlot (slotId, newName);
    };

    rackPanel.onCardMenuRequested = [this] (int slotId, juce::Point<int> pos)
    {
        showRackCardMenu (slotId, pos);
    };

    rackPanel.buildCardSubmenu = [this] (juce::PopupMenu& menu, int slotId)
    {
        fillRackCardMenu (menu, slotId);
    };

    rackPanel.onCardSubmenuChosen = [this] (int slotId, int result)
    {
        applyRackCardChoice (result, slotId);
    };

    rackPanel.onSlotChosen = [this] (int slotId)
    {
        //-- Phase 32: a selected BLOCK follows the sampler you click ---------
        // Selecting a block is how you say "this sample", so the next click on a
        // sampler is how you say "on that one" — and since a sampler holds the
        // file, the block takes that file too (name, length, offset). The rail
        // click this replaces was the note gesture, which means nothing here:
        // there are no notes in a clip selection.
        if (! pianoRoll.getSampleSelection().empty())
        {
            const auto* target = rack.findSlot (slotId);

            if (target != nullptr && target->isSampler()
                 && pianoRoll.mountSelectedSamplesToSlot (slotId))
            {
                focusSynth (slotId);
                layerPanel.repaint();
                groupPanel.repaint();
                rackPanel.repaint();
                return;
            }

            // Anything else is just inspected: an instrument has no file to hand
            // over, so there is nothing to mount.
            if (target != nullptr)
            {
                focusSynth (slotId);
                layerPanel.repaint();
                groupPanel.repaint();
                rackPanel.repaint();
            }

            return;
        }

        // Phase 27: clicking a sampler spotlights it and nothing else. It holds
        // one file; it cannot play notes, so mounting a selection on it would
        // be a silent mute dressed up as a routing change.
        if (const auto* slot = rack.findSlot (slotId); slot != nullptr && slot->isSampler())
        {
            focusSynth (slotId);
            layerPanel.repaint();
            groupPanel.repaint();
            rackPanel.repaint();
            return;
        }

        // "Put what I have selected on this synth." Phase 23: a group selection
        // is mounted as a GROUP (its own output, members' overrides cleared),
        // loose notes take the per-note override. One call, one rule — the roll
        // decides which of the two the selection is.
        if (! pianoRoll.mountSelectionTo (slotId))
            return;

        republish();
        focusSynth (slotId);
        layerPanel.repaint();
        groupPanel.repaint();
        rackPanel.repaint();
    };

    //-- Phase 16: clicking a card spotlights a synth ------------------------
    // Not a routing gesture any more. Selecting a synth is how you ask "what is
    // playing on this one?", and with groups in the picture a click that also
    // rewired notes would be far too easy to trigger by accident.
    rackPanel.onCardFocused = [this] (int slotId)
    {
        focusSynth (slotId);
    };

    //-- Phase 85: double-clicking a card opens the instrument ---------------
    // The name still renames itself (P79) — the panel settles that before it
    // reaches here. Everywhere else on the card means "show me this synth",
    // which is the same answer the piano roll gives to a note (P78) and the
    // same window the card menu's 打开插件界面 opens.
    //
    // Silent when there is nothing to open: a built-in oscillator has no
    // window, and a message box for "you double-clicked a sine wave" would be
    // the app arguing with the gesture instead of just doing nothing.
    rackPanel.onCardDoubleClicked = [this] (int slotId)
    {
        openSlotEditor (slotId);
    };

    //-- Phase 32: the × on a sampler takes its clips with it -----------------
    // Phase 34: through the question, not straight to the executor — deleting a
    // card is about to delete music.
    rackPanel.onSlotRemoveRequested = [this] (int slotId)
    {
        requestRemoveSlotAndItsClips (slotId);
    };

    //-- Phase 51: the sampler card's 拉伸 control ----------------------------
    // The panel cannot see blocks (they live in layers), so the card asks the
    // host what its blocks are in, and the host writes the choice back.
    rackPanel.samplerStretchState = [this] (int slotId)
    {
        return samplerStretchStateFor (slotId);
    };

    rackPanel.onSamplerStretchChosen = [this] (int slotId, int stretchMode)
    {
        applySamplerStretchToSlot (slotId, (StretchMode) stretchMode);
    };

    //-- Phase 52: the 只变调 range ------------------------------------------
    rackPanel.samplerPitchRangeState = [this] (int slotId)
    {
        return samplerPitchRangeStateFor (slotId);
    };

    rackPanel.onSamplerPitchRangeChanged = [this] (int slotId, int semitones)
    {
        applySamplerPitchRangeToSlot (slotId, semitones);
    };

    //-- Phase 32: a block left a sampler with nothing to play ----------------
    pianoRoll.onSampleReferencesChanged = [this]
    {
        pruneOrphanSamplerSlots();
    };

    pianoRoll.onSelectionChanged = [this]
    {
        updateRackRoutingFocus();
        syncGroupPanelSelection();
    };

    //-- Phase 29: the grid's own way out of the spotlight --------------------
    // The roll cannot clear it itself — the spotlight is the rack's answer to
    // "what is on this synth", and the session owns it. So the roll asks, and
    // this is the one place that answers.
    pianoRoll.onClearSpotlight = [this]
    {
        focusSynth (-1);
        layerPanel.repaint();
        groupPanel.repaint();
        rackPanel.repaint();
        pianoRoll.repaint();
    };

    //-- Phase 24: the inline boxes ask what a hosted synth can automate ------
    // Wiring lives here because the plugin host is the session's: a plugin may
    // only be inspected from the message thread, and this lambda is only ever
    // called from a menu or a paint on that thread. With nothing loaded it
    // answers "no parameters" and the boxes offer the three built-in targets.
    pianoRoll.setPluginParamLister ([this] (int slotId) -> std::vector<PluginParamInfo>
    {
        // One implementation, in the host, because the host is what owns the
        // plugin instances. The roll asks, the rack menu asks, the same answer
        // comes back.
        return pluginParamsForSlot (slotId);
    });

    //-- Phase 16: group actions the roll cannot finish on its own -----------
    pianoRoll.onGroupAutomationRequested = [this] (int, int groupId, int target)
    {
        ensureAutomationTrack (AutomationOwner::group, groupId, target);
    };

    pianoRoll.onGroupMountRequested = [this] (int layerIndex, int groupId, juce::Point<int> pos)
    {
        showGroupMountMenu (layerIndex, groupId, pos);
    };

    //-- Phase 50: "render this to a block" ----------------------------------
    // The roll names the scope; the host owns the renderer, the sample library
    // and the layer. Nothing about the bounce lives in the UI layer, so the
    // self-test can drive renderScopeToSample() without a mouse.
    pianoRoll.onRenderScopeRequested = [this] (const PianoRollComponent::RenderScopeRequest& request)
    {
        renderScopeToSample (request);
    };

    // A group that disappears takes its curve's row with it. Done here because
    // the model has no idea the automation window exists.
    pianoRoll.onGroupRemoved = [this] (int groupId)
    {
        automation.removeTracksForOwner (AutomationOwner::group, groupId);
    };

    //-- Phase 17: the group list is a second view on the same model ----------
    // Everything it can do already exists in the roll's group menu; the panel
    // only offers a place to click that is visible when the group is scrolled
    // off screen. Both entry points funnel through the same host functions, so
    // there is exactly one implementation of "mount this group".
    groupPanel.onMountRequested = [this] (int layerIndex, int groupId, juce::Point<int> pos)
    {
        showGroupMountMenu (layerIndex, groupId, pos);
    };

    groupPanel.onGroupMenuRequested = [this] (int layerIndex, int groupId, juce::Point<int> pos)
    {
        auto* layer = stack.getLayer (layerIndex);
        auto* group = layer != nullptr ? layer->findGroup (groupId) : nullptr;

        if (group == nullptr)
            return;

        juce::PopupMenu menu;
        menu.addSectionHeader (group->getName());

        // Phase 42: one verb, one entry — the parameters are the submenu.
        {
            juce::PopupMenu autoMenu;
            autoMenu.addItem (1, utf8 ("组音量"), true,
                              automation.contains (AutomationOwner::group, groupId,
                                                   (int) GroupAutomationTarget::level));
            autoMenu.addItem (5, utf8 ("组声相"), true,
                              automation.contains (AutomationOwner::group, groupId,
                                                   (int) GroupAutomationTarget::pan));
            menu.addSubMenu (utf8 ("创建自动化"), autoMenu);
        }

        menu.addItem (2, utf8 ("挂载音源…"));
        menu.addSeparator();
        menu.addItem (3, utf8 ("跟随图层输出"), group->getOutputSlotId() >= 0);
        menu.addSeparator();
        menu.addItem (4, group->isCollapsed() ? utf8 ("展开该编组") : utf8 ("折叠该编组"));

        juce::Component::SafePointer<MainComponent> safe (this);

        menu.showMenuAsync (juce::PopupMenu::Options()
                                .withTargetScreenArea (juce::Rectangle<int> (pos.x, pos.y, 1, 1)),
                            [safe, layerIndex, groupId, pos] (int result)
                            {
                                if (safe == nullptr || result <= 0)
                                    return;

                                auto* l = safe->stack.getLayer (layerIndex);
                                auto* g = l != nullptr ? l->findGroup (groupId) : nullptr;

                                if (g == nullptr)
                                    return;

                                if (result == 1 || result == 5)
                                {
                                    safe->ensureAutomationTrack (AutomationOwner::group, groupId,
                                                                 result == 5
                                                                   ? (int) GroupAutomationTarget::pan
                                                                   : (int) GroupAutomationTarget::level);
                                    return;
                                }

                                if (result == 3)
                                {
                                    g->setOutputSlotId (-1);
                                    safe->stack.notifyNotesChanged();
                                    safe->republish();
                                    safe->focusSynth (-1);
                                    return;
                                }

                                if (result == 4)
                                {
                                    g->setCollapsed (! g->isCollapsed());
                                    safe->groupPanel.repaint();
                                    return;
                                }

                                if (result == 2)
                                    safe->showGroupMountMenu (layerIndex, groupId, pos);
                            });
    };

    //-- Phase 60: clicking a row SELECTS the group ---------------------------
    // The panel lists groups; a list you can look at but not click is a report,
    // not a list. Selecting goes through the roll (which owns the selection and
    // will push the answer straight back through syncGroupPanelSelection), so
    // there is still exactly one selected group in the app, not one per view.
    groupPanel.onGroupSelected = [this] (int layerIndex, int groupId)
    {
        juce::ignoreUnused (layerIndex);   // the roll finds the sheet itself

        pianoRoll.selectGroup (groupId);
    };

    filePanel.onFoldersChanged = [this]
    {
        // Nothing downstream caches the folder list today; the hook exists so a
        // future "recent samples" strip has somewhere to listen.
        filePanel.repaint();
    };

    //-- Phase 20: click a file, hear three seconds of it --------------------
    // Decoding happens here because the panel is a view and this is the only
    // place that already owns a decoder path (the same one a drop uses). The
    // engine stops any preview already sounding, so the "no overlap" rule holds
    // even if the user clicks two rows as fast as they can.
    filePanel.onPreviewRequested = [this] (const juce::File& file) -> double
    {
        auto loaded = loadSampleFile (file);

        if (! loaded.ok())
        {
            showMessage (utf8 ("无法试听"), loaded.error, false);
            return 0.0;
        }

        const double seconds = juce::jmin (3.0, juce::jmax (0.05, loaded.buffer->lengthSeconds()));

        audio.getPlayback().previewSample (loaded.buffer, seconds);
        return seconds;
    };

    // A file that will not decode is a message, not a crash: the roll keeps its
    // blocks and the user is told which file and why.
    pianoRoll.onSampleLoadFailed = [this] (const juce::String& message)
    {
        showMessage (utf8 ("无法载入采样"), message, false);
    };

    mixerPanel.onInsertPluginRequested = [this] (int channel, int slot, juce::Point<int> pos)
    {
        showInsertPluginMenu (channel, slot, pos);
    };

    mixerPanel.onOpenPluginManager = [this]
    {
        openSettings (2);
    };

    //-- Phase 12: automation and editors from the console --------------------
    mixerPanel.onChannelMenuRequested = [this] (int channel, juce::Point<int> pos)
    {
        showChannelMenu (channel, pos);
    };

    mixerPanel.buildStripAutomationItems = [this] (juce::PopupMenu& menu, int channel,
                                                   MixerPanel::StripControl control)
    {
        fillStripAutomationItems (menu, channel, control);
    };

    mixerPanel.onStripAutomationChosen = [this] (int channel, MixerPanel::StripControl control,
                                                 int result)
    {
        applyStripAutomationChoice (channel, control, result);
    };

    mixerPanel.onInsertOpenRequested = [this] (int channel, int slot)
    {
        openInsertEditor (channel, slot);
    };

    layerPanel.onResetNoteRouting = [this] (int layerIndex)
    {
        resetLayerNoteRouting (layerIndex);
    };

    mixerPanel.levelSource = [this] (int channel, int side)
    {
        // Phase 11: a strip's meter is the CHANNEL's signal (post-insert), not a
        // layer's. A synth that bypasses the console never lights a strip —
        // which is the honest reading: nothing is arriving there.
        //
        // Phase 26: one side at a time. The strip draws two bars and the engine
        // keeps two numbers, so the answer is per side; "loudest of the two"
        // would draw the same bar twice and turn a stereo meter back into a
        // mono one with extra pixels.
        return channel == MixerModel::masterChannel
                 ? audio.getPlayback().getMasterPeak (side)
                 : audio.getPlayback().getChannelPeakSide (channel, side);
    };

    // Phase 10: the app opens on a blank session — one empty layer, no synths,
    // no notes. The demo arrangement is a test fixture now (loadDemoProject),
    // not the thing every launch starts with.
    buildBlankProject();

    //-- Phase 8: the history and the model listeners ------------------------
    // Both models are observed so ANY edit — a note drag, a fader move, an
    // insert change, an added automation track — becomes one undo step without
    // every gesture having to remember to say so.
    stack.addListener (this);
    rack.addListener (this);
    mixer.addListener (this);
    automation.addListener (this);

    pianoRoll.onNoteEdited = [this]
    {
        republish();
        automationWindow.repaint();
        envelopePanel.repaint();
        sampleEnvelopePanel.repaint();
    };

    // Phase 18: one snap switch for both editors, and the ruler as a locator.
    pianoRoll.onSnapChanged = [this] (bool on)
    {
        automationWindow.setSnapEnabled (on);
        automationWindow.repaint();
    };

    pianoRoll.onLocateRequested = [this] (double beats)
    {
        locatePlayhead (beats);
    };

    // Phase 19: drawing or dragging a note sounds it. The roll names the pitch
    // and the synth; the engine is the only thing here that can make noise.
    //
    // Phase 71: a hosted instrument is played for real — the preview sends it
    // the note-on the keyboard would, so what you hear while drawing IS the
    // patch on that slot rather than a sine standing in for it.
    pianoRoll.onAuditionRequested = [this] (int slotId, int pitch, float velocity, bool retrigger)
    {
        auditionPreview (slotId, pitch, velocity, retrigger);
    };

    //-- Phase 39: the keyboard, wired before anything can press a key --------
    midiHub = std::make_unique<MidiInputHub>();

    // Called on the MIDI thread. Only atomics and the engine's lock-free queue
    // are touched here: the model belongs to the message thread, which is why
    // the answer to "which synth" is cached in `liveTargetSlot`.
    midiHub->onNote = [this] (int pitch, float velocity, bool on)
    {
        // Phase 81: the beat is read HERE, on the MIDI thread. Handing the event
        // to the message thread and asking for the position there would turn a
        // 5 ms queue delay into 5 ms of wrong timing on every note of the take.
        // The takeover itself has to happen on the message thread (the collector
        // owns vectors), but the timestamp travels with the event.
        if (recording.load (std::memory_order_relaxed))
        {
            const double beatNow = audio.getPlayback().getPositionBeats();

            juce::MessageManager::callAsync ([this, pitch, velocity, on, beatNow]
            {
                if (recording.load())
                    recordNoteAt (pitch, velocity, on, beatNow);
            });
        }

        if (on)
            audio.getPlayback().noteOnLive (liveTargetSlot.load (std::memory_order_relaxed),
                                            pitch, velocity);
        else
            audio.getPlayback().noteOffLive (pitch);
    };

    // Phase 81: the knobs. A note take records what was played; this records
    // what the hand did while playing it, which is the only thing that can
    // become the group's level/pan curve.
    midiHub->onControl = [this] (int controller, int value7)
    {
        if (! recording.load (std::memory_order_relaxed))
            return;

        const double beatNow = audio.getPlayback().getPositionBeats();

        juce::MessageManager::callAsync ([this, controller, value7, beatNow]
        {
            if (recording.load())
                recordControlAt (controller, value7, beatNow);
        });
    };

    // The panels that claim plain letters — the roll groups on G and erases on
    // Delete — would swallow the note keys. A KeyListener is called BEFORE the
    // focused component's own handler, which is exactly the precedence the
    // typing keyboard needs.
    pianoRoll.addKeyListener (&typingForwarder);
    envelopePanel.addKeyListener (&typingForwarder);
    sampleEnvelopePanel.addKeyListener (&typingForwarder);

    refreshLiveTarget();

    cancelPendingUpdate();                   // building the blank session is not an edit
    history.clear();
    history.push_back (captureState());
    historyPos = 0;
    savedStateText = history.front();        // nothing to save yet
    dirtyCache     = false;

    // Phase 84: a brand-new session is `untitled` until it is saved, and that
    // name is where its unsaved work goes if the process never gets to close.
    recoveryFile = recovery::getFileFor (currentProject);

    // Phase 88: the metronome is off by default (a click nobody asked for is
    // the loudest thing in the room), at half level, counting in one bar.
    applyMetronomeSettings();

    // 60 Hz: the playhead is the one thing on screen that has to look like it
    // is moving, and 30 Hz reads as a stutter on a thin bright line.
    startTimerHz (60);
}

MainComponent::~MainComponent()
{
    stopTimer();
    cancelPendingUpdate();

    // Phase 71: before anything else can be torn down — a global listener that
    // outlives its owner is a crash on the next click anywhere.
    juce::Desktop::getInstance().removeGlobalMouseListener (&gestureWatcher);

    releaseHostedPreview();
    closeMixerWindow();

    stack.removeListener (this);
    rack.removeListener (this);
    mixer.removeListener (this);
    automation.removeListener (this);
}

//==============================================================================
//  Phase 8: undo history
//==============================================================================
juce::String MainComponent::captureState() const
{
    return project::toXmlText (stack, rack, audio.getBpm(), &mixer, &automation, &tempo,
                               &currentProject, &loopRegion, beatsPerBar);
}

void MainComponent::modelChanged()
{
    if (restoringHistory)
        return;

    // Phase 84: something was edited, so the next autosave pass has a reason to
    // serialise the session. Deliberately above the undo coalescing below: that
    // guard is about how many SNAPSHOTS one gesture produces, and this is not a
    // snapshot.
    recoveryDirty = true;

    // One snapshot per turn, not per notification: dragging a note fires a
    // notesChanged every mouse move, and each of those is NOT an undo step.
    if (! isUpdatePending())
        triggerAsyncUpdate();
}

void MainComponent::handleAsyncUpdate()
{
    commitHistory();
}

void MainComponent::commitHistory()
{
    if (restoringHistory || history.empty())
        return;

    // Phase 71: a gesture in flight is not a state yet. Everything that asks
    // for a snapshot meanwhile — the AsyncUpdater, hasUnsavedChanges(), a
    // panel's own flush — is answered by the capture taken at mouse-up.
    if (undoGestureActive)
        return;

    auto now = captureState();

    if (now == history[(size_t) historyPos])
    {
        // No new undo step, but the state may still differ from the file on
        // disk (the user undid back to it, or edited into a state that happens
        // to equal the saved one).
        dirtyCache = (now != savedStateText);
        return;
    }

    // A new edit after an undo throws away the redo branch, like every editor.
    if (historyPos + 1 < (int) history.size())
        history.resize ((size_t) historyPos + 1);

    history.push_back (std::move (now));

    if ((int) history.size() > maxHistoryStates)
        history.erase (history.begin());

    historyPos = (int) history.size() - 1;

    dirtyCache = (history[(size_t) historyPos] != savedStateText);

    repaint (topBarBounds);
}

bool MainComponent::canUndo() const { return historyPos > 0; }
bool MainComponent::canRedo() const { return historyPos + 1 < (int) history.size(); }

//==============================================================================
//  Phase 71: an undo step is a gesture
//==============================================================================
void MainComponent::GestureWatcher::mouseDown (const juce::MouseEvent&)
{
    owner.beginUndoGesture();
}

void MainComponent::GestureWatcher::mouseUp (const juce::MouseEvent&)
{
    owner.endUndoGesture();
}

void MainComponent::beginUndoGesture()
{
    // A second press without a release (a modal swallowed the up, the window
    // was closed mid-drag) still ends the one before it: the alternative is a
    // session that never records anything again.
    if (undoGestureActive)
    {
        endUndoGesture();
        return;
    }

    // Settle whatever the previous gesture left pending BEFORE the bracket
    // closes. Doing it the other way round would fold two separate actions —
    // click a fader, then drag a note — into one undo step.
    cancelPendingUpdate();
    commitHistory();

    undoGestureActive = true;
}

void MainComponent::endUndoGesture()
{
    if (! undoGestureActive)
        return;

    undoGestureActive = false;

    // The one snapshot this gesture is worth. Everything in between was
    // dropped, so a four-beat drag is one step rather than one per mouse move.
    cancelPendingUpdate();
    commitHistory();
}

void MainComponent::undo()
{
    // Flush whatever gesture is still in flight first, or Ctrl+Z right after a
    // drag would undo the state *before* the drag and then store the drag on
    // top of it.
    //
    // Phase 71: mid-drag, the open gesture IS the thing to undo, so it is
    // closed here (one step) and then stepped over — "undo" during a drag
    // cancels the drag, which is what the hand pressed for.
    cancelPendingUpdate();
    undoGestureActive = false;
    commitHistory();

    if (! canUndo())
        return;

    --historyPos;
    applyHistoryPosition();
}

void MainComponent::redo()
{
    cancelPendingUpdate();
    undoGestureActive = false;
    commitHistory();

    if (! canRedo())
        return;

    ++historyPos;
    applyHistoryPosition();
}

void MainComponent::applyHistoryPosition()
{
    const juce::String xml = history[(size_t) historyPos];

    // Phase 84: undo and redo are edits too, and this one can land exactly on
    // the state the file on disk holds — which is the moment the recovery copy
    // stops being true. `modelChanged` does not fire for it (restoringHistory is
    // up), so the flag is raised here.
    recoveryDirty = true;

    restoringHistory = true;
    restoreState (xml);
    restoringHistory = false;

    dirtyCache = (xml != savedStateText);

    repaint();
}

bool MainComponent::restoreState (const juce::String& xml, const juce::File* projectFile)
{
    // Phase 71: the session — and with it every slot id — is about to be
    // replaced. A note still held on a plugin preview would be held on a slot
    // that no longer exists, and nothing would ever let go of it.
    releaseHostedPreview();

    double bpmValue = audio.getBpm();
    juce::String error;

    const juce::File* base = projectFile != nullptr ? projectFile : &currentProject;

    if (! project::fromXmlText (xml, stack, rack, bpmValue, error,
                                &mixer, &automation, &plugins, &tempo, base, &loopRegion))
        return false;

    // Phase 87: an undo can move the loop (or remove it), and the region the
    // renderer holds is not part of the document — it has to be pushed.
    applyLoopToViews();

    setBpm (bpmValue);
    bpmField.setBpm (bpmValue, false);

    envelopePanel.close();
    sampleEnvelopePanel.close();
    pianoRoll.closeNoteAutomation();
    republish();

    layerPanel.repaint();
    rackPanel.repaint();
    automationWindow.repaint();
    mixerPanel.repaint();
    pianoRoll.repaint();
    return true;
}

//==============================================================================
//  Phase 71: the preview of a note
//==============================================================================
void MainComponent::auditionPreview (int slotId, int pitch, float velocity, bool retrigger)
{
    auto* slot = rack.findSlot (slotId);

    if (slot != nullptr && slot->isPlugin())
    {
        // One mouse move = one request, so the same pitch arriving again is the
        // same held key, not a new one. Re-sending it would restart the patch's
        // envelope on every pixel of travel and turn a drag into a machine gun.
        if (! retrigger && hostedPreview.slotId == slotId && hostedPreview.pitch == pitch)
        {
            hostedPreview.releaseAtMs = juce::Time::getMillisecondCounter() + hostedPreviewHoldMs;
            return;
        }

        // A different pitch, or a fresh gesture: let go of the old note first.
        // Two notes down at once is a chord, and a drag is not one.
        releaseHostedPreview();

        // The oscillator voice may still be ringing from the previous target
        // (routing can change mid-gesture). Ramp it out now, or the sine keeps
        // sounding under the plugin and it is the sine you hear.
        audio.getPlayback().stopAudition();

        audio.getPlayback().noteOnLive (slotId, pitch, velocity);

        hostedPreview.slotId      = slotId;
        hostedPreview.pitch       = pitch;
        hostedPreview.releaseAtMs = juce::Time::getMillisecondCounter() + hostedPreviewHoldMs;
        return;
    }

    // Back on a built-in synth (or nothing at all): the plugin must not keep
    // the note it was given, or it drones under the new preview.
    releaseHostedPreview();

    audio.getPlayback().auditionNote (pitch, velocity, slotId, retrigger);
}

void MainComponent::releaseHostedPreview()
{
    if (hostedPreview.pitch < 0)
        return;

    audio.getPlayback().noteOffLive (hostedPreview.pitch);

    hostedPreview.pitch  = -1;
    hostedPreview.slotId = -1;
}

//==============================================================================
//  Phase 8: the mixer window
//==============================================================================
bool MainComponent::isMixerWindowOpen() const
{
    return mixerWindow != nullptr && mixerWindow->isVisible();
}

void MainComponent::closeMixerWindow()
{
    if (mixerWindow == nullptr)
        return;

    mixerWindow->setVisible (false);
    mixerWindow.reset();      // the window detaches the panel on destruction
    repaint (topBarBounds);
}

void MainComponent::toggleMixerWindow()
{
    if (isMixerWindowOpen())
    {
        closeMixerWindow();
        return;
    }

    if (mixerWindow == nullptr)
    {
        mixerWindow = std::make_unique<MixerWindow> (mixerPanel);

        juce::Component::SafePointer<MainComponent> safe (this);

        mixerWindow->onCloseRequest = [safe]
        {
            if (safe != nullptr)
                safe->closeMixerWindow();
        };

        // Phase 43: 空格 and the typing keyboard while the console has focus.
        mixerWindow->setKeyForwarder (&pluginForwarder);
    }
    else
    {
        mixerWindow->setVisible (true);
        mixerWindow->toFront (true);
    }

    repaint (topBarBounds);
}

juce::Image MainComponent::createMixerSnapshot (int width, int height)
{
    mixerPanel.setSize (width, height);
    mixerPanel.resized();
    mixerPanel.updateMeters();

    return mixerPanel.createComponentSnapshot (mixerPanel.getLocalBounds(), true);
}

juce::Image MainComponent::createFileBrowserSnapshot (int width, int height)
{
    // The panel is laid out by the splitter, so it is grown for the shot and put
    // back afterwards: same class, same paint code, just a taller window than
    // the shell gives it.
    const auto previous = filePanel.getBounds();

    filePanel.setSize (width, height);

    auto image = filePanel.createComponentSnapshot (filePanel.getLocalBounds(), true);

    filePanel.setBounds (previous);
    return image;
}

juce::Image MainComponent::createClipCloseupSnapshot (int width, int height)
{
    // Same trick as the browser: grown for the shot, put back afterwards. Then
    // zoomed onto the demo block, because its trim bands and edges are the
    // point of this frame and they are tiny at the opening scale.
    const auto previousBounds     = pianoRoll.getBounds();
    const float previousBeatWidth = pianoRoll.getBeatWidth();
    const double previousScroll   = pianoRoll.getScrollBeats();

    // The highlight goes off for this frame. Since Phase 30 it no longer dims
    // anything, but it still paints rings and a wash over the notes that reach
    // the focused synth — and this shot is about what a SELECTION looks like,
    // on notes and on the block at the same time.
    const int previousFocus = focusedSlotId;
    focusSynth (-1);

    const auto* layer = stack.getActiveLayer();
    const auto* clip  = (layer != nullptr && ! layer->getSamples().empty())
                          ? &layer->getSamples().front() : nullptr;

    pianoRoll.setSize (width, height);
    pianoRoll.setBeatWidth (juce::jmax (pianoRoll.getBeatWidth(), 150.0f));

    if (clip != nullptr)
        pianoRoll.setScrollBeats (juce::jmax (0.0, clip->startBeat - 0.6));

    auto image = pianoRoll.createComponentSnapshot (pianoRoll.getLocalBounds(), true);

    pianoRoll.setBounds (previousBounds);
    pianoRoll.setBeatWidth (previousBeatWidth);
    pianoRoll.setScrollBeats (previousScroll);
    focusSynth (previousFocus);

    return image;
}

juce::Image MainComponent::createClipAutomationSnapshot (int width, int height,
                                                         SampleAutomationTarget target)
{
    // Phase 49: the block's own curve, unfolded. Same staging trick as the
    // closeup — grown for the frame, zoomed onto the demo block, everything put
    // back afterwards — plus two things that are the point of the picture: the
    // box is OPEN, and its curve has a shape to look at.
    const auto previousBounds     = pianoRoll.getBounds();
    const float previousBeatWidth = pianoRoll.getBeatWidth();
    const double previousScroll   = pianoRoll.getScrollBeats();
    const int previousFocus       = focusedSlotId;

    focusSynth (-1);

    auto* layer = stack.getActiveLayer();
    const auto* clip = (layer != nullptr && ! layer->getSamples().empty())
                         ? &layer->getSamples().front() : nullptr;

    pianoRoll.setSize (width, height);
    pianoRoll.setBeatWidth (juce::jmax (pianoRoll.getBeatWidth(), 150.0f));

    // The curve the frame shows, and the one it must leave behind: the demo's
    // own curve goes back on the way out, because a screenshot's staging is not
    // the user's project. `getSamples()` hands out const views, so the edit
    // goes through findSample — the same door every gesture uses.
    NoteEnvelope previousCurve;
    int  previousActiveCurveId = -1;
    int  stagedCurveId         = -1;
    bool hadStagedCurve        = true;

    if (clip != nullptr)
    {
        const int clipId = clip->id;
        auto*     block  = layer->findSample (clipId);

        if (block != nullptr)
        {
            // Phase 82: the box shows a CURVE, not a parameter. Staging the
            // demo therefore means "make sure the curve driving this target
            // exists, borrow it, put it back on the way out" — and delete it
            // again if the demo is what created it.
            hadStagedCurve        = block->curveDriving (target) != nullptr;
            previousCurve         = hadStagedCurve ? *block->curveDriving (target) : NoteEnvelope();
            previousActiveCurveId = block->curves.getActiveId();

            auto& staged = block->ensureCurveDriving (target);
            stagedCurveId = block->curves.findRoutedTo ((int) target)->id;

            if (target == SampleAutomationTarget::pan)
            {
                // A sweep across the field: the shape reads as "left, then
                // right", which is what a pan lane is for. Half-way is centre,
                // marked by the box's own centre hairline.
                staged.loadPoints ({ { 0.0f, 0.15f }, { 0.35f, 0.75f },
                                     { 0.70f, 0.25f }, { 1.0f, 0.62f } });
            }
            else
            {
                staged.loadPoints ({ { 0.0f, 0.0f }, { 0.30f, 1.0f },
                                     { 0.62f, 0.55f }, { 1.0f, 0.85f } });
            }

            staged.setEnabled (true);
            block->showCurveDriving (target);
        }

        pianoRoll.setScrollBeats (juce::jmax (0.0, clip->startBeat - 0.6));
        pianoRoll.openSampleAutomation (stack.getActiveLayerIndex(), clipId);
    }

    auto image = pianoRoll.createComponentSnapshot (pianoRoll.getLocalBounds(), true);

    pianoRoll.closeSampleAutomation();

    if (clip != nullptr)
        if (auto* block = layer->findSample (clip->id))
        {
            *block->curveDriving (target) = previousCurve;

            if (! hadStagedCurve)
                block->curves.remove (stagedCurveId);      // 演示造出来的，收回去

            block->curves.setActiveId (previousActiveCurveId);
        }

    pianoRoll.setBounds (previousBounds);
    pianoRoll.setBeatWidth (previousBeatWidth);
    pianoRoll.setScrollBeats (previousScroll);
    focusSynth (previousFocus);

    return image;
}

juce::Image MainComponent::createSpotlightSnapshot (int width, int height)
{
    // Same staging trick as the other framed shots: grown and zoomed for the
    // frame, put back afterwards. The zoom is the point — a highlight is a ring
    // and a wash on notes that are 12 px tall at the opening scale.
    const auto previousBounds     = pianoRoll.getBounds();
    const float previousBeatWidth = pianoRoll.getBeatWidth();
    const double previousScroll   = pianoRoll.getScrollBeats();
    const int    previousFocus    = focusedSlotId;

    pianoRoll.setSize (width, height);
    pianoRoll.setBeatWidth (juce::jmax (pianoRoll.getBeatWidth(), 90.0f));
    pianoRoll.setScrollBeats (2.0);

    // The first instrument, not the sampler: the demo's melody plays on it, and
    // a highlighted note is the thing this frame has to show.
    focusSynth (rack.getFirstInstrumentSlotId());

    auto image = pianoRoll.createComponentSnapshot (pianoRoll.getLocalBounds(), true);

    focusSynth (previousFocus);
    pianoRoll.setBounds (previousBounds);
    pianoRoll.setBeatWidth (previousBeatWidth);
    pianoRoll.setScrollBeats (previousScroll);

    return image;
}

juce::Image MainComponent::createStretchSnapshot (int width, int height)
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr || layer->getSamples().empty())
        return {};

    // Same staging trick as the other framed shots: grown and zoomed for the
    // frame, put back afterwards.
    const auto   previousBounds     = pianoRoll.getBounds();
    const float  previousBeatWidth  = pianoRoll.getBeatWidth();
    const double previousScroll     = pianoRoll.getScrollBeats();

    const SampleEvent sourceClip = layer->getSamples().front();

    // Three answers to "make it shorter", on the same file. The lengths are
    // chosen so each ratio is readable: halving is a clean octave / 2x, and
    // 0.75 leaves a pitch of +5.0 semitones — a number with a decimal in it,
    // which proves the badge is computing and not printing a constant.
    struct Frame { StretchMode mode; double scale; int pitch; double beat; };

    // Placed near the view the roll opens at, because a snapshot cannot move the
    // camera: the shell re-fits the scale whenever it lays the pane out, so the
    // frame has to work at whatever zoom the layout picks (~110 px/beat here).
    const Frame frames[3]
    {
        { StretchMode::resample, 0.40, 81, 19.6 },
        { StretchMode::tempo,    0.40, 74, 22.0 },
        { StretchMode::pitch,    0.50, 67, 24.4 }
    };

    std::vector<int> added;

    for (const auto& f : frames)
    {
        SampleEvent clip = sourceClip;
        clip.startBeat   = f.beat;
        clip.pitch       = f.pitch;
        clip.lengthBeats = sourceClip.lengthBeats * f.scale;
        clip.gainDb      = 0.0f;
        clip.stretchMode = f.mode;
        clip.groupId      = -1;

        added.push_back (layer->addSample (std::move (clip)));
    }

    // The note inspector is open by this point in the sequence (the main frame
    // documents it), and it floats over the left of the grid. Close it if it has
    // anything to close: this frame is about three blocks, not about that box.
    pianoRoll.closeNoteAutomation();

    pianoRoll.setSize (width, height);
    pianoRoll.setBeatWidth (juce::jmax (pianoRoll.getBeatWidth(), 72.0f));
    pianoRoll.setScrollBeats (19.3);

    auto image = pianoRoll.createComponentSnapshot (pianoRoll.getLocalBounds(), true);

    pianoRoll.setBounds (previousBounds);
    pianoRoll.setBeatWidth (previousBeatWidth);
    pianoRoll.setScrollBeats (previousScroll);

    // The three blocks leave with the frame they came for.
    pianoRoll.setSampleSelection (added);
    pianoRoll.deleteSelectedSamples();

    return image;
}

juce::Image MainComponent::createCardDropSnapshot (DropSnapshotKind kind, int width, int height)
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return {};

    const auto   previousBounds    = pianoRoll.getBounds();
    const float  previousBeatWidth = pianoRoll.getBeatWidth();
    const double previousScroll    = pianoRoll.getScrollBeats();

    pianoRoll.setSize (width, height);
    pianoRoll.setBeatWidth (juce::jmax (pianoRoll.getBeatWidth(), 90.0f));

    const auto grid = pianoRoll.getGridBounds();
    const int  index = stack.getActiveLayerIndex();

    if (kind == DropSnapshotKind::groupRow)
    {
        //-- P92: a group-list row hovering a group already on the sheet -----
        // The second verb of the same drag: the frame under the pointer turns
        // warm and the tag says "替换 这 → 那". Both groups have to be picked
        // from what is actually on screen, for the reason the note walk below
        // exists: a frame that is scrolled out photographs as an empty grid.
        int targetGroup = -1;
        int targetOcc   = -1;

        for (const auto& group : layer->getGroups())
        {
            for (int which : layer->getGroupOccurrences (group.getId()))
            {
                const auto frame = pianoRoll.getGroupFrameBoundsForTest (index, group.getId(), which);

                if (! frame.isEmpty() && grid.contains (frame.getCentre()))
                {
                    targetGroup = group.getId();
                    targetOcc   = which;
                    break;
                }
            }

            if (targetGroup >= 0)
                break;
        }

        int sourceGroup = -1;

        for (const auto& group : layer->getGroups())
            if (group.getId() != targetGroup && ! layer->getGroupOccurrences (group.getId()).empty())
            {
                sourceGroup = group.getId();
                break;
            }

        if (targetGroup < 0 || sourceGroup < 0)
            return {};

        pianoRoll.simulateDropPreview (groupDragDescription (index, sourceGroup),
                                       pianoRoll.getGroupFrameBoundsForTest (index, targetGroup, targetOcc)
                                           .getCentre());
    }
    else if (kind == DropSnapshotKind::instrumentCard)
    {
        const int slotId = rack.getFirstInstrumentSlotId();

        if (slotId < 0)
            return {};

        // Walk the notes until one is actually ON SCREEN: the preview is drawn
        // at a point, and a point outside the grid draws nothing, which would
        // photograph an empty frame and call it documentation.
        for (const auto& note : layer->getNotes())
        {
            pianoRoll.setScrollBeats (juce::jmax (0.0, note.startBeat - 1.5));

            const auto bounds = pianoRoll.getNoteScreenBounds (index, note.id);

            if (! bounds.isEmpty() && grid.contains (bounds.getCentre()))
            {
                pianoRoll.simulateDropPreview (instrumentDragDescription (slotId),
                                               bounds.getCentre());
                break;
            }
        }
    }
    else
    {
        const auto samplers = rack.getSamplerSlotIds();

        if (samplers.empty())
            return {};

        for (const auto& sample : layer->getSamples())
        {
            pianoRoll.setScrollBeats (juce::jmax (0.0, sample.startBeat - 1.5));

            const auto bounds = pianoRoll.getSampleScreenBounds (index, sample.id);

            if (bounds.isEmpty() || ! grid.contains (bounds.getCentre()))
                continue;

            // A block that names a sampler uses THAT card for the label; one
            // that follows its layer falls back to the first sampler, which is
            // what "replace this with a file from the rack" would hand over.
            const int slotId = sample.outputSlotId >= 0 ? sample.outputSlotId
                                                        : samplers.front();

            pianoRoll.simulateDropPreview (samplerDragDescription (slotId),
                                           juce::Point<float> (bounds.getX() + juce::jmin (bounds.getWidth() * 0.4f,
                                                                                           ui::pf (40.0)),
                                                               bounds.getBottom() - ui::pf (4.0)));
            break;
        }
    }

    auto image = pianoRoll.createComponentSnapshot (pianoRoll.getLocalBounds(), true);

    pianoRoll.simulateDropPreview ({}, {});
    pianoRoll.setBounds (previousBounds);
    pianoRoll.setBeatWidth (previousBeatWidth);
    pianoRoll.setScrollBeats (previousScroll);

    return image;
}

juce::Image MainComponent::createSelectionMarkSnapshot (bool wholeShell, bool spotlit)
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr || layer->getNotes().empty())
        return {};

    const auto   previousSelection = pianoRoll.getSelection();
    const int    previousFocus     = getFocusedSlotId();

    // One note per DISTINCT routed slot, found through routing::resolve — the
    // picture has to be produced by the same rule the mark is drawn from, or
    // the frame documents a routing the engine does not play.
    std::vector<int> picked;
    std::vector<int> seen;

    for (const auto& note : layer->getNotes())
    {
        const auto resolved = routing::resolve (*layer, note, rack);

        if (! resolved.isValid())
            continue;

        if (std::find (seen.begin(), seen.end(), resolved.slotId) != seen.end())
            continue;

        seen.push_back (resolved.slotId);
        picked.push_back (note.id);

        if (picked.size() >= 2)
            break;
    }

    // The spotlight would dim the whole grid and turn this frame into the very
    // picture this feature exists to NOT be. `spotlit` is the comparison frame:
    // the same card, lit by hand, so the two rings can be put side by side and
    // be seen to be one ring.
    focusSynth (spotlit && ! seen.empty() ? seen.front() : -1);

    pianoRoll.setSelection (picked);
    updateRackRoutingFocus();

    // The rack's own scroll offset is wherever the last card growth left it;
    // a documentation frame wants the top of the list, where the first card's
    // header is, not the bottom.
    const int previousScroll = rackPanel.getScrollY();
    rackPanel.setScrollY (0);

    // The RACK, not the whole window: the mark is a ring around a card's border,
    // and the shell's own frame is 1800 px wide with the rack squeezed into 300
    // of them — a mark that has to be hunted for documents nothing.
    // `wholeShell` is the other half of the claim: the grid around it is at full
    // brightness, which is exactly what the spotlight does NOT do.
    auto image = wholeShell ? createComponentSnapshot (getLocalBounds(), true)
                            : rackPanel.createComponentSnapshot (rackPanel.getLocalBounds(), true);

    rackPanel.setScrollY (previousScroll);

    pianoRoll.setSelection (previousSelection);
    updateRackRoutingFocus();
    focusSynth (previousFocus);

    return image;
}

juce::Image MainComponent::createGroupMuteSnapshot (int width, int height)
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr || layer->getGroups().empty())
        return {};

    // Reached through findGroup, not through getGroups().front(): the list is
    // handed out read-only, and a snapshot is allowed to borrow the model, not
    // to reach into it.
    auto* group = layer->findGroup (layer->getGroups().front().getId());

    if (group == nullptr)
        return {};

    const auto   previousBounds     = pianoRoll.getBounds();
    const float  previousBeatWidth  = pianoRoll.getBeatWidth();
    const double previousScroll     = pianoRoll.getScrollBeats();

    // Muted for this frame only. The demo itself is not muted on purpose: a
    // fixture that ships silent would be a picture of the feature and a lie
    // about the mix.
    const bool wasMuted = group->isMuted();
    group->setMuted (true);

    // Selecting the group puts the MIXED selection on screen next to the pill
    // that says the group is silent — the two halves of this phase in one frame.
    pianoRoll.selectGroup (group->getId());

    pianoRoll.setSize (width, height);
    pianoRoll.setBeatWidth (juce::jmax (pianoRoll.getBeatWidth(), 64.0f));
    pianoRoll.setScrollBeats (7.0);

    auto image = pianoRoll.createComponentSnapshot (pianoRoll.getLocalBounds(), true);

    group->setMuted (wasMuted);
    pianoRoll.setBounds (previousBounds);
    pianoRoll.setBeatWidth (previousBeatWidth);
    pianoRoll.setScrollBeats (previousScroll);

    return image;
}

//==============================================================================
juce::Image MainComponent::createAutomationStackSnapshot (int width, int height)
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return {};

    const int layerIndex = stack.getActiveLayerIndex();

    // The note to inspect: the longest one that is ALSO in a group. A stack
    // picture needs all three layers live, and two of the three are addressed
    // through membership — picking the longest note outright would usually land
    // on an ungrouped one and photograph a two-layer stack.
    //
    // Collected by id, then resolved through findNote: getNotes() hands out a
    // read-only list, and this frame has to write a curve into the note it picks.
    int    chosenId     = -1;
    double chosenLength = -1.0;

    for (const auto& note : layer->getNotes())
        if (note.groupId >= 0 && note.lengthBeats > chosenLength)
        {
            chosenLength = note.lengthBeats;
            chosenId     = note.id;
        }

    auto* chosen = chosenId >= 0 ? layer->findNote (chosenId) : nullptr;

    if (chosen == nullptr)
        return {};

    auto* group = layer->findGroup (chosen->groupId);

    if (group == nullptr)
        return {};

    //-- Everything this frame borrows, saved so it can be handed back --------
    auto& volumeLane = layer->automationFor (LayerAutomationTarget::volume);

    const auto savedVolumePoints = volumeLane.getPoints();
    const bool savedVolumeOn     = volumeLane.isEnabled();

    auto& groupLane = group->getLevelAutomation();

    const auto savedGroupPoints = groupLane.getPoints();
    const bool savedGroupOn     = groupLane.isEnabled();

    const auto route = routing::resolve (*layer, *chosen, rack);
    auto* slot = rack.findSlot (route.slotId);

    std::vector<AutomationPoint> savedSlotPoints;
    bool savedSlotOn = false;

    if (slot != nullptr)
    {
        auto& lane = slot->automationFor (SlotAutomationTarget::level);
        savedSlotPoints = lane.getPoints();
        savedSlotOn     = lane.isEnabled();
    }

    const auto savedEnvelope = chosen->envelope();
    const auto savedTarget   = chosen->modTarget();

    //-- Three shapes a reader can tell apart --------------------------------
    // Placed inside the note's own span (it is one beat long), because a lane
    // that only turns at beat 6 and beat 14 samples as a flat rule across a
    // one-beat note — a true picture of nothing. Three different periods, so the
    // product cannot look like a scaled copy of any single layer.
    const double beat = chosen->startBeat;
    const double span = juce::jmax (0.25, chosen->lengthBeats);

    volumeLane.loadPoints ({ { 0.0,            0.95f },
                             { beat,           0.90f },
                             { beat + span * 0.35, 0.45f },
                             { beat + span * 0.75, 0.80f },
                             { beat + span,    0.55f },
                             { 64.0,           0.45f } });
    volumeLane.setEnabled (true);

    groupLane.loadPoints ({ { 0.0,            0.85f },
                            { beat,           0.35f },
                            { beat + span * 0.45, 0.95f },
                            { beat + span * 0.80, 0.50f },
                            { beat + span,    0.85f },
                            { 64.0,           0.60f } });
    groupLane.setEnabled (true);

    if (slot != nullptr)
    {
        auto& lane = slot->automationFor (SlotAutomationTarget::level);
        lane.loadPoints ({ { 0.0,            1.0f },
                           { beat,           1.0f },
                           { beat + span * 0.55, 0.60f },
                           { beat + span,    0.85f },
                           { 64.0,           0.75f } });
        lane.setEnabled (true);
    }

    chosen->setModTarget (NoteModTarget::level);
    chosen->envelope() = NoteEnvelope::makeADSR();
    chosen->envelope().setEnabled (true);

    //-- The frame -----------------------------------------------------------
    envelopePanel.openFor (layerIndex, chosen->id);
    envelopePanel.setSize (width, height);
    envelopePanel.resized();

    auto image = envelopePanel.createComponentSnapshot (envelopePanel.getLocalBounds(), true);

    envelopePanel.close();
    sampleEnvelopePanel.close();

    //-- Hand it all back ----------------------------------------------------
    volumeLane.loadPoints (savedVolumePoints);
    volumeLane.setEnabled (savedVolumeOn);

    groupLane.loadPoints (savedGroupPoints);
    groupLane.setEnabled (savedGroupOn);

    if (slot != nullptr)
    {
        auto& lane = slot->automationFor (SlotAutomationTarget::level);
        lane.loadPoints (savedSlotPoints);
        lane.setEnabled (savedSlotOn);
    }

    chosen->envelope() = savedEnvelope;
    chosen->setModTarget (savedTarget);

    stack.touchLayers();

    return image;
}

//==============================================================================
juce::Image MainComponent::createSlotParamAutomationSnapshot (int width, int height)
{
    // The first synth the demo ships, whatever it is. The point of the frame is
    // the ROW: its name column says which synth, and its parameter column says
    // 音源参数 — the two things the card menu's submenu is asking for.
    auto* slot = rack.getSlot (0);

    if (slot == nullptr)
        return {};

    const int  slotId        = slot->id;
    const auto savedTarget   = slot->automationTarget;
    const bool savedExpanded = slot->automationExpanded;
    const int  savedParam    = slot->getPluginParam();
    const auto savedPoints   = slot->getPluginAutomation().getPoints();
    const bool savedOn       = slot->getPluginAutomation().isEnabled();

    rack.setPluginParam (slotId, 0);

    auto& lane = slot->getPluginAutomation();
    lane.loadPoints ({ { 0.0, 0.30f }, { 4.0, 0.95f },
                       { 10.0, 0.45f }, { 16.0, 0.80f }, { 64.0, 0.55f } });
    lane.setEnabled (true);

    rack.setAutomationTarget (slotId, SlotAutomationTarget::plugin);
    rack.setAutomationExpanded (slotId, true);

    // A row, and the row lit — a list where the new line is at the bottom under
    // three others shows the label but not the fact that it is selected.
    const int trackId = automation.addTrack (AutomationOwner::slot, slotId,
                                             (int) SlotAutomationTarget::plugin);

    if (trackId > 0)
    {
        automation.selectTrack (trackId);
        automationWindow.revealTrack (trackId);
    }

    automationWindow.setSize (width, height);
    automationWindow.resized();

    auto image = automationWindow.createComponentSnapshot (automationWindow.getLocalBounds(), true);

    //-- Hand it all back -----------------------------------------------------
    if (trackId > 0)
        automation.removeTrack (trackId);

    lane.loadPoints (savedPoints);
    lane.setEnabled (savedOn);

    rack.setPluginParam (slotId, savedParam);
    rack.setAutomationTarget (slotId, savedTarget);
    rack.setAutomationExpanded (slotId, savedExpanded);

    return image;
}

//==============================================================================
void MainComponent::showDemoMixedSelection(){    auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return;

    // Phase 29: the shot shows what a marquee can produce — notes AND the block
    // selected at the same time.
    //
    // Phase 35: it also shows what a CLICK can produce, now that the demo's
    // block is a member of 组 A. Building the two lists by hand here was the
    // manual work this phase replaces; calling the same command the label calls
    // means the picture cannot show a selection the user could not make.
    if (! layer->getGroups().empty())
    {
        pianoRoll.selectGroup (layer->getGroups().front().getId());
        return;
    }

    std::vector<int> notes;

    for (const auto& note : layer->getNotes())
        if (notes.size() < 4 && note.startBeat >= 12.0 && note.startBeat <= 20.0)
            notes.push_back (note.id);

    std::vector<int> clips;

    for (const auto& clip : layer->getSamples())
        clips.push_back (clip.id);

    pianoRoll.setMixedSelection (notes, clips);
}

namespace
{
    /** Phase 34: the question both cascade deletions ask, in words.

        One builder for the prompt and for the snapshot that documents it: a
        picture of a dialog whose text was typed a second time is a picture of a
        dialog that no longer exists. */
    juce::String samplerCascadeMessage (const juce::String& slotName,
                                        int mounted,
                                        const juce::StringArray& names)
    {
        juce::String text = utf8 ("「")
                              + (slotName.isNotEmpty() ? slotName : utf8 ("采样音源"))
                              + utf8 ("」正被 ") + juce::String (mounted) + utf8 (" 个采样块使用：\n")
                              + names.joinIntoString (utf8 ("、"));

        if (mounted > names.size())
            text << utf8 (" 等");

        return text + utf8 ("\n\n删除这个采样音源，这些采样块会一起从钢琴窗上消失。");
    }

    /** The two answers, in one place — see samplerCascadeMessage(). Both
        questions use them: "删除 / 取消" reads correctly whether the thing being
        deleted is a card or an orphan. */
    void addSamplerCascadeButtons (juce::AlertWindow& alert)
    {
        alert.addButton (utf8 ("删除"), 1, juce::KeyPress ((int) 'd'));
        alert.addButton (utf8 ("取消"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    }
}

namespace
{
    /** Phase 84: the question the startup offer asks, in words.

        One builder for the dialog and for the picture that documents it — the
        same rule the cascade prompt follows, for the same reason: a fixture
        typed a second time is a picture of a dialog nobody is shown. */
    /** A timestamp in the shape the rest of the interface is written in.

        `Time::toString` spells the month in English ("30 Oct 2026 9:35:02am"),
        which is the one thing in this dialog that would look pasted in from
        another program — and it changes shape with the system locale, so the
        same copy could read differently on two machines. */
    juce::String chineseTime (juce::Time time)
    {
        return juce::String (time.getYear()) + "-"
                 + juce::String (time.getMonth() + 1).paddedLeft ('0', 2) + "-"
                 + juce::String (time.getDayOfMonth()).paddedLeft ('0', 2) + " "
                 + juce::String (time.getHours()).paddedLeft ('0', 2) + ":"
                 + juce::String (time.getMinutes()).paddedLeft ('0', 2);
    }

    juce::String recoveryOfferMessage (const juce::String& projectName,
                                       juce::Time savedAt,
                                       juce::int64 bytes,
                                       int olderCopies,
                                       const juce::File& folder)
    {
        juce::String extra;

        if (olderCopies > 0)
            extra = juce::String (olderCopies) + utf8 (" 份更早的副本也还在：\n")
                      + folder.getFullPathName() + utf8 ("\n\n");

        // Whole kilobytes only: the exact byte count of a rescue file is not a
        // number anybody makes a decision with.
        const auto kilobytes = juce::jmax ((juce::int64) 1, bytes / 1024);

        return projectName + utf8 (" 在上次退出时还有未保存的改动。\n\n")
                 + utf8 ("保存于 ") + chineseTime (savedAt)
                 + utf8 ("，约 ") + juce::String (kilobytes) + utf8 (" KB\n\n")
                 + extra
                 + utf8 ("要恢复它吗？");
    }

    /** Three answers, in one place. "稍后" is not a no: it leaves the copy
        alone, so the next launch asks again — the only choice here that cannot
        destroy something because of a mis-click. */
    void addRecoveryButtons (juce::AlertWindow& alert)
    {
        alert.addButton (utf8 ("恢复"), 1, juce::KeyPress ((int) 'r'));
        alert.addButton (utf8 ("丢弃"), 0, juce::KeyPress ((int) 'd'));
        alert.addButton (utf8 ("稍后"), -1, juce::KeyPress (juce::KeyPress::escapeKey));
    }
}

juce::Image MainComponent::createSamplerLinkSnapshot()
{
    // Phase 32: the clip is the subject, so the notes are let go — a mixed
    // selection would put two kinds of white ring on screen and muddy which one
    // the rack is answering.
    if (auto* layer = stack.getActiveLayer())
    {
        std::vector<int> clips;

        for (const auto& clip : layer->getSamples())
            clips.push_back (clip.id);

        // Phase 33: nothing else is called here on purpose. Selecting a block
        // is what lights its sampler, so the highlight in this frame is the
        // automatic chain doing its job — setSampleSelection notifies, the host
        // re-derives the focus, the rack draws a second outline. A manual
        // updateRackRoutingFocus() here would photograph the wiring instead of
        // the behaviour, which is the whole point of a drop being selected.
        pianoRoll.setSampleSelection (clips);
    }

    return createComponentSnapshot (getLocalBounds(), true);
}

juce::Image MainComponent::createExportDialogSnapshot (bool mp3)
{
    // Phase 52: the dialog lives in its own (modal) window, so a headless run
    // builds the CONTENT and photographs that — the same widget the window
    // holds, with the same settings the button would hand it.
    ExportDialog dialog;

    ExportSettings demo;
    demo.format     = mp3 ? ExportFormat::mp3 : ExportFormat::wav;
    demo.bitDepth   = 24;
    demo.mp3Bitrate = 192;
    demo.mono       = true;

    dialog.setSettings (demo);
    dialog.setContentSeconds (getContentSeconds());
    dialog.setMp3Available (mp3::isAvailable());
    dialog.setSize (ui::px (480), ui::px (320));

    return dialog.createComponentSnapshot (dialog.getLocalBounds(), true);
}

juce::Image MainComponent::createSamplerPromptSnapshot()
{
    // Phase 34: a dialog lives in its own window, and a headless run cannot
    // photograph a modal one. So the window is BUILT — never shown, never
    // entered into a modal state — and rendered. Text and buttons come from the
    // same two builders the real prompt uses: a fixture typed here would drift
    // and the picture would document a question nobody is asked.
    juce::AlertWindow alert (utf8 ("删除采样音源？"),
                             samplerCascadeMessage (utf8 ("采样 · Kick 01"), 2,
                                                    juce::StringArray { utf8 ("Kick A"),
                                                                        utf8 ("Kick B") }),
                             juce::MessageBoxIconType::WarningIcon,
                             nullptr);

    addSamplerCascadeButtons (alert);

    return alert.createComponentSnapshot (alert.getLocalBounds(), true);
}

juce::Image MainComponent::createRecoveryPromptSnapshot()
{
    // Phase 84: built, never shown — same as the cascade prompt above. The
    // numbers are a plausible copy three minutes old: what the picture has to
    // prove is the shape of the question (a project, a time, three answers),
    // not the size of one particular file.
    juce::AlertWindow alert (utf8 ("有未保存的工作"),
                             recoveryOfferMessage (utf8 ("未命名工程"),
                                                   juce::Time::getCurrentTime()
                                                     - juce::RelativeTime::minutes (3),
                                                   46 * 1024, 1,
                                                   recovery::getFolder()),
                             juce::MessageBoxIconType::QuestionIcon,
                             nullptr);

    addRecoveryButtons (alert);

    return alert.createComponentSnapshot (alert.getLocalBounds(), true);
}

//==============================================================================
//  Phase 8: new project
//==============================================================================
void MainComponent::buildSession (bool withEmptyLayer)
{
    // The layers and the rack are replaced one after the other, so for a moment
    // the new layers see the old rack. A sweep in that window would delete the
    // previous session's samplers while their replacement has not arrived yet.
    const juce::ScopedValueSetter<bool> noSweep (suspendSamplerPrune, true);

    envelopePanel.close();
    sampleEnvelopePanel.close();
    pianoRoll.closeNoteAutomation();
    closeMixerWindow();

    automation.clear();
    mixer.resetToDefault();

    // Phase 11: a new session comes with ONE built-in synth, already pointed at
    // by the empty layer. One instrument means "click the grid and you hear
    // something".
    //
    // Phase 18: it is sent to console channel 1 out of the box. Phase 11 left it
    // unrouted to keep the console honest about "a fresh project mixes
    // nothing", but in practice that meant the fader, the inserts and the
    // channel meter were all dead on a new project — the first thing anyone
    // reaches for. Channel 1 is where a console expects its first instrument.
    std::vector<std::unique_ptr<Layer>> layers;
    std::vector<SynthSlot> slots;

    SynthSlot defaultSlot = RackModel::makeDefaultSlot();
    defaultSlot.mixerChannel = 0;

    if (withEmptyLayer)
        layers.push_back (std::make_unique<Layer> (utf8 ("Layer 1"), palette::layer1,
                                                  defaultSlot.id));

    // The demo fixture adds its own three layers and two synths, so it asks for
    // no layer — but it still starts from a rack it can route to.
    slots.push_back (defaultSlot);

    stack.restoreLayers (std::move (layers), 0);
    rack.restoreSlots (std::move (slots));

    setBpm (120.0);
    bpmField.setBpm (120.0, false);

    // Phase 91: a new session is 4/4 — the metre is part of the song, and the
    // previous song's is not inherited any more than its tempo was.
    setBeatsPerBar (meter::defaultBeatsPerBar);

    currentProject = juce::File();
    pianoRoll.resetTimeline();
    pianoRoll.setScrollBeats (0.0);
    pianoRoll.setPlayheadBeat (0.0);

    // Phase 16: the spotlight belongs to the previous session's rack.
    focusedSlotId = -1;
    applyFocus();

    // Slots are gone, so any loaded plugin instance is now unreferenced.
    reconcilePlugins();
    republish();

    layerPanel.repaint();
    rackPanel.repaint();
    automationWindow.repaint();
    pianoRoll.repaint();
    repaint();
}

void MainComponent::buildBlankProject()
{
    buildSession (true);
}

void MainComponent::newProject()
{
    buildBlankProject();

    // A new session must not inherit the old one's undo stack, and it starts
    // clean: there is nothing on disk to protect yet.
    cancelPendingUpdate();
    history.clear();
    history.push_back (captureState());
    historyPos = 0;
    savedStateText = history.front();
    dirtyCache     = false;

    repaint (topBarBounds);
}

void MainComponent::requestNewProject()
{
    if (! hasUnsavedChanges())
    {
        newProject();
        return;
    }

    juce::Component::SafePointer<MainComponent> safe (this);

    confirmDiscardChanges (utf8 ("新建工程"), [safe]
    {
        if (safe != nullptr)
            safe->newProject();
    });
}

juce::File MainComponent::getDemoSampleFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
             .getChildFile ("AtomicDAW")
             .getChildFile ("Samples")
             .getChildFile ("demo-clip.wav");
}

void MainComponent::loadDemoProject()
{    // NOT buildBlankProject(): that leaves one empty layer behind, and the demo
    // adds three of its own — which produced a four-layer project whose top
    // sheet was an empty impostor of "Layer 1".
    buildSession (false);

    buildDemoContent();
    applyDemoMixState();

    // Phase 11: the fixture leaves the pencil loaded from a melody note, so the
    // snapshot shows what a copied state looks like. The copy is a value, not a
    // link — dropping the selection right after does not clear it.
    if (auto* melody = stack.getLayer (2))
    {
        // Snapshot the id before touching the stack: setActiveLayerIndex()
        // notifies listeners, and a reference into the note vector is not a
        // thing to hold across a notification.
        const auto& notes = melody->getNotes();
        const int   templateId = notes.size() > 2 ? notes[2].id : -1;

        if (templateId >= 0)
        {
            const int previousActive = stack.getActiveLayerIndex();

            stack.setActiveLayerIndex (2);
            pianoRoll.setSelection ({ templateId });
            pianoRoll.copyNoteStateFromSelection();
            pianoRoll.setSelection ({});
            stack.setActiveLayerIndex (previousActive);
        }
    }

    republish();

    cancelPendingUpdate();
    history.clear();
    history.push_back (captureState());
    historyPos = 0;
    savedStateText = history.front();     // the fixture counts as "saved"
    dirtyCache     = false;

    layerPanel.repaint();
    rackPanel.repaint();
    automationWindow.repaint();
    pianoRoll.repaint();
    repaint();
}

//==============================================================================
//  Phase 8: settings
//==============================================================================
//  Phase 9: VST3
//==============================================================================
void MainComponent::ensurePluginScan()
{
    if (! plugins.hasScanned())
        plugins.scan();
}

void MainComponent::openSettings (int tabIndex)
{
    ensurePluginScan();

    auto* window = ensureSettingsWindow();
    window->refresh();
    window->showPage (tabIndex);
}

SettingsWindow* MainComponent::ensureSettingsWindow()
{
    if (settingsWindow == nullptr)
    {
        settingsWindow = std::make_unique<SettingsWindow> (plugins, audio.getDeviceManager(),
                                                           *midiHub);

        // Phase 39: the page asks, the host decides. Switching the source has to
        // release the notes the old one was holding, and that is a job for the
        // thing that knows both sides.
        settingsWindow->onMidiSourcesChanged = [this] (MidiSources sources)
        {
            setMidiSources (sources);
        };

        settingsWindow->getLiveTargetName = [this] { return getLiveTargetName(); };

        settingsWindow->onAddInstrument = [this] (const juce::PluginDescription& description)
        {
            addPluginInstrument (description);
        };

        settingsWindow->onAddEffect = [this] (const juce::PluginDescription& description)
        {
            // From the manager there is no insert slot to click, so it goes to
            // the channel the mixer is already showing, first free slot.
            pendingEffectChannel = mixerPanel.getSelectedChannel();
            pendingEffectSlot    = -1;
            addPluginEffect (description);
        };
    }

    return settingsWindow.get();
}

juce::Image MainComponent::createSettingsSnapshot (int tabIndex, int width, int height)
{
    ensurePluginScan();

    // The device selector builds its combo boxes from the device manager's
    // state at construction time and refreshes them from a timer. A headless
    // run never spins that timer, and the self-test may have built this window
    // before any device was open — so rebuild the window instead of
    // photographing an empty "none" list. The real app only ever opens this
    // window after the device is running, so this path is harness-only.
    settingsWindow.reset();

    auto* window = ensureSettingsWindow();
    window->refresh();

    return window->createPageSnapshot (tabIndex, width, height);
}

juce::Image MainComponent::createMidiKeyboardSnapshot()
{
    // The keyboard alone: the FL Studio map with nothing competing for the
    // space. Off, the page is two unticked boxes and a sentence.
    const auto previous = getMidiSources();

    setMidiSources ({ true, false });

    const auto image = createSettingsSnapshot (1);

    setMidiSources (previous);
    return image;
}

juce::Image MainComponent::createMidiBothSnapshot()
{
    // Both switches on, which is the state the whole phase exists for: the map
    // and the device list have to share one page without either being cropped
    // into the other. A picture is the only way to check that.
    const auto previous = getMidiSources();

    setMidiSources ({ true, true });

    const auto image = createSettingsSnapshot (1);

    setMidiSources (previous);
    return image;
}

bool MainComponent::addPluginInstrument (const juce::PluginDescription& description)
{
    juce::String error;
    const int instanceId = plugins.load (description, &error);

    if (instanceId <= 0)
    {
        showMessage (utf8 ("无法载入插件"),
                     description.name + "\n\n"
                       + (error.isNotEmpty() ? error : utf8 ("插件格式不支持或文件已损坏")),
                     true);
        return false;
    }

    const int slotId = rack.addPluginSynth (description, instanceId);

    // Point the active layer at it. Adding an instrument that then plays
    // nothing because no layer routes to it is the least useful possible
    // outcome.
    if (const int active = stack.getActiveLayerIndex(); active >= 0)
        stack.setLayerOutputSlot (active, slotId);

    republish();
    updateRackRoutingFocus();
    repaint();
    return true;
}

bool MainComponent::addPluginEffect (const juce::PluginDescription& description)
{
    int channel = pendingEffectChannel >= 0 ? pendingEffectChannel
                                            : mixerPanel.getSelectedChannel();

    channel = juce::jlimit (0, MixerModel::masterChannel, channel);

    int slot = pendingEffectSlot;

    if (! juce::isPositiveAndBelow (slot, MixerModel::numInserts)
         || mixer.getInsert (channel, slot).type != fx::Type::none)
    {
        slot = -1;

        for (int i = 0; i < MixerModel::numInserts; ++i)
            if (mixer.getInsert (channel, i).type == fx::Type::none)
            {
                slot = i;
                break;
            }
    }

    if (slot < 0)
    {
        showMessage (utf8 ("没有空插槽"),
                     utf8 ("当前通道的 8 个插入槽都占满了，先清掉一个再添加。"), true);
        return false;
    }

    juce::String error;
    const int instanceId = plugins.load (description, &error);

    if (instanceId <= 0)
    {
        showMessage (utf8 ("无法载入插件"),
                     description.name + "\n\n"
                       + (error.isNotEmpty() ? error : utf8 ("插件格式不支持或文件已损坏")),
                     true);
        return false;
    }

    mixer.setInsertPlugin (channel, slot, description, instanceId);

    pendingEffectChannel = -1;
    pendingEffectSlot    = -1;

    republish();
    mixerPanel.repaint();
    return true;
}

void MainComponent::showRackAddMenu (juce::Point<int> screenPosition)
{
    ensurePluginScan();

    std::vector<juce::PluginDescription> instruments;

    for (const auto& entry : plugins.getLibrary())
        if (entry.isInstrument)
            instruments.push_back (entry.description);

    juce::PopupMenu menu;
    menu.addSectionHeader (utf8 ("添加音源"));
    menu.addItem (1, utf8 ("内置合成器"));

    juce::PopupMenu vstMenu;
    vstMenu.addSectionHeader (utf8 ("VST3 乐器"));

    if (instruments.empty())
        vstMenu.addItem (2, plugins.hasScanned() ? utf8 ("没有找到已安装的乐器")
                                                 : utf8 ("尚未扫描"), false);
    else
        addPluginItems (vstMenu, instruments, 100);

    menu.addSubMenu (utf8 ("VST3 乐器"), vstMenu, ! instruments.empty());
    menu.addSeparator();
    menu.addItem (3, utf8 ("插件管理器…"));

    juce::Component::SafePointer<MainComponent> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (
                            juce::Rectangle<int> (screenPosition.x, screenPosition.y, 1, 1)),
                        [safe, instruments] (int result)
    {
        if (safe == nullptr || result <= 0)
            return;

        if (result == 1)
        {
            const int slotId = safe->rack.addSynth();

            // A blank project has no output slot at all, so the first synth
            // added should be the one the active layer plays — otherwise it sits
            // there silent until the user finds the layer's dropdown.
            if (const int active = safe->stack.getActiveLayerIndex(); active >= 0)
                if (auto* layer = safe->stack.getLayer (active);
                    layer != nullptr && safe->rack.findSlot (layer->getOutputSlotId()) == nullptr)
                    safe->stack.setLayerOutputSlot (active, slotId);

            return;
        }

        if (result == 3)
        {
            safe->openSettings (2);
            return;
        }

        const int index = result - 100;

        if (juce::isPositiveAndBelow (index, (int) instruments.size()))
            safe->addPluginInstrument (instruments[(size_t) index]);
    });
}

void MainComponent::fillRackCardMenu (juce::PopupMenu& menu, int slotId)
{
    auto* slot = rack.findSlot (slotId);

    if (slot == nullptr)
        return;

    ensurePluginScan();

    std::vector<juce::PluginDescription> instruments;

    for (const auto& entry : plugins.getLibrary())
        if (entry.isInstrument)
            instruments.push_back (entry.description);

    menu.addSectionHeader (slot->getDisplayName() + utf8 (" · 输出到 ")
                             + (slot->sendsToMixer() ? utf8 ("混音器通道 ")
                                                       + juce::String (slot->mixerChannel + 1)
                                                     : utf8 ("主输出")));

    //-- Phase 38: the plugin's own window ------------------------------------
    // A hosted instrument HAS a UI of its own, and until now there was no way to
    // reach it from the rack at all: openPluginEditor existed, but only the
    // mixer's insert menu called it. A plugin you cannot open is a plugin whose
    // every parameter is stuck at its default.
    //
    // Above 替换音源 because it is what you want most of the time, and it says
    // "未载入" instead of going missing when the instance is not loaded —
    // "the item is greyed out" is a much cheaper bug report than "it does
    // nothing".
    {
        const bool hosted  = slot->isPlugin();
        const bool loaded  = hosted && slot->pluginId > 0
                               && plugins.isLoaded (slot->pluginId);

        menu.addItem (41, ! hosted        ? utf8 ("打开插件界面（非插件音源）")
                            : loaded      ? utf8 ("打开插件界面")
                                          : utf8 ("打开插件界面 · 未载入"),
                      loaded);
    }

    menu.addSeparator();

    //-- Replace the instrument ----------------------------------------------
    juce::PopupMenu replace;
    replace.addSectionHeader (utf8 ("内置合成器"));

    static const Waveform waveforms[4] { Waveform::sine, Waveform::saw,
                                         Waveform::square, Waveform::triangle };

    for (int i = 0; i < 4; ++i)
        replace.addItem (10 + i, waveformName (waveforms[i]), true,
                         ! slot->isPlugin() && slot->waveform == waveforms[i]);

    juce::PopupMenu vst;
    vst.addSectionHeader (utf8 ("VST3 乐器"));

    if (instruments.empty())
        vst.addItem (199, plugins.hasScanned() ? utf8 ("没有找到已安装的乐器")
                                               : utf8 ("尚未扫描"), false);
    else
        addPluginItems (vst, instruments, 200, slot->pluginName);

    replace.addSeparator();
    replace.addSubMenu (utf8 ("VST3 乐器"), vst, ! instruments.empty());

    menu.addSubMenu (utf8 ("替换音源"), replace, ! slot->isSampler());

    //-- Console send ---------------------------------------------------------
    // Phase 25: the ceiling is 127 strips, so the list is the LIVE ones. A menu
    // with 127 entries is not a menu, and the ids below start at 1000 because
    // 21 + channel would otherwise collide with the automation and delete items
    // further down this very menu.
    juce::PopupMenu send;
    send.addItem (20, utf8 ("不发送（直接到主输出）"), true, ! slot->sendsToMixer());

    const int liveChannels = mixerPanel.getStripCount() - 1;

    for (int ch = 0; ch < liveChannels; ++ch)
        send.addItem (kSendToChannelBase + ch, utf8 ("通道 ") + juce::String (ch + 1), true,
                      slot->mixerChannel == ch);

    menu.addSubMenu (utf8 ("发送到混音器通道"), send, true);

    //-- Automation -----------------------------------------------------------
    // Phase 14: no badge on the card any more, so the card menu is the only
    // way in.
    //
    // Phase 42: ONE submenu, and the synth's own parameters live inside it.
    // Three top-level rows whose names all began with 创建自动化 · read like a
    // list of parameters; the verb is one verb, so it is one entry. The
    // parameters come from the same lister the roll's boxes use, which is why a
    // VST that is loaded offers them and a slot that has never had one says so
    // instead of showing an empty menu.
    {
        juce::PopupMenu autoMenu;

        addSlotAutomationItems (autoMenu, slotId, 30);

        autoMenu.addSeparator();
        autoMenu.addItem (32, slot->automationExpanded ? utf8 ("收起面板中的曲线")
                                                       : utf8 ("在面板中展开曲线"), true);

        menu.addSubMenu (utf8 ("创建自动化"), autoMenu);
    }

    menu.addSeparator();

    //-- Phase 16: the explicit form of the old click = re-route gesture ------
    // A plain click now spotlights the synth instead of moving notes onto it,
    // so the destructive half needs a place where it says what it does.
    // Phase 27: a sampler holds a file and has no oscillator, so "put my notes
    // on it" is not an action that can do anything except mute them.
    menu.addItem (33, utf8 ("把选中音符挂到该音源"),
                  ! pianoRoll.getSelection().empty() && ! slot->isSampler());

    // Phase 29: this is a TOGGLE now. It used to set the spotlight and nothing
    // else, so the only way to take the veil off was to open a project — the
    // grid stayed grey and the item that caused it kept saying "highlight".
    menu.addItem (34, focusedSlotId == slotId ? utf8 ("取消高亮（显示全部）")
                                              : utf8 ("高亮这个音源"),
                  true, focusedSlotId == slotId);

    menu.addSeparator();
    menu.addItem (40, utf8 ("删除这个音源"), rack.getNumSlots() > 1);
}

void MainComponent::applyRackCardChoice (int result, int slotId)
{
    if (rack.findSlot (slotId) == nullptr)
        return;                     // the card was deleted while the menu was up

    if (result >= 10 && result <= 13)
    {
        static const Waveform bench[4] { Waveform::sine, Waveform::saw,
                                         Waveform::square, Waveform::triangle };

        // The slot id survives, so every layer route, note override and
        // automation track still points at the same instrument.
        rack.replaceWithNative (slotId, bench[result - 10]);
        reconcilePlugins();          // the old instance has no owner now
        rackPanel.repaint();
        repaint();
        return;
    }

    if (result == 20)
    {
        rack.setMixerChannel (slotId, -1);
        mixerPanel.repaint();
        return;
    }

    if (result >= kSendToChannelBase && result < kSendToChannelBase + MixerModel::maxChannels)
    {
        const int channel = result - kSendToChannelBase;
        rack.setMixerChannel (slotId, channel);
        mixerPanel.setSelectedChannel (channel);   // show where it went
        mixerPanel.repaint();
        return;
    }

    if (applySlotAutomationChoice (result, slotId, 30))
        return;

    if (result == 32)
    {
        if (auto* current = rack.findSlot (slotId))
            rack.setAutomationExpanded (slotId, ! current->automationExpanded);

        return;
    }

    if (result == 33)
    {
        // Same call the card's plain click makes (Phase 23): a group goes to
        // the group, loose notes to their own overrides.
        if (pianoRoll.mountSelectionTo (slotId))
        {
            republish();
            focusSynth (slotId);
            layerPanel.repaint();
            groupPanel.repaint();
            rackPanel.repaint();
        }

        return;
    }

    if (result == 34)
    {
        // Toggle, and hand the keyboard back to the roll: the menu was the last
        // thing with focus, and a roll that answers Delete only after you click
        // it is a roll that looks broken for one keystroke.
        focusSynth (focusedSlotId == slotId ? -1 : slotId);
        pianoRoll.grabKeyboardFocus();
        return;
    }

    if (result == 40)
    {
        // Phase 32: one entry point for "delete a synth" — the × and this item
        // must agree about what happens to the clips mounted on a sampler.
        // Phase 34: both go through the question for the same reason.
        requestRemoveSlotAndItsClips (slotId);
        return;
    }

    if (result == 41)
    {
        // Phase 38: the hosted instrument's own editor. Same window class the
        // mixer inserts use, so a plugin opened from the rack behaves exactly
        // like one opened from a channel strip (one window per instance, and
        // the window closes itself when the instance is unloaded).
        //
        // Phase 85: the same entry point the double click uses, so "the menu
        // item and the gesture agree" is a property of the code and not
        // something two branches have to keep remembering.
        auto* slot = rack.findSlot (slotId);

        if (slot == nullptr || ! slot->isPlugin())
            return;

        if (! openSlotEditor (slotId))
        {
            showMessage (utf8 ("插件没有载入"),
                         utf8 ("这个 VST3 在当前会话里没能加载，所以没有界面可开。\n")
                           + utf8 ("可以在「替换音源」里换一个乐器，或在插件管理器里重新扫描。"),
                         true);
        }

        return;
    }

    //-- A VST3 from the sub-menu --------------------------------------------
    // Phase 21: the choice can arrive long after the menu was built (it is now
    // reachable through a hover sub-menu inside a value menu), so the library
    // is re-read here instead of carrying a captured copy around.
    if (result < 200)
        return;

    std::vector<juce::PluginDescription> instruments;

    for (const auto& entry : plugins.getLibrary())
        if (entry.isInstrument)
            instruments.push_back (entry.description);

    const int index = result - 200;

    if (! juce::isPositiveAndBelow (index, (int) instruments.size()))
        return;

    juce::String error;
    const int instanceId = plugins.load (instruments[(size_t) index], &error);

    if (instanceId <= 0)
    {
        showMessage (utf8 ("无法载入插件"),
                     instruments[(size_t) index].name + "\n\n"
                       + (error.isNotEmpty() ? error : utf8 ("插件格式不支持或文件已损坏")),
                     true);
        return;
    }

    rack.replaceWithPlugin (slotId, instruments[(size_t) index], instanceId);
    reconcilePlugins();
    rackPanel.repaint();
    repaint();
}

void MainComponent::showRackCardMenu (int slotId, juce::Point<int> screenPosition)
{
    if (rack.findSlot (slotId) == nullptr)
        return;

    juce::PopupMenu menu;
    fillRackCardMenu (menu, slotId);

    juce::Component::SafePointer<MainComponent> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (
                            juce::Rectangle<int> (screenPosition.x, screenPosition.y, 1, 1)),
                        [safe, slotId] (int result)
    {
        if (safe == nullptr || result <= 0)
            return;

        safe->applyRackCardChoice (result, slotId);
    });
}

//==============================================================================
void MainComponent::openInsertEditor (int channel, int slot)
{
    const auto& insert = mixer.getInsert (channel, slot);

    if (insert.type == fx::Type::none)
        return;

    if (insert.type == fx::Type::vst)
    {
        if (insert.pluginId <= 0 || plugins.getProcessorForEditor (insert.pluginId) == nullptr)
        {
            showMessage (utf8 ("无法打开效果器"),
                         utf8 ("这个插槽的 VST3 没有加载成功，请右键换一个效果器，或在插件管理器里重新扫描。"),
                         true);
            return;
        }

        openPluginEditor (insert.pluginId, fx::displayName (insert));
        return;
    }

    for (auto& window : insertWindows)
    {
        if (window != nullptr && window->getChannel() == channel && window->getSlot() == slot)
        {
            window->toFront (true);
            return;
        }
    }

    auto window = std::make_unique<InsertEditorWindow> (mixer, channel, slot);
    InsertEditorWindow* raw = window.get();

    window->onCloseRequest = [this, raw] { closeInsertEditor (raw); };

    // Phase 47: the built-in effect's window is its own top-level component —
    // without the forwarder, space stopped starting the transport and the
    // keyboard stopped playing notes the moment the user opened an effect.
    // Same instance as the mixer and plugin windows: two instances would be
    // two key tables, and a note held in one would never release in another.
    window->setKeyForwarder (&pluginForwarder);

    insertWindows.push_back (std::move (window));
}

bool MainComponent::openSlotEditor (int slotId)
{
    auto* slot = rack.findSlot (slotId);

    // A built-in oscillator or a sampler has no window of its own. Saying so
    // here — once — is why the double click can stay silent about it.
    if (slot == nullptr || ! slot->isPlugin())
        return false;

    // Phase 12's rule: ask the host for the live instance, never assume the id
    // still resolves. A plugin that failed to load keeps its id and its name,
    // which is exactly the case a user cannot tell from the outside.
    if (plugins.getProcessorForEditor (slot->pluginId) == nullptr)
        return false;

    openPluginEditor (slot->pluginId, slot->getDisplayName());

    return true;
}

void MainComponent::openPluginEditor (int instanceId, const juce::String& displayName)
{
    if (const auto it = pluginWindows.find (instanceId); it != pluginWindows.end())
    {
        it->second->toFront (true);
        return;
    }

    auto* processor = plugins.getProcessorForEditor (instanceId);

    if (processor == nullptr)
        return;

    auto window = std::make_unique<PluginEditorWindow> (*processor, displayName, instanceId);

    window->onCloseRequest = [this, instanceId] { closePluginEditor (instanceId); };

    // Phase 42: the plugin's window is a separate top-level component, so a key
    // pressed inside it never reaches this one. The window installs the
    // forwarder on itself and on its content (the plugin's own editor).
    window->setKeyForwarder (&pluginForwarder);

    pluginWindows[instanceId] = std::move (window);
}

void MainComponent::closePluginEditor (int instanceId)
{
    // Called from the window's own close button: deleting the window here would
    // delete the object whose callback is still on the stack.
    juce::Component::SafePointer<MainComponent> safe (this);

    juce::MessageManager::callAsync ([safe, instanceId]
    {
        if (safe != nullptr)
            safe->pluginWindows.erase (instanceId);
    });
}

void MainComponent::closeInsertEditor (InsertEditorWindow* window)
{
    juce::Component::SafePointer<MainComponent> safe (this);

    juce::MessageManager::callAsync ([safe, window]
    {
        if (safe == nullptr || window == nullptr)
            return;

        auto& list = safe->insertWindows;
        list.erase (std::remove_if (list.begin(), list.end(),
                                    [window] (const std::unique_ptr<InsertEditorWindow>& w)
                                    {
                                        return w.get() == window;
                                    }),
                    list.end());
    });
}

//==============================================================================
void MainComponent::showInsertPluginMenu (int channel, int slot, juce::Point<int> screenPosition)
{
    ensurePluginScan();

    std::vector<juce::PluginDescription> effects;

    for (const auto& entry : plugins.getLibrary())
        if (! entry.isInstrument)
            effects.push_back (entry.description);

    juce::PopupMenu menu;
    menu.addSectionHeader (utf8 ("VST3 效果器 · 插槽 ") + juce::String (slot + 1));

    if (effects.empty())
        menu.addItem (2, plugins.hasScanned() ? utf8 ("没有找到已安装的效果器")
                                              : utf8 ("尚未扫描"), false);
    else
        addPluginItems (menu, effects, 100);

    menu.addSeparator();
    menu.addItem (3, utf8 ("插件管理器…"));

    juce::Component::SafePointer<MainComponent> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (
                            juce::Rectangle<int> (screenPosition.x, screenPosition.y, 1, 1)),
                        [safe, channel, slot, effects] (int result)
    {
        if (safe == nullptr || result <= 0)
            return;

        if (result == 3)
        {
            safe->openSettings (2);
            return;
        }

        const int index = result - 100;

        if (! juce::isPositiveAndBelow (index, (int) effects.size()))
            return;

        safe->pendingEffectChannel = channel;
        safe->pendingEffectSlot    = slot;
        safe->addPluginEffect (effects[(size_t) index]);
    });
}

void MainComponent::reconcilePlugins()
{
    std::vector<int> keep;

    for (int i = 0; i < rack.getNumSlots(); ++i)
        if (const auto* slot = rack.getSlot (i); slot != nullptr && slot->pluginId > 0)
            keep.push_back (slot->pluginId);

    for (int channel = 0; channel <= MixerModel::masterChannel; ++channel)
        for (const auto& insert : mixer.getInserts (channel))
            if (insert.pluginId > 0)
                keep.push_back (insert.pluginId);

    for (int id : plugins.getInstanceIds())
        if (std::find (keep.begin(), keep.end(), id) == keep.end())
        {
            // The editor window has to die BEFORE the instance: its destructor
            // tears down an AudioProcessorEditor that holds a pointer to the
            // processor. Synchronous erase, not closePluginEditor() — that one
            // defers, which is right for a close button and wrong here.
            pluginWindows.erase (id);
            plugins.unload (id);
        }

    // Built-in effect windows whose slot has been emptied have nothing left to
    // show, and a window that keeps editing a slot somebody cleared is worse
    // than no window at all.
    insertWindows.erase (std::remove_if (insertWindows.begin(), insertWindows.end(),
                                         [this] (const std::unique_ptr<InsertEditorWindow>& window)
                                         {
                                             return window == nullptr
                                                      || mixer.getInsert (window->getChannel(),
                                                                          window->getSlot()).type
                                                           == fx::Type::none;
                                         }),
                         insertWindows.end());
}

void MainComponent::syncGroupPanelSelection()
{
    std::vector<GroupPanel::RowRef> rows;

    for (const auto& group : pianoRoll.getSelectedGroups())
        rows.push_back ({ group.first, group.second });

    groupPanel.setSelectedGroups (rows);
}

void MainComponent::updateRackRoutingFocus()
{
    // Phase 30: selecting notes no longer switches the spotlight ON. It used
    // to: `focusedSlotId = resolved` on every selection change. That is how the
    // grid "went grey" without anyone asking for it — click a note and the whole
    // sheet dimmed, and the only way back was a gesture almost nobody finds. A
    // highlight is now something the user turns on (a card's 高亮 item) and
    // turns off (empty grid / Esc / the same item).
    //
    // Phase 32: a CLIP is the exception, because a clip is mounted on a sampler
    // and the sampler is not a place the music "happens to come out of" — it
    // holds the file. Selecting a block is how you say "this sample", so the
    // card that owns it lights up. That is a positive mark (a second outline
    // around one card), not a veil, so it cannot repeat the Phase 30 mistake.
    const int clipSampler = pianoRoll.selectedSamplerSlotId();

    if (clipSampler >= 0)
    {
        focusedSlotId = clipSampler;
        focusFromSampleSelection = true;
    }
    else if (focusFromSampleSelection)
    {
        // The selection that caused the highlight is gone. Only take the
        // highlight away when WE put it there — a card the user lit by hand has
        // to survive every later click on the grid.
        focusedSlotId = -1;
        focusFromSampleSelection = false;
    }

    // The rack still needs to know whether anything is SELECTED — that is what
    // decides whether a plain card click mounts or merely inspects — and that is
    // a different question, answered by setSelectionActive in applyFocus().
    applyFocus();

    //-- Phase 54: where the selected notes come out --------------------------
    // The spotlight above answers "show me only this synth" and dims the sheet,
    // which is why it stays a deliberate act. This one answers the smaller
    // question the same click asks — "which card is playing what I just
    // selected?" — and dims nothing, so it can be automatic.
    //
    // Resolved through routing::resolve, not by reading outputSlotId: the
    // answer has three tiers (note override / group mount / layer default) and
    // a second copy of that rule here is precisely how the card would end up
    // naming a synth the ear does not hear.
    {
        std::vector<int> routed;
        auto selected = pianoRoll.getSelection();

        if (! selected.empty())
        {
            std::sort (selected.begin(), selected.end());

            for (int i = 0; i < stack.getNumLayers(); ++i)
            {
                const auto* layer = stack.getLayer (i);

                if (layer == nullptr)
                    continue;

                for (const auto& note : layer->getNotes())
                    if (std::binary_search (selected.begin(), selected.end(), note.id))
                        if (const auto resolved = routing::resolve (*layer, note, rack);
                            resolved.isValid())
                            routed.push_back (resolved.slotId);
            }
        }

        rackPanel.setSelectionRouteSlots (std::move (routed));
    }
}

void MainComponent::focusSynth (int slotId)
{
    focusedSlotId = (slotId >= 0 && rack.findSlot (slotId) != nullptr) ? slotId : -1;

    // An explicit choice outranks the one the clip selection was making, and it
    // must not be revoked by the next selection change.
    focusFromSampleSelection = false;

    applyFocus();
}

void MainComponent::applyFocus()
{
    // A deleted synth must not stay spotlighted: the id would resolve to
    // nothing and the roll would dim the whole grid for a synth that is gone.
    if (focusedSlotId >= 0 && rack.findSlot (focusedSlotId) == nullptr)
        focusedSlotId = -1;

    // Phase 23: "is there a selection" is not "is a synth spotlit". The rack
    // needs the first one — it decides whether a plain card click mounts the
    // selection or merely inspects a synth.
    //
    // Phase 32: a selected CLIP is a selection too. Without it, clicking a
    // sampler card while a block is selected fell through to "inspect only",
    // which is the one gesture the user makes to say "play this sample on that
    // one".
    rackPanel.setSelectionActive (! pianoRoll.getSelection().empty()
                                   || ! pianoRoll.getSampleSelection().empty());
    rackPanel.setRoutingFocus (focusedSlotId, focusedSlotId >= 0);
    pianoRoll.setFocusSlotId (focusedSlotId);
    layerPanel.setFocusSlotId (focusedSlotId);
    groupPanel.setFocusSlotId (focusedSlotId);

    // Phase 39: the MIDI thread reads this, so it is cached rather than derived
    // there. Kept in applyFocus() because "which synth is spotlit" and "which
    // layer is active" are the two inputs, and both land here.
    refreshLiveTarget();
}

//==============================================================================
//  Phase 39: playing the rack's synth from a keyboard
//==============================================================================
void MainComponent::setMidiSources (MidiSources next)
{
    if (midiHub == nullptr)
        return;

    const auto previous = midiHub->getSources();

    if (previous == next)
        return;

    // A source that goes away must not leave notes behind — that is the
    // difference between silence and a chord no key can ever release.
    //
    // Per source, though, not wholesale: turning the controller ON while a
    // chord is held on the computer keyboard must not interrupt it. That is the
    // entire point of Phase 40's two switches.
    if (previous.typingKeyboard && ! next.typingKeyboard)
        releaseAllTypingNotes();

    if (previous.externalController && ! next.externalController)
        audio.getPlayback().allLiveNotesOff();   // a port cannot say what it holds

    midiHub->setSources (next);

    // Leaving the controller source closes its ports; the device ticks have to
    // stop claiming they are open.
    if (! next.externalController)
        midiHub->refreshDevices();

    refreshLiveTarget();
}

MidiSources MainComponent::getMidiSources() const noexcept
{
    return midiHub != nullptr ? midiHub->getSources() : MidiSources {};
}

bool MainComponent::typingKeyboardLive() const noexcept
{
    return midiHub != nullptr && midiHub->getSources().typingKeyboard;
}

int MainComponent::getLiveTargetSlot() const
{
    // Phase 43: the rule moved into Routing.h — the piano roll's key column was
    // asking a different question and answering with the layer's internal synth
    // while this one played the spotlit VST. One question, one function.
    if (const auto* layer = stack.getLayer (stack.getActiveLayerIndex()))
        return routing::auditionSlotId (focusedSlotId, *layer, rack);

    return -1;
}

juce::String MainComponent::getLiveTargetName() const
{
    const int slotId = getLiveTargetSlot();

    if (slotId < 0)
        return utf8 ("无：机架里还没有音源");

    return rack.getSlotName (slotId);
}

void MainComponent::refreshLiveTarget()
{
    liveTargetSlot.store (getLiveTargetSlot(), std::memory_order_relaxed);
}

void MainComponent::liveNoteOn (int pitch, float velocity)
{
    audio.getPlayback().noteOnLive (getLiveTargetSlot(), pitch, velocity);
}

void MainComponent::liveNoteOff (int pitch)
{
    audio.getPlayback().noteOffLive (pitch);
}

void MainComponent::setTypingOctave (int octaves)
{
    const int clamped = juce::jlimit (-4, 4, octaves);

    if (clamped == typingOctave)
        return;

    // Keys held across the shift keep the note they started: their release is
    // matched by the stored note, not re-derived from the new mapping.
    typingOctave = clamped;
}

//==============================================================================
bool MainComponent::handleTypingKeyDown (const juce::KeyPress& key)
{
    if (! typingKeyboardLive())
        return false;

    // Ctrl and Alt combinations stay shortcuts — Ctrl+S has to keep saving even
    // with the keyboard routed to the synth.
    if (key.getModifiers().isCommandDown() || key.getModifiers().isAltDown())
        return false;

    const int code = key.getKeyCode() != 0 ? key.getKeyCode()
                                           : (int) key.getTextCharacter();

    // Octave shift. The two keys that cannot be notes: the number row is the
    // upper octave's black keys in FL Studio's layout, so the numpad is what is
    // left, and it is also where every DAW puts it.
    if (code == juce::KeyPress::numberPadAdd || code == juce::KeyPress::numberPadSubtract)
    {
        setTypingOctave (typingOctave + (code == juce::KeyPress::numberPadAdd ? 1 : -1));
        return true;
    }

    const int note = typing::noteForKeyCode (code, typingOctave);

    if (note < 0)
        return false;

    // Auto-repeat: a held key keeps firing keyPressed. One note, not twelve.
    for (const auto& held : typingHeld)
        if (held.first == code && held.second == note)
            return true;

    liveNoteOn (note, typingVelocity);
    typingHeld.push_back ({ code, note });
    return true;
}

bool MainComponent::releaseTypingKeysThatAreUp()
{
    if (typingHeld.empty())
        return false;

    bool released = false;

    for (auto it = typingHeld.begin(); it != typingHeld.end(); )
    {
        if (juce::KeyPress::isKeyCurrentlyDown (it->first))
        {
            ++it;
            continue;
        }

        liveNoteOff (it->second);
        it = typingHeld.erase (it);
        released = true;
    }

    return released;
}

void MainComponent::releaseAllTypingNotes()
{
    for (const auto& held : typingHeld)
        liveNoteOff (held.second);

    typingHeld.clear();
}

bool MainComponent::keyStateChanged (bool isKeyDown)
{
    juce::ignoreUnused (isKeyDown);

    // The only "a key came up" signal JUCE has: ask which mapped keys are still
    // down. It arrives from whichever panel has focus, because key-state
    // changes walk up the parent chain like key presses do.
    return releaseTypingKeysThatAreUp();
}

//==============================================================================
//  Phase 32: clips and the samplers that hold them
//==============================================================================
void MainComponent::removeSlotAndItsClips (int slotId)
{
    const auto* slot = rack.findSlot (slotId);

    if (slot == nullptr)
        return;

    // The rack has always kept one slot: an empty rack has nothing to play
    // anything, and "the last card cannot be closed" is the rule the × states.
    if (rack.getNumSlots() <= 1)
        return;

    const bool isSampler = slot->isSampler();

    // Phase 34: held across the block deletion below. deleteSamplesMountedOn()
    // notifies, and a sweep inside that notification would see the very slot we
    // are holding as an orphan and ask a second question about it — the user
    // would be told the card is gone while it is still on screen.
    const juce::ScopedValueSetter<bool> guard (pruningSamplers, true);

    // The blocks first, the slot second. A clip naming a slot that no longer
    // exists is a dangling mount; deleting the slot first would create one for
    // as long as the next line takes to run, and every listener in between
    // (audio, panels, undo) would see it.
    if (isSampler)
        pianoRoll.deleteSamplesMountedOn (slotId);

    // Phase 34: the slot is gone, so a "keep it" verdict about it is meaningless
    // — and leaving the id behind would make a future slot that reuses it
    // silently exempt from the sweep.
    pruneDeclinedSlots.erase (slotId);

    rack.removeSlot (slotId);

    reconcilePlugins();
    mixerPanel.repaint();
    rackPanel.repaint();
    repaint();
}

//==============================================================================
//  Phase 34: nothing that deletes music happens without being announced
//==============================================================================
void MainComponent::installSamplerCascadePrompt()
{
    askSamplerCascade = [] (const juce::String& title,
                            const juce::String& body,
                            std::function<void (bool)> onAnswer)
    {
        auto* alert = new juce::AlertWindow (title, body,
                                             juce::MessageBoxIconType::WarningIcon,
                                             nullptr);

        addSamplerCascadeButtons (*alert);

        alert->enterModalState (true,
                                juce::ModalCallbackFunction::create (
                                    [answer = std::move (onAnswer)] (int result)
                                    {
                                        if (answer != nullptr)
                                            answer (result == 1);
                                    }),
                                true);          // delete when dismissed
    };
}

void MainComponent::requestRemoveSlotAndItsClips (int slotId)
{
    const auto* slot = rack.findSlot (slotId);

    if (slot == nullptr)
        return;

    // Same rule as the executor: the last card cannot be closed. Asking about a
    // deletion that is then going to be refused is worse than not asking.
    if (rack.getNumSlots() <= 1)
        return;

    if (! slot->isSampler())
    {
        removeSlotAndItsClips (slotId);
        return;
    }

    const int mounted = pianoRoll.countSamplesMountedOn (slotId);

    // Nothing on the grid is holding this file: the cascade has nothing to
    // announce, and a prompt with no cost in it is noise.
    if (mounted <= 0)
    {
        removeSlotAndItsClips (slotId);
        return;
    }

    const auto names = pianoRoll.namesOfSamplesMountedOn (slotId, 4);

    juce::Component::SafePointer<MainComponent> safe (this);

    askSamplerCascade (utf8 ("删除采样音源？"),
                       samplerCascadeMessage (slot->getDisplayName(), mounted, names),
                       [safe, slotId] (bool confirmed)
                       {
                           if (safe != nullptr && confirmed)
                               safe->removeSlotAndItsClips (slotId);
                       });
}

void MainComponent::removeSamplerSlots (const std::vector<int>& slotIds)
{
    // Held for the whole pass, not per slot: removeSlot() notifies, and this
    // function is reached from a notification.
    const juce::ScopedValueSetter<bool> guard (pruningSamplers, true);

    bool changed = false;

    for (const int id : slotIds)
    {
        const auto* slot = rack.findSlot (id);

        if (slot == nullptr || ! slot->isSampler())
            continue;

        // One has to stay, or the rack has nothing to add the next one to.
        if (rack.getNumSlots() <= 1)
            break;

        pruneDeclinedSlots.erase (id);
        rack.removeSlot (id);
        changed = true;
    }

    if (! changed)
        return;

    applyFocus();
    rackPanel.repaint();
    mixerPanel.repaint();
    repaint();
}

//==============================================================================
//  Phase 51: the sampler card's stretch control
//
//  Stretch mode is a BLOCK property (LayerModel), but the control lives on the
//  rack card — the panel cannot see layers, so both halves of the control are
//  host code. State = what the slot's blocks agree on; apply = write the mode
//  onto every block that plays the slot and let the notification (undo,
//  badges, schedule) happen through the one channel every model edit uses.
//==============================================================================
int MainComponent::samplerStretchStateFor (int slotId) const
{
    // -2: no block plays this slot — the card says 拉伸 and offers everything
    // with nothing ticked. -1: the blocks disagree — 混合, and no check mark
    // pretends otherwise.
    int state = -2;

    for (int i = 0; i < stack.getNumLayers(); ++i)
        if (const auto* layer = stack.getLayer (i))
            for (const auto& clip : layer->getSamples())
                if (clip.outputSlotId == slotId)
                {
                    const int mode = (int) clip.stretchMode;

                    if (state == -2)
                        state = mode;
                    else if (state != mode)
                        return -1;
                }

    return state;
}

void MainComponent::applySamplerStretchToSlot (int slotId, StretchMode mode)
{
    bool changed = false;

    // getSamples() is deliberately const-only (the P45 const audit); mutation
    // goes through findSample, the same door every other clip edit uses.
    for (int i = 0; i < stack.getNumLayers(); ++i)
        if (auto* layer = stack.getLayer (i))
            for (const auto& probe : layer->getSamples())
                if (auto* clip = layer->findSample (probe.id);
                    clip != nullptr && clip->outputSlotId == slotId
                      && clip->stretchMode != mode)
                {
                    clip->stretchMode = mode;
                    changed = true;
                }

    if (! changed)
        return;

    // One notification for the whole edit: undo captures it as one step, the
    // roll repaints its badges, and the schedule republishes with the new
    // ratios. This is the same channel every model edit uses — no callback
    // chains of "who remembers to tell whom".
    stack.notifyNotesChanged();
    rackPanel.repaint();
}

//-- Phase 52: the 只变调 range ------------------------------------------------
// Same shape as the stretch pair above, and for the same reason: the value
// belongs to the BLOCKS, the card only shows it and reports a change.
int MainComponent::samplerPitchRangeStateFor (int slotId) const
{
    int state = -2;

    for (int i = 0; i < stack.getNumLayers(); ++i)
        if (const auto* layer = stack.getLayer (i))
            for (const auto& clip : layer->getSamples())
                if (clip.outputSlotId == slotId)
                {
                    if (state == -2)
                        state = clip.pitchRangeSemitones;
                    else if (state != clip.pitchRangeSemitones)
                        return -1;          // 混合
                }

    return state;
}

void MainComponent::applySamplerPitchRangeToSlot (int slotId, int semitones)
{
    const int value = juce::jlimit (SampleEvent::minPitchRangeSemitones,
                                    SampleEvent::maxPitchRangeSemitones,
                                    semitones);

    bool changed = false;

    for (int i = 0; i < stack.getNumLayers(); ++i)
        if (auto* layer = stack.getLayer (i))
            for (const auto& probe : layer->getSamples())
                if (auto* clip = layer->findSample (probe.id);
                    clip != nullptr && clip->outputSlotId == slotId
                      && clip->pitchRangeSemitones != value)
                {
                    clip->pitchRangeSemitones = value;
                    changed = true;
                }

    if (! changed)
        return;

    // One notification for the whole edit — undo sees a single step, and the
    // schedule republishes with the new ceiling (it is read at publish time in
    // PlaybackEngine, not by the audio thread).
    stack.notifyNotesChanged();
    rackPanel.repaint();
}

void MainComponent::pruneOrphanSamplerSlots()
{
    // Three reasons to keep out: we are already inside a sweep (removeSlot
    // notifies, and this is reached from a notification); an undo is restoring
    // layers and rack in two steps; a session rebuild or a project load is doing
    // the same. In all three the layers and the rack are momentarily talking
    // about different songs, and a sweep there would delete a sampler that the
    // next line is about to reference.
    if (pruningSamplers || restoringHistory || suspendSamplerPrune)
        return;

    // Fast out. The sweep is now reached from every notesChanged, which during a
    // note drag means every mouse move; a song without samplers must not pay for
    // a walk over every clip to discover there is nothing to walk to.
    bool anySampler = false;

    for (int i = 0; i < rack.getNumSlots() && ! anySampler; ++i)
        if (const auto* slot = rack.getSlot (i); slot != nullptr && slot->isSampler())
            anySampler = true;

    if (! anySampler)
        return;

    // Phase 34: a prompt is already on screen, or queued behind the current
    // gesture. Asking a second time before the first answer arrives would mean
    // one dialog per mouse move.
    if (prunePromptQueued)
        return;

    if (orphanSamplerIds().empty())
        return;

    // Phase 34: the deletion now needs an answer, and the sweep is usually
    // reached from inside a drag. Ask from the message queue so the mouse-up
    // that the gesture is waiting for is not swallowed by a modal window.
    prunePromptQueued = true;

    if (! deferSamplerPrompt)
    {
        askToPruneOrphanSamplerSlots();
        return;
    }

    juce::Component::SafePointer<MainComponent> safe (this);

    juce::MessageManager::callAsync ([safe]
    {
        if (safe != nullptr)
            safe->askToPruneOrphanSamplerSlots();
    });
}

std::vector<int> MainComponent::orphanSamplerIds()
{
    // What every clip on every layer still names. A sampler that appears in
    // this list has something to play; one that does not is holding a file for
    // nobody.
    std::vector<int> referenced;

    for (int i = 0; i < stack.getNumLayers(); ++i)
        if (const auto* layer = stack.getLayer (i))
            for (const auto& clip : layer->getSamples())
                if (clip.outputSlotId >= 0)
                    referenced.push_back (clip.outputSlotId);

    std::vector<int> orphans;

    for (int i = 0; i < rack.getNumSlots(); ++i)
    {
        const auto* slot = rack.getSlot (i);

        if (slot == nullptr || ! slot->isSampler())
            continue;

        const bool named = std::find (referenced.begin(), referenced.end(), slot->id)
                             != referenced.end();

        // Being named again retires the "keep it" verdict: it was an answer
        // about a sampler with nothing to play, and that stopped being true.
        if (named)
        {
            pruneDeclinedSlots.erase (slot->id);
            continue;
        }

        if (pruneDeclinedSlots.find (slot->id) == pruneDeclinedSlots.end())
            orphans.push_back (slot->id);
    }

    return orphans;
}

void MainComponent::askToPruneOrphanSamplerSlots()
{
    // Re-discovered rather than carried over: the model may have moved on while
    // the prompt sat in the queue, and a question about a slot that some clip
    // names again is a question about nothing.
    std::vector<int> orphans = orphanSamplerIds();

    // "The rack keeps one slot" — offering to delete a card that the executor is
    // going to refuse is not a question, it is a bug report waiting to happen.
    const int removable = juce::jmax (0, rack.getNumSlots() - 1);

    if ((int) orphans.size() > removable)
        orphans.resize ((size_t) removable);

    if (orphans.empty())
    {
        prunePromptQueued = false;
        return;
    }

    juce::StringArray names;

    for (const int id : orphans)
        if (const auto* slot = rack.findSlot (id))
            names.add (slot->getDisplayName().isNotEmpty() ? slot->getDisplayName() : utf8 ("采样音源"));

    const juce::String detail = (orphans.size() == 1
                                   ? utf8 ("钢琴窗里已经没有采样块使用这个采样音源：\n")
                                   : utf8 ("钢琴窗里已经没有采样块使用这些采样音源：\n"))
                                + names.joinIntoString (utf8 ("、"))
                                + utf8 ("\n\n要删掉吗？");

    juce::Component::SafePointer<MainComponent> safe (this);

    askSamplerCascade (utf8 ("清理不再使用的采样音源？"), detail,
                       [safe, orphans] (bool confirmed)
                       {
                           if (safe == nullptr)
                               return;

                           safe->prunePromptQueued = false;

                           if (confirmed)
                           {
                               safe->removeSamplerSlots (orphans);
                               return;
                           }

                           // "Keep it" is remembered: the sweep runs on every
                           // model change, and re-asking on the next drag would
                           // turn an answer into a nag.
                           for (const int id : orphans)
                               safe->pruneDeclinedSlots.insert (id);
                       });
}


void MainComponent::showGroupMountMenu (int layerIndex, int groupId, juce::Point<int> screenPosition)
{
    auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
        return;

    auto* group = layer->findGroup (groupId);

    if (group == nullptr)
        return;

    juce::PopupMenu menu;
    menu.addSectionHeader (group->getName() + utf8 (" · 挂载音源"));
    menu.addItem (1, utf8 ("跟随图层（不单独挂载）"), true, group->followsLayerOutput());

    for (int i = 0; i < rack.getNumSlots(); ++i)
    {
        auto* slot = rack.getSlot (i);

        if (slot == nullptr)
            continue;

        // Phase 27: a group is mounted on an INSTRUMENT. A sampler holds a
        // file, not a player, so offering it here would mean "mount this group
        // on a slot that cannot sound a note" — and the user would have no way
        // to tell that from "the group went silent".
        if (slot->isSampler())
            continue;

        menu.addItem (10 + slot->id, slot->getDisplayName(), true,
                      group->getOutputSlotId() == slot->id);
    }

    juce::Component::SafePointer<MainComponent> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetScreenArea (juce::Rectangle<int> (screenPosition.x,
                                                                         screenPosition.y, 1, 1)),
                        [safe, layerIndex, groupId] (int result)
                        {
                            if (safe == nullptr || result <= 0)
                                return;

                            auto* targetLayer = safe->stack.getLayer (layerIndex);
                            auto* targetGroup = targetLayer != nullptr
                                                  ? targetLayer->findGroup (groupId) : nullptr;

                            if (targetGroup == nullptr)
                                return;     // dissolved while the menu was open

                            targetGroup->setOutputSlotId (result == 1 ? -1 : result - 10);

                            safe->stack.notifyNotesChanged();
                            safe->republish();
                            safe->focusSynth (result == 1 ? -1 : result - 10);
                            safe->pianoRoll.repaint();
                            safe->layerPanel.repaint();
                            safe->rackPanel.repaint();
                            safe->automationWindow.repaint();
                        });
}

void MainComponent::resetLayerNoteRouting (int layerIndex)
{
    auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
        return;

    for (auto& note : layer->getNotes())
        note.outputSlotOverride = -1;

    stack.notifyNotesChanged();

    republish();
    updateRackRoutingFocus();
    layerPanel.repaint();
    rackPanel.repaint();
}

//==============================================================================
void MainComponent::showSettingsMenu (juce::Point<int> screenPosition)
{
    juce::PopupMenu menu;
    menu.addSectionHeader (utf8 ("视图与编辑"));

    menu.addItem (1, utf8 ("吸附到 1/16 网格"), true, pianoRoll.isSnapEnabled());
    menu.addItem (2, utf8 ("Ghost 透明度 20%"), true,
                  std::abs (pianoRoll.getGhostAlpha() - 0.20f) < 0.01f);
    menu.addItem (3, utf8 ("Ghost 透明度 30%"), true,
                  std::abs (pianoRoll.getGhostAlpha() - 0.30f) < 0.01f);
    menu.addItem (4, utf8 ("Ghost 透明度 45%"), true,
                  std::abs (pianoRoll.getGhostAlpha() - 0.45f) < 0.01f);
    menu.addItem (5, utf8 ("Ghost 透明度 60%"), true,
                  std::abs (pianoRoll.getGhostAlpha() - 0.60f) < 0.01f);

    menu.addSeparator();
    menu.addItem (6, utf8 ("打开混音台窗口"), true);

    juce::Component::SafePointer<MainComponent> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (
                            juce::Rectangle<int> (screenPosition.x, screenPosition.y, 1, 1)),
                        [safe] (int result)
    {
        if (safe == nullptr || result <= 0)
            return;

        switch (result)
        {
            case 1: safe->pianoRoll.setSnapEnabled (! safe->pianoRoll.isSnapEnabled()); break;
            case 2: safe->pianoRoll.setGhostAlpha (0.20f); break;
            case 3: safe->pianoRoll.setGhostAlpha (0.30f); break;
            case 4: safe->pianoRoll.setGhostAlpha (0.45f); break;
            case 5: safe->pianoRoll.setGhostAlpha (0.60f); break;
            case 6: safe->toggleMixerWindow(); break;
            default: break;
        }

        safe->repaint();
    });
}

//==============================================================================
//  Phase 8: model listeners -> undo capture
//==============================================================================
// Phase 33: the sampler sweep hangs off the MODEL, not off the roll's delete
// command. It used to be a callback the piano roll fired from
// deleteSelectedSamples() — which meant every other way a block can disappear
// (a cut, an undo, a paste that replaced one, a group prune, code that edits the
// model directly) left its sampler behind, and the rule "no clip, no sampler"
// was true only for the one path someone remembered to wire. A block cannot
// vanish without one of these two notifications, so this is where the rule
// belongs. pruneOrphanSamplerSlots() is cheap when there is nothing to do.
void MainComponent::layersChanged()             { modelChanged(); pruneOrphanSamplerSlots(); }
void MainComponent::notesChanged()              { modelChanged(); pruneOrphanSamplerSlots(); }
void MainComponent::rackChanged()
{
    reconcilePlugins();       // a removed slot may be the last user of a plugin
    updateRackRoutingFocus();
    modelChanged();
}

void MainComponent::mixerChanged()
{
    reconcilePlugins();
    modelChanged();
}
void MainComponent::automationTracksChanged()
{
    // Phase 80: the BPM field's menu ticks on whether a tempo row exists, and
    // the row can also be DELETED — from the window, not from here. So the tick
    // is refreshed wherever the track list changes, never inferred by the field.
    bpmField.setTempoAutomationShown (hasTempoAutomationTrack());

    modelChanged();
}

//==============================================================================
bool MainComponent::startAudio()
{
    const bool running = audio.start();

    repaint (topBarBounds);
    return running;
}

void MainComponent::playTransport()
{
    playTransport (true);
}

void MainComponent::playTransport (bool openDevice)
{
    // Phase 51: the run's own start is remembered, because stop returns there
    // now (see stopTransport). Read BEFORE play(): the engine's play() starts
    // moving the position, and the beat the user chose is the one on the
    // playhead at this instant.
    playStartBeat = audio.getPositionBeats();

    audio.play (openDevice);
    pianoRoll.setTransportActive (audio.isTransportEngaged());

    rackPanel.repaint();
    repaint (topBarBounds);
}

//==============================================================================
void MainComponent::locatePlayhead (double beats)
{
    const double clamped = juce::jmax (0.0, juce::jmin (pianoRoll.getTimelineBeats(), beats));

    // setPositionBeats, not parkAt: clicking the ruler while the transport is
    // rolling must seek, not stop. When it is paused the state stays paused,
    // which is exactly what a DAW does.
    audio.setPositionBeats (clamped);

    pianoRoll.setPlayheadBeat (clamped);
    pianoRoll.setTransportActive (audio.isTransportEngaged());
    envelopePanel.setPlayheadBeat (clamped);
    layerPanel.setPlayheadBeat (clamped);
    rackPanel.setPlayheadBeat (clamped);
    automationWindow.setPlayheadBeat (clamped);

    repaint (topBarBounds);
}

void MainComponent::stopTransport()
{
    // Phase 51: a stop press returns the playhead to where THIS run started —
    // "play from bar 20, stop" parks the head at bar 20, ready for the same
    // run again. P32 removed that with a hardcoded zero because it made stop a
    // second play-from-here; with the run's own beat it is what a stop key
    // means, and rewind-to-top stays with Return and the ruler.
    //
    // Pressed while stopped it does NOT move, because the playhead is also the
    // "start playback here" control: click bar 20, then hit stop by reflex,
    // and being thrown anywhere would be the transport undoing a positioning
    // gesture.
    //
    // The engine's stopTransport() always rewinds (that is what its own name
    // promises, and what the offline renderer relies on), so the park position
    // is written straight after it — both calls happen inside one message and no
    // view is repainted between them.
    if (! stopDecision (audio.isTransportEngaged()).stop)
        return;

    // Phase 81: the take ends where the playhead was when Stop was pressed, so
    // this has to happen BEFORE the park moves it. Park first and the last note
    // would be as long as wherever the run had begun.
    finishRecording();

    parkTransportAt (stopDecision (true, playStartBeat).beat);
}

void MainComponent::rewindTransport()
{
    // The explicit rewind: from anywhere, rolling or not, back to the top.
    parkTransportAt (0.0);
}

void MainComponent::parkTransportAt (double target)
{
    const double beat = juce::jmax (0.0, target);

    audio.stopTransport();
    audio.setPositionBeats (beat);

    pianoRoll.setPlayheadBeat (beat);
    pianoRoll.setTransportActive (false);
    envelopePanel.setPlayheadBeat (beat);
    layerPanel.setPlayheadBeat (beat);
    rackPanel.setPlayheadBeat (beat);
    automationWindow.setPlayheadBeat (beat);

    rackPanel.repaint();
    repaint (topBarBounds);
}

//==============================================================================
//-- Phase 81: recording -------------------------------------------------------
// A take is two products of one pass of the record button: a buffer of what the
// sound card heard, and a list of what the keyboard played. They land in
// different places because they are different things — the audio becomes a
// sample (a file, a sampler slot, a block), the notes become an automatically
// named group whose level/pan lanes carry the CC the hand played at the same
// time.
juce::File MainComponent::getRecordingsFolder() const
{
    if (recordingsFolder != juce::File())
        return recordingsFolder;

    return juce::File::getSpecialLocation (juce::File::userMusicDirectory)
             .getChildFile ("AtomicDAW Recordings");
}

juce::Rectangle<int> MainComponent::getRecordButtonBounds() const
{
    // Right of Stop, left of the position readout: the three things a transport
    // has (go, stop, capture) read as one cluster, and the gap to the readout
    // marks where the transport ends and the readouts begin.
    return { getStopButtonBounds().getRight() + ui::px (6), topBarBounds.getY() + ui::px (12),
             ui::px (30), ui::px (28) };
}

bool MainComponent::startRecording()
{
    if (recording.load())
        return true;

    // Phase 88: rolling comes first, because play() rewinds a playhead parked at
    // the very end of the timeline. The take begins wherever the playhead ends
    // up, not where it was before the transport was engaged — reading the beat
    // first would put the take's first bar somewhere the song never went.
    if (! audio.isPlaying())
        playTransport();

    const int countIn = juce::jlimit (0, 64, countInBeats);

    // The count-in runs where the playhead already is. Nothing here moves it:
    // the four clicks are the four beats BEFORE the downbeat, and the downbeat
    // is the beat the playhead is sitting on.
    if (countIn > 0)
        audio.getPlayback().startPreRoll (countIn);

    recordStartBeat = audio.getPositionBeats();
    takeCollector.begin (recordStartBeat);
    recording.store (true);

    // The tape is armed now, so its first callback is the count-in's first
    // sample — and skipped past exactly the length of the count-in so the
    // take's first sample is the downbeat. Starting the recorder when the
    // count-in ends instead would be a timer tick late, and the take would
    // begin just after its own first note.
    const double sampleRate = juce::jmax (1.0, audio.getPlayback().getSampleRate());
    const int skipSamples = countIn > 0
        ? (int) std::llround ((double) countIn * audio.getPlayback().getSecondsPerBeat() * sampleRate)
        : 0;

    // The audio side may refuse (no capture channel, or none free). That is not
    // a reason to refuse the MIDI side: a keyboard take is still a take, and the
    // button has to mean something on a machine with no microphone.
    audio.beginRecording (300.0, skipSamples);

    repaint (topBarBounds);
    return true;
}

//==============================================================================
//  Phase 88: the metronome
//==============================================================================
void MainComponent::toggleMetronome()
{
    setMetronomeEnabled (! metronomeEnabled);
}

void MainComponent::setMetronomeEnabled (bool shouldClick)
{
    if (metronomeEnabled == shouldClick)
        return;

    metronomeEnabled = shouldClick;
    applyMetronomeSettings();
    repaint (getMetronomeButtonBounds());
}

void MainComponent::setMetronomeVolume (float volume01)
{
    metronomeVolume = juce::jlimit (0.0f, 1.0f, volume01);
    applyMetronomeSettings();
    repaint (getMetronomeButtonBounds());
}

void MainComponent::setCountInBeats (int beats)
{
    countInBeats = juce::jlimit (0, 64, beats);
    repaint (getMetronomeButtonBounds());
}

int MainComponent::getCountInBeatsLeft()
{
    return audio.getPlayback().getPreRollBeatsLeft();
}

void MainComponent::applyMetronomeSettings()
{
    auto& click = audio.getPlayback().getMetronome();

    click.setEnabled (metronomeEnabled);
    click.setVolume (metronomeVolume);

    // The accent follows the project's own bar length, so a 3/4 song clicks in
    // three. `ui::` has nothing to do with it — this is music, not geometry.
    click.setBeatsPerBar (meter::clamp (beatsPerBar));
}

void MainComponent::setBeatsPerBar (int beats)
{
    const int clamped = meter::clamp (beats);

    if (clamped == beatsPerBar)
        return;

    beatsPerBar = clamped;

    // Three readers, one push: the ruler's numbers and bar-line weight, where
    // "复制到后一小节" lands, and the click's accent. Nothing else caches it.
    pianoRoll.setBeatsPerBar (beatsPerBar);
    applyMetronomeSettings();

    // Deliberately NOT `modelChanged()` here. This also runs while a file is
    // being loaded and while a session is being reset, and neither of those is
    // an edit: a snapshot taken there would hand the NEXT song a phantom undo
    // step. The menu registers the step, because that is the only caller where
    // the user did something.
    repaint();
}


void MainComponent::showMeterMenu (juce::Point<int> screenPos)
{
    juce::PopupMenu menu;

    menu.addSectionHeader (utf8 ("每小节拍数"));

    // 2..12 over 4. `meter::clamp` accepts 1..16 — this is the list of metres
    // people write in, not the list of metres that exist, and a menu of sixteen
    // entries buries the four that anyone presses.
    for (int beats = 2; beats <= 12; ++beats)
        menu.addItem (beats, juce::String (beats) + "/4", true, beats == beatsPerBar);

    juce::Component::SafePointer<MainComponent> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (
                            juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1)),
                        [safe] (int result)
                        {
                            if (safe == nullptr || result <= 0)
                                return;

                            const int before = safe->beatsPerBar;

                            safe->setBeatsPerBar (result);

                            // One gesture, one step — and none at all when the
                            // metre did not actually change.
                            if (safe->beatsPerBar != before)
                                safe->modelChanged();
                        });
}


void MainComponent::showMetronomeMenu()
{
    juce::PopupMenu menu;

    menu.addSectionHeader (utf8 ("节拍器"));
    menu.addItem (1, utf8 ("节拍器 开 / 关"), true, metronomeEnabled);
    menu.addSeparator();

    menu.addSectionHeader (utf8 ("音量"));
    for (int percent : { 0, 25, 50, 75, 100 })
        menu.addItem (100 + percent, juce::String (percent) + "%", true,
                      (int) std::lround (metronomeVolume * 100.0f) == percent);

    menu.addSeparator();
    menu.addSectionHeader (utf8 ("预备拍"));

    // "1 小节" rather than "4 拍": the count-in is a bar of the song's own
    // metre, and a 3/4 song counts in three.
    //
    // The ids start at 210, not 200. They used to be 200/201/202 and the volume
    // items are `100 + percent` — so 100% was ALSO 200, and because the volume
    // branch is tested first (`100..200`), selecting 预备拍 ▸ 关 quietly set the
    // click to full volume instead of turning the count-in off. Three ids below
    // the volume range would have collided with 0/25/50; 210 is the first free
    // one and keeps "预备拍" contiguous.
    const int barBeats = meter::clamp (beatsPerBar);
    menu.addItem (210, utf8 ("关"), true, countInBeats == 0);
    menu.addItem (211, utf8 ("1 小节（") + juce::String (barBeats) + utf8 (" 拍）"),
                  true, countInBeats == barBeats);
    menu.addItem (212, utf8 ("2 小节（") + juce::String (barBeats * 2) + utf8 (" 拍）"),
                  true, countInBeats == barBeats * 2);

    // The second door to the metre. The ruler's corner is the one that is
    // always on screen, but it is a 96×42 cell that says "4/4" and nothing
    // else — nobody clicks it looking for a setting. This menu is where the
    // metronome already lives, and the count-in above already reads the metre,
    // so the two sit next to each other here. Same value, two entries, and both
    // go through `setBeatsPerBar` (P79: a name is one datum, not a copy).
    menu.addSeparator();
    menu.addSectionHeader (utf8 ("拍号"));

    juce::PopupMenu metres;

    for (int beats = 2; beats <= 12; ++beats)
        metres.addItem (300 + beats, juce::String (beats) + "/4", true, beats == barBeats);

    menu.addSubMenu (utf8 ("每小节拍数"), metres);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this)
                                                  .withTargetScreenArea (getMetronomeButtonBounds()),
                        [this] (int result)
                        {
                            if (result == 0)
                                return;

                            if (result == 1)
                                toggleMetronome();
                            else if (result >= 100 && result < 200)
                                setMetronomeVolume ((float) (result - 100) / 100.0f);
                            else if (result == 200)
                                setMetronomeVolume (1.0f);      // 100%
                            else if (result == 210)
                                setCountInBeats (0);
                            else if (result == 211)
                                setCountInBeats (meter::clamp (beatsPerBar));
                            else if (result == 212)
                                setCountInBeats (meter::clamp (beatsPerBar) * 2);
                            else if (result > 300)
                            {
                                // One gesture, one step — and none at all when
                                // the metre did not actually change.
                                const int previous = beatsPerBar;

                                setBeatsPerBar (result - 300);

                                if (beatsPerBar != previous)
                                    modelChanged();
                            }
                        });
}



void MainComponent::finishRecording()
{
    if (! recording.load())
        return;

    // Phase 88: a count-in still running when the take is closed would keep
    // clicking at a transport that is no longer counting towards anything.
    audio.getPlayback().cancelPreRoll();

    recording.store (false);

    const double endBeat = juce::jmax (recordStartBeat, audio.getPositionBeats());
    Take take = takeCollector.finish (endBeat);

    commitTake (std::move (take), audio.endRecording(), audio.getRecordSampleRate());

    repaint (topBarBounds);
}

void MainComponent::toggleRecording()
{
    if (recording.load())
        finishRecording();
    else
        startRecording();
}

void MainComponent::applyRecordedControls (NoteGroup& group, const Take& take) const
{
    struct Mapping
    {
        RecordTarget target;
        AutomationLane* lane;
    };

    const Mapping mappings[] { { RecordTarget::level, &group.getLevelAutomation() },
                               { RecordTarget::pan,   &group.getPanAutomation() } };

    for (const auto& mapping : mappings)
    {
        auto points = decimateRecordedCurve (take.controls, mapping.target);

        // Fewer than two points is not a curve, and loadPoints() would turn one
        // point into a flat line across the whole project — a take where the
        // hand touched the knob once must not silently rewrite the level.
        if (points.size() < 2)
            continue;

        mapping.lane->loadPoints (std::move (points));
        mapping.lane->setEnabled (true);
    }
}

int MainComponent::commitTake (Take take, const juce::AudioBuffer<float>& audioTake, double sampleRate)
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return -1;

    lastRecordGroupId = -1;
    lastRecordClipId  = -1;

    const bool hasAudio  = audioTake.getNumSamples() > 0 && sampleRate > 0.0;
    const bool hasNotes  = ! take.notes.empty();

    // Nothing arrived: do not bump the counter and do not leave an empty group
    // behind. A record press that captured silence is a press, not a take.
    if (! hasAudio && ! hasNotes)
        return -1;

    // One number per take, shared by the WAV and the group: "录音 3" is one
    // thing the user did, not two files that happen to be numbered alike.
    ++recordingCounter;

    //-- 1. the audio: file -> sampler -> block -------------------------------
    if (hasAudio)
    {
        if (auto buffer = makeSampleBuffer (audioTake, sampleRate))
        {
            const auto name = juce::String (utf8 ("录音 ")) + juce::String (recordingCounter);
            const auto file = getRecordingsFolder().getChildFile (name + ".wav");
            const bool wrote = writeSampleFile (file, *buffer);

            SampleEvent clip;
            clip.name = file.getFileName();
            clip.path = wrote ? file.getFullPathName() : juce::String();
            clip.audio = buffer;
            clip.startBeat = juce::jmax (0.0, take.startBeat);
            clip.pitch = 60;
            clip.laneSpan = 4;

            // The buffer is real seconds; the block is beats. Under a tempo map
            // those differ, so the width is measured on the map rather than
            // divided by a BPM — the same rule the exporter follows (P80).
            auto& playback = audio.getPlayback();
            const double endOfTake = playback.beatAtSeconds (playback.secondsAtBeat (clip.startBeat)
                                                             + buffer->lengthSeconds());
            clip.lengthBeats = juce::jmax (1.0, endOfTake - clip.startBeat);

            clip.outputSlotId = rack.addSampler (buffer, clip.path, clip.name);
            lastRecordClipId  = layer->addSample (std::move (clip));
        }
    }

    //-- 2. the notes: one group per take -------------------------------------
    std::vector<int> noteIds;
    noteIds.reserve (take.notes.size());

    for (const auto& recorded : take.notes)
    {
        NoteEvent note;
        note.pitch       = recorded.pitch;
        note.velocity    = recorded.velocity;
        note.startBeat   = recorded.startBeat;
        note.lengthBeats = juce::jmax (1.0 / 64.0, recorded.endBeat - recorded.startBeat);

        noteIds.push_back (layer->addNote (std::move (note)));
    }

    if (! noteIds.empty())
    {
        const int groupIndex = (int) layer->getGroups().size();
        const int groupId = layer->addGroup (utf8 ("录音 ") + juce::String (recordingCounter),
                                             groupPalette::nextGroupColour (groupIndex));

        layer->assignNotesToGroup (noteIds, groupId, 0);
        lastRecordGroupId = groupId;

        if (auto* group = layer->findGroup (groupId))
            applyRecordedControls (*group, take);
    }

    commitHistory();
    repaint();
    return lastRecordGroupId >= 0 ? lastRecordGroupId : lastRecordClipId;
}

//-- Test seams ---------------------------------------------------------------
void MainComponent::beginTakeForTest (double startBeat)
{
    recordStartBeat = startBeat;
    takeCollector.begin (startBeat);
    recording.store (true);
    lastRecordGroupId = -1;
    lastRecordClipId  = -1;
}

void MainComponent::recordNoteAt (int pitch, float velocity, bool on, double beat)
{
    if (on)
        takeCollector.noteOn (pitch, velocity, beat);
    else
        takeCollector.noteOff (pitch, beat);
}

void MainComponent::recordControlAt (int controller, int value7, double beat)
{
    takeCollector.control (controller, value7, beat);
}

void MainComponent::takeAudioForTest (const juce::AudioBuffer<float>& audioToUse, double sampleRate)
{
    testTakeAudio.makeCopyOf (audioToUse);
    testTakeSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
}

int MainComponent::endTakeForTest (double endBeat)
{
    if (! recording.load())
        return -1;

    recording.store (false);
    Take take = takeCollector.finish (endBeat);

    const int result = commitTake (std::move (take), testTakeAudio, testTakeSampleRate);

    testTakeAudio.setSize (0, 0);
    return result;
}

void MainComponent::setBpm (double newBpm)
{
    audio.setBpm (newBpm);

    // Keep the field showing the value the engine actually accepted, so typing
    // "999" snaps the display to 300 instead of lying about it.
    bpmField.setBpm (audio.getBpm(), false);
    pianoRoll.setBpm (audio.getBpm());

    repaint (topBarBounds);
}

bool MainComponent::hasTempoAutomationTrack() const
{
    return automation.contains (AutomationOwner::tempo, 0, 0);
}

void MainComponent::showTempoAutomation()
{
    // ownerId and target are both meaningless for this owner (there is one
    // curve), and addTrack's dedupe means asking twice selects the row that is
    // already there instead of adding a second one that edits the same curve.
    const int trackId = automation.addTrack (AutomationOwner::tempo, 0, 0);

    if (trackId <= 0)
        return;                       // the track list is full

    // A row appended below the fold is the same as no row at all — the same
    // reason the rack scrolls to the curve it just created.
    automationWindow.revealTrack (trackId);
    automationWindow.repaint();

    // No need to tick the menu item here: addTrack notified, and
    // automationTracksChanged() is where that tick comes from.
}

void MainComponent::timerCallback()
{
    // Phase 84: above every early-out below on purpose. The `! audio.isPlaying()`
    // return further down is the state the transport is in while somebody sits
    // and edits, and protecting only during playback would protect the wrong
    // half of the session.
    autosaveTick();

    // Phase 71: a plugin preview is a held key, not a fire-and-forget voice —
    // the engine will keep it down until it is told otherwise. This is what
    // lets go: a little after the last mouse move, like the oscillator preview
    // letting go of its own envelope. Above every early-out below, because a
    // preview sounds with the transport stopped and with nothing else running.
    if (hostedPreview.pitch >= 0)
    {
        const juce::uint32 now = juce::Time::getMillisecondCounter();

        // Signed difference, so the 49-day wrap of the millisecond counter
        // cannot leave a note held for the rest of the session.
        if ((juce::int32) (now - hostedPreview.releaseAtMs) >= 0)
            releaseHostedPreview();
    }

    // Phase 39: a key released while another window had focus never produces a
    // key-up event here, and a stuck note is the worst thing a keyboard can do.
    // This sits above the transport early-out on purpose: playing an instrument
    // with the transport stopped is the normal case, not an exception.
    if (! typingHeld.empty())
        releaseTypingKeysThatAreUp();

    // The active layer can change without the spotlight moving, and the MIDI
    // thread only ever reads the cache.
    refreshLiveTarget();

    // Phase 42: meters are not transport state, so they are updated ABOVE the
    // early-out below. With the updates inside it, stopping the transport froze
    // the last frame of every meter on screen — the engine had already let go of
    // the values, but nothing repainted them, and 右上角的 master 表 stayed at
    // whatever the last loud block was until something else forced a redraw.
    //
    // The `hasMeters()` half is the other side of that coin: once every bar has
    // fallen to the floor there is nothing left to draw, so the rack's 60 Hz
    // repaint stops instead of running for the rest of the session.
    if (audio.isPlaying() || audio.getPlayback().hasMeters())
    {
        rackPanel.updateMeters();
        mixerPanel.updateMeters();
        repaint (getMasterMeterBounds());
    }

    if (! audio.isPlaying())
        return;

    // getDisplayPositionBeats(), not getPositionBeats(): the audio callback only
    // reports a position once per buffer, so the raw value would step.
    const double beats = audio.getDisplayPositionBeats();

    pianoRoll.setPlayheadBeat (beats);
    envelopePanel.setPlayheadBeat (beats);
    layerPanel.setPlayheadBeat (beats);
    rackPanel.setPlayheadBeat (beats);
    automationWindow.setPlayheadBeat (beats);

    // Only the numbers that actually change are repainted, not the whole bar.
    repaint (getPositionReadoutBounds().expanded (2, 0));
}

//==============================================================================
void MainComponent::buildDemoContent()
{
    // Phase 11: a session starts with one built-in synth ("Synth A"), so the
    // fixture reuses it and adds exactly one more. Adding two here would leave
    // the default synth unused and rename the pair to B/C.
    const int synthA = rack.getSlotIdAt (0);
    const int synthB = rack.addSynth();

    const int l1 = stack.addLayer ("Layer 1", palette::layer1);
    const int l2 = stack.addLayer ("Layer 2", palette::layer2);
    const int l3 = stack.addLayer ("Layer 3", palette::layer3);

    // Layer 1 — low pedal line, long notes. Goes to Synth A.
    if (auto* layer = stack.getLayer (l1))
    {
        const int roots[] { 36, 36, 43, 41, 36, 34, 41, 39 };

        for (int bar = 0; bar < 8; ++bar)
            layer->addNote ({ 0, roots[bar], bar * 4.0, 3.5, 0.85f });
    }

    // Layer 2 — sustained triads, deliberately overlapping Layer 1 in pitch.
    // Sent to Synth B, so the two sheets are also two different instruments.
    if (auto* layer = stack.getLayer (l2))
    {
        const int chords[4][3] { { 60, 64, 67 }, { 62, 65, 69 }, { 59, 62, 67 }, { 57, 60, 64 } };

        for (int bar = 0; bar < 8; ++bar)
            for (int voice = 0; voice < 3; ++voice)
                layer->addNote ({ 0, chords[bar % 4][voice], bar * 4.0 + 0.5, 3.0, 0.70f });
    }

    // Layer 3 — melody, back on Synth A, except for one note that bypasses the
    // layer and goes straight to Synth B (the visible proof of note routing).
    if (auto* layer = stack.getLayer (l3))
    {
        const int    melody[] { 72, 74, 76, 79, 76, 74, 72, 71, 72, 76, 79, 81, 79, 76, 74, 72 };
        const double lengths[] { 0.5, 0.5, 0.75, 1.0 };

        std::vector<int> ids;

        for (int i = 0; i < 32; ++i)
            ids.push_back (layer->addNote ({ 0, melody[i % 16], i * 2.0, lengths[i % 4], 0.90f }));

        // The fourth melody note (beat 6) is handed to Synth B directly.
        // Deliberately early in the timeline: the inspector opens to its right,
        // so both the note and its badge stay visible in screenshots.
        const int bypassIndex = 3;

        if (juce::isPositiveAndBelow (bypassIndex, (int) ids.size()))
        {
            if (auto* note = layer->findNote (ids[(size_t) bypassIndex]))
            {
                note->outputSlotOverride = synthB;
                demoOverrideNote = note->id;
                demoOverrideLayer = l3;
            }
        }
    }

    stack.setLayerOutputSlot (l1, synthA);
    stack.setLayerOutputSlot (l2, synthB);
    stack.setLayerOutputSlot (l3, synthA);

    //-- Phase 16: a group, mounted on Synth B --------------------------------
    // The melody notes between bars 3 and 6 become "组 A": mounted on Synth B
    // (so they leave the layer's Synth A behind) and given a level curve. One
    // fixture that shows all three new ideas at once — the mark in the roll,
    // the mount over the layer default, and a row in the automation window.
    // Placed where the screenshot looks, not at the end of the timeline.
    if (auto* layer = stack.getLayer (l3))
    {
        std::vector<int> members;

        for (const auto& note : layer->getNotes())
            if (note.startBeat >= 8.0 && note.startBeat < 24.0)
                members.push_back (note.id);

        if (! members.empty())
        {
            const int groupId = layer->addGroup (groupPalette::defaultName (0),
                                                 groupPalette::group1);
            layer->assignNotesToGroup (members, groupId);

            if (auto* group = layer->findGroup (groupId))
            {
                group->setOutputSlotId (synthB);

                auto& lane = group->getLevelAutomation();
                lane.setEnabled (true);
                lane.loadPoints ({ { 8.0, 1.0f }, { 15.0, 0.35f }, { 23.0, 1.0f } });
            }
        }
    }

    //-- Phase 17: a second group, so the list has two rows ------------------
    // Layer 1's pedal line split in half: two groups with different mounts is
    // the only way the group panel can show that "发送音源" is per-group and not
    // a property of the layer.
    if (auto* layer = stack.getLayer (l1))
    {
        std::vector<int> pedalMembers;

        for (const auto& note : layer->getNotes())
            if (note.startBeat >= 16.0)
                pedalMembers.push_back (note.id);

        if (! pedalMembers.empty())
        {
            const int groupId = layer->addGroup (groupPalette::defaultName (1),
                                                 groupPalette::group2);
            layer->assignNotesToGroup (pedalMembers, groupId);

            if (auto* group = layer->findGroup (groupId))
                group->setLevelDb (-3.5f);
        }
    }

    //-- Phase 17: a sample clip on the active sheet -------------------------
    // The fixture writes its own WAV instead of shipping a binary: the decoder
    // is hand-written, so the test material has to be something this build can
    // produce. Written once, reused afterwards.
    if (auto* layer = stack.getLayer (l3))
    {
        const auto sampleFile = getDemoSampleFile();

        if (! sampleFile.existsAsFile() || sampleFile.getSize() < 1024)
            writeTestSampleFile (sampleFile, 2.5, 44100.0, 2, 196.0);

        auto loaded = loadSampleFile (sampleFile);

        if (loaded.ok())
        {
            SampleEvent clip;
            clip.name        = sampleFile.getFileName();
            clip.path        = sampleFile.getFullPathName();
            clip.audio       = loaded.buffer;

            // Placed where the roll opens, above the group band: a fixture that
            // has to be scrolled to is not a fixture. Pitch 81 is the highest
            // lane whose four-row block still fits under the top edge.
            clip.startBeat   = 12.0;
            clip.pitch       = 81;
            clip.laneSpan    = 4;
            clip.gainDb      = -1.5f;

            // Width comes from the real duration, rounded up to the beat grid —
            // exactly what addSampleAt() does for a dropped file.
            clip.lengthBeats = juce::jmax (1.0, std::ceil (SampleEvent::beatsForSeconds (
                                                        loaded.buffer->lengthSeconds(),
                                                        audio.getBpm())));

            // Phase 27: the demo's clip is mounted on a sampler, exactly like a
            // dropped one. Leaving it unmounted would mean the fixture never
            // exercises the feature it is supposed to demonstrate — and the
            // rack in the snapshot would show two instruments and no sampler.
            clip.outputSlotId = rack.addSampler (loaded.buffer, clip.path, clip.name);

            // Phase 49: the demo block wears its volume curve, because a
            // feature with no example in the demo is a feature nobody
            // discovers. A ramp up and a longer ramp down — the same shape the
            // old fades drew, so the demo sounds like it always did.
            clip.ensureCurveDriving (SampleAutomationTarget::volume).loadPoints ({ { 0.00f, 0.0f },
                                           { 0.12f, 1.0f },
                                           { 0.70f, 1.0f },
                                           { 1.00f, 0.0f } });
            clip.ensureCurveDriving (SampleAutomationTarget::volume).setEnabled (true);

            const int clipId = layer->addSample (std::move (clip));

            // Phase 35: the block joins 组 A, which is the group this sheet
            // already has — so the demo contains a MIXED group out of the box.
            // Without a member of each kind in one group, the new rule ("select
            // the whole group" → both kinds lit) has nothing to demonstrate and
            // the roll would keep showing groups as a notes-only idea.
            if (auto* groupA = layer->getGroups().empty() ? nullptr : &layer->getGroups().front())
                layer->assignSamplesToGroup ({ clipId }, groupA->getId());
        }

        // First run only: point the browser at the folder holding that file, so
        // a fresh install has one row to drag instead of an empty list. Never
        // overwritten once the user has folders of their own.
        if (filePanel.getNumFolders() == 0)
            filePanel.addFolder (sampleFile.getParentDirectory());
    }

    // Layer 3 stays active (added last), so Layers 1 and 2 render as ghosts.
    stack.setActiveLayerIndex (l3);
    // The mix state is applied by the constructor, AFTER the history has been
    // seeded: that makes "undo" in a fresh session do something real (drop the
    // demo's faders and curves) instead of sitting there greyed out.

    // Publish explicitly instead of relying on the last notify to have covered
    // everything. Building the demo mutates notes without notifying, so this is
    // the one guaranteed "the engine now knows what exists" point.
    audio.getPlayback().publishSchedule (stack, rack, audio.getBpm(), 0.0, &mixer, &tempo);
}

//==============================================================================
void MainComponent::applyDemoMixState()
{
    // A project that starts with every fader at unity and no curve would make
    // Phase 7 look like Phase 4. This is the state the snapshot shows.
    constexpr double len = timeline::defaultLengthBeats;

    //-- Layer 1: low pedal line, volume curve + a little drive --------------
    if (auto* layer = stack.getLayer (0))
    {
        layer->setLevelDb (-4.0f);
        layer->setPan (-0.12f);

        auto& lane = layer->automationFor (LayerAutomationTarget::volume);
        lane.resetFlat (0.62f, len);
        lane.addPoint (8.0,  0.95f, len);
        lane.addPoint (16.0, 0.55f, len);
        lane.addPoint (24.0, 1.00f, len);
        lane.addPoint (36.0, 0.42f, len);
        lane.addPoint (48.0, 0.88f, len);
        lane.setEnabled (true);

        layer->setAutomationTarget (LayerAutomationTarget::volume);
    }

    //-- Layer 2: chords panned across the stereo field ----------------------
    if (auto* layer = stack.getLayer (1))
    {
        layer->setLevelDb (-6.0f);

        auto& lane = layer->automationFor (LayerAutomationTarget::pan);
        lane.resetFlat (0.5f, len);
        lane.movePoint (0, 0.0,  0.18f, len);
        lane.movePoint (1, len,  0.18f, len);
        lane.addPoint (16.0, 0.84f, len);
        lane.addPoint (32.0, 0.16f, len);
        lane.addPoint (48.0, 0.78f, len);
        lane.setEnabled (true);

        layer->setAutomationTarget (LayerAutomationTarget::pan);
    }

    //-- Layer 3: melody, no automation yet (the window shows it switched off)
    if (auto* layer = stack.getLayer (2))
    {
        layer->setLevelDb (-2.5f);
        layer->setPan (0.12f);
    }

    //-- Synth A: shared by layers 1 and 3, filter swept by its lane ---------
    if (auto* slot = rack.getSlot (0))
    {
        slot->levelDb = -4.0f;
        slot->pan     = -0.08f;

        auto& lane = slot->automationFor (SlotAutomationTarget::filter);
        lane.resetFlat (0.55f, len);
        lane.addPoint (12.0, 0.90f, len);
        lane.addPoint (24.0, 0.18f, len);
        lane.addPoint (36.0, 0.75f, len);
        lane.addPoint (48.0, 0.30f, len);
        lane.setEnabled (true);

        rack.setAutomationTarget (slot->id, SlotAutomationTarget::filter);
    }

    //-- Synth B: pushed back in the image, level lane left switched off -----
    if (auto* slot = rack.getSlot (1))
    {
        slot->levelDb = -8.0f;
        slot->pan     = 0.18f;
        rack.setAutomationTarget (slot->id, SlotAutomationTarget::level);
    }

    //-- Console: two synths, two channels, three inserts --------------------
    // Phase 11: a channel is fed by a synth, so the demo has to route them. This
    // is also the picture of "the console is not in the way by default": it took
    // these two sends to put anything on the strips at all.
    if (auto* slotA = rack.getSlot (0))
        rack.setMixerChannel (slotA->id, 0);

    if (auto* slotB = rack.getSlot (1))
        rack.setMixerChannel (slotB->id, 1);

    mixer.setInsertType (0, 0, fx::Type::drive);
    mixer.setInsertParam (0, 0, 0.28f, 0.4f);
    mixer.setInsertType (1, 0, fx::Type::lowPass);
    mixer.setInsertParam (1, 0, 0.72f, 0.4f);
    mixer.setInsertType (1, 1, fx::Type::delay);
    mixer.setInsertParam (1, 1, 0.34f, 0.30f);

    //-- The automation window's opening set --------------------------------
    automation.clear();

    if (auto* layer = stack.getLayer (0))
        automation.addTrack (AutomationOwner::layer, layer->getId(),
                             (int) LayerAutomationTarget::volume);

    if (auto* layer = stack.getLayer (1))
        automation.addTrack (AutomationOwner::layer, layer->getId(),
                             (int) LayerAutomationTarget::pan);

    if (auto* layer = stack.getLayer (2))
        automation.addTrack (AutomationOwner::layer, layer->getId(),
                             (int) LayerAutomationTarget::volume);

    if (auto* slot = rack.getSlot (0))
        automation.addTrack (AutomationOwner::slot, slot->id,
                             (int) SlotAutomationTarget::filter);

    // Phase 16: the demo group's level curve, so the window opens with a row
    // that says "组 A · 组音量" next to the layers and the synth. One group row
    // is enough to prove the phase: two identical-looking rows only push the
    // last row of the opening set below the fold.
    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        auto* layer = stack.getLayer (i);

        if (layer == nullptr || layer->getGroups().empty())
            continue;

        automation.addTrack (AutomationOwner::group, layer->getGroups().front().getId(),
                             (int) GroupAutomationTarget::level);
        break;
    }

    // Phase 18: the master bus's own volume curve. The demo needs it for the
    // same reason it carries a group curve — a row is the only honest proof
    // that "the master can be automated" is not just a menu item.
    {
        auto& masterLane = mixer.automationFor (0);
        masterLane.loadPoints ({ { 0.0, 0.86f }, { 12.0, 1.0f }, { 28.0, 0.90f }, { 64.0, 0.96f } });
        masterLane.setEnabled (true);

        automation.addTrack (AutomationOwner::master, 0, 0);
    }

    automation.selectTrack (automation.getNumTracks() > 0
                              ? automation.getTracks().front().id : -1);

    stack.touchLayers();
}

//==============================================================================
void MainComponent::republish()
{
    audio.getPlayback().publishSchedule (stack, rack, audio.getBpm(), 0.0, &mixer, &tempo);
    automationWindow.repaint();
    mixerPanel.repaint();
}

void MainComponent::fillChannelMenu (juce::PopupMenu& menu, int channel)
{
    const bool isMaster  = channel == MixerModel::masterChannel;
    const int  slotId    = isMaster ? -1 : mixerPanel.slotIdOnChannel (channel);
    const int  count     = mixer.getChannelCount();

    juce::String title;

    if (isMaster)
    {
        title = utf8 ("主输出");
    }
    else
    {
        title = utf8 ("通道 ") + juce::String (channel + 1) + " · "
                  + (slotId >= 0 ? rack.getSlotName (slotId) : utf8 ("无音源"));
    }

    menu.addSectionHeader (title);

    // Phase 42: the parameters of a strip's own synth are in the same submenu,
    // because a channel strip IS a synth's console view. One verb = one entry:
    // 创建自动化, and the list of what can be automated lives inside it.
    {
        juce::PopupMenu autoMenu;

        if (isMaster)
        {
            autoMenu.addItem (1, utf8 ("主输出音量"), true,
                              automation.contains (AutomationOwner::master, 0, 0));
            autoMenu.addItem (2, utf8 ("主输出声相"), true,
                              automation.contains (AutomationOwner::master, 0, 1));
            autoMenu.addItem (3, utf8 ("主输出静音"), true,
                              automation.contains (AutomationOwner::master, 0, 2));
        }
        else if (slotId >= 0)
        {
            addSlotAutomationItems (autoMenu, slotId, 1);
        }
        else
        {
            // Phase 18 / item 10 said the same thing as a greyed pair of items.
            // A submenu with nothing in it says it once, and says why.
            autoMenu.addItem (1, utf8 ("这个通道还没有音源"), false, false);
        }

        menu.addSubMenu (utf8 ("创建自动化"), autoMenu);
    }

    menu.addSeparator();
    menu.addItem (20, utf8 ("重置通道条"), true);

    if (! isMaster)
    {
        // Phase 26: ANY strip can go, not just the top one. `removeChannelAt`
        // closes the gap and renumbers, and `deleteStrip` unplugs the synths
        // feeding this strip before that happens — the pair is what makes "the
        // strip is gone" true for every reference at once.
        //
        // Greyed (not hidden) at the floor: eight strips is the shipped console,
        // and a delete that cannot run should say so where it lives.
        menu.addItem (21, utf8 ("删除通道条"), mixer.canRemoveChannel());
        menu.addItem (22, utf8 ("增加通道条"), count < MixerModel::maxChannels);
    }
}

void MainComponent::applyChannelChoice (int result, int channel)
{
    const bool isMaster = channel == MixerModel::masterChannel;
    const int  slotId   = isMaster ? -1 : mixerPanel.slotIdOnChannel (channel);

    if (isMaster)
    {
        // Phase 43: 1..3 = 主音量 / 主声相 / 主静音, i.e. lane index = result - 1.
        if (result >= 1 && result <= MixerModel::numMasterParameters)
        {
            ensureAutomationTrack (AutomationOwner::master, 0, result - 1);
            return;
        }
    }
    else if (slotId >= 0 && applySlotAutomationChoice (result, slotId, 1))
    {
        return;
    }

    if (result == 20)
    {
        mixerPanel.resetStrip (channel);
        repaint();

        return;
    }

    if (result == 21)
    {
        // Phase 26: this channel, not the last one. The panel keeps the
        // selection pointing at a live strip, so nothing here has to.
        mixerPanel.deleteStrip (channel);

        repaint();

        return;
    }

    if (result == 22)
    {
        mixer.addChannel();
        repaint();
    }
}

//==============================================================================
//  Phase 43: one control, one automation line
//==============================================================================
namespace
{
    /** The id the host's single automation item uses inside a control's menu.
        Nothing else in that menu is above `MixerPanel::stripMenuActionId`. */
    constexpr int kStripAutoItemId = 100;
}

void MainComponent::fillStripAutomationItems (juce::PopupMenu& menu, int channel,
                                              MixerPanel::StripControl control)
{
    const bool isMaster = channel == MixerModel::masterChannel;

    if (isMaster)
    {
        // The master strip has three lanes and no solo: it feeds the device, so
        // "solo the output" is not a thing you can mean.
        int lane = -1;
        juce::String text;

        if (control == MixerPanel::StripControl::fader)     { lane = 0; text = utf8 ("创建主输出音量自动化"); }
        else if (control == MixerPanel::StripControl::pan)  { lane = 1; text = utf8 ("创建主输出声相自动化"); }
        else if (control == MixerPanel::StripControl::mute) { lane = 2; text = utf8 ("创建静音自动化"); }

        if (lane < 0)
            return;

        menu.addItem (kStripAutoItemId, text, true,
                      automation.contains (AutomationOwner::master, 0, lane));
        return;
    }

    const int slotId = mixerPanel.slotIdOnChannel (channel);

    if (slotId < 0)
    {
        // A strip with nothing routed into it has no curve to point at, and the
        // line says why instead of vanishing.
        menu.addItem (kStripAutoItemId, utf8 ("这个通道还没有音源"), false, false);
        return;
    }

    SlotAutomationTarget target = SlotAutomationTarget::level;
    juce::String         text   = utf8 ("创建音量自动化");

    switch (control)
    {
        case MixerPanel::StripControl::fader: target = SlotAutomationTarget::level; break;
        case MixerPanel::StripControl::pan:   target = SlotAutomationTarget::pan;
                                              text   = utf8 ("创建声相自动化"); break;
        case MixerPanel::StripControl::mute:  target = SlotAutomationTarget::mute;
                                              text   = utf8 ("创建静音自动化"); break;
        case MixerPanel::StripControl::solo:  target = SlotAutomationTarget::solo;
                                              text   = utf8 ("创建独奏自动化"); break;
    }

    menu.addItem (kStripAutoItemId, text, true,
                  automation.contains (AutomationOwner::slot, slotId, (int) target));
}

void MainComponent::applyStripAutomationChoice (int channel, MixerPanel::StripControl control,
                                                int result)
{
    if (result != kStripAutoItemId)
        return;

    if (channel == MixerModel::masterChannel)
    {
        const int lane = control == MixerPanel::StripControl::fader ? 0
                       : control == MixerPanel::StripControl::pan   ? 1
                       : control == MixerPanel::StripControl::mute  ? 2
                                                                    : -1;

        if (lane >= 0)
            ensureAutomationTrack (AutomationOwner::master, 0, lane);

        return;
    }

    const int slotId = mixerPanel.slotIdOnChannel (channel);

    if (slotId < 0)
        return;

    const int target = control == MixerPanel::StripControl::fader ? (int) SlotAutomationTarget::level
                     : control == MixerPanel::StripControl::pan   ? (int) SlotAutomationTarget::pan
                     : control == MixerPanel::StripControl::mute  ? (int) SlotAutomationTarget::mute
                                                                  : (int) SlotAutomationTarget::solo;

    ensureAutomationTrack (AutomationOwner::slot, slotId, target);
}

void MainComponent::showChannelMenu (int channel, juce::Point<int> screenPos)
{
    juce::PopupMenu menu;
    fillChannelMenu (menu, channel);

    juce::Component::SafePointer<MainComponent> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetScreenArea (juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1))
                            .withMinimumWidth (ui::px (170)),
                        [safe, channel] (int result)
                        {
                            if (safe == nullptr || result <= 0)
                                return;

                            safe->applyChannelChoice (result, channel);
                        });
}

void MainComponent::fillAutomationMenu (juce::PopupMenu& menu, AutomationOwner owner, int ownerId)
{
    bool expanded = false;
    juce::String ownerName;

    if (owner == AutomationOwner::master)
    {
        ownerName = utf8 ("主输出");
        expanded  = mixer.automationFor (0).isEnabled();
    }
    else if (owner == AutomationOwner::layer)
    {
        if (auto* layer = stack.findLayerById (ownerId))
        {
            expanded  = layer->isAutomationExpanded();
            ownerName = layer->getName();
        }
    }
    else if (owner == AutomationOwner::group)
    {
        for (int i = 0; i < stack.getNumLayers(); ++i)
            if (auto* layer = stack.getLayer (i))
                if (auto* group = layer->findGroup (ownerId))
                {
                    ownerName = group->getName();
                    expanded  = group->getLevelAutomation().isEnabled();
                    break;
                }
    }
    else if (auto* slot = rack.findSlot (ownerId))
    {
        expanded  = slot->automationExpanded;
        ownerName = slot->getDisplayName();
    }

    // Phase 14: no more "自动化" dropdown badges anywhere. The only way to
    // create a track is a context menu on the control itself, and the item
    // spells out what it will create. The tick means "this curve already has a
    // row", so the menu doubles as the place to find an existing curve.
    //
    // Phase 21: this is now also a SUB-MENU of the numeric control's value
    // menu, so it is built into a caller's menu instead of showing itself.

    menu.addSectionHeader (ownerName + (owner == AutomationOwner::layer ? utf8 (" · 图层自动化")
                                        : owner == AutomationOwner::group ? utf8 (" · 编组自动化")
                                        : owner == AutomationOwner::master ? utf8 (" · 主输出自动化")
                                                                          : utf8 (" · 音源自动化")));

    juce::PopupMenu autoMenu;

    if (owner == AutomationOwner::group)
    {
        // Phase 20: a group has two automatable parameters now, so it is listed
        // exactly like a layer. (Phase 16 said one, and said so in this menu.)
        autoMenu.addItem (1, utf8 ("组音量"), true,
                          automation.contains (AutomationOwner::group, ownerId,
                                               (int) GroupAutomationTarget::level));
        autoMenu.addItem (2, utf8 ("组声相"), true,
                          automation.contains (AutomationOwner::group, ownerId,
                                               (int) GroupAutomationTarget::pan));
    }
    else if (owner == AutomationOwner::slot)
    {
        // Phase 42: a synth's menu is the same one the rack card builds, ids and
        // all — 滤波器 / 音量 / 音源参数 ▶.
        addSlotAutomationItems (autoMenu, ownerId, 1);
    }
    else
    {
        const bool isMaster = owner == AutomationOwner::master;

        autoMenu.addItem (1, isMaster ? utf8 ("主音量") : utf8 ("音量"), true,
                          automation.contains (owner, ownerId, 0));
        autoMenu.addItem (2, isMaster ? utf8 ("主声相") : utf8 ("声相"), true,
                          automation.contains (owner, ownerId, 1));

        // Phase 43: the master's third lane. A layer has no mute lane of its own
        // (its mute is the layer panel's switch), so this item exists only here.
        if (isMaster)
            autoMenu.addItem (3, utf8 ("主静音"), true,
                              automation.contains (owner, ownerId, 2));
    }

    // Phase 42: the verb is the menu, the parameters are inside it.
    menu.addSubMenu (utf8 ("创建自动化"), autoMenu);

    // Only an owner that HAS an inline lane in a panel can be expanded there.
    // The master bus has no面板 row, so its two verbs are the two parameters.
    if (owner == AutomationOwner::layer || owner == AutomationOwner::slot)
    {
        menu.addSeparator();
        menu.addItem (3, expanded ? utf8 ("收起面板中的曲线") : utf8 ("在面板中展开曲线"), true);
    }
}

void MainComponent::showAutomationMenu (AutomationOwner owner, int ownerId, juce::Point<int> screenPos)
{
    // The master bus is the one owner whose id is legitimately 0.
    if (owner != AutomationOwner::master && ownerId <= 0)
        return;

    juce::PopupMenu menu;
    fillAutomationMenu (menu, owner, ownerId);

    juce::Component::SafePointer<MainComponent> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetScreenArea (juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1)),
                        [safe, owner, ownerId] (int result)
                        {
                            if (safe == nullptr || result <= 0)
                                return;

                            safe->applyAutomationChoice (result, owner, ownerId);
                        });
}

void MainComponent::applyAutomationChoice (int result, AutomationOwner owner, int ownerId)
{
    // Phase 43: id 3 means "展开/收起曲线" only for an owner that HAS a panel row.
    // The master just gained a third PARAMETER with the same id — it has no
    // in-panel curve, so 3 there is 主静音 and must fall through to the lane.
    if (result == 3
        && (owner == AutomationOwner::layer || owner == AutomationOwner::slot))
    {
        // Re-read the state instead of trusting a copy taken when the menu was
        // built: the same curve can be toggled from three places, and the menu
        // may now be reached through a hover sub-menu that stayed open longer.
        if (owner == AutomationOwner::layer)
        {
            const int index = stack.indexOfLayerId (ownerId);

            if (index >= 0)
            {
                const bool expanded = stack.getLayer (index) != nullptr
                                        && stack.getLayer (index)->isAutomationExpanded();
                stack.setLayerAutomationExpanded (index, ! expanded);
            }
        }
        else if (owner == AutomationOwner::slot)
        {
            if (auto* slot = rack.findSlot (ownerId))
                rack.setAutomationExpanded (ownerId, ! slot->automationExpanded);
        }

        return;
    }

    // Everything else is a parameter choice: lane on, track added, row revealed.
    //
    // Phase 42: a synth's submenu hands out the same ids the rack card does
    // (1 = 滤波器, 2 = 音量, 101 + index = a plugin parameter), so the same
    // reader decides what the click meant.
    if (owner == AutomationOwner::slot && applySlotAutomationChoice (result, ownerId, 1))
        return;

    ensureAutomationTrack (owner, ownerId, result - 1);
}

void MainComponent::ensureAutomationTrack (AutomationOwner owner, int ownerId, int target)
{
    if (owner == AutomationOwner::layer)
    {
        auto* layer = stack.findLayerById (ownerId);

        if (layer == nullptr)
            return;

        const auto lane = (LayerAutomationTarget) juce::jlimit (0, 1, target);

        // A row for a curve that is switched off looks broken in the window, so
        // asking for automation also turns the lane on.
        layer->automationFor (lane).setEnabled (true);

        const int index = stack.indexOfLayerId (ownerId);

        if (index >= 0)
        {
            stack.setLayerAutomationTarget (index, lane);
            stack.setLayerAutomationExpanded (index, true);
        }
    }
    else if (owner == AutomationOwner::group)
    {
        NoteGroup* group = nullptr;

        for (int i = 0; i < stack.getNumLayers() && group == nullptr; ++i)
            if (auto* layer = stack.getLayer (i))
                group = layer->findGroup (ownerId);

        if (group == nullptr)
            return;

        const auto lane = (GroupAutomationTarget) juce::jlimit (0, 1, target);

        group->automationFor (lane).setEnabled (true);

        // The group list draws a curve badge per lane, so it has to redraw and
        // the engine has to re-publish: turning a lane on changes what the next
        // block sounds like.
        stack.notifyNotesChanged();
        groupPanel.repaint();
        republish();
    }
    else if (owner == AutomationOwner::master)
    {
        // No panel lane to expand: the master strip has no inline curve row, so
        // "create automation" here means exactly "turn the lane on".
        mixer.automationFor (juce::jlimit (0, MixerModel::numMasterParameters - 1, target))
             .setEnabled (true);

        mixer.notify();
    }
    else
    {
        auto* slot = rack.findSlot (ownerId);

        if (slot == nullptr)
            return;

        auto lane = (SlotAutomationTarget) juce::jlimit (0, numSlotAutomationTargets - 1,
                                                         target);

        // Phase 42: a `plugin` lane with no parameter chosen is a row that
        // renders nothing. Take the instrument's first parameter — the one the
        // plugin itself calls its first — so the row means something the moment
        // it exists, and fall back to the filter when there are none.
        if (lane == SlotAutomationTarget::plugin && ! slot->drivesPluginParam())
        {
            const auto params = pluginParamsForSlot (ownerId);

            if (! params.empty())
                rack.setPluginParam (ownerId, params.front().index);
            else
                lane = SlotAutomationTarget::filter;

            slot = rack.findSlot (ownerId);

            if (slot == nullptr)
                return;
        }

        slot->automationFor (lane).setEnabled (true);
        rack.setAutomationTarget (ownerId, lane);
        rack.setAutomationExpanded (ownerId, true);
    }

    const int trackId = automation.addTrack (owner, ownerId, target);

    // A row created below the fold is indistinguishable from no row at all.
    if (trackId > 0)
        automationWindow.revealTrack (trackId);

    republish();
    automationWindow.repaint();
    layerPanel.repaint();
    rackPanel.repaint();
    pianoRoll.repaint();
}

void MainComponent::ensureRackAutomationTrack (int slotId)
{
    auto* slot = rack.findSlot (slotId);

    if (slot == nullptr)
        return;

    ensureAutomationTrack (AutomationOwner::slot, slotId, (int) slot->automationTarget);
}

//==============================================================================
//  Phase 42: automating a hosted instrument's own parameters
//==============================================================================
std::vector<PluginParamInfo> MainComponent::pluginParamsForSlot (int slotId)
{
    std::vector<PluginParamInfo> out;

    const auto* targetSlot = rack.findSlot (slotId);

    if (targetSlot == nullptr || targetSlot->pluginId <= 0)
        return out;

    // The message thread only: `getProcessorForEditor` hands out the live
    // instance, and this is only ever called from a menu build or a paint.
    auto* instance = plugins.getProcessorForEditor (targetSlot->pluginId);

    if (instance == nullptr)
        return out;

    const auto& parameters = instance->getParameters();

    for (int i = 0; i < parameters.size(); ++i)
    {
        auto* parameter = parameters[i];

        if (parameter == nullptr)
            continue;

        PluginParamInfo info;
        info.index        = i;
        info.name         = parameter->getName (64);
        info.defaultValue = parameter->getDefaultValue();

        if (info.name.isEmpty())
            info.name = utf8 ("参数 ") + juce::String (i + 1);

        out.push_back (std::move (info));
    }

    return out;
}

void MainComponent::ensureSlotPluginParamTrack (int slotId, int paramIndex)
{
    auto* slot = rack.findSlot (slotId);

    if (slot == nullptr || paramIndex < 0)
        return;

    // Parameter first: the lane is only meaningful with an address, and a track
    // pointing at parameter -1 would render nothing while looking finished.
    rack.setPluginParam (slotId, paramIndex);
    slot = rack.findSlot (slotId);

    if (slot == nullptr)
        return;

    auto& lane = slot->getPluginAutomation();

    // A brand-new lane starts flat at 1.0 — "the parameter's full value" — which
    // would jump the knob the moment the curve is created. Starting at the value
    // the parameter actually has means creating automation changes nothing until
    // the user draws something.
    if (! lane.isEnabled() || lane.getPoints().size() <= 2)
    {
        float current = 1.0f;

        if (auto* instance = plugins.getProcessorForEditor (slot->pluginId))
            if (juce::isPositiveAndBelow (paramIndex, instance->getParameters().size()))
                if (auto* parameter = instance->getParameters()[paramIndex])
                    current = parameter->getValue();

        lane.resetFlat (current, juce::jmax (1.0, audio.getPlayback().getLengthBeats()));
    }

    lane.setEnabled (true);

    rack.setAutomationTarget (slotId, SlotAutomationTarget::plugin);
    rack.setAutomationExpanded (slotId, true);

    const int trackId = automation.addTrack (AutomationOwner::slot, slotId,
                                             (int) SlotAutomationTarget::plugin);

    if (trackId > 0)
        automationWindow.revealTrack (trackId);

    republish();
    automationWindow.repaint();
    rackPanel.repaint();
}

void MainComponent::addSlotAutomationItems (juce::PopupMenu& menu, int slotId, int idBase)
{
    auto* slot = rack.findSlot (slotId);

    if (slot == nullptr)
        return;

    menu.addItem (idBase, utf8 ("滤波器"), ! slot->isSampler(),
                  automation.contains (AutomationOwner::slot, slotId,
                                       (int) SlotAutomationTarget::filter));
    menu.addItem (idBase + 1, utf8 ("音量"), true,
                  automation.contains (AutomationOwner::slot, slotId,
                                       (int) SlotAutomationTarget::level));
    // Phase 43: 声相 / 静音 / 独奏, in their own id band — `idBase + 3` would be
    // 33 on a rack card, and 33/34/35 are already spoken for there (see
    // kSlotAutoTargetBase). `+ 0..2` maps to pan/mute/solo.
    menu.addItem (idBase + kSlotAutoTargetBase, utf8 ("声相"), true,
                  automation.contains (AutomationOwner::slot, slotId,
                                       (int) SlotAutomationTarget::pan));
    menu.addItem (idBase + kSlotAutoTargetBase + 1, utf8 ("静音"), true,
                  automation.contains (AutomationOwner::slot, slotId,
                                       (int) SlotAutomationTarget::mute));
    menu.addItem (idBase + kSlotAutoTargetBase + 2, utf8 ("独奏"), true,
                  automation.contains (AutomationOwner::slot, slotId,
                                       (int) SlotAutomationTarget::solo));

    const auto params = pluginParamsForSlot (slotId);

    // Same ceiling the roll's parameter boxes use: a plugin with a thousand
    // parameters is a plugin whose menu cannot be read, and the tail is said out
    // loud rather than silently dropped.
    constexpr int maxListed = 64;

    juce::PopupMenu paramMenu;

    if (params.empty())
    {
        paramMenu.addItem (idBase + 2, slot->isPlugin() ? utf8 ("插件未载入，没有可自动化的参数")
                                                        : utf8 ("这个音源没有插件参数"),
                           false, false);
    }
    else
    {
        for (int i = 0; i < (int) params.size() && i < maxListed; ++i)
        {
            const auto& info = params[(size_t) i];

            paramMenu.addItem (idBase + kSlotAutoParamBase + info.index, info.name, true,
                               slot->drivesPluginParam()
                                 && slot->getPluginParam() == info.index
                                 && automation.contains (AutomationOwner::slot, slotId,
                                                         (int) SlotAutomationTarget::plugin));
        }

        // The "there are more" line sits one below where the parameter ids start:
        // a number in a band nothing else can be, because the obvious
        // `idBase + 3` is 声相 now and 34 on a card is the spotlight.
        if ((int) params.size() > maxListed)
            paramMenu.addItem (idBase + kSlotAutoParamBase - 1,
                               utf8 ("还有 ") + juce::String ((int) params.size() - maxListed)
                                                 + utf8 (" 个参数没有列出"), false, false);
    }

    menu.addSubMenu (utf8 ("音源参数"), paramMenu);
}

bool MainComponent::applySlotAutomationChoice (int result, int slotId, int idBase)
{
    // Phase 43: the higher band first. 20030 is also ">= 10030", so testing the
    // parameter band first would read 声相 as parameter 10030 and quietly pick a
    // plugin parameter that does not exist.
    if (result >= idBase + kSlotAutoTargetBase)
    {
        switch (result - idBase - kSlotAutoTargetBase)
        {
            case 0: ensureAutomationTrack (AutomationOwner::slot, slotId,
                                           (int) SlotAutomationTarget::pan);  return true;
            case 1: ensureAutomationTrack (AutomationOwner::slot, slotId,
                                           (int) SlotAutomationTarget::mute); return true;
            case 2: ensureAutomationTrack (AutomationOwner::slot, slotId,
                                           (int) SlotAutomationTarget::solo); return true;
            default: return true;      // inside our band, so it was ours
        }
    }

    if (result >= idBase + kSlotAutoParamBase)
    {
        ensureSlotPluginParamTrack (slotId, result - idBase - kSlotAutoParamBase);
        return true;
    }

    if (result == idBase)
    {
        ensureAutomationTrack (AutomationOwner::slot, slotId,
                               (int) SlotAutomationTarget::filter);
        return true;
    }

    if (result == idBase + 1)
    {
        ensureAutomationTrack (AutomationOwner::slot, slotId,
                               (int) SlotAutomationTarget::level);
        return true;
    }

    return false;
}

//==============================================================================
juce::Rectangle<int> MainComponent::getPlayButtonBounds() const
{
    return { ui::px (188), topBarBounds.getY() + ui::px (12), ui::px (34), ui::px (28) };
}

juce::Rectangle<int> MainComponent::getStopButtonBounds() const
{
    // Phase 32: the pause plate's slot. Two buttons, side by side, 4 px apart —
    // the same rhythm the pair had when there were three.
    return { ui::px (226), topBarBounds.getY() + ui::px (12), ui::px (30), ui::px (28) };
}

juce::Rectangle<int> MainComponent::getPositionReadoutBounds() const
{
    return { ui::px (300), topBarBounds.getY() + ui::px (5), ui::px (84), ui::px (42) };
}

juce::Rectangle<int> MainComponent::getBpmFieldBounds() const
{
    return { ui::px (392), topBarBounds.getY() + ui::px (12), ui::px (84), ui::px (28) };
}

juce::Rectangle<int> MainComponent::getMasterMeterBounds() const
{
    return { getWidth() - ui::px (196), topBarBounds.getY() + ui::px (14),
             ui::px (176), ui::px (24) };
}

//-- Phase 6: the file cluster -----------------------------------------------
// Phase 25: 保存 / 载入 / 导出 moved out of the right edge and into the edit
// cluster, between 新建 and 撤销. They were parked against the master meter,
// which is the far corner of the window from the project they belong to; a file
// verb now sits next to the other project verb instead of across the screen.
juce::Rectangle<int> MainComponent::getFileClusterBounds() const
{
    // Union of the three, derived from the first one so there is a single place
    // that decides where the cluster starts.
    return getSaveButtonBounds().getUnion (getExportButtonBounds());
}

juce::Rectangle<int> MainComponent::getSaveButtonBounds() const
{
    // A section break after 新建: history and files are two different jobs even
    // though they now share a row.
    return { getNewButtonBounds().getRight() + ui::px (10), topBarBounds.getY() + ui::px (12),
             ui::px (58), ui::px (28) };
}

juce::Rectangle<int> MainComponent::getLoadButtonBounds() const
{
    return getSaveButtonBounds().withX (getSaveButtonBounds().getRight() + ui::px (6));
}

juce::Rectangle<int> MainComponent::getExportButtonBounds() const
{
    return getLoadButtonBounds().withX (getLoadButtonBounds().getRight() + ui::px (6));
}

juce::Rectangle<int> MainComponent::getProjectNameBounds() const
{
    // Still right-aligned against the master meter: the project's name is a
    // status readout, not a control, so it stays where it was when the file
    // buttons left this corner.
    const juce::Rectangle<int> nameArea { getWidth() - ui::px (558), topBarBounds.getY() + ui::px (14),
                                          ui::px (156), ui::px (24) };

    // Only when it cannot possibly overlap the device status text.
    return getWidth() >= ui::px (1360) ? nameArea : juce::Rectangle<int>();
}

//-- Phase 8: the edit cluster ------------------------------------------------
// New / save / load / export / undo / redo / mixer / settings sit between the
// tempo field and the device status. Order: the project action first, then the
// three file verbs (Phase 25 moved them here), then history, then the console,
// then settings.
juce::Rectangle<int> MainComponent::getNewButtonBounds() const
{
    // Phase 88 moved it 36 px right: the metronome took the slot between the
    // tempo field and the project verb, which is where it belongs musically and
    // where there was a gap that only looked like room for nothing.
    return { getMetronomeButtonBounds().getRight() + ui::px (10),
             topBarBounds.getY() + ui::px (12), ui::px (54), ui::px (28) };
}

juce::Rectangle<int> MainComponent::getMetronomeButtonBounds() const
{
    // Right of the tempo field, left of 新建. Same plate height as every other
    // button on the row; narrow, because the glyph does the talking.
    return { getBpmFieldBounds().getRight() + ui::px (6), topBarBounds.getY() + ui::px (12),
             ui::px (34), ui::px (28) };
}

juce::Rectangle<int> MainComponent::getUndoButtonBounds() const
{
    // Phase 25: an icon, not the word. Two glyphs in a 32 px plate, which also
    // wins back the room the three file buttons needed.
    return { getExportButtonBounds().getRight() + ui::px (10), topBarBounds.getY() + ui::px (12),
             ui::px (32), ui::px (28) };
}

juce::Rectangle<int> MainComponent::getRedoButtonBounds() const
{
    return { getUndoButtonBounds().getRight() + ui::px (4), topBarBounds.getY() + ui::px (12),
             ui::px (32), ui::px (28) };
}

juce::Rectangle<int> MainComponent::getMixerButtonBounds() const
{
    return { getRedoButtonBounds().getRight() + ui::px (10), topBarBounds.getY() + ui::px (12),
             ui::px (76), ui::px (28) };
}

juce::Rectangle<int> MainComponent::getSettingsButtonBounds() const
{
    return { getMixerButtonBounds().getRight() + ui::px (6), topBarBounds.getY() + ui::px (12),
             ui::px (30), ui::px (28) };
}

juce::String MainComponent::formatBarBeat() const
{
    const double beats = audio.getDisplayPositionBeats();

    const int bar  = (int) std::floor (beats / 4.0) + 1;
    const int beat = (int) std::floor (std::fmod (beats, 4.0)) + 1;
    const int tick = (int) std::floor (std::fmod (beats, 1.0) * 4.0) + 1;

    return juce::String (bar).paddedLeft ('0', 2) + "."
             + juce::String (beat) + "." + juce::String (tick);
}

juce::String MainComponent::formatTime() const
{
    const double seconds = audio.getDisplayPositionBeats() * 60.0 / audio.getBpm();
    const int    minutes = (int) (seconds / 60.0);

    return juce::String (minutes) + ":"
             + juce::String (seconds - (double) minutes * 60.0, 1).paddedLeft ('0', 4);
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (kAppBackground);

    //-- Phase 22: menu bar ---------------------------------------------------
    // Darker than the transport bar below it, so the two strips read as
    // "commands" and "transport" instead of one thick header.
    {
        g.setColour (theme::bgApp);
        g.fillRect (menuBarBounds);

        for (int i = 0; i < numMenus; ++i)
        {
            const auto item = getMenuBarItemBounds (i).toFloat();
            const bool hot  = hoveredMenuItem == i;

            if (hot)
            {
                g.setColour (juce::Colours::white.withAlpha (0.10f));
                g.fillRoundedRectangle (item, ui::pf (4));
            }

            g.setColour ((hot ? juce::Colours::white : kAccent).withAlpha (hot ? 0.98f : 0.62f));
            g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.5f))));
            g.drawText (menuTitle (i), item, juce::Justification::centred, false);
        }

        g.setColour (kDivider);
        g.drawHorizontalLine (menuBarBounds.getBottom() - 1,
                              (float) menuBarBounds.getX(), (float) menuBarBounds.getRight());
    }

    //-- Top bar --------------------------------------------------------------
    g.setColour (kTopBar);
    g.fillRect (topBarBounds);

    g.setColour (juce::Colours::white.withAlpha (0.95f));
    g.setFont (boldFont (17.0f));
    g.drawText ("AtomicDAW", topBarBounds.getX() + ui::px (18), topBarBounds.getY(),
                ui::px (160), topBarBounds.getHeight(),
                juce::Justification::centredLeft, false);

    //-- Transport ------------------------------------------------------------
    {
        const bool playing = audio.isPlaying();
        const bool paused  = audio.isPaused();

        // One look for two buttons, so they read as a group.
        auto drawButtonPlate = [&g] (juce::Rectangle<float> button, bool hovered,
                                     bool enabled, juce::Colour tint, bool filled)
        {
            g.setColour (tint.withAlpha (filled ? 0.22f : (hovered && enabled ? 0.12f : 0.055f)));
            g.fillRoundedRectangle (button, ui::pf (6));

            g.setColour (tint.withAlpha (filled ? 0.90f : (enabled ? (hovered ? 0.55f : 0.30f) : 0.16f)));
            g.drawRoundedRectangle (button, ui::pf (6), 1.0f);
        };

        //-- Play --------------------------------------------------------------
        {
            auto button = getPlayButtonBounds().toFloat();

            drawButtonPlate (button, hoveringPlay, true, kAccent, playing);

            g.setColour (kAccent.withAlpha (playing ? 1.0f : 0.92f));
            g.fillPath (playGlyph (button));
        }

        //-- Stop --------------------------------------------------------------
        {
            auto button = getStopButtonBounds().toFloat();

            drawButtonPlate (button, hoveringStop, playing || paused, juce::Colours::white, false);

            g.setColour (juce::Colours::white.withAlpha (playing || paused ? 0.72f : 0.45f));
            g.fillRect (stopGlyph (button));
        }

        //-- Record (Phase 81) -------------------------------------------------
        // Theme.h keeps a red that means "recording", separate from the accent
        // the play button and the playhead use. Until Phase 81 nothing was ever
        // red for that reason; the dot is the first thing that is.
        {
            auto button = getRecordButtonBounds().toFloat();

            drawButtonPlate (button, hoveringRecord, true, theme::record, recording.load());

            auto dot = juce::Rectangle<float> (button.getCentreX() - ui::pf (5.0f),
                                               button.getCentreY() - ui::pf (5.0f),
                                               ui::pf (10.0f), ui::pf (10.0f));

            g.setColour (theme::record.withAlpha (recording.load() ? 1.0f : (hoveringRecord ? 0.85f : 0.62f)));
            g.fillEllipse (dot);
        }

        //-- Position ----------------------------------------------------------
        {
            auto readout = getPositionReadoutBounds();

            // Phase 50 (DESIGN.md §5): both readouts are tabular. The bar/beat
            // pair changes at every beat and the timecode at every second, and
            // with proportional digits the whole block shivers sideways each
            // time a 1 becomes a 0.
            g.setColour (juce::Colours::white.withAlpha (0.92f));
            ui::drawTabular (g, formatBarBeat(),
                             readout.withTrimmedBottom (ui::px (21)).toFloat(),
                             ui::title (14.0f), juce::Justification::centredLeft);

            g.setColour (juce::Colours::white.withAlpha (0.32f));
            ui::drawTabular (g, formatTime(),
                             readout.withTrimmedTop (ui::px (25)).toFloat(),
                             ui::font (9.5f), juce::Justification::centredLeft);

            // Phase 88: during a count-in the readout says what it is waiting
            // for. The position is deliberately NOT moving (that is what a
            // count-in is), so a frozen bar number there reads as a stalled
            // transport rather than as four, three, two, one.
            if (const int countingDown = getCountInBeatsLeft(); countingDown > 0)
            {
                g.setColour (kTopBar);
                g.fillRect (readout);

                g.setColour (theme::record.withAlpha (0.95f));
                g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (11.0f))));
                g.drawText (utf8 ("预备 ") + juce::String (countingDown), readout,
                            juce::Justification::centred, false);
            }
        }

        //-- Metronome (Phase 88) ---------------------------------------------
        {
            auto button = getMetronomeButtonBounds().toFloat();

            drawButtonPlate (button, hoveringMetronome, true, kAccent, metronomeEnabled);

            // A metronome seen from the front: a body that tapers upward and an
            // arm leaning right. Drawn in strokes like every other glyph on this
            // row — a filled silhouette would be the only solid shape among
            // them.
            g.setColour (kAccent.withAlpha (metronomeEnabled ? 0.95f
                                               : (hoveringMetronome ? 0.72f : 0.42f)));

            const float centreX  = button.getCentreX();
            const float topY     = button.getY() + ui::pf (5.5f);
            const float bottomY  = button.getBottom() - ui::pf (5.5f);
            const float halfBase = ui::pf (6.4f);
            const float halfTop  = ui::pf (1.8f);

            juce::Path body;
            body.startNewSubPath (centreX - halfBase, bottomY);
            body.lineTo (centreX - halfTop, topY);
            body.lineTo (centreX + halfTop, topY);
            body.lineTo (centreX + halfBase, bottomY);
            body.closeSubPath();
            g.strokePath (body, juce::PathStrokeType (ui::pf (1.2f)));

            // The arm, with its counterweight. The weight is the detail that
            // makes this read as a metronome rather than as a lambda: without
            // it, a tapered outline crossed by a diagonal is just a letter.
            const juce::Point<float> armEnd (centreX + ui::pf (2.7f), topY + ui::pf (5.0f));

            juce::Path arm;
            arm.startNewSubPath (centreX, bottomY - ui::pf (1.5f));
            arm.lineTo (armEnd);
            g.strokePath (arm, juce::PathStrokeType (ui::pf (1.2f)));

            g.fillEllipse (juce::Rectangle<float> (armEnd.x - ui::pf (1.6f),
                                                  armEnd.y - ui::pf (1.6f),
                                                  ui::pf (3.2f), ui::pf (3.2f)));
        }

        //-- Phase 8: new / undo / redo / mixer / settings ---------------------
        // The phase chip that used to sit here is gone: five working buttons are
        // worth more than a label. The phase itself moved into the project name
        // slot on the right, where it costs nothing.
        {
            auto drawPlate = [&g] (juce::Rectangle<float> r, bool hovered, bool enabled,
                                   juce::Colour tint, bool filled)
            {
                g.setColour (tint.withAlpha (filled ? 0.22f
                                                    : (hovered && enabled ? 0.16f : 0.06f)));
                g.fillRoundedRectangle (r, ui::pf (6));

                g.setColour (tint.withAlpha (filled ? 0.85f : (enabled ? (hovered ? 0.6f : 0.22f)
                                                                      : 0.10f)));
                g.drawRoundedRectangle (r, ui::pf (6), 1.0f);
            };

            auto drawLabelButton = [&] (juce::Rectangle<int> bounds, const juce::String& label,
                                        bool hovered, bool accent, bool filled, bool enabled = true)
            {
                const auto r    = bounds.toFloat();
                const auto tint = accent ? kAccent : juce::Colours::white;

                drawPlate (r, hovered, enabled, tint, filled);

                g.setColour (juce::Colours::white.withAlpha (enabled ? (hovered ? 0.95f : 0.72f) : 0.22f));
                g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.5f))));
                g.drawText (label, bounds, juce::Justification::centred, false);
            };

            drawLabelButton (getNewButtonBounds(), utf8 ("新建"), hoveringNew, false, false);

            //-- Files (Phase 25: moved here, between 新建 and 撤销) -----------
            {
                auto drawFileButton = [&] (juce::Rectangle<int> bounds, const juce::String& label,
                                           bool hovered, bool busy)
                {
                    const auto r = bounds.toFloat();

                    drawPlate (r, hovered, true, juce::Colours::white, false);

                    g.setColour (juce::Colours::white.withAlpha (busy ? 0.45f : (hovered ? 0.95f : 0.72f)));
                    g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.5f))));
                    g.drawText (label, bounds, juce::Justification::centred, false);
                };

                drawFileButton (getSaveButtonBounds(), utf8 ("保存"), hoveringSave, false);
                drawFileButton (getLoadButtonBounds(), utf8 ("载入"), hoveringLoad, false);
                drawFileButton (getExportButtonBounds(), exporting ? utf8 ("导出中") : utf8 ("导出"),
                                hoveringExport, exporting);
            }

            //-- Undo / redo (Phase 25: icons) --------------------------------
            // The words were unambiguous but wide, and the file verbs needed the
            // room. A curved arrow with the head on the side it returns to is
            // read correctly even by people who read the plain circular glyph
            // as "refresh": the direction is in the arrowhead, not in the loop.
            {
                auto drawHistoryButton = [&] (juce::Rectangle<int> bounds, bool redo,
                                              bool hovered, bool enabled)
                {
                    drawPlate (bounds.toFloat(), hovered, enabled, juce::Colours::white, false);

                    const auto r  = bounds.toFloat();
                    const auto c  = r.getCentre();
                    const float rad = ui::pf (5.2f);
                    const float head = ui::pf (3.4f);

                    const juce::Colour ink = juce::Colours::white.withAlpha (enabled ? (hovered ? 0.98f : 0.80f)
                                                                                    : 0.22f);
                    g.setColour (ink);

                    juce::Path arrow;

                    // The arc runs over the top from the tail side to the head
                    // side; JUCE measures the angle clockwise from 12 o'clock.
                    const float from = redo ? -juce::MathConstants<float>::halfPi
                                            : juce::MathConstants<float>::halfPi;
                    const float to   = -from;

                    arrow.addCentredArc (c.x, c.y, rad, rad, 0.0f, from, to, true);
                    g.strokePath (arrow, juce::PathStrokeType (ui::pf (1.5f),
                                                               juce::PathStrokeType::mitered,
                                                               juce::PathStrokeType::rounded));

                    // The head sits at the end of the arc, pointing the way the
                    // gesture goes: back for undo, on for redo.
                    const float sign = redo ? 1.0f : -1.0f;
                    const juce::Point<float> tip { c.x + sign * rad, c.y };

                    juce::Path cap;
                    cap.startNewSubPath (tip.x - sign * head, tip.y - head);
                    cap.lineTo (tip.x, tip.y);
                    cap.lineTo (tip.x - sign * head, tip.y + head);

                    g.strokePath (cap, juce::PathStrokeType (ui::pf (1.5f),
                                                             juce::PathStrokeType::mitered,
                                                             juce::PathStrokeType::rounded));
                };

                drawHistoryButton (getUndoButtonBounds(), false, hoveringUndo, canUndo());
                drawHistoryButton (getRedoButtonBounds(), true, hoveringRedo, canRedo());
            }

            //-- Mixer ---------------------------------------------------------
            drawLabelButton (getMixerButtonBounds(), utf8 ("混音台"),
                             hoveringMixer, true, isMixerWindowOpen());

            //-- Settings (gear) ----------------------------------------------
            {
                const auto r = getSettingsButtonBounds().toFloat();

                drawPlate (r, hoveringSettings, true, juce::Colours::white, false);

                const auto c = r.getCentre();
                const float outer = ui::pf (6.4);
                const float inner = ui::pf (4.0);

                juce::Path gear;

                for (int i = 0; i < 8; ++i)
                {
                    const float a0 = (float) i * juce::MathConstants<float>::twoPi / 8.0f;
                    const float a1 = a0 + juce::MathConstants<float>::twoPi / 16.0f;

                    gear.addPieSegment (c.x - outer, c.y - outer, outer * 2.0f, outer * 2.0f,
                                        a0, a1, 0.55f);
                }

                gear.addEllipse (c.x - inner, c.y - inner, inner * 2.0f, inner * 2.0f);

                g.setColour (juce::Colours::white.withAlpha (hoveringSettings ? 0.95f : 0.70f));
                g.fillPath (gear);

                // The hole in the middle: painted, not a hole, so the gear reads
                // against the dark bar.
                g.setColour (kTopBar);
                g.fillEllipse (c.x - ui::pf (1.9), c.y - ui::pf (1.9), ui::pf (3.8), ui::pf (3.8));
            }
        }

        // Audio device status
        {
            const bool running = audio.isRunning();
            const int x = getSettingsButtonBounds().getRight() + ui::px (16);

            g.setColour (running ? theme::live : kPlayhead.withAlpha (0.75f));
            g.fillEllipse ((float) x, (float) topBarBounds.getCentreY() - ui::pf (4),
                           ui::pf (8), ui::pf (8));

            // The project name comes next; only print the device name if there
            // is still room for it, otherwise the two would draw over each
            // other on a narrow window. Phase 25: the file buttons no longer
            // bound this from the left — they moved into the edit cluster — so
            // the limit is the name readout, or the window edge when it is
            // hidden.
            const int limitX = getProjectNameBounds().isEmpty()
                                 ? getWidth() - ui::px (8)
                                 : getProjectNameBounds().getX() - ui::px (8);
            const int available = limitX - (x + ui::px (18));

            if (available >= ui::px (90))
            {
                g.setColour (juce::Colours::white.withAlpha (0.45f));
                g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.0f))));
                g.drawText (running ? audio.getStatusText() : utf8 ("音频离线 · 仅界面"),
                            juce::Rectangle<int> (x + ui::px (16), topBarBounds.getY(),
                                                  juce::jmin (ui::px (190), available),
                                                  topBarBounds.getHeight()),
                            juce::Justification::centredLeft, true);
            }
        }
    }

    //-- Master output meter ---------------------------------------------------
    // The end of the chain the whole phase is about: what actually leaves the
    // mixer, after every fader and every automation curve.
    {
        auto area = getMasterMeterBounds();

        auto label = area.removeFromLeft (ui::px (46));
        g.setColour (juce::Colours::white.withAlpha (0.30f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (8.5f))));
        g.drawText ("MASTER", label.withTrimmedRight (ui::px (6)),
                    juce::Justification::centredRight, false);

        auto bars = area.reduced (0, ui::px (2)).toFloat();
        const auto& playback = audio.getPlayback();

        mixerlook::drawMeter (g, bars.removeFromTop (ui::pf (9)), playback.getMasterPeak (0), kAccent);
        mixerlook::drawMeter (g, bars.removeFromTop (ui::pf (9)), playback.getMasterPeak (1), kAccent);
    }

    //-- Project name ----------------------------------------------------------
    // Phase 25: the file buttons left this corner (they now live between 新建
    // and 撤销), so what remains here is the readout they used to anchor.
    if (! getProjectNameBounds().isEmpty())
    {
        const juce::String name = currentProject.existsAsFile()
                                    ? currentProject.getFileNameWithoutExtension()
                                    : utf8 ("未命名工程");

        g.setColour (dirtyCache ? theme::warn
                                : juce::Colours::white.withAlpha (0.34f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));
        g.drawText ((dirtyCache ? juce::String (utf8 ("● ")) : juce::String())
                      + name + utf8 ("  ·  P88"),
                    getProjectNameBounds(), juce::Justification::centredRight, true);
    }

    // The space the key hints used to occupy is now the project name: knowing
    // which file is open is more useful than a reminder of two shortcuts.
    g.setColour (kDivider);
    g.drawHorizontalLine (topBarBounds.getBottom() - 1,
                          (float) topBarBounds.getX(), (float) topBarBounds.getRight());
}

void MainComponent::resized()
{
    auto area = getLocalBounds();

    // Phase 22: the menu bar is its own strip above the transport. Putting it
    // inside the transport bar would have meant carving 100 px out of a row
    // that is already full at the minimum window width.
    menuBarBounds = area.removeFromTop (ui::px (24));

    topBarBounds = area.removeFromTop (ui::px (52));

    bpmField.setBounds (getBpmFieldBounds());

    // Phase 8: no mixer strip along the bottom any more — the console has its
    // own window, and the height it used to take now belongs to the roll.
    //
    // Phase 17: each side column is two panels now, stacked. The split is a
    // stored pixel height clamped against the column, so folding every row in
    // the top panel does not silently rescale the bottom one.
    auto leftColumn  = area.removeFromLeft (LayerPanel::panelWidth);
    auto rightColumn = area.removeFromRight (RackPanel::panelWidth);

    {
        const int minTop = ui::px (150);
        const int minBot = ui::px (110);
        const int band   = splitterThickness;

        // The band is part of what the lower pane costs the column, so the
        // clamp has to count it — otherwise the splitter can be pushed off the
        // bottom of the window and there is no way to drag it back.
        const int limit  = juce::jmax (0, leftColumn.getHeight() - minTop - minBot - band);
        filePaneHeight   = juce::jlimit (minBot, juce::jmax (minBot, limit), filePaneHeight);

        auto lower = leftColumn.removeFromBottom (filePaneHeight + band);
        leftSplitter.setBounds (lower.removeFromTop (band));
        layerPanel.setBounds (leftColumn);
        filePanel.setBounds (lower);
    }

    {
        const int minTop = ui::px (180);
        const int minBot = ui::px (120);
        const int band   = splitterThickness;

        const int limit  = juce::jmax (0, rightColumn.getHeight() - minTop - minBot - band);
        groupPaneHeight  = juce::jlimit (minBot, juce::jmax (minBot, limit), groupPaneHeight);

        auto lower = rightColumn.removeFromBottom (groupPaneHeight + band);
        rightSplitter.setBounds (lower.removeFromTop (band));
        rackPanel.setBounds (rightColumn);
        groupPanel.setBounds (lower);
    }

    //-- Phase 12: the split between the roll and the automation window ------
    // The automation window is shorter than the roll on purpose: the roll is
    // where the music is, the curves are the correction layer under it. But the
    // ratio is the user's to change — dragging the band is how a DAW says "I am
    // editing curves now", and it must move the roll, not just the curves.
    const int travel = juce::jmax (0, area.getHeight() - splitterThickness);
    const int minAuto = ui::px (120);
    const int maxAuto = juce::jmax (minAuto, travel - ui::px (150));

    if (automationPaneHeight <= 0)
        automationPaneHeight = juce::jlimit (minAuto, maxAuto,
                                             juce::roundToInt ((float) travel * 0.30f));

    automationPaneHeight = juce::jlimit (minAuto, maxAuto, automationPaneHeight);

    if (travel < minAuto + ui::px (80))
    {
        // Too short for two panes: the roll gets everything, and the splitter
        // and the curve list are hidden rather than drawn as two slivers.
        splitterBar.setVisible (false);
        automationWindow.setVisible (false);
        pianoRoll.setBounds (area);
        return;
    }

    splitterBar.setVisible (true);
    automationWindow.setVisible (true);

    pianoRoll.setBounds (area.removeFromTop (travel - automationPaneHeight));
    splitterBar.setBounds (area.removeFromTop (splitterThickness));
    automationWindow.setBounds (area);
}

//==============================================================================
int MainComponent::getSplitTravel() const
{
    const int top = topBarBounds.isEmpty() ? ui::px (52) : topBarBounds.getHeight();

    return juce::jmax (0, getHeight() - top - splitterThickness);
}

void MainComponent::setAutomationPaneHeight (int pixels)
{
    const int travel = getSplitTravel();
    const int minAuto = ui::px (120);
    const int maxAuto = juce::jmax (minAuto, travel - ui::px (150));

    automationPaneHeight = juce::jlimit (minAuto, maxAuto, pixels);
    resized();
}


//==============================================================================
void MainComponent::mouseDown (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();

    // Phase 22: the menu bar owns its strip.
    if (menuBarBounds.contains (pos))
    {
        for (int i = 0; i < numMenus; ++i)
            if (getMenuBarItemBounds (i).contains (pos))
            {
                showMenuBarMenu (i);
                return;
            }

        return;
    }

    if (getPlayButtonBounds().contains (pos))
    {
        playTransport();
        return;
    }

    if (getStopButtonBounds().contains (pos))
    {
        stopTransport();
        return;
    }

    if (getRecordButtonBounds().contains (pos))
    {
        toggleRecording();
        return;
    }

    if (getMetronomeButtonBounds().contains (pos))
    {
        // Phase 88: left toggles, right asks about level and count-in. A
        // toggle that needs a menu first would make "just turn it on" two
        // gestures, which is the one thing a metronome must not be.
        if (e.mods.isPopupMenu())
            showMetronomeMenu();
        else
            toggleMetronome();

        return;
    }

    if (getSaveButtonBounds().contains (pos))
    {
        askToSaveProject();
        return;
    }

    if (getLoadButtonBounds().contains (pos))
    {
        askToLoadProject();
        return;
    }

    if (getExportButtonBounds().contains (pos))
    {
        askToExportAudio();
        return;
    }

    //-- Phase 8 --------------------------------------------------------------
    if (getNewButtonBounds().contains (pos))       { requestNewProject(); return; }
    if (getUndoButtonBounds().contains (pos))      { undo();              return; }
    if (getRedoButtonBounds().contains (pos))      { redo();              return; }
    if (getMixerButtonBounds().contains (pos))     { toggleMixerWindow(); return; }
    if (getSettingsButtonBounds().contains (pos))
    {
        // Left click: the settings window. Right click: the two view options,
        // which are not device settings and would be lost in a tab named
        // "audio" or "MIDI".
        if (e.mods.isPopupMenu())
            showSettingsMenu (e.getScreenPosition());
        else
            openSettings (0);

        return;
    }

    // Clicking the backdrop dismisses the note inspector.
    envelopePanel.close();
    sampleEnvelopePanel.close();
}

void MainComponent::mouseMove (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();

    //-- Phase 22: menu bar hover -------------------------------------------
    {
        int over = -1;

        for (int i = 0; i < numMenus; ++i)
            if (getMenuBarItemBounds (i).contains (pos))
                over = i;

        if (over != hoveredMenuItem)
        {
            hoveredMenuItem = over;
            repaint (menuBarBounds);
        }
    }

    const bool newPlay   = getPlayButtonBounds().contains (pos);
    const bool newStop   = getStopButtonBounds().contains (pos);
    const bool newRecord = getRecordButtonBounds().contains (pos);
    const bool newSave   = getSaveButtonBounds().contains (pos);
    const bool newLoad   = getLoadButtonBounds().contains (pos);
    const bool newExport = getExportButtonBounds().contains (pos);
    const bool newNew    = getNewButtonBounds().contains (pos);
    const bool newUndo   = getUndoButtonBounds().contains (pos);
    const bool newRedo   = getRedoButtonBounds().contains (pos);
    const bool newMixer  = getMixerButtonBounds().contains (pos);
    const bool newGear   = getSettingsButtonBounds().contains (pos);
    const bool newClick  = getMetronomeButtonBounds().contains (pos);

    setMouseCursor ((newPlay || newStop || newRecord || newSave || newLoad || newExport
                      || newNew || newUndo || newRedo || newMixer || newGear || newClick
                      || hoveredMenuItem >= 0)
                      ? juce::MouseCursor::PointingHandCursor
                      : juce::MouseCursor::NormalCursor);

    if (newPlay != hoveringPlay || newStop != hoveringStop || newRecord != hoveringRecord
         || newSave != hoveringSave || newLoad != hoveringLoad || newExport != hoveringExport
         || newNew != hoveringNew || newUndo != hoveringUndo || newRedo != hoveringRedo
         || newMixer != hoveringMixer || newGear != hoveringSettings
         || newClick != hoveringMetronome)
    {
        hoveringPlay   = newPlay;
        hoveringStop   = newStop;
        hoveringRecord = newRecord;
        hoveringSave   = newSave;
        hoveringLoad   = newLoad;
        hoveringExport = newExport;
        hoveringNew    = newNew;
        hoveringUndo   = newUndo;
        hoveringRedo   = newRedo;
        hoveringMixer  = newMixer;
        hoveringSettings = newGear;
        hoveringMetronome = newClick;
        repaint (topBarBounds);
    }
}

void MainComponent::mouseExit (const juce::MouseEvent&)
{
    if (hoveredMenuItem >= 0)
    {
        hoveredMenuItem = -1;
        repaint (menuBarBounds);
    }

    if (hoveringPlay || hoveringStop || hoveringRecord || hoveringMetronome
         || hoveringSave || hoveringLoad || hoveringExport
         || hoveringNew || hoveringUndo || hoveringRedo || hoveringMixer || hoveringSettings)
    {
        hoveringPlay = hoveringStop = hoveringRecord = hoveringMetronome = false;
        hoveringSave = hoveringLoad = hoveringExport = false;
        hoveringNew = hoveringUndo = hoveringRedo = hoveringMixer = hoveringSettings = false;
        repaint (topBarBounds);
    }
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    // Phase 39: with the source set to the typing keyboard, a note key is a
    // note before it is a shortcut. The KeyListener on the panels covers the
    // case where they have focus; this covers the host holding it.
    if (handleTypingKeyDown (key))
        return true;

    if (key.getModifiers().isCommandDown())
    {
        // Phase 52: the standard "settings" shortcut. Checked before the letter
        // comparisons because Ctrl+, has no letter to compare.
        if (key.getKeyCode() == ',' || key.getTextCharacter() == ',')
        {
            openSettings (0);
            return true;
        }

        const auto character = juce::CharacterFunctions::toUpperCase (
                                  (juce::juce_wchar) (key.getKeyCode() != 0 ? key.getKeyCode()
                                                                            : key.getTextCharacter()));

        if (character == 'S')
        {
            // Phase 22: 保存 writes where it already points; 另存为 always asks.
            if (key.getModifiers().isShiftDown()) askToSaveProject();
            else                                  saveProject();

            return true;
        }

        if (character == 'O') { askToLoadProject();  return true; }
        if (character == 'E') { askToExportAudio();  return true; }
        if (character == 'N') { requestNewProject(); return true; }
        if (character == 'Q') { requestQuit();       return true; }

        // Phase 22: copy / cut / paste. The roll only claims these when it has
        // a curve box selected; otherwise they fall through to here.
        if (character == 'C' && ! key.getModifiers().isShiftDown())
        {
            copyEditSelection();
            return true;
        }

        if (character == 'X') { cutEditSelection();   return true; }
        if (character == 'V') { pasteEditSelection(); return true; }

        if (character == 'Z')
        {
            if (key.getModifiers().isShiftDown()) redo();
            else                                  undo();

            return true;
        }
        if (character == 'Y') { redo(); return true; }
    }

    if (key.getTextCharacter() == 'm' || key.getTextCharacter() == 'M')
    {
        toggleMixerWindow();
        return true;
    }

    // Phase 33: space is a PLAY/STOP toggle. Phase 32 had made it stop-only,
    // which left the keyboard unable to start playback at all — playing is the
    // one transport verb a key has to be able to do.
    if (key == juce::KeyPress::spaceKey)
    {
        if (spaceStartsPlayback (audio.isTransportEngaged()))
            playTransport();
        else
            stopTransport();

        return true;
    }

    // Return is the other verb: rewind to the top, whatever the state.
    if (key == juce::KeyPress::returnKey)
    {
        rewindTransport();
        return true;
    }

    return false;
}

bool MainComponent::handleGlobalKeyPress (const juce::KeyPress& key)
{
    // A text field inside the plugin's own editor is a text field: typing a
    // preset name is not playing notes, and "every letter plays a note" would
    // make the plugin's own search boxes unusable. Everything else — space,
    // return, the note keys, the Ctrl commands — goes through the same handler
    // the main window uses, so there is one key map and not two.
    if (auto* focused = juce::Component::getCurrentlyFocusedComponent())
        if (dynamic_cast<juce::TextEditor*> (focused) != nullptr)
            return false;

    return keyPressed (key);
}

//==============================================================================
//-- Phase 22: menu bar --------------------------------------------------------
namespace
{
    /** The modifier the user actually presses. The menu lists the shortcut the
        keyboard handler listens for, so these two strings and keyPressed() have
        to agree — they are written next to each other on purpose. */
   #if JUCE_MAC
    const juce::String kCommandKey ("Cmd+");
   #else
    const juce::String kCommandKey ("Ctrl+");
   #endif
}

juce::String MainComponent::menuTitle (int index)
{
    switch (index)
    {
        case 0:  return utf8 ("文件");
        case 1:  return utf8 ("编辑");
        case 2:  return utf8 ("工具");
        default: return {};
    }
}

juce::Rectangle<int> MainComponent::getMenuBarItemBounds (int index) const
{
    if (! juce::isPositiveAndBelow (index, numMenus))
        return {};

    static constexpr int widths[numMenus] { 54, 54, 54 };   // design px

    int x = menuBarBounds.getX() + ui::px (8);

    for (int i = 0; i < index; ++i)
        x += ui::px (widths[i]);

    return { x, menuBarBounds.getY() + ui::px (2), ui::px (widths[index]),
             menuBarBounds.getHeight() - ui::px (4) };
}

void MainComponent::fillFileMenu (juce::PopupMenu& menu) const
{
    const bool hasProject = currentProject.existsAsFile();

    auto addCommand = [&menu] (int id, const juce::String& text,
                               const juce::String& keys, bool enabled = true)
    {
        juce::PopupMenu::Item item;

        item.itemID               = id;
        item.text                 = text;
        item.isEnabled            = enabled;
        item.shortcutKeyDescription = keys;

        menu.addItem (item);
    };

    addCommand (1, utf8 ("新建"),        kCommandKey + "N");
    addCommand (2, utf8 ("打开…"),       kCommandKey + "O");
    addCommand (3, utf8 ("保存"),        kCommandKey + "S");
    addCommand (4, utf8 ("另存为…"),     kCommandKey + "Shift+S");

    menu.addSeparator();

    // Phase 52: the two commands that used to live only on a button and a
    // shortcut. A menu is where a user looks for "export" and "settings"; the
    // button and Ctrl+E stay, and all three land on the same dialog.
    addCommand (6, utf8 ("导出…"), kCommandKey + "E", getContentSeconds() > 0.001);

    menu.addSeparator();
    addCommand (7, utf8 ("设置…"), kCommandKey + ",");

    menu.addSeparator();
    addCommand (5, utf8 ("退出"),        kCommandKey + "Q");

    juce::ignoreUnused (hasProject);
}

void MainComponent::fillEditMenu (juce::PopupMenu& menu) const
{
    auto addCommand = [&menu] (int id, const juce::String& text,
                               const juce::String& keys, bool enabled = true)
    {
        juce::PopupMenu::Item item;

        item.itemID               = id;
        item.text                 = text;
        item.isEnabled            = enabled;
        item.shortcutKeyDescription = keys;

        menu.addItem (item);
    };

    addCommand (11, utf8 ("撤销"), kCommandKey + "Z",         canUndo());
    addCommand (12, utf8 ("重做"), kCommandKey + "Shift+Z",   canRedo());

    menu.addSeparator();

    addCommand (13, utf8 ("复制"), kCommandKey + "C",         canCopyEditSelection());
    addCommand (14, utf8 ("剪切"), kCommandKey + "X",         canCutEditSelection());
    addCommand (15, utf8 ("粘贴"), kCommandKey + "V",         canPasteEditSelection());

    menu.addSeparator();

    // The menu says which clipboard it will act on, so "粘贴" is never a guess.
    juce::String where;

    if (pianoRoll.canPasteAutomationCurve())
        where = utf8 ("当前：自动化曲线");

    //-- Phase 24/63: a copied object is not "N notes" ------------------------
    // The paste places whole appearances carrying their name, mount and curves,
    // so the status line has to say so — otherwise the menu describes a paste
    // that never runs. This branch comes FIRST because an object clipboard need
    // not hold any loose notes or blocks: a group of notes only leaves both
    // loose lists empty, and the "剪贴板为空" fallback used to swallow it.
    else if (pianoRoll.isGroupClipboard())
    {
        const int objects = pianoRoll.getGroupClipboardCount();

        where = objects == 1
                  ? utf8 ("整体：组「") + pianoRoll.getGroupClipboardName() + utf8 ("」")
                  : utf8 ("整体：") + juce::String (objects) + utf8 (" 个组");

        if (const int lanes = pianoRoll.getGroupClipboardLiveLanes(); lanes > 0)
            where += utf8 (" · ") + juce::String (lanes) + utf8 (" 条曲线");
    }
    else if (pianoRoll.canPasteNotes() && pianoRoll.canPasteSamples())
    {
        // Phase 29: copy fills both clipboards in one gesture, so "both" is a
        // real state and the header has to name both — a line that said only
        // "8 个音符" would describe half of what the paste will do.
        where = utf8 ("音符 ") + juce::String (pianoRoll.getNoteClipboardSize())
                  + utf8 (" · 采样 ") + juce::String (pianoRoll.getSampleClipboardSize());
    }
    else if (pianoRoll.canPasteNotes())
        where = utf8 ("当前：") + juce::String (pianoRoll.getNoteClipboardSize()) + utf8 (" 个音符");
    else if (pianoRoll.canPasteSamples())
        where = utf8 ("剪贴板：") + juce::String (pianoRoll.getSampleClipboardSize()) + utf8 (" 个采样");
    else
        where = utf8 ("剪贴板为空");

    menu.addSectionHeader (utf8 ("粘贴目标 · ") + where);
}

void MainComponent::fillToolsMenu (juce::PopupMenu& menu) const
{
    using Tool = PianoRollComponent::Tool;

    auto addCommand = [&menu] (int id, const juce::String& text,
                               const juce::String& keys, bool enabled = true, bool ticked = false)
    {
        juce::PopupMenu::Item item;

        item.itemID                 = id;
        item.text                   = text;
        item.isEnabled              = enabled;
        item.isTicked               = ticked;
        item.shortcutKeyDescription = keys;

        menu.addItem (item);
    };

    //-- Modes ----------------------------------------------------------------
    // Ticked, not radio buttons: JUCE's tick is the one mark the platform draws
    // consistently in a dark popup, and "which mode am I in" is the single
    // question this block answers. The arrow in front of the label is the same
    // signal the strip uses (a lit plate), spelled for a menu.
    const Tool current = pianoRoll.getTool();

    menu.addSectionHeader (utf8 ("模式 · ") + (current == Tool::pencil ? utf8 ("画笔")
                                              : current == Tool::eraser ? utf8 ("橡皮")
                                              : current == Tool::select ? utf8 ("选择")
                                              : current == Tool::hand   ? utf8 ("抓手")
                                                                        : utf8 ("刻刀")));

    addCommand (21, utf8 ("画笔（点击空白画音符）"), {}, true, current == Tool::pencil);
    addCommand (22, utf8 ("橡皮（点击删除）"),       {}, true, current == Tool::eraser);
    addCommand (23, utf8 ("选择（拖动 / 框选）"),     {}, true, current == Tool::select);
    addCommand (24, utf8 ("抓手（拖动平移）"),       {}, true, current == Tool::hand);
    addCommand (25, utf8 ("刻刀（切开音符或采样）"),  {}, true, current == Tool::knife);

    menu.addSeparator();

    //-- Actions --------------------------------------------------------------
    // Same guards as the strip: a greyed item here and a dead button there are
    // the same rule, so both read hasAnySelection().
    const bool hasSelection = pianoRoll.hasAnySelection();

    addCommand (26, pianoRoll.hasPencilState() ? utf8 ("清除画笔状态")
                                               : utf8 ("复制选中音符的画笔状态"),
                kCommandKey + "Shift+C",
                pianoRoll.hasPencilState() || hasSelection);

    menu.addSeparator();

    // The pair reads as one control with two directions in the strip; in a menu
    // it reads as two verbs, which is why they sit next to each other here too.
    const bool inGroup = pianoRoll.getSelectedGroupId() >= 0;

    addCommand (27, inGroup ? utf8 ("编组（选中已在组内，点此选中整组）") : utf8 ("编组"),
                "G", hasSelection);
    addCommand (28, utf8 ("解组"), "Shift+G", hasSelection);

    menu.addSeparator();

    addCommand (29, utf8 ("吸附到 1/16 网格"), {}, true, pianoRoll.isSnapEnabled());
}

void MainComponent::applyToolsChoice (int itemId)
{
    using Tool = PianoRollComponent::Tool;

    switch (itemId)
    {
        case 21: pianoRoll.setTool (Tool::pencil); break;
        case 22: pianoRoll.setTool (Tool::eraser); break;
        case 23: pianoRoll.setTool (Tool::select); break;
        case 24: pianoRoll.setTool (Tool::hand);   break;
        case 25: pianoRoll.setTool (Tool::knife);  break;

        case 26:
            // The strip's two states in one item: lit means "clear it", dark
            // means "load it from the selection AND switch to the pencil" —
            // copying is only useful if the next thing you do is draw.
            if (pianoRoll.hasPencilState())
                pianoRoll.clearPencilState();
            else if (pianoRoll.copyNoteStateFromSelection())
                pianoRoll.setTool (Tool::pencil);
            break;

        case 27: if (pianoRoll.hasAnySelection()) pianoRoll.groupSelection();   break;
        case 28: if (pianoRoll.hasAnySelection()) pianoRoll.ungroupSelection(); break;
        case 29: pianoRoll.setSnapEnabled (! pianoRoll.isSnapEnabled());        break;

        default: break;
    }

    // The menu is not inside the strip, so the strip's own repaint does not
    // happen and the lit plates would keep describing the old mode.
    pianoRoll.repaint();
    repaint();
}

void MainComponent::applyFileChoice (int itemId)
{
    switch (itemId)
    {
        case 1: requestNewProject(); break;
        case 2: askToLoadProject();  break;
        case 3: saveProject();       break;
        case 4: askToSaveProject();  break;
        case 5: requestQuit();       break;
        case 6: askToExportAudio();  break;
        case 7: openSettings (0);    break;
        default: break;
    }
}

void MainComponent::applyEditChoice (int itemId)
{
    switch (itemId)
    {
        case 11: undo(); break;
        case 12: redo(); break;
        case 13: copyEditSelection(); break;
        case 14: cutEditSelection();  break;
        case 15: pasteEditSelection(); break;
        default: break;
    }
}

void MainComponent::showMenuBarMenu (int index)
{
    juce::PopupMenu menu;

    if (index == 0)
        fillFileMenu (menu);
    else if (index == 1)
        fillEditMenu (menu);
    else
        fillToolsMenu (menu);

    hoveredMenuItem = index;
    repaint (menuBarBounds);

    juce::Component::SafePointer<MainComponent> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetComponent (this)
                            .withTargetScreenArea (localAreaToGlobal (getMenuBarItemBounds (index))),
                        [safe, index] (int result)
                        {
                            if (safe == nullptr)
                                return;

                            safe->hoveredMenuItem = -1;
                            safe->repaint (safe->menuBarBounds);

                            if (result <= 0)
                                return;

                            if (index == 0)
                                safe->applyFileChoice (result);
                            else if (index == 1)
                                safe->applyEditChoice (result);
                            else
                                safe->applyToolsChoice (result);
                        });
}

//==============================================================================
//  Phase 84: crash recovery
//==============================================================================
void MainComponent::autosaveTick()
{
    if (! recoveryEnabled || exporting || undoGestureActive || restoringHistory || recoveryPromptOpen)
        return;

    const auto now = juce::Time::getMillisecondCounter();

    // Signed difference: the millisecond counter wraps after 49 days, and the
    // obvious `<` would then stop saving for the rest of that session.
    if ((juce::int32) (now - nextRecoveryAt) < 0)
        return;

    nextRecoveryAt = now + recoveryIntervalMs;

    if (! recoveryDirty)
        return;                       // nothing has been edited since the last pass

    recoveryDirty = false;

    // The document itself, not `dirtyCache`. That cache is refreshed by
    // commitHistory, which runs a turn later, so trusting it here would drop the
    // first edit after a pause — the one an autosave is most likely to meet.
    // One capture answers both questions: is this still unsaved, and is it
    // different from what is already written.
    const auto xml = captureState();

    if (xml == savedStateText)
    {
        // Undone back to the file on disk. Nothing is waiting to be lost, and a
        // copy that stayed would offer the user their own current document back
        // as if it were news.
        dropRecovery();
        return;
    }

    if (xml == lastRecoveryText)
        return;

    juce::String error;

    // This is NOT a save. Handing the string to saveProjectToFile would move
    // `currentProject` into the recovery folder, and the next Ctrl+S would put
    // the user's song among the rescue files.
    if (recovery::write (recoveryFile, xml, currentProject, error))
        lastRecoveryText = xml;

    // A failure is not reported: the next pass tries again, and a dialog about
    // a rescue copy the user did not ask for is worse than silently retrying.
}

void MainComponent::dropRecovery()
{
    if (lastRecoveryText.isEmpty() && ! recoveryFile.existsAsFile())
        return;                       // never written and nothing there

    recovery::discard (recoveryFile);
    lastRecoveryText.clear();
}

void MainComponent::resetRecoveryFor (const juce::File& projectFile)
{
    // The old name is the one that has to be removed; `dropRecovery` reads the
    // member, so it has to run before the member changes.
    dropRecovery();

    recoveryFile  = recovery::getFileFor (projectFile);
    recoveryDirty = false;
}

void MainComponent::offerRecoveryIfAny()
{
    if (! recoveryEnabled || recoveryPromptOpen)
        return;

    std::vector<recovery::Entry> offers;

    for (const auto& entry : recovery::list())
    {
        switch (recovery::inspect (entry.file, entry.projectFile))
        {
            case recovery::Decision::offer:
                offers.push_back (entry);
                break;

            case recovery::Decision::stale:
            case recovery::Decision::unusable:
                // Belongs to a project that has been saved since, or is not a
                // session at all. Neither will ever be offered, and startup is
                // the last moment anything remembers they exist.
                recovery::discard (entry.file);
                break;

            case recovery::Decision::none:
                break;
        }
    }

    if (offers.empty())
        return;

    const auto newest = offers.front();

    const auto name = newest.projectFile.existsAsFile()
                        ? newest.projectFile.getFileNameWithoutExtension()
                        : juce::String (utf8 ("未命名工程"));

    auto* alert = new juce::AlertWindow (utf8 ("有未保存的工作"),
                                         recoveryOfferMessage (name, newest.savedAt,
                                                               newest.bytes,
                                                               (int) offers.size() - 1,
                                                               recovery::getFolder()),
                                         juce::MessageBoxIconType::QuestionIcon,
                                         this);

    addRecoveryButtons (*alert);

    recoveryPromptOpen = true;

    juce::Component::SafePointer<MainComponent> safe (this);

    alert->enterModalState (true,
                            juce::ModalCallbackFunction::create (
                                [safe, newest] (int result)
                                {
                                    if (safe == nullptr)
                                        return;

                                    safe->recoveryPromptOpen = false;

                                    if (result == 1)
                                    {
                                        juce::String error;

                                        if (! safe->recoverFromFile (newest.file, error))
                                            safe->showMessage (utf8 ("恢复失败"), error, true);

                                        return;
                                    }

                                    if (result == 0)
                                        recovery::discard (newest.file);

                                    // 稍后 asks again on the next launch. Silence
                                    // is the only answer here that cannot destroy
                                    // something because of a mis-click, and the
                                    // copy costs a few kilobytes on disk.
                                }),
                            true);      // delete when dismissed
}

bool MainComponent::recoverFromFile (const juce::File& file, juce::String& error)
{
    if (! file.existsAsFile())
    {
        error = utf8 ("恢复文件已经不在了");
        return false;
    }

    const auto xml = file.loadFileAsString();

    if (xml.isEmpty())
    {
        error = utf8 ("恢复文件是空的");
        return false;
    }

    // The identity comes from the copy itself: a session that was never saved
    // has no other record of where it belongs.
    //
    // Phase 86: that identity is also what the copy's relative sample paths are
    // relative TO. The copy lives in the rescue folder, so resolving against
    // its own location would look for every clip in `Documents/AtomicDAW/`.
    const auto  projectPath = recovery::readProjectPath (file);
    const juce::File projectFileForPaths (projectPath);

    bool restored = false;

    {
        const juce::ScopedValueSetter<bool> guard (restoringHistory, true);
        restored = restoreState (xml, &projectFileForPaths);
    }

    if (! restored)
    {
        error = utf8 ("恢复文件已经不能解析成工程了");
        return false;
    }

    currentProject = projectPath.isEmpty() ? juce::File() : juce::File (projectPath);
    recoveryFile   = file;

    // One undo step, exactly like opening a file: the history of a session that
    // was crashed out of is not the history of this one. Taken from the restored
    // model rather than from `xml`, because the copy carries two bookkeeping
    // attributes that a fresh capture does not — otherwise every later undo
    // would see a phantom difference at the bottom of the stack.
    cancelPendingUpdate();
    history.clear();
    history.push_back (captureState());
    historyPos = 0;

    // Still unsaved, on purpose. `savedStateText` keeps pointing at whatever the
    // disk holds (often nothing), which is what makes the very next close ask
    // before this is thrown away a second time.
    dirtyCache = (history.front() != savedStateText);

    lastRecoveryText = history.front();
    recoveryDirty    = false;

    repaint();
    return true;
}

void MainComponent::saveProject()
{
    if (! currentProject.existsAsFile())
    {
        // Never saved: this IS "save as", so ask for a path.
        askToSaveProject();
        return;
    }

    juce::String error;

    // No "saved!" dialog on the happy path: Ctrl+S is pressed while working,
    // and a modal box every few seconds is a reason to stop pressing it.
    if (! saveProjectToFile (currentProject, error))
        showMessage (utf8 ("保存失败"), error, true);
}

void MainComponent::requestQuit()
{
    // Exactly the close button's path, because it is the one that knows how to
    // ask about unsaved work and how to hand the quitting over to the dialog.
    if (! handleCloseRequest())
        return;

    if (auto* app = juce::JUCEApplication::getInstance())
        app->systemRequestedQuit();
}

//==============================================================================
//-- Phase 22: copy / cut / paste ---------------------------------------------
bool MainComponent::canCopyEditSelection() const
{
    return pianoRoll.canCopyAutomationCurve()
             || ! pianoRoll.getSelection().empty()
             || ! pianoRoll.getSampleSelection().empty();
}

bool MainComponent::canCutEditSelection() const
{
    return canCopyEditSelection();
}

bool MainComponent::canPasteEditSelection() const
{
    return pianoRoll.canPasteAutomationCurve() || pianoRoll.canPasteNotes()
             || pianoRoll.canPasteSamples();
}

void MainComponent::copyEditSelection()
{
    // One rule, stated once: a selected curve answers first. If you can see a
    // box with a lit frame, that box is what Ctrl+C copies — anything else
    // would make the answer depend on invisible state.
    if (pianoRoll.canCopyAutomationCurve() && pianoRoll.copyAutomationCurve())
        return;

    // Phase 29: then everything that is selected — notes AND clips. A marquee
    // can hold both, and copying one of them would be the invisible choice this
    // whole design keeps out of the clipboard.
    if (! pianoRoll.getSelection().empty() || ! pianoRoll.getSampleSelection().empty())
        pianoRoll.copySelectionToClipboards();
}

void MainComponent::cutEditSelection()
{
    if (pianoRoll.canCopyAutomationCurve() && pianoRoll.cutAutomationCurve())
    {
        repaint();
        return;
    }

    if (! pianoRoll.getSelection().empty() || ! pianoRoll.getSampleSelection().empty())
    {
        pianoRoll.copySelectionToClipboards();

        // Cut is copy-then-delete over BOTH kinds, through the same two entry
        // points a user would use: nothing here can delete something the copy
        // did not take, and both are no-ops when nothing of that kind is
        // selected.
        pianoRoll.deleteSelectedSamples();
        pianoRoll.deleteSelectedNotes();
    }

    repaint();
}

void MainComponent::pasteEditSelection()
{
    if (pianoRoll.canPasteAutomationCurve())
    {
        askHowToPasteCurve();
        return;
    }

    // Phase 29: paste everything both clipboards hold. Copy fills both from one
    // gesture, so this is not a choice between them — it is the same selection
    // coming back.
    pianoRoll.pasteEditClipboard();
    repaint();
}

void MainComponent::askHowToPasteCurve()
{
    if (! pianoRoll.canPasteAutomationCurve())
        return;

    auto* window = new juce::AlertWindow (utf8 ("粘贴自动化曲线"),
                                          pianoRoll.describeCurvePaste(),
                                          juce::MessageBoxIconType::QuestionIcon);

    window->addButton (utf8 ("拉伸"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton (utf8 ("不拉伸"), 2);
    window->addButton (utf8 ("取消"), 0, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<MainComponent> safe (this);

    window->enterModalState (true,
                             juce::ModalCallbackFunction::create ([safe] (int result)
                             {
                                 if (safe == nullptr || result <= 0)
                                     return;

                                 safe->pianoRoll.pasteAutomationCurve (result == 1);
                                 safe->repaint();
                             }),
                             true);      // deleteWhenDismissed
}

//==============================================================================
//-- Phase 6: project files ---------------------------------------------------
juce::File MainComponent::getProjectDirectory()
{
    auto directory = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                       .getChildFile ("AtomicDAW");

    directory.createDirectory();
    return directory;
}

bool MainComponent::saveProjectToFile (const juce::File& file, juce::String& error)
{
    if (! project::save (file, stack, rack, audio.getBpm(), error, &mixer, &automation, &tempo,
                         &loopRegion))
        return false;

    currentProject = file;

    // A gesture still in flight belongs to what was just written, so flush it
    // before remembering "this is the saved state" — otherwise the same edit
    // would be reported as unsaved a moment later.
    cancelPendingUpdate();
    commitHistory();
    savedStateText = captureState();
    dirtyCache     = false;

    // Phase 84: the disk now holds this session, so there is nothing left for a
    // recovery copy to be newer than. A save-as also has to move the copy: its
    // name is derived from the project path.
    resetRecoveryFor (file);

    repaint (topBarBounds);
    return true;
}

bool MainComponent::loadProjectFromFile (const juce::File& file, juce::String& error)
{
    double newBpm = audio.getBpm();
    int    newBeatsPerBar = beatsPerBar;

    // project::load parses into temporaries and only commits at the end, so a
    // corrupt file leaves the open session exactly as it was.
    //
    // Phase 33: the commit itself replaces the layers and then the rack, and the
    // notification from the first half would let the sampler sweep run against a
    // rack that still belongs to the previous song. The sweep below runs once,
    // on the finished session.
    bool loaded = false;

    {
        const juce::ScopedValueSetter<bool> noSweep (suspendSamplerPrune, true);
        loaded = project::load (file, stack, rack, newBpm, error,
                                &mixer, &automation, &plugins, &tempo, &loopRegion,
                                &newBeatsPerBar);
    }

    if (! loaded)
        return false;

    // Loading a different song rewinds: leaving the playhead mid-timeline over
    // material it was never positioned against is disorienting (and the old
    // demo note ids below would point at notes that no longer exist).
    audio.stopTransport();
    audio.setBpm (newBpm);

    // Phase 87: the loaded song carries its own loop; until now the renderer
    // was repeating the previous project's region.
    applyLoopToViews();

    bpmField.setBpm (audio.getBpm(), false);
    pianoRoll.setBpm (audio.getBpm());

    // Phase 91: the song brings its own metre, applied before anything is
    // measured in bars — the same order the loop is applied in.
    setBeatsPerBar (newBeatsPerBar);

    pianoRoll.setSelection ({});
    pianoRoll.setPlayheadBeat (0.0);
    pianoRoll.setTransportActive (false);

    // The loaded song brings notes that may sit past the default 64 beats; the
    // roll has to be long enough to scroll to them.
    pianoRoll.resetTimeline();

    envelopePanel.close();
    sampleEnvelopePanel.close();
    layerPanel.setPlayheadBeat (-1.0);
    rackPanel.setPlayheadBeat (-1.0);

    // The demo's "open the inspector on this note" ids belonged to the old
    // project; keeping them would make the next snapshot open a dead note.
    demoOverrideLayer = -1;
    demoOverrideNote  = -1;

    // Same for the roll's inline box: it addresses a note by id.
    pianoRoll.closeNoteAutomation();

    currentProject = file;

    // Phase 84: the previous project's unsaved work was just thrown away by
    // opening something else, so its copy must go with it — otherwise the next
    // launch offers to restore a session the user already walked away from.
    resetRecoveryFor (file);

    // Phase 32: a file written by an older build can carry samplers that no
    // clip names any more (deleting a clip used to leave them behind). Loading
    // is the one moment the reference rule can be applied to the whole project
    // at once, and the sweep is silent when there is nothing to drop.
    pruneOrphanSamplerSlots();

    // A different song: the undo history of the previous one is meaningless now
    // (undone far enough it would teleport the user into another project), and
    // what is on screen is exactly what is in the file.
    cancelPendingUpdate();
    history.clear();
    history.push_back (captureState());
    historyPos = 0;
    savedStateText = history.front();
    dirtyCache     = false;

    // Phase 86: the song is open. If parts of it are silent because their files
    // moved, this is the moment to say so — the alternative is the user finding
    // out by pressing play and hearing a hole where the drums were.
    offerSampleRelink();

    repaint();
    return true;
}

//==============================================================================
//  Phase 87: the loop region
//==============================================================================
juce::Image MainComponent::createLoopSnapshot (double startBeat, double endBeat)
{
    LoopRegion region;
    region.enabled   = true;
    region.startBeat = juce::jmax (0.0, startBeat);
    region.endBeat   = juce::jmax (region.startBeat + LoopRegion::minLengthBeats, endBeat);

    setLoopRegion (region);

    pianoRoll.repaint();
    const auto full = pianoRoll.createComponentSnapshot (pianoRoll.getLocalBounds(), true);

    return full.getClippedImage (juce::Rectangle<int> (0, 0, full.getWidth(),
                                                       juce::jmin (full.getHeight(), ui::px (300))));
}

juce::Image MainComponent::createMetronomeSnapshot (int countInBeatsToShow)
{
    setMetronomeEnabled (true);
    setMetronomeVolume (0.75f);
    setCountInBeats (countInBeatsToShow);

    // Start a count-in so the readout shows the state the player actually sees.
    // A headless run has no device, so nothing consumes it — which is exactly
    // what a still needs: the first count, frozen.
    audio.getPlayback().startPreRoll (countInBeatsToShow);

    const auto still = createComponentSnapshot (topBarBounds, true);

    audio.getPlayback().cancelPreRoll();
    return still;
}

void MainComponent::setLoopRegion (const LoopRegion& region)
{
    if (region == loopRegion)
        return;

    loopRegion = region;
    applyLoopToViews();

    // An edit like any other: the region is in the project file, so moving it
    // makes the session unsaved and closing without asking after one would be a
    // lie about what the file will contain.
    modelChanged();
}

void MainComponent::applyLoopToViews()
{
    // Two consumers, one order. The renderer first: a picture that shows a loop
    // the transport is not repeating is worse than one that is a frame behind.
    audio.getPlayback().setLoopRegion (loopRegion);
    pianoRoll.setLoopRegion (loopRegion);
}

//==============================================================================
//  Phase 86: audio that did not come back with the project
//==============================================================================
std::vector<MainComponent::MissingSample> MainComponent::collectMissingSamples()
{
    std::vector<MissingSample> missing;

    for (int i = 0; i < stack.getNumLayers(); ++i)
        if (auto* layer = stack.getLayer (i))
            for (auto& clip : layer->getSamples())
                if (clip.path.isNotEmpty() && ! juce::File (clip.path).existsAsFile())
                    missing.push_back ({ &clip, nullptr });

    // Samplers are collected last and in rack order, which is the same order the
    // repair walks them in. The two loops being separate is not an ordering
    // statement about the model — it is just that clips and slots are stored in
    // different places, and the pairing below only depends on the answer list
    // matching the question list.
    for (int i = 0; i < rack.getNumSlots(); ++i)
        if (auto* slot = rack.getSlot (i); slot != nullptr && slot->isSampler())
            if (slot->samplePath.isNotEmpty() && ! juce::File (slot->samplePath).existsAsFile())
                missing.push_back ({ nullptr, slot });

    return missing;
}

int MainComponent::relinkSamplesTo (const juce::File& folder)
{
    auto missing = collectMissingSamples();

    if (missing.empty() || ! folder.isDirectory())
        return 0;

    std::vector<juce::String> wanted;
    wanted.reserve (missing.size());

    for (const auto& entry : missing)
        wanted.push_back (entry.path());

    const auto found = samplepath::relinkFrom (wanted, folder);

    int repaired = 0;

    for (size_t i = 0; i < missing.size() && i < found.size(); ++i)
    {
        if (! found[i].existsAsFile())
            continue;               // that name is not in this folder either

        const auto loaded = loadSampleFile (found[i]);

        if (! loaded.ok())
            continue;               // found the name, could not read the audio

        // The repaired path is written as an ABSOLUTE one: the model never
        // holds a relative path, and `toStored` turns it back into the portable
        // form the next time the project is saved.
        missing[i].path() = found[i].getFullPathName();

        if (missing[i].clip != nullptr)
            missing[i].clip->audio = loaded.buffer;
        else
            missing[i].slot->sampleBuffer = loaded.buffer;

        ++repaired;
    }

    if (repaired > 0)
    {
        // The schedule froze the old (null) buffers at publish time, so it has
        // to be rebuilt, not merely repainted — a repaint would show a waveform
        // over a clip that still plays silence.
        stack.notifyNotesChanged();
        rackPanel.repaint();
        repaint();
    }

    return repaired;
}

void MainComponent::offerSampleRelink()
{
    if (! sampleRelinkPrompt)
        return;

    const auto missing = collectMissingSamples();

    if (missing.empty())
        return;

    const auto example = missing.front().path();

    juce::String text;
    text << utf8 ("这个工程里有 ") << juce::String ((int) missing.size())
         << utf8 (" 个采样文件不在原来的位置了。\n\n")
         << utf8 ("例如：") << example << utf8 ("\n\n")
         << utf8 ("选一个文件夹，我会按文件名把它们找回来；找不到的保持原样，不会乱认。");

    juce::Component::SafePointer<MainComponent> safe (this);

    juce::AlertWindow::showAsync (
        juce::MessageBoxOptions()
            .withIconType (juce::MessageBoxIconType::QuestionIcon)
            .withTitle (utf8 ("采样文件找不到"))
            .withMessage (text)
            .withButton (utf8 ("定位文件夹…"))
            .withButton (utf8 ("先不管"))
            .withAssociatedComponent (this),
        [safe] (int result)
        {
            if (safe == nullptr || result != 1)
                return;

            safe->chooseFolderForRelink();
        });
}

void MainComponent::chooseFolderForRelink()
{
    chooser = std::make_unique<juce::FileChooser> (utf8 ("采样文件现在在哪个文件夹"),
                                                   getProjectDirectory());

    juce::Component::SafePointer<MainComponent> safe (this);

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectDirectories,
                          [safe] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr)
                                  return;

                              const auto folder = fc.getResult();

                              if (! folder.isDirectory())
                                  return;

                              if (safe->relinkSamplesTo (folder) == 0)
                                  safe->showMessage (utf8 ("还是没找到"),
                                                     utf8 ("这个文件夹里没有同名的文件。\n")
                                                       + utf8 ("把采样和工程放进同一个文件夹，或者放在它下面的 media 里，再打开一次工程也可以。"),
                                                     true);
                          });
}

//==============================================================================
bounce::ExportResult MainComponent::exportAudioToFile (const juce::File& file,
                                                    const std::function<bool (double)>& progress,
                                                    double tailSeconds)
{
    // Freeze the session first. Everything the renderer needs — routing, both
    // faders, both kinds of automation, every note envelope — is copied into
    // this schedule by publishSchedule(), so the bounce cannot read a model
    // while the message thread is editing it.
    PlaybackEngine staging;
    staging.setSampleRate (bounce::exportSampleRate);
    staging.publishSchedule (stack, rack, audio.getBpm(), 0.0, &mixer);

    auto schedule = staging.getSchedule();

    if (schedule == nullptr)
    {
        bounce::ExportResult failed;
        failed.message = utf8 ("无法生成渲染快照");
        return failed;
    }

    return bounce::renderToFile (file, *schedule, bounce::exportSampleRate, tailSeconds, progress,
                              &plugins);
}

double MainComponent::getContentSeconds() const
{
    PlaybackEngine probe;
    probe.setSampleRate (bounce::exportSampleRate);
    probe.publishSchedule (stack, rack, audio.getBpm(), 0.0, &mixer);

    auto schedule = probe.getSchedule();

    if (schedule == nullptr)
        return 0.0;

    return schedule->tempo.secondsAtBeat (juce::jmax (0.0, schedule->contentBeats));
}

//==============================================================================
//-- Phase 6: the dialogs ----------------------------------------------------
void MainComponent::showMessage (const juce::String& title, const juce::String& message, bool isError)
{
    juce::AlertWindow::showMessageBoxAsync (isError ? juce::MessageBoxIconType::WarningIcon
                                                    : juce::MessageBoxIconType::InfoIcon,
                                            title, message, utf8 ("好"));
}

void MainComponent::askToSaveProject (std::function<void (bool saved)> onDone)
{
    const auto initial = currentProject.existsAsFile()
                           ? currentProject
                           : getProjectDirectory().getChildFile (juce::String ("Untitled")
                                                                   + project::fileExtension);

    chooser = std::make_unique<juce::FileChooser> (utf8 ("保存工程"), initial,
                                                   juce::String ("*") + project::fileExtension);

    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                            | juce::FileBrowserComponent::canSelectFiles
                            | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, onDone] (const juce::FileChooser& fc)
                          {
                              auto file = fc.getResult();

                              if (file.getFullPathName().isEmpty())
                              {
                                  if (onDone != nullptr)
                                      onDone (false);           // cancelled

                                  return;
                              }

                              if (file.getFileExtension().isEmpty())
                                  file = file.withFileExtension (project::fileExtension);

                              juce::String error;

                              if (saveProjectToFile (file, error))
                              {
                                  // The caller may be a quit / new / load flow that
                                  // is about to throw the session away; a "saved!"
                                  // box in front of that is just one more click.
                                  if (onDone != nullptr)
                                      onDone (true);
                                  else
                                      showMessage (utf8 ("工程已保存"),
                                                   file.getFileName() + "\n\n" + file.getFullPathName(),
                                                   false);
                              }
                              else
                              {
                                  showMessage (utf8 ("保存失败"), error, true);

                                  if (onDone != nullptr)
                                      onDone (false);
                              }
                          });
}

//==============================================================================
//-- Phase 10: unsaved changes ------------------------------------------------
bool MainComponent::hasUnsavedChanges()
{
    if (restoringHistory)
        return false;

    // A drag that has not reached the async capture yet is still an edit.
    cancelPendingUpdate();
    commitHistory();

    return dirtyCache;
}

void MainComponent::confirmDiscardChanges (const juce::String& action,
                                          std::function<void()> onProceed)
{
    const auto projectName = currentProject.existsAsFile()
                               ? currentProject.getFileNameWithoutExtension()
                               : juce::String (utf8 ("未命名工程"));

    auto* alert = new juce::AlertWindow (utf8 ("保存改动？"),
                                         projectName + utf8 (" 有未保存的改动。\n\n")
                                           + action + utf8 ("前要先保存吗？"),
                                         juce::MessageBoxIconType::QuestionIcon,
                                         this);

    alert->addButton (utf8 ("保存"),     1, juce::KeyPress ((int) 's'));
    alert->addButton (utf8 ("不保存"),   0, juce::KeyPress ((int) 'n'));
    alert->addButton (utf8 ("取消"),    -1, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<MainComponent> safe (this);

    alert->enterModalState (true,
                            juce::ModalCallbackFunction::create (
                                [safe, onProceed] (int result)
                                {
                                    if (safe == nullptr || result < 0)
                                        return;                    // cancelled

                                    if (result == 0)
                                    {
                                        // Phase 84: "不保存" is an answer about
                                        // this work, not just about this file.
                                        // Keeping the copy would resurrect it on
                                        // the next launch as if the user had
                                        // never said no.
                                        safe->dropRecovery();
                                        onProceed();
                                        return;
                                    }

                                    safe->askToSaveProject ([onProceed] (bool saved)
                                    {
                                        if (saved)
                                            onProceed();
                                    });
                                }),
                            true);      // delete when dismissed
}

bool MainComponent::handleCloseRequest()
{
    if (! hasUnsavedChanges())
        return true;

    auto* app = juce::JUCEApplication::getInstance();

    confirmDiscardChanges (utf8 ("关闭程序"), [app]
    {
        if (app != nullptr)
            app->systemRequestedQuit();
    });

    // The dialog owns the decision from here on; the window must stay open.
    return false;
}

void MainComponent::askToLoadProject()
{
    if (! hasUnsavedChanges())
    {
        showLoadChooser();
        return;
    }

    juce::Component::SafePointer<MainComponent> safe (this);

    confirmDiscardChanges (utf8 ("载入工程"), [safe]
    {
        if (safe != nullptr)
            safe->showLoadChooser();
    });
}

void MainComponent::showLoadChooser()
{
    chooser = std::make_unique<juce::FileChooser> (utf8 ("打开工程"),
                                                   getProjectDirectory(),
                                                   juce::String ("*") + project::fileExtension);

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto file = fc.getResult();

                              if (file.getFullPathName().isEmpty())
                                  return;

                              juce::String error;

                              if (loadProjectFromFile (file, error))
                                  showMessage (utf8 ("工程已载入"),
                                               file.getFileName() + "\n"
                                                 + juce::String (stack.getNumLayers()) + utf8 (" 个图层 · ")
                                                 + juce::String (rack.getNumSlots()) + utf8 (" 个音源 · ")
                                                 + juce::String (audio.getBpm(), 1) + " BPM",
                                               false);
                              else
                                  showMessage (utf8 ("载入失败"), error, true);
                          });
}

void MainComponent::askToExportAudio()
{
    showExportDialog();
}

void MainComponent::showExportDialog()
{
    if (exporting)
    {
        showMessage (utf8 ("导出"), utf8 ("已经有一个导出任务正在进行。"), false);
        return;
    }

    const double contentSeconds = getContentSeconds();

    if (contentSeconds <= 0.001)
    {
        showMessage (utf8 ("没有可导出的内容"),
                     utf8 ("工程里还没有音符。先画几个音符，再导出。"), false);
        return;
    }

    // One dialog at a time. A second Ctrl+E while it is up would otherwise put
    // two copies of the same four choices on screen, each with its own idea of
    // what will be written.
    if (exportDialogOpen)
        return;

    auto* dialog = new ExportDialog();
    dialog->setSettings (exportSettings);
    dialog->setContentSeconds (contentSeconds);
    dialog->setMp3Available (mp3::isAvailable());

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (dialog);
    options.dialogTitle                  = utf8 ("导出");
    options.dialogBackgroundColour       = theme::surface1;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar            = false;
    options.resizable                    = false;
    options.componentToCentreAround      = this;

    auto* window = options.launchAsync();

    exportDialogOpen = true;

    juce::Component::SafePointer<MainComponent> safeThis (this);
    juce::Component::SafePointer<juce::DialogWindow> safeWindow (window);

    // Closing the window by any route (Esc, ×, either button) has to clear the
    // flag; the two button callbacks below are not the only ways out, which is
    // why the hook lives on the dialog's own destruction.
    dialog->onClosedByUser = [safeThis]
    {
        if (auto* component = safeThis.getComponent())
            component->exportDialogOpen = false;
    };

    dialog->onCancel = [safeWindow]
    {
        if (auto* w = safeWindow.getComponent())
            w->exitModalState (0);
    };

    dialog->onExport = [safeThis, safeWindow] (const ExportSettings& settings)
    {
        if (auto* component = safeThis.getComponent())
        {
            component->exportDialogOpen = false;
            component->exportSettings   = settings;
            component->beginExport (settings);
        }

        if (auto* w = safeWindow.getComponent())
            w->exitModalState (0);
    };
}

void MainComponent::beginExport (const ExportSettings& settings)
{
    const juce::String extension = settings.extension();

    auto initial = getProjectDirectory().getChildFile ("AtomicDAW-Mix." + extension);

    if (currentProject.existsAsFile())
        initial = currentProject.getParentDirectory()
                    .getChildFile (currentProject.getFileNameWithoutExtension() + "." + extension);

    chooser = std::make_unique<juce::FileChooser> (utf8 ("导出 ") + extension.toUpperCase(),
                                                   initial, settings.wildcard());

    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                            | juce::FileBrowserComponent::canSelectFiles
                            | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, settings] (const juce::FileChooser& fc)
                          {
                              auto file = fc.getResult();

                              if (file.getFullPathName().isEmpty())
                                  return;

                              // The extension follows the FORMAT, not whatever
                              // the chooser was left on: an MP3 named .wav is
                              // a file that opens in the wrong program.
                              if (! file.hasFileExtension (settings.extension()))
                                  file = file.withFileExtension (settings.extension());

                              // Snapshot on the message thread; the render runs
                              // on its own thread with its own copy.
                              PlaybackEngine staging;
                              staging.setSampleRate (bounce::exportSampleRate);
                              staging.publishSchedule (stack, rack, audio.getBpm(), 0.0, &mixer);

                              auto schedule = staging.getSchedule();

                              if (schedule == nullptr)
                              {
                                  showMessage (utf8 ("导出失败"), utf8 ("无法生成渲染快照"), true);
                                  return;
                              }

                              exporting = true;
                              repaint (topBarBounds);

                              juce::Component::SafePointer<MainComponent> safeThis (this);

                              auto* task = new ExportTask (schedule, file, settings, [safeThis]
                              {
                                  if (auto* component = safeThis.getComponent())
                                      component->exportFinished();
                              });

                              task->launchThread();
                          });
}

void MainComponent::exportFinished()
{
    exporting = false;
    repaint (topBarBounds);
}

//==============================================================================
//-- Phase 50: render a scope into a sample block ----------------------------
scope::FreeBeat MainComponent::freeBeatOnLayer (int layerIndex, double lengthBeats) const
{
    const auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr || lengthBeats <= 0.0)
        return {};

    std::vector<std::pair<double, double>> occupied;
    double contentEnd = 0.0;

    for (const auto& note : layer->getNotes())
    {
        occupied.push_back ({ note.startBeat, note.endBeat() });
        contentEnd = juce::jmax (contentEnd, note.endBeat());
    }

    for (const auto& clip : layer->getSamples())
    {
        occupied.push_back ({ clip.startBeat, clip.endBeat() });
        contentEnd = juce::jmax (contentEnd, clip.endBeat());
    }

    return scope::findFreeBeat (std::move (occupied), lengthBeats, contentEnd);
}

void MainComponent::renderScopeToSample (const PianoRollComponent::RenderScopeRequest& request)
{
    auto* layer = stack.getLayer (request.layerIndex);

    if (layer == nullptr)
    {
        showMessage (utf8 ("无法渲染"), utf8 ("当前没有可写入的图层。"), true);
        return;
    }

    // Freeze the session exactly as the export does: one mixer, one schedule.
    PlaybackEngine staging;
    staging.setSampleRate (bounce::exportSampleRate);
    staging.publishSchedule (stack, rack, audio.getBpm(), 0.0, &mixer);

    auto full = staging.getSchedule();

    if (full == nullptr)
    {
        showMessage (utf8 ("无法渲染"), utf8 ("无法生成渲染快照"), true);
        return;
    }

    scope::Scope asked;

    asked.layerIndex = request.layerIndex;
    asked.noteIds    = request.noteIds;
    asked.sampleIds  = request.sampleIds;

    if (request.wholeLayer)
    {
        asked.kind = scope::Scope::Kind::layer;
    }
    else if (request.groupId >= 0)
    {
        asked.kind    = scope::Scope::Kind::group;
        asked.groupId = request.groupId;
        asked.groupOccurrence = request.groupOccurrence;   // Phase 58: this copy
    }
    else
    {
        asked.kind = scope::Scope::Kind::selection;
    }

    auto extracted = scope::extract (*full, asked);

    if (extracted.empty || extracted.lengthSeconds <= 0.001)
    {
        showMessage (utf8 ("没有可渲染的内容"),
                     utf8 ("这次选择里没有音符或采样。"), false);
        return;
    }

    const double secondsPerBeat = juce::jmax (1.0e-9, 60.0 / juce::jmax (1.0, audio.getBpm()));
    const double lengthBeats    = extracted.lengthSeconds / secondsPerBeat;

    // Ask before rendering, not after: the answer only depends on the model, and
    // "there is nowhere to put it" is much cheaper to learn now than in four
    // minutes.
    if (! freeBeatOnLayer (request.layerIndex, lengthBeats).found)
    {
        showMessage (utf8 ("没有地方可以放"),
                     utf8 ("这个图层里找不到一段 ") + juce::String (lengthBeats, 2)
                       + utf8 (" 拍长的空白位置。\n\n先挪开一些内容，或者渲染到别的图层。"),
                     true);
        return;
    }

    const auto rendersDir = getProjectDirectory().getChildFile ("Renders");
    rendersDir.createDirectory();

    ++scopeRenderSerial;

    const auto file = rendersDir.getChildFile ("render-" + juce::String (scopeRenderSerial)
                                                 + "-" + juce::Time::getCurrentTime().formatted ("%H%M%S")
                                                 + ".wav");

    const juce::String displayName = utf8 ("渲染 ") + juce::String (scopeRenderSerial);

    auto schedule = std::make_shared<PublishedSchedule> (std::move (extracted.schedule));
    const double start = extracted.startSeconds;
    const int    targetLayer = request.layerIndex;

    juce::Component::SafePointer<MainComponent> safeThis (this);

    auto* task = new ScopeRenderTask (schedule, file, start, &plugins,
                                      [safeThis, targetLayer, lengthBeats, displayName]
                                      (const juce::File& rendered, const bounce::ExportResult& result)
    {
        auto* component = safeThis.getComponent();

        if (component == nullptr)
            return;

        if (! result.ok)
        {
            rendered.deleteFile();
            component->showMessage (utf8 ("渲染失败"),
                                    result.message.isNotEmpty() ? result.message
                                                                : utf8 ("未知错误"),
                                    true);
            return;
        }

        component->insertRenderedSample (rendered, targetLayer, lengthBeats, displayName);
    });

    task->launchThread();
}

void MainComponent::insertRenderedSample (const juce::File& file, int layerIndex,
                                          double lengthBeats, const juce::String& name)
{
    juce::ignoreUnused (lengthBeats);   // the file's own length is the answer

    // All of the arithmetic — where it fits, what it is mounted on — lives in
    // scope::insertSampleBlock. The window's share is the part that needs a
    // window: telling the user, selecting the block, and writing one undo step.
    const auto inserted = scope::insertSampleBlock (stack, rack, layerIndex, file,
                                                    audio.getBpm(), name);

    if (! inserted.ok)
    {
        // Either the layer changed while the bounce ran, or the file did not
        // decode. The audio is kept on disk in both cases: deleting a render the
        // user waited for would be worse than a stray file.
        showMessage (inserted.message.contains (juce::String::fromUTF8 (u8"空位"))
                       ? utf8 ("没有地方可以放") : utf8 ("渲染的文件读不回来"),
                     inserted.message + "\n\n" + file.getFullPathName(),
                     true);
        return;
    }

    pianoRoll.setSampleSelection ({ inserted.sampleId });
    pianoRoll.repaint();
    repaint();

    // One undo step for the whole bounce, like a drop.
    commitHistory();
}

//==============================================================================
void MainComponent::setDemoPlayheadBeats (double beats)
{
    // Paused, not playing: --snapshot has no audio device, and this is exactly
    // what the app looks like the moment after pressing Pause mid-timeline.
    audio.parkAt (beats);

    pianoRoll.setBpm (audio.getBpm());
    pianoRoll.setPlayheadBeat (beats);
    pianoRoll.setTransportActive (true);

    envelopePanel.setPlayheadBeat (beats);
    layerPanel.setPlayheadBeat (beats);
    rackPanel.setPlayheadBeat (beats);

    // Phase 8: the automation window has its own playhead, and it is only
    // meaningful if it is driven by the same transport tick as the roll.
    automationWindow.setPlayheadBeat (beats);

    // Prime the meters by rendering one block *at* the parked position, then
    // putting the playhead back. Without this every meter in a screenshot is a
    // dead track, which says nothing about whether the mix works. This is the
    // level the desk would show with the transport rolling here.
    {
        juce::AudioBuffer<float> block (2, 1024);
        audio.getPlayback().renderNextBlock (block, 1024);
        audio.getPlayback().setPositionBeats (beats);

    }
}

//==============================================================================
void MainComponent::showDemoNoteSelection()
{
    if (auto* layer = stack.getActiveLayer())
    {
        std::vector<int> ids;

        for (const auto& note : layer->getNotes())
            if (ids.size() < 3 && note.groupId < 0)
                ids.push_back (note.id);

        pianoRoll.setSelection (ids);
    }
}

void MainComponent::showDemoSampleSelection()
{
    // Phase 28: a clip, not a note. Clips and notes are mutually exclusive
    // selections, so selecting the block necessarily clears the notes above —
    // which is what the screenshot should show.
    if (auto* layer = stack.getActiveLayer())
        if (! layer->getSamples().empty())
            pianoRoll.setSampleSelection ({ layer->getSamples().front().id });
}

void MainComponent::prepareSnapshotBrowser()
{
    // A fixture tree so the browser image documents the FEATURE (a real
    // hierarchy, four levels deep) rather than whatever directories happen to
    // exist on the machine that built this binary. It lives in the temp folder
    // and is added with persist = false, so the user's folder list is untouched.
    const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("atomic-browser-demo");

    const auto drums  = root.getChildFile ("Drums");
    const auto kicks  = drums.getChildFile ("Kicks");
    const auto hard   = kicks.getChildFile ("Hard");
    const auto snares = drums.getChildFile ("Snares");
    const auto bass   = root.getChildFile ("Bass");
    const auto pads   = root.getChildFile ("Pad");

    if (! root.isDirectory())
    {
        for (auto* dir : { &drums, &kicks, &hard, &snares, &bass, &pads })
            dir->createDirectory();

        writeTestSampleFile (hard.getChildFile ("kick-hard-01.wav"), 0.30, 44100.0, 1, 55.0);
        writeTestSampleFile (kicks.getChildFile ("kick-soft-01.wav"), 0.30, 44100.0, 1, 70.0);
        writeTestSampleFile (snares.getChildFile ("snare-tight.wav"), 0.20, 44100.0, 1, 220.0);
        writeTestSampleFile (bass.getChildFile ("sub-01.wav"), 0.40, 44100.0, 1, 55.0);
        writeTestSampleFile (pads.getChildFile ("warm-pad.wav"), 0.50, 44100.0, 1, 330.0);
        writeTestSampleFile (root.getChildFile ("top-loop.wav"), 0.40, 44100.0, 1, 440.0);
    }

    // The shell's browser is filled from the machine's own folder list. The
    // shot wants THIS tree and nothing else, and the folder list must not be
    // rewritten on the way out.
    filePanel.clearFoldersForSnapshot();
    filePanel.addFolder (root, false);

    // The scan is asynchronous by design; a screenshot needs the result NOW, so
    // this is the one place that waits for it.
    filePanel.waitForPendingScans();

    // Phase 33: the tree now opens FOLDED, so a frame of it would be one row and
    // two triangles. One branch is unfolded here — without touching the real
    // store — so the picture documents BOTH states: Drums open, everything
    // underneath it still closed.
    filePanel.setSubFolderExpanded (drums, true, false);
}

void MainComponent::openDemoNoteAutomation()
{    auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return;

    // The longest note on the active sheet: a box under a 1/16 note would have
    // to be widened anyway, and a demo should show the box sitting on a note it
    // actually matches in width.
    const NoteEvent* best = nullptr;

    for (const auto& note : layer->getNotes())
        if (best == nullptr || note.lengthBeats > best->lengthBeats)
            best = &note;

    if (best != nullptr)
        pianoRoll.openNoteAutomation (stack.getActiveLayerIndex(), best->id);
}

void MainComponent::openDemoGroupAutomation()
{
    // Phase 21: the group's version of the call above, for the snapshot. Opens
    // the box on the biggest group of the active sheet and makes sure its level
    // curve has a shape to photograph — a flat line would leave the reader
    // wondering whether the box works at all.
    pianoRoll.closeNoteAutomation();

    auto* layer = stack.getActiveLayer();

    if (layer == nullptr || layer->getGroups().empty())
        return;

    const auto& groups = layer->getGroups();

    const int index = stack.getActiveLayerIndex();

    // Widest group gives the most readable curve, because the box is as wide as
    // the group's own beat span.
    //
    // P82: "widest" is now "widest that fits". The box is exactly the span, so a
    // group wider than the roll produces a box whose right edge — and with it
    // the route dropdown's caret — is clipped off the edge of the component.
    // A frame that documents two dropdowns cannot afford to show one and a half.
    const double visible = pianoRoll.getVisibleBeats();
    const double fitCap  = visible * 0.80;

    int    bestGroup = groups.front().getId();
    double bestSpan  = -1.0;
    double bestLo    = 0.0;

    for (int pass = 0; pass < 2 && bestSpan < 0.0; ++pass)
    {
        for (const auto& group : groups)
        {
            double lo = 1.0e9, hi = -1.0e9;

            for (const auto& note : layer->getNotes())
                if (note.groupId == group.getId())
                {
                    lo = juce::jmin (lo, note.startBeat);
                    hi = juce::jmax (hi, note.endBeat());
                }

            const double span = hi - lo;

            if (span <= 0.0 || (pass == 0 && span > fitCap))
                continue;

            if (span > bestSpan)
            {
                bestSpan  = span;
                bestGroup = group.getId();
                bestLo    = lo;
            }
        }
    }

    pianoRoll.openGroupAutomation (index, bestGroup, 0);

    // Put the head of the chosen group just inside the left edge, so the box
    // lands centred-ish instead of running out of the right side.
    pianoRoll.setScrollBeats (juce::jmax (0.0, bestLo - visible * 0.08));

    if (auto* group = layer->findGroup (bestGroup))
    {
        auto& lane = group->automationFor (GroupAutomationTarget::level);

        if (lane.getPoints().size() < 3)
        {
            // A ramp across the group: quiet at the head, full at the tail. Any
            // curve would do; this one is obviously a curve.
            double lo = 0.0, hi = 4.0;

            for (const auto& note : layer->getNotes())
                if (note.groupId == bestGroup)
                {
                    lo = note.startBeat;
                    hi = juce::jmax (hi, note.endBeat());
                }

            lane.resetFlat (0.35f, pianoRoll.getTimelineBeats());
            lane.addPoint (lo + (hi - lo) * 0.5, 0.55f, pianoRoll.getTimelineBeats());
            lane.addPoint (hi, 0.95f, pianoRoll.getTimelineBeats());
            lane.setEnabled (true);
        }
    }
    
    // P82: a SECOND curve, parked on 空 and made the one on screen. The frame
    // has to show two independent facts in one row — the name on the left and
    // the destination on the right — and "a curve that drives nothing" is the
    // state that proves the two are not the same statement written twice.
    if (auto* group = layer->findGroup (bestGroup))
    {
        auto& bank = group->getCurveBank();

        // Idempotent: this function runs once per snapshot frame, and eight
        // copies of one demo curve would be a worse picture than none.
        if (bank.getNumCurves() < 4)
        {
            const int extraId = bank.add (AutomationLane(), utf8 ("手绘"));

            if (extraId >= 0)
            {
                double lo = 0.0, hi = 4.0;

                for (const auto& note : layer->getNotes())
                    if (note.groupId == bestGroup)
                    {
                        lo = note.startBeat;
                        hi = juce::jmax (hi, note.endBeat());
                    }

                auto& lane = bank.find (extraId)->curve;
                lane.resetFlat (0.25f, pianoRoll.getTimelineBeats());
                lane.addPoint (lo + (hi - lo) * 0.35, 0.90f, pianoRoll.getTimelineBeats());
                lane.addPoint (lo + (hi - lo) * 0.70, 0.20f, pianoRoll.getTimelineBeats());
                lane.setEnabled (true);
                bank.setActiveId (extraId);
            }
        }
    }

    stack.notifyNotesChanged();
    repaint();
}

//==============================================================================
void MainComponent::openEnvelopePanel (int layerIndex, int noteId)
{
    auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr || layer->findNote (noteId) == nullptr)
        return;

    envelopePanel.openFor (layerIndex, noteId);

    // Place the panel beside the note, then clamp it inside the window so it
    // never hangs half off the edge.
    const auto noteArea = pianoRoll.getNoteScreenBounds (layerIndex, noteId)
                            .getSmallestIntegerContainer()
                          + pianoRoll.getPosition();

    const int w = NoteEnvelopePanel::panelWidth;
    const int h = NoteEnvelopePanel::panelHeight;

    int x = noteArea.getRight() + 16;

    if (x + w > getWidth() - 12)
        x = noteArea.getX() - w - 16;

    if (x < LayerPanel::panelWidth + 12)
        x = LayerPanel::panelWidth + 12;

    int y = noteArea.getBottom() + 12;

    if (y + h > getHeight() - 12)
        y = noteArea.getY() - h - 12;

    y = juce::jlimit (topBarBounds.getBottom() + 12,
                      juce::jmax (topBarBounds.getBottom() + 12, getHeight() - h - 12), y);

    envelopePanel.setBounds (x, y, w, h);
    envelopePanel.toFront (true);
}

void MainComponent::openSampleEnvelopePanel (int layerIndex, int sampleId)
{
    auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr || layer->findSample (sampleId) == nullptr)
        return;

    sampleEnvelopePanel.openFor (layerIndex, sampleId);

    // Same placement rule as the note inspector: beside the block, then clamped
    // inside the window. A stacked clip that sits near the edge is the common
    // case here, so the flip-to-the-other-side branch earns its keep.
    auto blockArea = pianoRoll.getSampleScreenBounds (layerIndex, sampleId)
                       .getSmallestIntegerContainer();

    if (blockArea.isEmpty())
        blockArea = juce::Rectangle<int> (pianoRoll.getX() + pianoRoll.getWidth() / 2,
                                          pianoRoll.getY() + pianoRoll.getHeight() / 2, 1, 1);
    else
        blockArea += pianoRoll.getPosition();

    const int w = SampleEnvelopePanel::panelWidth;
    const int h = SampleEnvelopePanel::panelHeight;

    int x = blockArea.getRight() + 16;

    if (x + w > getWidth() - 12)
        x = blockArea.getX() - w - 16;

    if (x < LayerPanel::panelWidth + 12)
        x = LayerPanel::panelWidth + 12;

    int y = blockArea.getBottom() + 12;

    if (y + h > getHeight() - 12)
        y = blockArea.getY() - h - 12;

    y = juce::jlimit (topBarBounds.getBottom() + 12,
                      juce::jmax (topBarBounds.getBottom() + 12, getHeight() - h - 12), y);

    sampleEnvelopePanel.setBounds (x, y, w, h);
    sampleEnvelopePanel.toFront (true);
}

void MainComponent::openDemoRecording()
{
    // The take is performed through the production path: begin, feed, commit.
    // A demo that built the block and the group by hand would prove that the
    // model accepts them, not that recording produces them.
    constexpr double startBeat = 4.0;
    constexpr double takeBeats = 3.0;

    // The hero shot already has a note inspector open over beats 4-6, which is
    // exactly where the take lands. Two "look at this" marks on one spot is
    // none: the take gets the space.
    envelopePanel.close();

    beginTakeForTest (startBeat);

    // In the middle of the visible keyboard and above the block's own rows: the
    // demo's written notes sit between 8 and 24 beats, so beats 4-7 are free
    // here — and a take at the very top of the window would be covered by the
    // very automation box that proves the curve landed.
    const int pitches[] { 67, 71, 74, 79 };

    for (int i = 0; i < 4; ++i)
    {
        recordNoteAt (pitches[i], 0.86f, true,  startBeat + i * 0.75);
        recordNoteAt (pitches[i], 0.86f, false, startBeat + i * 0.75 + 0.7);
    }

    // A hand on the fader: up over the first half of the take, easing back.
    for (int i = 0; i <= 8; ++i)
        recordControlAt (7, 78 + (i % 4) * 14, startBeat + i * (takeBeats / 8.0));

    recordControlAt (10, 44, startBeat);
    recordControlAt (10, 84, startBeat + takeBeats);

    // 1.5 s of tone where the microphone would be. The snapshot has no device,
    // and a silent take would land a block with no waveform in it.
    constexpr double rate = 48000.0;
    juce::AudioBuffer<float> voice (2, (int) (1.5 * rate));

    for (int i = 0; i < voice.getNumSamples(); ++i)
    {
        const double t     = (double) i / rate;
        const double fade  = std::exp (-2.2 * t / 1.5);
        const double value = std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * t) * fade * 0.55;

        voice.setSample (0, i, (float) value);
        voice.setSample (1, i, (float) value);
    }

    takeAudioForTest (voice, rate);

    // The demo's WAV goes to the temp folder, not to Music and not to docs/:
    // a fixture that drops files into the repository is a fixture that keeps
    // showing up in `git status`.
    setRecordingsFolder (juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("AtomicDAW-demo-takes"));

    const int groupId = endTakeForTest (startBeat + takeBeats);

    locatePlayhead (startBeat);

    if (groupId > 0)
    {
        pianoRoll.selectGroup (groupId);

        // Open the group's own curve box. A still cannot show "the automation
        // went into the group" any other way: the lane lives in the model, and
        // the box is the one place it is drawn as a curve.
        pianoRoll.openGroupAutomation (stack.getActiveLayerIndex(), groupId,
                                       (int) GroupAutomationTarget::level);
    }

    repaint();
}

void MainComponent::openDemoTempoAutomation()
{
    showTempoAutomation();

    // To the TOP of the list: a project-level row appended as the seventh one
    // sits below the fold in a 312 px pane, and the screenshot has to show the
    // row, not the fact that it exists.
    const int tempoTrackId = automation.findTrackId (AutomationOwner::tempo, 0, 0);

    if (tempoTrackId > 0)
    {
        automation.moveTrack (tempoTrackId, -automation.getNumTracks());

        // The list was scrolled to the bottom to reveal the row when it was
        // appended, and a reorder does not move the scroll with it — without
        // this second reveal the row it just moved would be the one cut off.
        automationWindow.revealTrack (tempoTrackId);
    }

    constexpr double len = timeline::defaultLengthBeats;
    auto& lane = tempo.getAutomation();

    // 90 BPM to start, 120 a little later, 132 at the crest, easing back to 96 —
    // a shape with a middle, a crest and a tail, so the row shows curvature
    // rather than one straight line. The beats sit inside the first two bars on
    // purpose: the lane scrolls with the piano roll, whose snapshot is zoomed to
    // roughly nine beats, and a crest at beat 32 is a crest nobody can see.
    lane.resetFlat (tempo::valueForBpm (90.0), len);                 // {0,90} .. {64,90}
    lane.addPoint (2.0, tempo::valueForBpm (120.0), len);            // 索引 1
    lane.addPoint (4.0, tempo::valueForBpm (132.0), len);            // 索引 2
    lane.movePoint (3, len, tempo::valueForBpm (96.0), len);         // 尾巴（索引 3）

    tempo.setAutomated (true);
    republish();
}

void MainComponent::openDemoRename()
{
    // The second sheet if there is one: renaming the top row would put the box
    // where the eye already is, and the point of the frame is the row you are
    // NOT looking at.
    layerPanel.beginRenameLayer (stack.getNumLayers() > 1 ? 1 : 0);
}

//==============================================================================
void MainComponent::renameSynthSlot (int slotId, const juce::String& newName)
{
    auto* slot = rack.findSlot (slotId);

    if (slot == nullptr)
        return;

    // The word the blocks are still wearing. Read BEFORE the rack changes it,
    // and read from `name` rather than getDisplayName(): a block's name is a
    // copy of `name` (that is how a dropped file's block gets its title), so
    // `name` is the thing the copies can be told apart from.
    const auto previous = slot->name;

    rack.setSlotName (slotId, newName);

    if (slot->name == previous)
        return;      // refused, or the same word — nothing to carry across

    //-- Follow the blocks ---------------------------------------------------
    // A block is a window onto a sampler, and its title is that sampler's name:
    // the two are the same word by construction (see PianoRollComponent::
    // sampleLabelText). Renaming the sampler therefore renames its blocks.
    //
    // The test is `sample.name == previous`, not `outputSlotId == slotId`
    // alone: a block that was once renamed by hand, or one dropped from a file
    // and then pointed at this sampler, carries its OWN name and must keep it.
    // Only the copies still wearing the old word follow.
    bool followed = false;

    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        auto* layer = stack.getLayer (i);

        if (layer == nullptr)
            continue;

        for (auto& sample : layer->getSamples())
        {
            if (sample.outputSlotId != slotId || sample.name != previous)
                continue;

            sample.name = slot->name;
            followed    = true;
        }
    }

    if (followed)
        stack.notifyNotesChanged();
}

void MainComponent::openDemoEnvelopePanel()
{
    if (demoOverrideLayer >= 0 && demoOverrideNote >= 0
         && stack.getLayer (demoOverrideLayer) != nullptr
         && stack.getLayer (demoOverrideLayer)->findNote (demoOverrideNote) != nullptr)
    {
        openEnvelopePanel (demoOverrideLayer, demoOverrideNote);
        return;
    }

    if (auto* layer = stack.getActiveLayer())
        if (! layer->getNotes().empty())
            openEnvelopePanel (stack.getActiveLayerIndex(), layer->getNotes().front().id);
}

void MainComponent::showDemoRoutingOverride()
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr || layer->getNotes().empty() || rack.getNumSlots() < 2)
        return;

    int otherSlot = -1;

    for (int i = 0; i < rack.getNumSlots(); ++i)
        if (rack.getSlotIdAt (i) != layer->getOutputSlotId())
        {
            otherSlot = rack.getSlotIdAt (i);
            break;
        }

    if (otherSlot < 0)
        return;

    // Two notes, not all of them: "borrowed a couple of notes onto another
    // synth" is the state this button exists for, and the 复位 pill says how
    // many there are. It reads 3 here, not 2: buildDemoContent already puts one
    // note on Synth B by itself (beat 6), and the count is honest about that.
    auto& notes = layer->getNotes();
    const int count = juce::jmin (2, (int) notes.size());

    for (int i = 0; i < count; ++i)
        notes[(size_t) i].outputSlotOverride = otherSlot;

    stack.notifyNotesChanged();
    republish();
    updateRackRoutingFocus();

    // Phase 30: this used to park the spotlight on the borrowed synth. It no
    // longer does — the demo is the picture of what a project looks like at
    // rest, and a permanent highlight badge in the corner of it says "you are
    // in a mode" when nobody clicked anything. The override pill above already
    // states the routing in words ("3 notes on Synth B").

    layerPanel.repaint();
    rackPanel.repaint();
    pianoRoll.repaint();
    repaint();
}

void MainComponent::showDemoConsoleRouting()
{
    // Phase 23 photo staging: 通道 1 → 通道 2 → 通道 3 → 主输出. That chain is
    // the requirement in one glance (two strips feeding a third, which feeds the
    // master), and a console where every chip reads "主输出" documents nothing
    // about the feature.
    //
    // Deliberately done AFTER the self-test, like every other staging helper
    // here: it changes the audible topology, and the audio checks must keep
    // running against the plain console they were written for.
    if (mixer.getChannelCount() < 3)
        return;

    mixer.setChannelOutput (0, 1);
    mixer.setChannelOutput (1, 2);

    republish();
    mixerPanel.repaint();
    repaint();
}

} // namespace atomic
