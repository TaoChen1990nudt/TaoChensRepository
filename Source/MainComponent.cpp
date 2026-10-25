#include "MainComponent.h"

#include "Util/Metrics.h"

#include "Model/Routing.h"
#include "UI/MixerLook.h"
#include "Util/Text.h"

#include <algorithm>
#include <cmath>

#include <cstring>

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

    const juce::Colour kAppBackground { 0xff0d0e11 };
    const juce::Colour kTopBar        { 0xff16171c };
    const juce::Colour kDivider       { 0xff262832 };
    const juce::Colour kAccent        { 0xff4fd1c5 };
    const juce::Colour kPlayhead      { 0xffff5f6d };

    juce::Font boldFont (float height)
    {
        return juce::Font (juce::FontOptions{}.withHeight (atomic::ui::sp (height)).withStyle ("Bold"));
    }

    /** The square a transport glyph is drawn into.

        Derived from the button, never from a fixed inset. Phase 13 grew the
        transport buttons to 36 px and the old `y + 12 … bottom - 12` play
        triangle collapsed into a 12 x 4 sliver, while the pause bars stayed
        anchored to the same fixed offset and sank to the bottom of the plate.
        Both were the same mistake: a glyph has to scale with its button and
        sit on its centre. */
    juce::Rectangle<float> transportIconBox (juce::Rectangle<float> button, float fraction)
    {
        const float side = button.getHeight() * fraction;

        return juce::Rectangle<float> (side, side).withCentre (button.getCentre());
    }

    /** Play / pause / stop share one glyph size so the three read as a set. */
    constexpr float kTransportGlyph = 0.46f;

    /** The three glyphs, defined once so the self-test measures the same
        geometry the paint routine fills. */
    juce::Path playGlyph (juce::Rectangle<float> button)
    {
        const auto  icon  = transportIconBox (button, kTransportGlyph);
        const float width = icon.getWidth() * 0.88f;

        // Nudged right by a tenth of its own width: a right-pointing arrow
        // carries its mass on the left, so a literally centred one reads as
        // sitting off-centre inside the plate.
        const float left = icon.getCentreX() - width * 0.5f + width * 0.10f;

        juce::Path triangle;
        triangle.addTriangle (left, icon.getY(),
                              left, icon.getBottom(),
                              left + width, icon.getCentreY());
        return triangle;
    }

    struct PauseGlyph
    {
        juce::Rectangle<float> left, right;
    };

    PauseGlyph pauseGlyph (juce::Rectangle<float> button)
    {
        const auto  icon = transportIconBox (button, kTransportGlyph);
        const float barW = icon.getWidth() * 0.30f;
        const float gap  = icon.getWidth() * 0.24f;
        const float x0   = icon.getCentreX() - (barW * 2.0f + gap) * 0.5f;

        return { { x0, icon.getY(), barW, icon.getHeight() },
                 { x0 + barW + gap, icon.getY(), barW, icon.getHeight() } };
    }

    juce::Rectangle<float> stopGlyph (juce::Rectangle<float> button)
    {
        const auto  icon = transportIconBox (button, kTransportGlyph);
        const float side = icon.getWidth() * 0.86f;

        return juce::Rectangle<float> (side, side).withCentre (icon.getCentre());
    }

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
                    std::function<void()> onFinished)
            : juce::ThreadWithProgressWindow (utf8 ("正在导出音频"), true, true, 20000, utf8 ("取消")),
              schedule (std::move (scheduleToRender)),
              destination (std::move (destinationFile)),
              finishedCallback (std::move (onFinished))
        {
        }

        void run() override
        {
            setStatusMessage (utf8 ("离线渲染中…"));

            result = wav::renderToFile (destination, *schedule, wav::exportSampleRate, 0.05,
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
                      + juce::String (result.seconds, 2) + utf8 (" 秒 · 44.1 kHz · 16-bit 立体声")
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
        std::function<void()> finishedCallback;
        wav::ExportResult result;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ExportTask)
    };

    /** Fills a popup with vendor submenus.

        A machine with Arturia, u-he and Steinberg installed has 90 instruments;
        one flat scrolling column of them is a wall nobody wants to read. Ids are
        `firstId + index` into `entries`, so the caller's switch is unchanged. */
    void addPluginItems (juce::PopupMenu& menu,
                         const std::vector<juce::PluginDescription>& entries,
                         int firstId)
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
                    sub.addItem (firstId + i, entries[(size_t) i].name);

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
    lastY    = e.getPosition().y;
    repaint();
}

void VerticalSplitter::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging)
        return;

    const int dy = e.getPosition().y - lastY;

    if (dy == 0)
        return;

    lastY += dy;

    if (onDrag != nullptr)
        onDrag (dy);
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

    splitterBar.onDrag = [this] (int deltaY)
    {
        // Dragging DOWN pushes the automation window down, so it gets shorter.
        setAutomationPaneHeight (automationPaneHeight - deltaY);
    };

    // The mixer lives in its own window (Phase 8), so it is NOT a child here.
    // It is still a member: the engine, the meters and the undo snapshots all
    // talk to the panel, never to the window.
    mixerPanel.setSize (ui::px (1180), ui::px (300));

    bpmField.setBpm (audio.getBpm(), false);
    bpmField.onBpmChanged = [this] (double newBpm) { setBpm (newBpm); };

    // The inspector floats above everything and starts hidden.
    addChildComponent (envelopePanel);

    pianoRoll.onNoteDoubleClicked = [this] (int layerIndex, int noteId)
    {
        openEnvelopePanel (layerIndex, noteId);
    };

    // Clicking the canvas dismisses the inspector, like any popover.
    pianoRoll.onCanvasMouseDown = [this]
    {
        envelopePanel.close();
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

    rackPanel.onCardMenuRequested = [this] (int slotId, juce::Point<int> pos)
    {
        showRackCardMenu (slotId, pos);
    };

    rackPanel.onSlotChosen = [this] (int slotId)
    {
        // "Put the notes I have selected on this synth." A per-note override,
        // not a change to the layer: one exception should stay one exception.
        pianoRoll.routeSelectionTo (slotId);
        republish();
        focusSynth (slotId);
        layerPanel.repaint();
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

    pianoRoll.onSelectionChanged = [this]
    {
        updateRackRoutingFocus();
    };

    //-- Phase 16: group actions the roll cannot finish on its own -----------
    pianoRoll.onGroupAutomationRequested = [this] (int, int groupId, int target)
    {
        ensureAutomationTrack (AutomationOwner::group, groupId, target);
    };

    pianoRoll.onGroupMountRequested = [this] (int layerIndex, int groupId, juce::Point<int> pos)
    {
        showGroupMountMenu (layerIndex, groupId, pos);
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
        menu.addItem (1, utf8 ("创建自动化 · 组音量"), true,
                      automation.contains (AutomationOwner::group, groupId,
                                           (int) GroupAutomationTarget::level));
        menu.addItem (5, utf8 ("创建自动化 · 组声相"), true,
                      automation.contains (AutomationOwner::group, groupId,
                                           (int) GroupAutomationTarget::pan));
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

    mixerPanel.onInsertOpenRequested = [this] (int channel, int slot)
    {
        openInsertEditor (channel, slot);
    };

    layerPanel.onResetNoteRouting = [this] (int layerIndex)
    {
        resetLayerNoteRouting (layerIndex);
    };

    mixerPanel.levelSource = [this] (int channel)
    {
        // Phase 11: a strip's meter is the CHANNEL's signal (post-insert), not a
        // layer's. A synth that bypasses the console never lights a strip —
        // which is the honest reading: nothing is arriving there.
        return channel == MixerModel::masterChannel
                 ? audio.getPlayback().getMasterPeak (0)
                 : audio.getPlayback().getChannelPeak (channel);
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
    pianoRoll.onAuditionRequested = [this] (int slotId, int pitch, float velocity, bool retrigger)
    {
        audio.getPlayback().auditionNote (pitch, velocity, slotId, retrigger);
    };

    cancelPendingUpdate();                   // building the blank session is not an edit
    history.clear();
    history.push_back (captureState());
    historyPos = 0;
    savedStateText = history.front();        // nothing to save yet
    dirtyCache     = false;

    // 60 Hz: the playhead is the one thing on screen that has to look like it
    // is moving, and 30 Hz reads as a stutter on a thin bright line.
    startTimerHz (60);
}

MainComponent::~MainComponent()
{
    stopTimer();
    cancelPendingUpdate();

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
    return project::toXmlText (stack, rack, audio.getBpm(), &mixer, &automation);
}

void MainComponent::modelChanged()
{
    if (restoringHistory)
        return;

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

void MainComponent::undo()
{
    // Flush whatever gesture is still in flight first, or Ctrl+Z right after a
    // drag would undo the state *before* the drag and then store the drag on
    // top of it.
    cancelPendingUpdate();
    commitHistory();

    if (! canUndo())
        return;

    --historyPos;
    applyHistoryPosition();
}

void MainComponent::redo()
{
    cancelPendingUpdate();
    commitHistory();

    if (! canRedo())
        return;

    ++historyPos;
    applyHistoryPosition();
}

void MainComponent::applyHistoryPosition()
{
    const juce::String xml = history[(size_t) historyPos];

    restoringHistory = true;
    restoreState (xml);
    restoringHistory = false;

    dirtyCache = (xml != savedStateText);

    repaint();
}

void MainComponent::restoreState (const juce::String& xml)
{
    double bpmValue = audio.getBpm();
    juce::String error;

    if (! project::fromXmlText (xml, stack, rack, bpmValue, error, &mixer, &automation, &plugins))
        return;

    setBpm (bpmValue);
    bpmField.setBpm (bpmValue, false);

    envelopePanel.close();
    pianoRoll.closeNoteAutomation();
    republish();

    layerPanel.repaint();
    rackPanel.repaint();
    automationWindow.repaint();
    mixerPanel.repaint();
    pianoRoll.repaint();
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

//==============================================================================
//  Phase 8: new project
//==============================================================================
void MainComponent::buildSession (bool withEmptyLayer)
{
    envelopePanel.close();
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
        settingsWindow = std::make_unique<SettingsWindow> (plugins, audio.getDeviceManager());

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

void MainComponent::showRackCardMenu (int slotId, juce::Point<int> screenPosition)
{
    auto* slot = rack.findSlot (slotId);

    if (slot == nullptr)
        return;

    ensurePluginScan();

    std::vector<juce::PluginDescription> instruments;

    for (const auto& entry : plugins.getLibrary())
        if (entry.isInstrument)
            instruments.push_back (entry.description);

    juce::PopupMenu menu;
    menu.addSectionHeader (slot->name + utf8 (" · 输出到 ")
                             + (slot->sendsToMixer() ? utf8 ("混音器通道 ")
                                                       + juce::String (slot->mixerChannel + 1)
                                                     : utf8 ("主输出")));

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
        addPluginItems (vst, instruments, 200);

    replace.addSeparator();
    replace.addSubMenu (utf8 ("VST3 乐器"), vst, ! instruments.empty());

    menu.addSubMenu (utf8 ("替换音源"), replace, true);

    //-- Console send ---------------------------------------------------------
    juce::PopupMenu send;
    send.addItem (20, utf8 ("不发送（直接到主输出）"), true, ! slot->sendsToMixer());

    for (int ch = 0; ch < MixerModel::numChannels; ++ch)
        send.addItem (21 + ch, utf8 ("通道 ") + juce::String (ch + 1), true,
                      slot->mixerChannel == ch);

    menu.addSubMenu (utf8 ("发送到混音器通道"), send, true);

    //-- Automation -----------------------------------------------------------
    // Phase 14: no badge on the card any more, so the card menu is the only
    // way in. Same wording as the layer row's menu: the item names both the
    // action and the parameter it will automate.
    menu.addItem (30, utf8 ("创建自动化 · 滤波器"), true,
                  automation.contains (AutomationOwner::slot, slotId, 0));
    menu.addItem (31, utf8 ("创建自动化 · 音量"), true,
                  automation.contains (AutomationOwner::slot, slotId, 1));
    menu.addItem (32, slot->automationExpanded ? utf8 ("收起面板中的曲线")
                                               : utf8 ("在面板中展开曲线"), true);

    menu.addSeparator();

    //-- Phase 16: the explicit form of the old click = re-route gesture ------
    // A plain click now spotlights the synth instead of moving notes onto it,
    // so the destructive half needs a place where it says what it does.
    menu.addItem (33, utf8 ("把选中音符挂到该音源"), ! pianoRoll.getSelection().empty());
    menu.addItem (34, utf8 ("高亮这个音源"), true, focusedSlotId == slotId);

    menu.addSeparator();
    menu.addItem (40, utf8 ("删除这个音源"), rack.getNumSlots() > 1);

    juce::Component::SafePointer<MainComponent> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (
                            juce::Rectangle<int> (screenPosition.x, screenPosition.y, 1, 1)),
                        [safe, slotId, instruments] (int result)
    {
        if (safe == nullptr || result <= 0)
            return;

        if (safe->rack.findSlot (slotId) == nullptr)
            return;                 // the card was deleted while the menu was up

        if (result >= 10 && result <= 13)
        {
            static const Waveform bench[4] { Waveform::sine, Waveform::saw,
                                             Waveform::square, Waveform::triangle };

            // The slot id survives, so every layer route, note override and
            // automation track still points at the same instrument.
            safe->rack.replaceWithNative (slotId, bench[result - 10]);
            safe->reconcilePlugins();          // the old instance has no owner now
            safe->rackPanel.repaint();
            safe->repaint();
            return;
        }

        if (result == 20)
        {
            safe->rack.setMixerChannel (slotId, -1);
            safe->mixerPanel.repaint();
            return;
        }

        if (result >= 21 && result < 21 + MixerModel::numChannels)
        {
            const int channel = result - 21;
            safe->rack.setMixerChannel (slotId, channel);
            safe->mixerPanel.setSelectedChannel (channel);   // show where it went
            safe->mixerPanel.repaint();
            return;
        }

        if (result == 30 || result == 31)
        {
            safe->ensureAutomationTrack (AutomationOwner::slot, slotId, result - 30);
            return;
        }

        if (result == 32)
        {
            auto* current = safe->rack.findSlot (slotId);

            if (current != nullptr)
                safe->rack.setAutomationExpanded (slotId, ! current->automationExpanded);

            return;
        }

        if (result == 33)
        {
            safe->pianoRoll.routeSelectionTo (slotId);
            safe->republish();
            safe->focusSynth (slotId);
            safe->layerPanel.repaint();
            safe->rackPanel.repaint();
            return;
        }

        if (result == 34)
        {
            safe->focusSynth (slotId);
            return;
        }

        if (result == 40)
        {
            safe->rack.removeSlot (slotId);
            safe->reconcilePlugins();
            safe->republish();
            safe->rackPanel.repaint();
            safe->mixerPanel.repaint();
            return;
        }

        const int index = result - 200;

        if (! juce::isPositiveAndBelow (index, (int) instruments.size()))
            return;

        juce::String error;
        const int instanceId = safe->plugins.load (instruments[(size_t) index], &error);

        if (instanceId <= 0)
        {
            safe->showMessage (utf8 ("无法载入插件"),
                               instruments[(size_t) index].name + "\n\n"
                                 + (error.isNotEmpty() ? error : utf8 ("插件格式不支持或文件已损坏")),
                               true);
            return;
        }

        safe->rack.replaceWithPlugin (slotId, instruments[(size_t) index], instanceId);
        safe->reconcilePlugins();
        safe->rackPanel.repaint();
        safe->repaint();
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

    insertWindows.push_back (std::move (window));
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

void MainComponent::updateRackRoutingFocus()
{
    // The selection is the *default* driver of the spotlight: picking notes on
    // a sheet should light up the synth they play on. Clicking a card sets the
    // focus explicitly and does not go through here, so the card cannot snap
    // back to the selection it was just asked to ignore.
    if (! pianoRoll.getSelection().empty())
    {
        const int resolved = pianoRoll.getSelectedRoutingSlotId();

        if (resolved >= 0)
            focusedSlotId = resolved;
    }

    applyFocus();
}

void MainComponent::focusSynth (int slotId)
{
    focusedSlotId = (slotId >= 0 && rack.findSlot (slotId) != nullptr) ? slotId : -1;
    applyFocus();
}

void MainComponent::applyFocus()
{
    // A deleted synth must not stay spotlighted: the id would resolve to
    // nothing and the roll would dim the whole grid for a synth that is gone.
    if (focusedSlotId >= 0 && rack.findSlot (focusedSlotId) == nullptr)
        focusedSlotId = -1;

    rackPanel.setRoutingFocus (focusedSlotId, focusedSlotId >= 0);
    pianoRoll.setFocusSlotId (focusedSlotId);
    layerPanel.setFocusSlotId (focusedSlotId);
    groupPanel.setFocusSlotId (focusedSlotId);
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

        menu.addItem (10 + slot->id, slot->name, true,
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
void MainComponent::layersChanged()             { modelChanged(); }
void MainComponent::notesChanged()              { modelChanged(); }
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
void MainComponent::automationTracksChanged()   { modelChanged(); }

//==============================================================================
bool MainComponent::startAudio()
{
    const bool running = audio.start();

    repaint (topBarBounds);
    return running;
}

void MainComponent::playTransport()
{
    audio.play();
    pianoRoll.setTransportActive (audio.isTransportEngaged());

    rackPanel.repaint();
    repaint (topBarBounds);
}

void MainComponent::pauseTransport()
{
    audio.pause();

    const double beats = audio.getPositionBeats();

    pianoRoll.setPlayheadBeat (beats);
    pianoRoll.setTransportActive (audio.isTransportEngaged());
    layerPanel.setPlayheadBeat (beats);
    rackPanel.setPlayheadBeat (beats);
    automationWindow.setPlayheadBeat (beats);

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
{    audio.stopTransport();

    pianoRoll.setPlayheadBeat (0.0);
    pianoRoll.setTransportActive (false);
    envelopePanel.setPlayheadBeat (-1.0);
    layerPanel.setPlayheadBeat (-1.0);
    rackPanel.setPlayheadBeat (-1.0);
    automationWindow.setPlayheadBeat (-1.0);

    rackPanel.repaint();
    repaint (topBarBounds);
}

void MainComponent::togglePlay()
{
    if (audio.isPlaying())
        pauseTransport();
    else
        playTransport();
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

void MainComponent::timerCallback()
{
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
    rackPanel.updateMeters();
    mixerPanel.updateMeters();

    // Only the numbers that actually change are repainted, not the whole bar.
    repaint (getPositionReadoutBounds().expanded (2, 0));
    repaint (getMasterMeterBounds());
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

            layer->addSample (std::move (clip));
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
    audio.getPlayback().publishSchedule (stack, rack, audio.getBpm(), 0.0, &mixer);
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
    audio.getPlayback().publishSchedule (stack, rack, audio.getBpm(), 0.0, &mixer);
    automationWindow.repaint();
    mixerPanel.repaint();
}

void MainComponent::showChannelMenu (int channel, juce::Point<int> screenPos)
{
    const bool isMaster  = channel == MixerModel::masterChannel;
    const int  slotId    = isMaster ? -1 : mixerPanel.slotIdOnChannel (channel);
    const bool hasSource = isMaster || slotId >= 0;
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

    juce::PopupMenu menu;
    menu.addSectionHeader (title);

    if (isMaster)
    {
        menu.addItem (1, utf8 ("创建自动化 · 主输出音量"), true,
                      automation.contains (AutomationOwner::master, 0, 0));
        menu.addItem (2, utf8 ("创建自动化 · 主输出声相"), true,
                      automation.contains (AutomationOwner::master, 0, 1));
    }
    else
    {
        // Phase 18 / item 10: a channel with no source GREYS the automation
        // items instead of answering with a dialog. The menu then says both
        // "this is where automation lives" and "there is nothing to automate
        // yet" in one glance, and never steals focus to say it.
        menu.addItem (1, utf8 ("创建自动化 · 滤波器"), hasSource,
                      hasSource && automation.contains (AutomationOwner::slot, slotId, 0));
        menu.addItem (2, utf8 ("创建自动化 · 音量"), hasSource,
                      hasSource && automation.contains (AutomationOwner::slot, slotId, 1));
    }

    menu.addSeparator();
    menu.addItem (20, utf8 ("重置通道条"), true);

    if (! isMaster)
    {
        // Only the last strip can be deleted: a channel is an INDEX, and
        // renumbering a live one would silently re-point every synth routed to
        // it. Growing and shrinking therefore always happens at the top end.
        menu.addItem (21, utf8 ("删除通道条"),
                      count > MixerModel::minChannels && channel == count - 1);
        menu.addItem (22, utf8 ("增加通道条"), count < MixerModel::maxChannels);
    }

    juce::Component::SafePointer<MainComponent> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetScreenArea (juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1))
                            .withMinimumWidth (ui::px (170)),
                        [safe, channel, slotId, isMaster] (int result)
                        {
                            if (safe == nullptr || result <= 0)
                                return;

                            if (result == 1 || result == 2)
                            {
                                const int target = result - 1;

                                if (isMaster)
                                    safe->ensureAutomationTrack (AutomationOwner::master, 0, target);
                                else if (slotId >= 0)
                                    safe->ensureAutomationTrack (AutomationOwner::slot, slotId, target);

                                return;
                            }

                            if (result == 20)
                            {
                                safe->mixerPanel.resetStrip (channel);
                                safe->repaint();

                                return;
                            }

                            if (result == 21)
                            {
                                safe->mixerPanel.deleteLastStrip();
                                safe->mixerPanel.setSelectedChannel (
                                    juce::jmin (safe->mixer.getChannelCount() - 1,
                                                safe->mixerPanel.getSelectedChannel()));
                                safe->repaint();

                                return;
                            }

                            if (result == 22)
                            {
                                safe->mixer.addChannel();
                                safe->repaint();
                            }
                        });
}

void MainComponent::showAutomationMenu (AutomationOwner owner, int ownerId, juce::Point<int> screenPos)
{
    // The master bus is the one owner whose id is legitimately 0.
    if (owner != AutomationOwner::master && ownerId <= 0)
        return;

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
        ownerName = slot->name;
    }

    // Phase 14: no more "自动化" dropdown badges anywhere. The only way to
    // create a track is a context menu on the control itself, and the item
    // spells out what it will create. The tick means "this curve already has a
    // row", so the menu doubles as the place to find an existing curve.

    juce::PopupMenu menu;
    menu.addSectionHeader (ownerName + (owner == AutomationOwner::layer ? utf8 (" · 图层自动化")
                                        : owner == AutomationOwner::group ? utf8 (" · 编组自动化")
                                        : owner == AutomationOwner::master ? utf8 (" · 主输出自动化")
                                                                          : utf8 (" · 音源自动化")));

    if (owner == AutomationOwner::group)
    {
        // Phase 20: a group has two automatable parameters now, so it is listed
        // exactly like a layer. (Phase 16 said one, and said so in this menu.)
        menu.addItem (1, utf8 ("创建自动化 · 组音量"), true,
                      automation.contains (AutomationOwner::group, ownerId,
                                           (int) GroupAutomationTarget::level));
        menu.addItem (2, utf8 ("创建自动化 · 组声相"), true,
                      automation.contains (AutomationOwner::group, ownerId,
                                           (int) GroupAutomationTarget::pan));
    }
    else
    {
        const bool isMaster = owner == AutomationOwner::master;

        menu.addItem (1, utf8 ("创建自动化 · ") + (owner == AutomationOwner::layer ? utf8 ("音量")
                                                                                  : isMaster ? utf8 ("主音量")
                                                                                             : utf8 ("滤波器")),
                      true, automation.contains (owner, ownerId, 0));
        menu.addItem (2, utf8 ("创建自动化 · ") + (owner == AutomationOwner::layer ? utf8 ("声相")
                                                                                  : isMaster ? utf8 ("主声相")
                                                                                             : utf8 ("音量")),
                      true, automation.contains (owner, ownerId, 1));
    }

    // Only an owner that HAS an inline lane in a panel can be expanded there.
    // The master bus has no面板 row, so its two verbs are the two parameters.
    if (owner == AutomationOwner::layer || owner == AutomationOwner::slot)
    {
        menu.addSeparator();
        menu.addItem (3, expanded ? utf8 ("收起面板中的曲线") : utf8 ("在面板中展开曲线"), true);
    }

    juce::Component::SafePointer<MainComponent> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetScreenArea (juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1)),
                        [safe, owner, ownerId, expanded] (int result)
                        {
                            if (safe == nullptr || result <= 0)
                                return;

                            if (result == 3)
                            {
                                if (owner == AutomationOwner::layer)
                                {
                                    const int index = safe->stack.indexOfLayerId (ownerId);

                                    if (index >= 0)
                                        safe->stack.setLayerAutomationExpanded (index, ! expanded);
                                }
                                else if (owner == AutomationOwner::slot)
                                {
                                    safe->rack.setAutomationExpanded (ownerId, ! expanded);
                                }

                                return;
                            }

                            // Everything else is a parameter choice: lane on,
                            // track added, row revealed.
                            safe->ensureAutomationTrack (owner, ownerId, result - 1);
                        });
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

        const auto lane = (SlotAutomationTarget) juce::jlimit (0, 1, target);

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
juce::Rectangle<int> MainComponent::getPlayButtonBounds() const
{
    return { ui::px (188), topBarBounds.getY() + ui::px (12), ui::px (34), ui::px (28) };
}

juce::Rectangle<int> MainComponent::getPauseButtonBounds() const
{
    return { ui::px (226), topBarBounds.getY() + ui::px (12), ui::px (30), ui::px (28) };
}

juce::Rectangle<int> MainComponent::getStopButtonBounds() const
{
    return { ui::px (260), topBarBounds.getY() + ui::px (12), ui::px (30), ui::px (28) };
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
// Three buttons right-aligned against the master meter. They live in the top
// bar rather than a menu because saving and bouncing are the two actions this
// phase is about, and a menu would hide both of them.
juce::Rectangle<int> MainComponent::getFileClusterBounds() const
{
    return { getWidth() - ui::px (196 + 12 + 186), topBarBounds.getY() + ui::px (12),
             ui::px (186), ui::px (28) };
}

juce::Rectangle<int> MainComponent::getSaveButtonBounds() const
{
    return getFileClusterBounds().withWidth (ui::px (58));
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
    auto nameArea = getFileClusterBounds().withX (getFileClusterBounds().getX() - ui::px (164))
                                          .withWidth (ui::px (156));

    // Only when it cannot possibly overlap the device status text.
    return getWidth() >= ui::px (1360) ? nameArea : juce::Rectangle<int>();
}

//-- Phase 8: the edit cluster ------------------------------------------------
// New / undo / redo / mixer / settings sit between the tempo field and the
// device status. Left to right in the order they are reached for: the project
// action first, then the two history buttons, then the console, then settings.
juce::Rectangle<int> MainComponent::getNewButtonBounds() const
{
    return { ui::px (486), topBarBounds.getY() + ui::px (12), ui::px (54), ui::px (28) };
}

juce::Rectangle<int> MainComponent::getUndoButtonBounds() const
{
    return { getNewButtonBounds().getRight() + ui::px (6), topBarBounds.getY() + ui::px (12),
             ui::px (46), ui::px (28) };
}

juce::Rectangle<int> MainComponent::getRedoButtonBounds() const
{
    return { getUndoButtonBounds().getRight() + ui::px (4), topBarBounds.getY() + ui::px (12),
             ui::px (46), ui::px (28) };
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
        const bool engaged = audio.isTransportEngaged();

        // One look for three buttons, so they read as a group.
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

        //-- Pause -------------------------------------------------------------
        {
            auto button = getPauseButtonBounds().toFloat();

            // Paused is a state worth seeing: the plate is lit while it holds.
            drawButtonPlate (button, hoveringPause, playing || paused, juce::Colours::white, paused);

            g.setColour (juce::Colours::white.withAlpha (playing || paused ? 0.85f : 0.28f));

            const auto bars = pauseGlyph (button);

            g.fillRect (bars.left);
            g.fillRect (bars.right);
        }

        //-- Stop --------------------------------------------------------------
        {
            auto button = getStopButtonBounds().toFloat();

            drawButtonPlate (button, hoveringStop, engaged, juce::Colours::white, false);

            g.setColour (juce::Colours::white.withAlpha (engaged ? 0.62f : 0.28f));
            g.fillRect (stopGlyph (button));
        }

        //-- Position ----------------------------------------------------------
        {
            auto readout = getPositionReadoutBounds();

            g.setColour (juce::Colours::white.withAlpha (0.92f));
            g.setFont (juce::Font (juce::FontOptions{}.withHeight (atomic::ui::sp (14.0f)).withStyle ("Bold")));
            g.drawText (formatBarBeat(),
                        readout.withTrimmedBottom (ui::px (21)),
                        juce::Justification::centredLeft, false);

            g.setColour (juce::Colours::white.withAlpha (0.32f));
            g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));
            g.drawText (formatTime(),
                        readout.withTrimmedTop (ui::px (25)),
                        juce::Justification::centredLeft, false);
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

            // Undo / redo are words, not symbols: the two circular-arrow glyphs
            // that every app draws are read right-to-left by half the people who
            // use them, and there is room here for the unambiguous label.
            drawLabelButton (getUndoButtonBounds(), utf8 ("撤销"), hoveringUndo, false, false, canUndo());
            drawLabelButton (getRedoButtonBounds(), utf8 ("重做"), hoveringRedo, false, false, canRedo());

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

            g.setColour (running ? juce::Colour (0xff5ecf9a) : kPlayhead.withAlpha (0.75f));
            g.fillEllipse ((float) x, (float) topBarBounds.getCentreY() - ui::pf (4),
                           ui::pf (8), ui::pf (8));

            // The file buttons come next; only print the device name if there is
            // still room for it, otherwise the two would draw over each other on
            // a narrow window.
            const int available = getFileClusterBounds().getX() - (x + ui::px (18)) - ui::px (8);

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

    //-- File actions ----------------------------------------------------------
    // Save / Load / Export sit in the top bar, not behind a menu: this phase is
    // about getting the project out of the app, so the way out has to be visible.
    {
        auto drawFileButton = [&g] (juce::Rectangle<int> bounds, const juce::String& label,
                                    bool hovered, bool accent, bool busy)
        {
            const auto r   = bounds.toFloat();
            const auto tint = accent ? kAccent : juce::Colours::white;

            g.setColour (tint.withAlpha (hovered && ! busy ? 0.16f : 0.06f));
            g.fillRoundedRectangle (r, ui::pf (6));

            g.setColour (tint.withAlpha (hovered && ! busy ? 0.60f : 0.24f));
            g.drawRoundedRectangle (r, ui::pf (6), 1.0f);

            g.setColour (juce::Colours::white.withAlpha (busy ? 0.45f : (hovered ? 0.95f : 0.66f)));
            g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.5f))));
            g.drawText (label, bounds, juce::Justification::centred, false);
        };

        drawFileButton (getSaveButtonBounds(), utf8 ("保存"), hoveringSave, false, false);
        drawFileButton (getLoadButtonBounds(), utf8 ("载入"), hoveringLoad, false, false);
        drawFileButton (getExportButtonBounds(),
                        exporting ? utf8 ("导出中") : utf8 ("导出"),
                        hoveringExport, true, exporting);

        if (! getProjectNameBounds().isEmpty())
        {
            const juce::String name = currentProject.existsAsFile()
                                        ? currentProject.getFileNameWithoutExtension()
                                        : utf8 ("未命名工程");

            g.setColour (dirtyCache ? juce::Colour (0xffffc46b)
                                    : juce::Colours::white.withAlpha (0.34f));
            g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));
            g.drawText ((dirtyCache ? juce::String (utf8 ("● ")) : juce::String())
                          + name + utf8 ("  ·  P20"),
                        getProjectNameBounds(), juce::Justification::centredRight, true);
        }
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
        const int limit  = juce::jmax (0, leftColumn.getHeight() - minTop - minBot);
        filePaneHeight   = juce::jlimit (minBot, juce::jmax (minBot, limit), filePaneHeight);

        auto lower = leftColumn.removeFromBottom (filePaneHeight);
        layerPanel.setBounds (leftColumn);
        filePanel.setBounds (lower);
    }

    {
        const int minTop = ui::px (180);
        const int minBot = ui::px (120);
        const int limit  = juce::jmax (0, rightColumn.getHeight() - minTop - minBot);
        groupPaneHeight  = juce::jlimit (minBot, juce::jmax (minBot, limit), groupPaneHeight);

        auto lower = rightColumn.removeFromBottom (groupPaneHeight);
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

    if (getPlayButtonBounds().contains (pos))
    {
        playTransport();
        return;
    }

    if (getPauseButtonBounds().contains (pos))
    {
        pauseTransport();
        return;
    }

    if (getStopButtonBounds().contains (pos))
    {
        stopTransport();
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
}

void MainComponent::mouseMove (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();

    const bool newPlay   = getPlayButtonBounds().contains (pos);
    const bool newPause  = getPauseButtonBounds().contains (pos);
    const bool newStop   = getStopButtonBounds().contains (pos);
    const bool newSave   = getSaveButtonBounds().contains (pos);
    const bool newLoad   = getLoadButtonBounds().contains (pos);
    const bool newExport = getExportButtonBounds().contains (pos);
    const bool newNew    = getNewButtonBounds().contains (pos);
    const bool newUndo   = getUndoButtonBounds().contains (pos);
    const bool newRedo   = getRedoButtonBounds().contains (pos);
    const bool newMixer  = getMixerButtonBounds().contains (pos);
    const bool newGear   = getSettingsButtonBounds().contains (pos);

    setMouseCursor ((newPlay || newPause || newStop || newSave || newLoad || newExport
                      || newNew || newUndo || newRedo || newMixer || newGear)
                      ? juce::MouseCursor::PointingHandCursor
                      : juce::MouseCursor::NormalCursor);

    if (newPlay != hoveringPlay || newPause != hoveringPause || newStop != hoveringStop
         || newSave != hoveringSave || newLoad != hoveringLoad || newExport != hoveringExport
         || newNew != hoveringNew || newUndo != hoveringUndo || newRedo != hoveringRedo
         || newMixer != hoveringMixer || newGear != hoveringSettings)
    {
        hoveringPlay   = newPlay;
        hoveringPause  = newPause;
        hoveringStop   = newStop;
        hoveringSave   = newSave;
        hoveringLoad   = newLoad;
        hoveringExport = newExport;
        hoveringNew    = newNew;
        hoveringUndo   = newUndo;
        hoveringRedo   = newRedo;
        hoveringMixer  = newMixer;
        hoveringSettings = newGear;
        repaint (topBarBounds);
    }
}

void MainComponent::mouseExit (const juce::MouseEvent&)
{
    if (hoveringPlay || hoveringPause || hoveringStop
         || hoveringSave || hoveringLoad || hoveringExport
         || hoveringNew || hoveringUndo || hoveringRedo || hoveringMixer || hoveringSettings)
    {
        hoveringPlay = hoveringPause = hoveringStop = false;
        hoveringSave = hoveringLoad = hoveringExport = false;
        hoveringNew = hoveringUndo = hoveringRedo = hoveringMixer = hoveringSettings = false;
        repaint (topBarBounds);
    }
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    if (key.getModifiers().isCommandDown())
    {
        const auto character = juce::CharacterFunctions::toUpperCase (
                                  (juce::juce_wchar) (key.getKeyCode() != 0 ? key.getKeyCode()
                                                                            : key.getTextCharacter()));

        if (character == 'S') { askToSaveProject();  return true; }
        if (character == 'O') { askToLoadProject();  return true; }
        if (character == 'E') { askToExportAudio();  return true; }
        if (character == 'N') { requestNewProject(); return true; }
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

    if (key == juce::KeyPress::spaceKey)
    {
        togglePlay();
        return true;
    }

    // Return to zero, the way a DAW's transport keys behave.
    if (key == juce::KeyPress::returnKey)
    {
        stopTransport();
        return true;
    }

    return false;
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
    if (! project::save (file, stack, rack, audio.getBpm(), error, &mixer, &automation))
        return false;

    currentProject = file;

    // A gesture still in flight belongs to what was just written, so flush it
    // before remembering "this is the saved state" — otherwise the same edit
    // would be reported as unsaved a moment later.
    cancelPendingUpdate();
    commitHistory();
    savedStateText = captureState();
    dirtyCache     = false;

    repaint (topBarBounds);
    return true;
}

bool MainComponent::loadProjectFromFile (const juce::File& file, juce::String& error)
{
    double newBpm = audio.getBpm();

    // project::load parses into temporaries and only commits at the end, so a
    // corrupt file leaves the open session exactly as it was.
    if (! project::load (file, stack, rack, newBpm, error, &mixer, &automation, &plugins))
        return false;

    // Loading a different song rewinds: leaving the playhead mid-timeline over
    // material it was never positioned against is disorienting (and the old
    // demo note ids below would point at notes that no longer exist).
    audio.stopTransport();
    audio.setBpm (newBpm);

    bpmField.setBpm (audio.getBpm(), false);
    pianoRoll.setBpm (audio.getBpm());
    pianoRoll.setSelection ({});
    pianoRoll.setPlayheadBeat (0.0);
    pianoRoll.setTransportActive (false);

    // The loaded song brings notes that may sit past the default 64 beats; the
    // roll has to be long enough to scroll to them.
    pianoRoll.resetTimeline();

    envelopePanel.close();
    layerPanel.setPlayheadBeat (-1.0);
    rackPanel.setPlayheadBeat (-1.0);

    // The demo's "open the inspector on this note" ids belonged to the old
    // project; keeping them would make the next snapshot open a dead note.
    demoOverrideLayer = -1;
    demoOverrideNote  = -1;

    // Same for the roll's inline box: it addresses a note by id.
    pianoRoll.closeNoteAutomation();

    currentProject = file;

    // A different song: the undo history of the previous one is meaningless now
    // (undone far enough it would teleport the user into another project), and
    // what is on screen is exactly what is in the file.
    cancelPendingUpdate();
    history.clear();
    history.push_back (captureState());
    historyPos = 0;
    savedStateText = history.front();
    dirtyCache     = false;

    repaint();
    return true;
}

//==============================================================================
wav::ExportResult MainComponent::exportAudioToFile (const juce::File& file,
                                                    const std::function<bool (double)>& progress,
                                                    double tailSeconds)
{
    // Freeze the session first. Everything the renderer needs — routing, both
    // faders, both kinds of automation, every note envelope — is copied into
    // this schedule by publishSchedule(), so the bounce cannot read a model
    // while the message thread is editing it.
    PlaybackEngine staging;
    staging.setSampleRate (wav::exportSampleRate);
    staging.publishSchedule (stack, rack, audio.getBpm(), 0.0, &mixer);

    auto schedule = staging.getSchedule();

    if (schedule == nullptr)
    {
        wav::ExportResult failed;
        failed.message = utf8 ("无法生成渲染快照");
        return failed;
    }

    return wav::renderToFile (file, *schedule, wav::exportSampleRate, tailSeconds, progress,
                              &plugins);
}

double MainComponent::getContentSeconds() const
{
    PlaybackEngine probe;
    probe.setSampleRate (wav::exportSampleRate);
    probe.publishSchedule (stack, rack, audio.getBpm(), 0.0, &mixer);

    auto schedule = probe.getSchedule();

    if (schedule == nullptr)
        return 0.0;

    return juce::jmax (0.0, schedule->contentBeats) * schedule->secondsPerBeat;
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

    auto initial = getProjectDirectory().getChildFile ("AtomicDAW-Mix.wav");

    if (currentProject.existsAsFile())
        initial = currentProject.getParentDirectory()
                    .getChildFile (currentProject.getFileNameWithoutExtension() + ".wav");

    chooser = std::make_unique<juce::FileChooser> (utf8 ("导出 WAV"), initial, "*.wav");

    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                            | juce::FileBrowserComponent::canSelectFiles
                            | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto file = fc.getResult();

                              if (file.getFullPathName().isEmpty())
                                  return;

                              if (file.getFileExtension().isEmpty())
                                  file = file.withFileExtension ("wav");

                              // Snapshot on the message thread; the render runs
                              // on its own thread with its own copy.
                              PlaybackEngine staging;
                              staging.setSampleRate (wav::exportSampleRate);
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

                              auto* task = new ExportTask (schedule, file, [safeThis]
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
void MainComponent::openDemoNoteAutomation()
{
    auto* layer = stack.getActiveLayer();

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

    // Phase 16: park the spotlight on the synth the demo just borrowed onto.
    // Called after updateRackRoutingFocus() because that one re-derives the
    // focus from the selection and would otherwise undo this.
    focusSynth (otherSlot);

    layerPanel.repaint();
    rackPanel.repaint();
    pianoRoll.repaint();
    repaint();
}

//==============================================================================
juce::String MainComponent::runSelfTest()
{
    juce::String report (utf8 ("AtomicDAW — headless rule check (Phase 1 + 2 + 3 + 4 + 5 + 6 + 7)\n"));
    int failures = 0;

    auto check = [&report, &failures] (const juce::String& name, bool passed, const juce::String& detail)
    {
        report << (passed ? "  [PASS] " : "  [FAIL] ") << name;

        if (detail.isNotEmpty())
            report << "   (" << detail << ")";

        report << "\n";

        if (! passed)
            ++failures;
    };

    //-- Phase 10: what a fresh session looks like ----------------------------
    // Checked BEFORE the fixture is built, because this is exactly what the app
    // hands a user who double-clicks the icon.
    const bool startedBlank = rack.getNumSlots() == 1
                                && rack.getSlot (0) != nullptr
                                && ! rack.getSlot (0)->isPlugin()
                                && stack.getNumLayers() == 1
                                && stack.getLayer (0) != nullptr
                                && stack.getLayer (0)->getNotes().empty()
                                && stack.getLayer (0)->getOutputSlotId() == rack.getSlotIdAt (0)
                                && ! hasUnsavedChanges();

    check ("A fresh session: one empty layer, one built-in synth, no notes",
           startedBlank,
           juce::String (stack.getNumLayers()) + utf8 (" layers / ")
             + juce::String (rack.getNumSlots()) + utf8 (" synths")
             + utf8 (" / ") + (rack.getSlot (0) != nullptr && rack.getSlot (0)->isPlugin()
                                 ? "plugin" : "built-in")
             + utf8 (" / clean: ") + (hasUnsavedChanges() ? "no" : "yes"));

    // Phase 18: the console's first strip is wired to the built-in synth out of
    // the box. Phase 11 deliberately left it unrouted, which meant a brand new
    // project had a dead fader, dead inserts and a dead meter on every strip —
    // the first controls anyone reaches for.
    check ("The fresh synth is sent to mixer channel 1",
           rack.getSlot (0) != nullptr && rack.getSlot (0)->mixerChannel == 0,
           rack.getSlot (0) != nullptr ? juce::String (rack.getSlot (0)->mixerChannel) : "-");


    // Everything below is written against the demo arrangement, so build it.
    loadDemoProject();

    const int activeIndex = stack.getActiveLayerIndex();
    int       ghostIndex  = (activeIndex == 0 ? 1 : 0);

    auto* active = stack.getLayer (activeIndex);
    auto* ghost  = stack.getLayer (ghostIndex);

    if (active == nullptr || ghost == nullptr)
    {
        report << "  [FAIL] model has no usable layers\n";
        return report;
    }

    //-- Phase 1 / 20: ghosts are opt-in, and click-through when they are on --
    int  ghostNoteId = -1;
    juce::Point<float> ghostPoint;

    // Every non-active sheet starts with its eye closed (Phase 20), so the
    // ghost half of this test has to open one first. That is also the rule
    // being tested: no eye, no ghost.
    for (int g = 0; g < stack.getNumLayers(); ++g)
        if (g != activeIndex)
            stack.setLayerVisible (g, false);

    {
        bool anyGhostShown = false;

        for (int g = 0; g < stack.getNumLayers(); ++g)
            if (g != activeIndex && pianoRoll.isLayerShown (g))
                anyGhostShown = true;

        check ("A closed eye paints no ghost", ! anyGhostShown,
               juce::String (stack.getNumLayers() - 1) + " non-active sheet(s)");
        check ("The eye does not hide the sheet being edited",
               pianoRoll.isLayerShown (activeIndex), "active layer still painted");
    }

    for (int g = 0; g < stack.getNumLayers(); ++g)
        if (g != activeIndex)
            stack.setLayerVisible (g, true);

    // Any non-active sheet will do: with the mixer taking the bottom of the
    // window, the lowest layer is no longer guaranteed to be on screen. What
    // the test is about is the click-through rule, not which sheet proves it.
    for (int g = 0; g < stack.getNumLayers() && ghostNoteId < 0; ++g)
    {
        if (g == activeIndex)
            continue;

        auto* ghostLayer = stack.getLayer (g);

        if (ghostLayer == nullptr || ! pianoRoll.isLayerShown (g))
            continue;

        for (const auto& ghostNote : ghostLayer->getNotes())
        {
            const auto bounds = pianoRoll.getNoteScreenBounds (g, ghostNote.id);

            if (bounds.isEmpty() || ! pianoRoll.getLocalBounds().toFloat().contains (bounds.getCentre()))
                continue;

            bool coveredByActive = false;

            for (const auto& activeNote : active->getNotes())
                if (pianoRoll.getNoteScreenBounds (activeIndex, activeNote.id).contains (bounds.getCentre()))
                    coveredByActive = true;

            if (! coveredByActive)
            {
                ghostNoteId = ghostNote.id;
                ghostIndex  = g;
                ghostPoint  = bounds.getCentre();
                break;
            }
        }
    }

    check ("Found a testable ghost note", ghostNoteId != -1,
           "layer " + juce::String (ghostIndex + 1));


    if (ghostNoteId != -1)
        check ("Ghost note is click-through (30% layer)", pianoRoll.getEditableNoteIdAt (ghostPoint) == -1,
               "raw id " + juce::String (ghostNoteId));

    const auto& activeNotes = active->getNotes();

    if (! activeNotes.empty())
    {
        const auto& first = activeNotes.front();
        const auto centre = pianoRoll.getNoteScreenBounds (activeIndex, first.id).getCentre();

        check ("Active layer note IS editable", pianoRoll.getEditableNoteIdAt (centre) == first.id,
               "expected id " + juce::String (first.id));
    }

    if (ghostNoteId != -1)
    {
        stack.setActiveLayerIndex (ghostIndex);

        check ("After switching sheets, former ghost becomes editable",
               pianoRoll.getEditableNoteIdAt (ghostPoint) == ghostNoteId,
               "layer " + juce::String (ghostIndex + 1) + " now active");

        stack.setActiveLayerIndex (activeIndex);
    }

    if (! activeNotes.empty())
    {
        const auto centre = pianoRoll.getNoteScreenBounds (activeIndex, activeNotes.front().id).getCentre();

        // Phase 20 flipped this assertion. The eye used to be "does this layer
        // exist"; now it is "show the other sheets too", so the sheet you are
        // editing stays editable with its eye closed. Clicking a note you can
        // see and having nothing happen is the worse behaviour by far.
        stack.setLayerVisible (activeIndex, false);
        check ("The active layer stays editable with its eye closed",
               pianoRoll.getEditableNoteIdAt (centre) == activeNotes.front().id,
               "eye toggled off");
        stack.setLayerVisible (activeIndex, true);
    }

    // Put the eyes back the way the fixture had them. Phase 20 opens every
    // ghost to test click-through, and leaving them open would make an
    // otherwise untouched session report unsaved changes — which is exactly
    // what the Phase 10 check downstream is watching for.
    for (int g = 0; g < stack.getNumLayers(); ++g)
        stack.setLayerVisible (g, false);

    //-- Phase 2: the envelope must live INSIDE the note ----------------------
    {
        const int tempId = active->addNote ({ 0, 40, 60.0, 1.0, 0.8f });

        if (auto* temp = active->findNote (tempId))
            temp->envelope = NoteEnvelope::makeFadeOut();

        pianoRoll.setSelection ({ tempId });

        const auto copies = pianoRoll.duplicateSelectedNotes (2.0);

        check ("Duplicate creates a distinct note id",
               copies.size() == 1 && copies.front() != tempId,
               "src " + juce::String (tempId) + " -> copy "
                 + juce::String (copies.empty() ? -1 : copies.front()));

        if (! copies.empty())
        {
            auto* src = active->findNote (tempId);
            auto* dup = active->findNote (copies.front());

            check ("Duplicated note carries an identical envelope",
                   src != nullptr && dup != nullptr && src->envelope.hasSameShapeAs (dup->envelope),
                   "2-point fade-out curve");

            if (src != nullptr && dup != nullptr)
            {
                dup->envelope.addPoint (0.4f, 0.1f);

                check ("Editing the copy leaves the original untouched",
                       ! src->envelope.hasSameShapeAs (dup->envelope), "independent value copies");
            }
        }

        openEnvelopePanel (activeIndex, tempId);
        check ("Inspector opens for the double-clicked note",
               envelopePanel.isOpen() && envelopePanel.getEditedNoteId() == tempId,
               "id " + juce::String (tempId));


        active->removeNote (tempId);

        stack.notifyNotesChanged();

        check ("Inspector closes when its note is deleted", ! envelopePanel.isOpen(), "no dangling pointer");

        for (int id : copies)
            active->removeNote (id);

        pianoRoll.setSelection (std::vector<int>{});
        stack.notifyNotesChanged();
    }

    //-- Phase 3: routing priority, and the engine actually obeying it -------
    {
        LayerStack testStack;
        RackModel  testRack;                       // Synth A + Synth B
        const int  slotA = testRack.getSlotIdAt (0);
        const int  slotB = testRack.getSlotIdAt (1);

        const int testLayerIndex = testStack.addLayer ("T", palette::layer1);
        auto* testLayer = testStack.getLayer (testLayerIndex);
        testStack.setLayerOutputSlot (testLayerIndex, slotA);

        const int noteViaLayer  = testLayer->addNote ({ 0, 60, 0.0, 2.0, 0.9f });
        const int noteViaBypass = testLayer->addNote ({ 0, 67, 0.0, 2.0, 0.9f });
        testLayer->findNote (noteViaBypass)->outputSlotOverride = slotB;

        const auto routeLayer  = routing::resolve (*testLayer, *testLayer->findNote (noteViaLayer), testRack);
        const auto routeBypass = routing::resolve (*testLayer, *testLayer->findNote (noteViaBypass), testRack);

        check ("Un-overridden note follows the layer default",
               routeLayer.slotId == slotA && ! routeLayer.overridden, "Synth A");
        check ("Note override beats the layer default",
               routeBypass.slotId == slotB && routeBypass.overridden, "Synth B");

        // A whole-struct copy is exactly what Layer::addNote does on duplicate.
        const int copyId = testLayer->addNote (*testLayer->findNote (noteViaBypass));
        check ("A duplicated note keeps its output override",
               testLayer->findNote (copyId)->outputSlotOverride == slotB, "Synth B");
        testLayer->removeNote (copyId);

        // The engine, not the label: render and measure each synth separately.
        PlaybackEngine engine;
        engine.setSampleRate (48000.0);
        engine.publishSchedule (testStack, testRack, 120.0);

        const auto both = engine.renderOffline (1.2);

        check ("Both notes are audible on their routed synths",
               both.peakForSlot (slotA) > 0.005f && both.peakForSlot (slotB) > 0.005f,
               "A " + juce::String (both.peakForSlot (slotA), 3)
                 + " / B " + juce::String (both.peakForSlot (slotB), 3));

        // Remove the layer-default note: only the bypassed one remains. If the
        // override were ignored, Synth B would now be silent and Synth A loud.
        testLayer->removeNote (noteViaLayer);
        engine.publishSchedule (testStack, testRack, 120.0);

        const auto soloBypass = engine.renderOffline (1.2);

        check ("With only the overridden note, the synths are isolated",
               soloBypass.peakForSlot (slotA) == 0.0f && soloBypass.peakForSlot (slotB) > 0.005f,
               "A " + juce::String (soloBypass.peakForSlot (slotA), 3)
                 + " / B " + juce::String (soloBypass.peakForSlot (slotB), 3));

        // The micro envelope must actually shape the rendered audio.
        juce::AudioBuffer<float> buffer (2, 48000);   // the note is 2 beats = 1.0 s

        if (auto* note = testLayer->findNote (noteViaBypass))
            note->envelope = NoteEnvelope::makeFadeOut();

        engine.publishSchedule (testStack, testRack, 120.0);
        engine.renderWindow (0.0, 48000, buffer, nullptr);

        const float fadeEarly = buffer.getRMSLevel (0, 4800, 4800);    // 0.10 - 0.20 s
        const float fadeLate  = buffer.getRMSLevel (0, 38400, 4800);   // 0.80 - 0.90 s

        check ("Engine applies the note's fade-out envelope",
               fadeLate < fadeEarly * 0.35f,
               "late/early " + juce::String (fadeLate / juce::jmax (1.0e-6f, fadeEarly), 2));

        if (auto* note = testLayer->findNote (noteViaBypass))
            note->envelope = NoteEnvelope::makeFlat();

        engine.publishSchedule (testStack, testRack, 120.0);
        engine.renderWindow (0.0, 48000, buffer, nullptr);

        const float flatEarly = buffer.getRMSLevel (0, 4800, 4800);
        const float flatLate  = buffer.getRMSLevel (0, 38400, 4800);

        check ("A flat envelope keeps the level steady",
               flatLate > flatEarly * 0.85f,
               "late/early " + juce::String (flatLate / juce::jmax (1.0e-6f, flatEarly), 2));

        // Transport bookkeeping.
        engine.setPositionBeats (0.0);
        juce::AudioBuffer<float> block (2, 512);
        engine.renderNextBlock (block, 512);

        check ("Transport advances by exactly one block",
               std::abs (engine.getPositionBeats() - 512.0 / 48000.0 * 2.0) < 1.0e-9,
               juce::String (engine.getPositionBeats(), 5) + " beats");

        // A route pointing at a deleted synth must fall back, not go silent.
        if (auto* note = testLayer->findNote (noteViaBypass))
            note->outputSlotOverride = 9999;

        const auto dangling = routing::resolve (*testLayer, *testLayer->findNote (noteViaBypass), testRack);

        check ("A dangling override falls back to the layer default",
               dangling.slotId == slotA && ! dangling.overridden && dangling.danglingOverride,
               "orphaned id 9999 -> Synth A");
    }

    //-- Phase 4: transport, tempo, and the playhead driving the envelopes ---
    {
        LayerStack t4;
        RackModel  r4;
        const int  slotA = r4.getSlotIdAt (0);

        const int li = t4.addLayer ("T4", palette::layer1);
        auto* l4 = t4.getLayer (li);
        t4.setLayerOutputSlot (li, slotA);

        // One note, four beats long, starting on beat 4. At 120 BPM that is
        // 2.0 s .. 4.0 s of audio.
        const int nid = l4->addNote ({ 0, 60, 4.0, 4.0, 0.9f });

        if (auto* n = l4->findNote (nid))
            n->envelope = NoteEnvelope::makeFlat();

        PlaybackEngine pe;
        pe.setSampleRate (48000.0);
        pe.publishSchedule (t4, r4, 120.0);

        auto peakBetween = [&pe] (double from, double to)
        {
            const int n = juce::jmax (1, (int) std::llround ((to - from) * 48000.0));
            juce::AudioBuffer<float> b (2, n);
            pe.renderWindow (from, n, b, nullptr);
            return b.getMagnitude (0, 0, n);
        };

        // Requirements 4 and 5, measured rather than asserted by inspection:
        // the note exists only between its own edges.
        check ("Silent before the playhead reaches the note",
               peakBetween (1.0, 1.9) < 1.0e-6f, "1.0-1.9 s");
        check ("Note sounds once the playhead is inside it",
               peakBetween (2.2, 2.7) > 0.005f,
               "peak " + juce::String (peakBetween (2.2, 2.7), 3));
        check ("Note stops when the playhead leaves its right edge",
               peakBetween (4.2, 4.7) < 1.0e-6f, "4.2-4.7 s");

        //-- Tempo is a scale factor, never a stored position -----------------
        pe.setPositionBeats (4.0);
        pe.publishSchedule (t4, r4, 60.0);

        check ("A tempo change leaves the playhead on the same beat",
               std::abs (pe.getPositionBeats() - 4.0) < 1.0e-9, "still beat 4");
        check ("A tempo change rescales wall-clock time",
               std::abs (pe.getPosition() - 4.0) < 1.0e-9,
               "beat 4 @ 60 BPM = 4.00 s, was 2.00 s");

        {
            auto current = pe.getSchedule();

            check ("The note's start time follows the tempo",
                   current != nullptr && ! current->notes.empty()
                     && std::abs (current->notes.front().startSeconds - 4.0) < 1.0e-9,
                   "beat 4 -> 4.00 s");
        }

        //-- Playback speed ---------------------------------------------------
        auto beatsPerSecond = [&t4, &r4] (double bpm)
        {
            PlaybackEngine e;
            e.setSampleRate (48000.0);
            e.publishSchedule (t4, r4, bpm);
            e.setPositionBeats (0.0);

            juce::AudioBuffer<float> oneSecond (2, 48000);
            e.renderNextBlock (oneSecond, 48000);
            return e.getPositionBeats();
        };

        check ("One second of audio advances 2 beats at 120 BPM",
               std::abs (beatsPerSecond (120.0) - 2.0) < 1.0e-6,
               juce::String (beatsPerSecond (120.0), 4) + " beats");
        check ("One second of audio advances 1 beat at 60 BPM",
               std::abs (beatsPerSecond (60.0) - 1.0) < 1.0e-6,
               juce::String (beatsPerSecond (60.0), 4) + " beats");

        //-- The loop point can fall inside a block ---------------------------
        {
            PlaybackEngine loop;
            loop.setSampleRate (48000.0);
            loop.publishSchedule (t4, r4, 120.0);

            const double length = loop.getLengthBeats();
            loop.setPositionBeats (length - 0.25);

            juce::AudioBuffer<float> crossing (2, 48000);   // 1 s == 2 beats
            loop.renderNextBlock (crossing, 48000);

            const double expected = (length - 0.25) + 2.0 - length;

            check ("A block straddling the loop point keeps every sample",
                   std::abs (loop.getPositionBeats() - expected) < 1.0e-3,
                   "wrapped to beat " + juce::String (loop.getPositionBeats(), 3));
        }

        //-- The same path the device uses, block by block --------------------
        // renderWindow proves the mixer; this proves the *transport*, by
        // walking blocks exactly as the audio callback does and noting where
        // sound first and last appears relative to the note's own edges.
        {
            PlaybackEngine walk;
            walk.setSampleRate (48000.0);
            walk.publishSchedule (t4, r4, 120.0);
            walk.setPositionBeats (0.0);

            juce::AudioBuffer<float> blk (2, 512);
            double firstAudibleBeat = -1.0;
            double lastAudibleBeat  = -1.0;

            for (int i = 0; i < 400; ++i)      // 400 * 512 / 48000 = 4.27 s
            {
                const double blockStart = walk.getPositionBeats();
                walk.renderNextBlock (blk, 512);

                if (blk.getMagnitude (0, 0, 512) > 1.0e-6f)
                {
                    if (firstAudibleBeat < 0.0)
                        firstAudibleBeat = blockStart;

                    lastAudibleBeat = blockStart;
                }
            }

            check ("Sound begins at the note's head while the transport runs",
                   firstAudibleBeat >= 3.97 && firstAudibleBeat <= 4.02,
                   "first audible block @ beat " + juce::String (firstAudibleBeat, 3));
            check ("Sound ends at the note's tail while the transport runs",
                   lastAudibleBeat >= 7.97 && lastAudibleBeat <= 8.02,
                   "last audible block @ beat " + juce::String (lastAudibleBeat, 3));
        }

        //-- The animation is driven by the same predicate the test checks ----
        NoteEvent probe;
        probe.startBeat   = 4.0;
        probe.lengthBeats = 4.0;

        check ("Envelope progress is 0 at the note head",
               std::abs (PianoRollComponent::envelopeProgress (probe, 4.0)) < 1.0e-9, "beat 4");
        check ("Envelope progress is 0.5 halfway through",
               std::abs (PianoRollComponent::envelopeProgress (probe, 6.0) - 0.5) < 1.0e-9, "beat 6");
        check ("Envelope progress reaches 1 on the tail",
               std::abs (PianoRollComponent::envelopeProgress (probe, 8.0) - 1.0) < 1.0e-9, "beat 8");
        check ("There is no progress outside the note",
               PianoRollComponent::envelopeProgress (probe, 3.99) < 0.0
                 && PianoRollComponent::envelopeProgress (probe, 8.01) < 0.0,
               "3.99 / 8.01");

        //-- Play / pause / stop ----------------------------------------------
        // play(false): the state machine without taking the sound card.
        AudioEngine transport (t4, r4);

        transport.play (false);
        check ("Play starts the transport",
               transport.isPlaying() && transport.isTransportEngaged(), "playing");

        transport.setPositionBeats (8.0);
        transport.pause();
        check ("Pause holds the playhead where it was",
               transport.isPaused() && std::abs (transport.getPositionBeats() - 8.0) < 1.0e-9,
               "paused at beat 8");

        transport.stopTransport();
        check ("Stop rewinds to the top",
               transport.getTransportState() == TransportState::stopped
                 && transport.getPositionBeats() == 0.0,
               "stopped at beat 0");
    }

    //-- Phase 5: faders, pan, mute / solo, macro automation -----------------
    {
        LayerStack t5;
        RackModel  r5;
        const int  slotA = r5.getSlotIdAt (0);
        const int  slotB = r5.getSlotIdAt (1);
        constexpr double len = timeline::defaultLengthBeats;

        const int li  = t5.addLayer ("Mix 1", palette::layer1);
        const int li2 = t5.addLayer ("Mix 2", palette::layer2);

        auto* l5 = t5.getLayer (li);
        auto* l6 = t5.getLayer (li2);

        t5.setLayerOutputSlot (li,  slotA);
        t5.setLayerOutputSlot (li2, slotB);

        // Long, flat notes: every measurement below is about the mixer, never
        // about an envelope. 24 beats = 12 s at 120 BPM.
        for (auto* layer : { l5, l6 })
        {
            const int id = layer->addNote ({ 0, layer == l5 ? 48 : 60, 0.0, 24.0, 1.0f });

            if (auto* note = layer->findNote (id))
                note->envelope = NoteEnvelope::makeFlat();

            const int top = layer->addNote ({ 0, layer == l5 ? 72 : 67, 0.0, 24.0, 0.8f });

            if (auto* note = layer->findNote (top))
                note->envelope = NoteEnvelope::makeFlat();
        }

        PlaybackEngine e5;
        e5.setSampleRate (48000.0);

        // Deterministic window RMS. resetMixState() clears the synth filters'
        // memory, so one measurement can never colour the next.
        auto render = [&e5] (double from, double to, RenderStats* statsOut) -> float
        {
            const int n = juce::jmax (1, (int) std::llround ((to - from) * 48000.0));
            juce::AudioBuffer<float> b (2, n);
            e5.resetMixState();
            e5.renderWindow (from, n, b, statsOut);
            return b.getRMSLevel (0, 0, n);
        };

        //-- Fader ------------------------------------------------------------
        // Layer 2 exists to prove summing and solo. Park it muted for the
        // single-strip tests, otherwise its constant signal is inside every
        // measurement and a -6 dB fader appears to be -2.6 dB.
        t5.setLayerMuted (li2, true);

        t5.setLayerLevelDb (li, 0.0f);
        t5.setLayerPan (li, 0.0f);
        e5.publishSchedule (t5, r5, 120.0);
        const float unityRms = render (2.0, 3.0, nullptr);

        t5.setLayerLevelDb (li, -6.0f);
        e5.publishSchedule (t5, r5, 120.0);
        const float minus6Rms = render (2.0, 3.0, nullptr);

        check ("A -6 dB layer fader halves the level",
               std::abs (minus6Rms / juce::jmax (1.0e-6f, unityRms) - 0.501f) < 0.05f,
               "x" + juce::String (minus6Rms / juce::jmax (1.0e-6f, unityRms), 3));

        //-- Pan ---------------------------------------------------------------
        t5.setLayerLevelDb (li, 0.0f);
        t5.setLayerPan (li, -1.0f);
        e5.publishSchedule (t5, r5, 120.0);

        {
            juce::AudioBuffer<float> b (2, 24000);
            e5.resetMixState();
            e5.renderWindow (2.0, 24000, b, nullptr);

            const float left  = b.getRMSLevel (0, 0, 24000);
            const float right = b.getRMSLevel (1, 0, 24000);

            check ("Hard left places the layer in the left channel only",
                   left > 0.01f && right < left * 0.02f,
                   "L " + juce::String (left, 3) + " / R " + juce::String (right, 3));
        }

        t5.setLayerPan (li, 0.0f);
        t5.setLayerMuted (li, true);
        e5.publishSchedule (t5, r5, 120.0);

        check ("Mute silences the layer",
               render (2.0, 3.0, nullptr) < 1.0e-6f, "layer 1 muted");

        t5.setLayerMuted (li, false);

        //-- Solo (layers) -----------------------------------------------------
        {
            t5.setLayerMuted (li2, false);       // solo must silence an AUDIBLE layer

            RenderStats soloStats;
            t5.setLayerSoloed (li, true);
            e5.publishSchedule (t5, r5, 120.0);
            render (2.0, 3.0, &soloStats);
            t5.setLayerSoloed (li, false);

            check ("Soloing one layer silences the others",
                   soloStats.peakForLayer (li) > 0.002f && soloStats.peakForLayer (li2) == 0.0f,
                   "layer 1 " + juce::String (soloStats.peakForLayer (li), 3)
                     + " / layer 2 " + juce::String (soloStats.peakForLayer (li2), 3));
        }

        //-- Mute / solo (synths) ---------------------------------------------
        {
            RenderStats mutedSlot;
            r5.setMuted (slotB, true);
            e5.publishSchedule (t5, r5, 120.0);
            render (2.0, 3.0, &mutedSlot);
            r5.setMuted (slotB, false);

            check ("Muting a synth silences everything routed to it",
                   mutedSlot.peakForLayer (li2) == 0.0f && mutedSlot.peakForLayer (li) > 0.002f,
                   "Synth B silent, Synth A untouched");

            RenderStats soloSlot;
            r5.setSoloed (slotB, true);
            e5.publishSchedule (t5, r5, 120.0);
            render (2.0, 3.0, &soloSlot);
            r5.setSoloed (slotB, false);

            check ("Soloing a synth isolates it",
                   soloSlot.peakForLayer (li2) > 0.002f && soloSlot.peakForLayer (li) == 0.0f,
                   "Synth B only");
        }

        //-- Summing -----------------------------------------------------------
        {
            // Publish first: the solo above left the schedule in a soloed state,
            // and measuring a stale schedule is how this check failed once.
            // Both strips at unity, so the expected sum is sqrt(2).
            t5.setLayerLevelDb (li,  0.0f);
            t5.setLayerLevelDb (li2, 0.0f);
            t5.setLayerPan (li,  0.0f);
            t5.setLayerPan (li2, 0.0f);
            t5.setLayerMuted (li2, false);

            e5.publishSchedule (t5, r5, 120.0);
            const float bothRms = render (2.0, 3.0, nullptr);

            t5.setLayerMuted (li2, true);
            e5.publishSchedule (t5, r5, 120.0);
            const float oneRms = render (2.0, 3.0, nullptr);

            // Two different note sets do not add as +6 dB: their partials are
            // incoherent, so sqrt(2) is a ceiling, not a prediction. What has to
            // be true is that the second layer is genuinely in the sum.
            check ("Layers sum into the master bus",
                   bothRms > oneRms * 1.2f,
                   juce::String (bothRms / juce::jmax (1.0e-6f, oneRms), 2) + "x louder together");
        }

        //-- Macro automation: volume -----------------------------------------
        auto& lane = l5->getAutomation();
        lane.resetFlat (1.0f, len);
        lane.addPoint (8.0, 0.2f, len);
        lane.movePoint (2, len, 0.2f, len);     // hold it down to the end
        lane.setEnabled (true);
        l5->setAutomationTarget (LayerAutomationTarget::volume);
        e5.publishSchedule (t5, r5, 120.0);

        {
            const float early = render (0.5, 1.0, nullptr);    // beats 1..2, curve still high
            const float late  = render (6.5, 7.0, nullptr);    // beats 13..14, curve at 0.2

            check ("A volume automation lane drives the layer in real time",
                   late < early * 0.4f,
                   "late/early " + juce::String (late / juce::jmax (1.0e-6f, early), 2));
        }

        //-- Macro automation: pan --------------------------------------------
        // Volume first OFF: with two lanes resident, leaving the volume curve
        // running would duck the very signal the pan test measures.
        lane.setEnabled (false);

        auto& panLane = l5->automationFor (LayerAutomationTarget::pan);
        panLane.resetFlat (0.5f, len);
        panLane.movePoint (0, 0.0, 0.0f, len);
        panLane.movePoint (1, len, 1.0f, len);
        panLane.setEnabled (true);
        l5->setAutomationTarget (LayerAutomationTarget::pan);
        e5.publishSchedule (t5, r5, 120.0);

        {
            juce::AudioBuffer<float> b (2, 24000);
            e5.resetMixState();
            e5.renderWindow (0.2, 24000, b, nullptr);          // beats 0.4..4.4

            const float left  = b.getRMSLevel (0, 0, 24000);
            const float right = b.getRMSLevel (1, 0, 24000);

            check ("A pan automation lane offsets the layer pan",
                   left > 0.005f && right < left * 0.5f,
                   "L " + juce::String (left, 3) + " / R " + juce::String (right, 3));
        }

        panLane.setEnabled (false);
        l5->setAutomationTarget (LayerAutomationTarget::volume);

        //-- Macro automation: the synth's filter ----------------------------
        {
            auto* slot = r5.findSlot (slotA);

            slot->getAutomation().resetFlat (1.0f, len);
            slot->getAutomation().setEnabled (true);
            slot->automationTarget = SlotAutomationTarget::filter;

            e5.publishSchedule (t5, r5, 120.0);
            const float open = render (2.0, 3.0, nullptr);

            slot->getAutomation().resetFlat (0.0f, len);
            e5.publishSchedule (t5, r5, 120.0);
            const float closed = render (2.0, 3.0, nullptr);

            check ("A synth filter lane audibly reshapes the synth",
                   closed < open * 0.55f,
                   "closed/open " + juce::String (closed / juce::jmax (1.0e-6f, open), 2));

            // The engine never reads the model per block: after publishing,
            // editing the curve without republishing must change nothing.
            slot->getAutomation().setEnabled (false);
            const float afterLiveEdit = render (2.0, 3.0, nullptr);

            check ("The published schedule is immutable",
                   std::abs (afterLiveEdit - closed) < 1.0e-7f,
                   "editing without republish is inaudible");

            slot->getAutomation().setEnabled (false);
        }

        //-- The documented automation algebra ---------------------------------
        {
            LayerMix probe;
            probe.gain = 0.5f;
            probe.volumePoints = { { 0.0, 0.5f }, { len, 0.5f } };

            check ("Disabled automation leaves the fader alone",
                   std::abs (probe.gainAt (0.0) - 0.5f) < 1.0e-6f, "-6 dB stays -6 dB");

            probe.volumeAutomated = true;

            check ("Enabled volume automation multiplies the fader",
                   std::abs (probe.gainAt (0.0) - 0.25f) < 1.0e-6f, "0.5 x 0.5");

            probe.pan = 0.25f;
            probe.panAutomated = true;
            probe.panPoints = { { 0.0, 0.5f }, { len, 0.5f } };

            check ("Pan automation offsets the fader instead of replacing it",
                   std::abs (probe.panAt (0.0) - 0.25f) < 1.0e-6f,
                   "0.25 fader + (0.5 curve -> 0.0 offset) = 0.25");
        }

        //-- Meters are fed by the same render --------------------------------
        {
            t5.setLayerMuted (li2, false);
            e5.publishSchedule (t5, r5, 120.0);
            e5.setPositionBeats (2.0);

            juce::AudioBuffer<float> block (2, 512);
            e5.renderNextBlock (block, 512);

            check ("Layer and master meters follow the mix",
                   e5.getLayerPeak (li) > 0.001f && e5.getMasterPeak (0) > 0.001f,
                   "layer " + juce::String (e5.getLayerPeak (li), 3)
                     + " / master " + juce::String (e5.getMasterPeak (0), 3));
        }
    }

    //-- Phase 6: saving, loading, and the offline bounce --------------------
    {
        // A project that uses one of everything the file has to carry: two
        // layers, a route that is NOT the rack's first synth, a pan lane, a
        // note with its own envelope and its own output override, a synth
        // whose waveform and level differ from the defaults, and a synth lane.
        LayerStack t6;
        RackModel  r6;
        const int  slotA = r6.getSlotIdAt (0);
        const int  slotB = r6.getSlotIdAt (1);
        constexpr double len = timeline::defaultLengthBeats;

        const int la = t6.addLayer (utf8 ("Lead 主奏"), palette::layer3);
        const int lb = t6.addLayer ("Pad", palette::layer2);

        t6.setLayerOutputSlot (la, slotB);      // deliberately not slotA
        t6.setLayerLevelDb (la, -7.5f);
        t6.setLayerPan (la, -0.33f);
        t6.setLayerMuted (lb, true);
        // Phase 20: the eye now defaults to closed, so the round-trip test has
        // to set it OPEN — writing the default and reading it back would pass
        // even if the attribute were never saved.
        t6.setLayerVisible (lb, true);
        t6.setLayerAutomationTarget (la, LayerAutomationTarget::pan);
        t6.setLayerAutomationExpanded (la, true);

        auto* layerA = t6.getLayer (la);
        auto* layerB = t6.getLayer (lb);

        auto& lane = layerA->getAutomation();
        lane.resetFlat (0.3f, len);
        lane.addPoint (11.5, 0.87f, len);
        lane.setEnabled (true);

        const int na = layerA->addNote ({ 0, 61, 1.25, 2.75, 0.66f });
        auto* noteA = layerA->findNote (na);
        noteA->envelope = NoteEnvelope::makeFadeOut();
        noteA->envelope.addPoint (0.5f, 0.42f);
        noteA->outputSlotOverride = slotB;

        layerB->addNote ({ 0, 84, 8.0, 0.5, 0.9f });

        auto* synthB = r6.findSlot (slotB);
        synthB->waveform = Waveform::square;
        synthB->levelDb  = -9.25f;
        synthB->pan      = 0.41f;
        r6.setAutomationTarget (slotB, SlotAutomationTarget::level);
        synthB->automationFor (SlotAutomationTarget::level).resetFlat (0.2f, len);
        synthB->automationFor (SlotAutomationTarget::level).addPoint (4.0, 0.95f, len);
        synthB->automationFor (SlotAutomationTarget::level).setEnabled (true);
        r6.setAutomationExpanded (slotB, true);

        //-- In-memory round trip --------------------------------------------
        const juce::String xml = project::toXmlText (t6, r6, 97.5);

        LayerStack t6b;
        RackModel  r6b;
        double     bpmBack = 0.0;
        juce::String error;

        const bool parsed = project::fromXmlText (xml, t6b, r6b, bpmBack, error);

        check ("A serialised project parses back", parsed, error.isNotEmpty() ? error : "xml");
        check ("Tempo is restored", std::abs (bpmBack - 97.5) < 1.0e-6,
               juce::String (bpmBack, 2) + " BPM");
        check ("Layer count, names and colours are restored",
               t6b.getNumLayers() == 2
                 && t6b.getLayer (0) != nullptr && t6b.getLayer (0)->getName() == utf8 ("Lead 主奏")
                 && t6b.getLayer (0)->getColour() == palette::layer3,
               "2 layers");
        check ("The active layer is restored",
               t6b.getActiveLayerIndex() == t6.getActiveLayerIndex(),
               "index " + juce::String (t6b.getActiveLayerIndex()));
        check ("Channel strip settings are restored",
               t6b.getLayer (0)->getLevelDb() == -7.5f
                 && std::abs (t6b.getLayer (0)->getPan() + 0.33f) < 1.0e-6f
                 && t6b.getLayer (1)->isMuted()
                 && t6b.getLayer (1)->isVisible()
                 && t6b.getLayer (0)->getOutputSlotId() == slotB,
               "-7.5 dB / pan -0.33 / pad muted+eye open");
        check ("Layer automation target and curve survive",
               t6b.getLayer (0)->getAutomationTarget() == LayerAutomationTarget::pan
                 && t6b.getLayer (0)->isAutomationExpanded()
                 && t6b.getLayer (0)->getAutomation().isEnabled()
                 && t6b.getLayer (0)->getAutomation().hasSameShapeAs (lane),
               juce::String ((int) t6b.getLayer (0)->getAutomation().getPoints().size()) + " points");
        check ("A note's envelope and output override survive",
               t6b.getLayer (0)->findNote (na) != nullptr
                 && t6b.getLayer (0)->findNote (na)->outputSlotOverride == slotB
                 && t6b.getLayer (0)->findNote (na)->envelope.hasSameShapeAs (noteA->envelope)
                 && std::abs (t6b.getLayer (0)->findNote (na)->startBeat - 1.25) < 1.0e-6
                 && std::abs (t6b.getLayer (0)->findNote (na)->lengthBeats - 2.75) < 1.0e-6,
               "note id " + juce::String (na));

        {
            auto* restoredSlot = r6b.findSlot (slotB);

            check ("Synth settings, ids and automation are restored",
                   r6b.getNumSlots() == 2
                     && r6b.getSlotIdAt (0) == slotA
                     && restoredSlot != nullptr
                     && restoredSlot->waveform == Waveform::square
                     && restoredSlot->levelDb == -9.25f
                     && std::abs (restoredSlot->pan - 0.41f) < 1.0e-6f
                     && restoredSlot->getAutomation().isEnabled()
                     && restoredSlot->automationTarget == SlotAutomationTarget::level
                     && restoredSlot->automationExpanded
                     && restoredSlot->getAutomation().hasSameShapeAs (synthB->getAutomation()),
                   "Synth B waveform / level / pan / lane");
        }

        {
            // The id counter must resume above the restored ids, or the next
            // note added silently steals the inspector's target.
            const int freshId = t6b.getLayer (0)->addNote ({ 0, 60, 20.0, 1.0, 0.5f });

            check ("Note ids continue after the highest restored id",
                   freshId > na && t6b.getLayer (0)->findNote (freshId) != nullptr,
                   "new id " + juce::String (freshId));
        }

        //-- Through an actual file ------------------------------------------
        const auto testDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                               .getChildFile ("AtomicDAW-selftest");
        testDir.createDirectory();

        const auto projectFile = testDir.getChildFile ("roundtrip.atomic");

        check ("The project writes a .atomic file",
               project::save (projectFile, t6, r6, 97.5, error), error);
        check ("The file is XML with an AtomicDAW root",
               projectFile.loadFileAsString().contains ("<AtomicDAW"), projectFile.getFileName());

        LayerStack t6c;
        RackModel  r6c;
        double     bpmFile = 0.0;

        check ("The project loads back from disk",
               project::load (projectFile, t6c, r6c, bpmFile, error)
                 && t6c.getNumLayers() == 2
                 && std::abs (bpmFile - 97.5) < 1.0e-6,
               error);
        check ("UTF-8 layer names survive the file",
               t6c.getLayer (0) != nullptr && t6c.getLayer (0)->getName() == utf8 ("Lead 主奏"),
               t6c.getLayer (0) != nullptr ? t6c.getLayer (0)->getName() : "no layer");

        //-- A broken file must not destroy the open session ------------------
        {
            const auto brokenFile = testDir.getChildFile ("broken.atomic");
            brokenFile.replaceWithText ("<AtomicDAW version=\"1\"><Layers /></AtomicDAW>");

            const int layersBefore = t6c.getNumLayers();
            const int notesBefore  = (int) t6c.getLayer (0)->getNotes().size();

            juce::String brokenError;
            const bool refused = ! project::load (brokenFile, t6c, r6c, bpmFile, brokenError);

            check ("A project with no layers is refused", refused && brokenError.isNotEmpty(), brokenError);
            check ("A refused load leaves the session untouched",
                   t6c.getNumLayers() == layersBefore
                     && (int) t6c.getLayer (0)->getNotes().size() == notesBefore,
                   "still " + juce::String (layersBefore) + " layers");
        }

        //-- Offline bounce ---------------------------------------------------
        {
            LayerStack tx;
            RackModel  rx;
            const int  slotX = rx.getSlotIdAt (0);
            const int  lx    = tx.addLayer ("Bounce", palette::layer1);

            tx.setLayerOutputSlot (lx, slotX);
            tx.setLayerLevelDb (lx, 0.0f);

            auto* bounceLayer = tx.getLayer (lx);

            const int bn1 = bounceLayer->addNote ({ 0, 69, 0.0, 4.0, 0.9f });
            bounceLayer->findNote (bn1)->envelope = NoteEnvelope::makeFlat();

            const int bn2 = bounceLayer->addNote ({ 0, 72, 6.0, 2.0, 0.9f });
            bounceLayer->findNote (bn2)->envelope = NoteEnvelope::makeFlat();

            PlaybackEngine stage;
            stage.setSampleRate (wav::exportSampleRate);
            stage.publishSchedule (tx, rx, 120.0);

            auto schedule = stage.getSchedule();

            check ("The export length is the last note's end, not the 64-beat grid",
                   schedule != nullptr && std::abs (schedule->contentBeats - 8.0) < 1.0e-9,
                   juce::String (schedule == nullptr ? -1.0 : schedule->contentBeats, 3) + " beats");

            const auto unityFile  = testDir.getChildFile ("bounce-unity.wav");
            const auto quietFile  = testDir.getChildFile ("bounce-minus12.wav");

            const auto unity = wav::renderToFile (unityFile, *schedule, wav::exportSampleRate, 0.0);
            const auto quiet = [&]
            {
                tx.setLayerLevelDb (lx, -12.0f);
                stage.publishSchedule (tx, rx, 120.0);
                return wav::renderToFile (quietFile, *stage.getSchedule(), wav::exportSampleRate, 0.0);
            }();

            check ("Export writes a WAV file", unity.ok && unityFile.existsAsFile(), unity.message);
            check ("Export lasts exactly the content, 8 beats at 120 BPM",
                   std::abs (unity.seconds - 4.0) < 0.002, juce::String (unity.seconds, 3) + " s");
            check ("The bounce is audible", unity.peak > 0.01f,
                   "peak " + juce::String (unity.peak, 3)
                     + " / rms " + juce::String (unity.rms, 3));

            // The header, read back byte by byte: a file that merely "exists"
            // is not a WAV, and the point of the phase is a file other software
            // can open.
            {
                juce::FileInputStream in (unityFile);

                char riff[4] {}, wave[4] {}, fmt[4] {}, data[4] {};
                in.read (riff, 4);
                in.readInt();
                in.read (wave, 4);
                in.read (fmt, 4);
                const int fmtSize  = in.readInt();
                const int format   = in.readShort();
                const int channels = in.readShort();
                const int rate     = in.readInt();
                in.readInt();
                in.readShort();
                const int bits     = in.readShort();
                in.read (data, 4);
                const int dataSize = in.readInt();

                check ("The exported header is canonical 16-bit stereo 44.1 kHz PCM",
                       in.openedOk()
                         && std::memcmp (riff, "RIFF", 4) == 0
                         && std::memcmp (wave, "WAVE", 4) == 0
                         && std::memcmp (fmt, "fmt ", 4) == 0
                         && fmtSize == 16 && format == 1
                         && channels == 2 && rate == 44100 && bits == 16,
                       juce::String (channels) + " ch / " + juce::String (rate) + " Hz / "
                         + juce::String (bits) + " bit");

                check ("The data chunk matches the file size",
                       dataSize == unity.numSamples * 4 && unityFile.getSize() == 44 + dataSize,
                       juce::String (dataSize) + " bytes");
            }

            check ("The bounce obeys the mixer, not just the notes",
                   quiet.ok && unity.rms > 0.0f
                     && std::abs (quiet.rms / unity.rms - 0.251f) < 0.03f,
                   "-12 dB fader -> x" + juce::String (quiet.rms / juce::jmax (1.0e-6f, unity.rms), 3)
                     + " (unity rms " + juce::String (unity.rms, 5)
                     + " / quiet rms " + juce::String (quiet.rms, 5)
                     + " / peaks " + juce::String (unity.peak, 3) + " " + juce::String (quiet.peak, 3) + ")");

            {
                const auto cancelFile = testDir.getChildFile ("cancelled.wav");
                const auto cancelled = wav::renderToFile (cancelFile, *schedule, wav::exportSampleRate, 0.0,
                                                          [] (double progress) { return progress < 0.1; });

                check ("Cancelling leaves no half-written WAV",
                       ! cancelled.ok && ! cancelFile.existsAsFile(), cancelled.message);
            }

            check ("The open project reports a usable export length",
                   getContentSeconds() > 1.0,
                   juce::String (getContentSeconds(), 2) + " s");
        }
    }

    //-- Phase 7: automation tracks, channel inserts, master, scrolling -------
    {
        LayerStack t7;
        RackModel  r7;
        MixerModel x7;

        t7.addLayer ("Mix", palette::layer1);
        auto* layer7 = t7.getLayer (0);
        t7.setLayerOutputSlot (0, r7.getFirstSlotId());
        t7.setLayerLevelDb (0, 0.0f);

        const int note7 = layer7->addNote ({ 0, 72, 0.0, 4.0, 0.9f });
        layer7->findNote (note7)->envelope = NoteEnvelope::makeFlat();

        PlaybackEngine e7;
        e7.setSampleRate (44100.0);

        // Phase 11: a channel is fed by a synth, not by a layer. Route this
        // layer's synth to channel 1 (index 0) so there is something to process.
        r7.setMixerChannel (r7.getFirstSlotId(), 0);

        const double len = 8.0;      // 两条曲线都铺满 8 拍，足够覆盖被渲染的窗口

        auto rmsWith = [&] (MixerModel& model)
        {
            e7.publishSchedule (t7, r7, 120.0, 0.0, &model);

            juce::AudioBuffer<float> b (2, 44100);
            e7.resetMixState();
            e7.renderWindow (0.25, 44100, b, nullptr);

            return b.getRMSLevel (0, 0, 44100);
        };

        const float dry = rmsWith (x7);

        x7.setInsertType (0, 0, fx::Type::lowPass);
        x7.setInsertParam (0, 0, 0.0f, 0.5f);           // 120 Hz: nearly closed
        const float filtered = rmsWith (x7);

        check ("A channel insert processes that channel only",
               dry > 0.001f && filtered < dry * 0.6f,
               "low pass " + juce::String (filtered / juce::jmax (1.0e-6f, dry), 2) + "x");

        x7.clearChannelInserts (0);
        x7.setInsertType (1, 0, fx::Type::lowPass);
        x7.setInsertParam (1, 0, 0.0f, 0.5f);
        const float otherChannel = rmsWith (x7);

        check ("An insert on an empty channel changes nothing",
               std::abs (otherChannel / juce::jmax (1.0e-6f, dry) - 1.0f) < 0.02f,
               "x" + juce::String (otherChannel / juce::jmax (1.0e-6f, dry), 3));

        x7.clearChannelInserts (1);
        x7.setMasterLevelDb (-12.0f);
        const float quietMaster = rmsWith (x7);

        check ("The master fader scales the finished mix",
               std::abs (quietMaster / juce::jmax (1.0e-6f, dry) - 0.251f) < 0.02f,
               "-12 dB -> x" + juce::String (quietMaster / juce::jmax (1.0e-6f, dry), 3));

        x7.setMasterLevelDb (0.0f);

        // Both lanes of one owner live at the same time.
        layer7->automationFor (LayerAutomationTarget::volume).resetFlat (0.5f, len);
        layer7->automationFor (LayerAutomationTarget::volume).setEnabled (true);
        layer7->automationFor (LayerAutomationTarget::pan).resetFlat (0.2f, len);
        layer7->automationFor (LayerAutomationTarget::pan).setEnabled (true);

        e7.publishSchedule (t7, r7, 120.0, 0.0, &x7);
        auto bothLanes = e7.getSchedule();

        check ("Volume and pan lanes are published side by side",
               bothLanes != nullptr && ! bothLanes->layers.empty()
                 && bothLanes->layers[0].volumeAutomated
                 && bothLanes->layers[0].panAutomated
                 && std::abs (bothLanes->layers[0].gainAt (0.0) - 0.5f) < 1.0e-5f,
               "gain 0.5 with pan offset");

        layer7->automationFor (LayerAutomationTarget::volume).setEnabled (false);
        layer7->automationFor (LayerAutomationTarget::pan).setEnabled (false);

        //-- Phase 20: a group's pan curve steers its notes -------------------
        // The model half (does the lane exist, is it published) is easy; this
        // is the half that matters: does the sound move.
        {
            const int group7 = layer7->addGroup (utf8 ("组 Z"), groupPalette::group2);
            layer7->assignNotesToGroup ({ note7 }, group7);

            layer7->findGroup (group7)->getPanAutomation().resetFlat (1.0f, len);   // hard right
            layer7->findGroup (group7)->getPanAutomation().setEnabled (true);

            e7.publishSchedule (t7, r7, 120.0, 0.0, &x7);

            juce::AudioBuffer<float> b (2, 44100);
            e7.resetMixState();
            e7.renderWindow (0.25, 44100, b, nullptr);

            const float left  = b.getRMSLevel (0, 0, 44100);
            const float right = b.getRMSLevel (1, 0, 44100);

            check ("A group pan curve pushes its notes to one side",
                   right > left * 4.0f,
                   "L " + juce::String (left, 4) + " / R " + juce::String (right, 4));

            layer7->findGroup (group7)->getPanAutomation().setEnabled (false);
            layer7->assignNotesToGroup ({ note7 }, -1);
            layer7->removeGroup (group7);
        }
    }

    //-- Phase 7: the automation track list ----------------------------------
    {
        AutomationModel ax;
        LayerStack tx;
        RackModel  rx;

        tx.addLayer ("Auto", palette::layer2);
        auto* lx = tx.getLayer (0);

        const int trackV = ax.addTrack (AutomationOwner::layer, lx->getId(),
                                        (int) LayerAutomationTarget::volume);
        const int trackP = ax.addTrack (AutomationOwner::layer, lx->getId(),
                                        (int) LayerAutomationTarget::pan);

        check ("One layer can publish two automation tracks",
               trackV > 0 && trackP > 0 && trackV != trackP && ax.getNumTracks() == 2,
               juce::String (ax.getNumTracks()) + " tracks");

        const int again = ax.addTrack (AutomationOwner::layer, lx->getId(),
                                       (int) LayerAutomationTarget::volume);

        check ("Asking for the same parameter twice selects it instead of duplicating",
               again == trackV && ax.getNumTracks() == 2,
               "id " + juce::String (again));

        const bool moved = ax.moveTrack (trackP, -1);

        check ("Automation tracks can be reordered",
               moved && ax.getTracks().front().id == trackP && ax.getTracks().back().id == trackV,
               "pan track now first");

        ax.removeTrack (trackV);

        check ("Deleting a track leaves the others alone",
               ax.getNumTracks() == 1 && ax.getTracks().front().id == trackP,
               "1 track left");

        ax.addTrack (AutomationOwner::layer, lx->getId(), (int) LayerAutomationTarget::volume);
        ax.removeTracksForOwner (AutomationOwner::layer, lx->getId());

        check ("Deleting the owner takes its tracks with it",
               ax.getNumTracks() == 0, "0 tracks");
    }

    //-- Phase 7: mixer and track list survive a save/load --------------------
    {
        LayerStack t8;
        RackModel  r8;
        MixerModel mx;

        t8.addLayer ("Persist", palette::layer4);
        const int slot8 = r8.getFirstSlotId();
        t8.setLayerOutputSlot (0, slot8);

        mx.setInsertType (2, 3, fx::Type::delay);
        mx.setInsertParam (2, 3, 0.4f, 0.6f);
        mx.setInsertType (MixerModel::masterChannel, 0, fx::Type::air);
        mx.setMasterLevelDb (-6.0f);
        mx.setMasterPan (-0.25f);

        AutomationModel ax;
        ax.addTrack (AutomationOwner::layer, t8.getLayer (0)->getId(),
                     (int) LayerAutomationTarget::pan);

        const juce::String xml = project::toXmlText (t8, r8, 120.0, &mx, &ax);

        LayerStack t8b;
        RackModel  r8b;
        MixerModel mxb;
        AutomationModel axb;
        double bpm8 = 0.0;
        juce::String err8;

        const bool parsed8 = project::fromXmlText (xml, t8b, r8b, bpm8, err8, &mxb, &axb);

        check ("Mixer inserts survive the file",
               parsed8
                 && mxb.getInsert (2, 3).type == fx::Type::delay
                 && std::abs (mxb.getInsert (2, 3).p2 - 0.6f) < 1.0e-4f
                 && mxb.getInsert (MixerModel::masterChannel, 0).type == fx::Type::air
                 && mxb.getInsert (0, 3).type == fx::Type::none,
               juce::String (loadedInsertName8 (mxb)));

        check ("The master strip survives the file",
               std::abs (mxb.getMasterLevelDb() + 6.0f) < 1.0e-4f
                 && std::abs (mxb.getMasterPan() + 0.25f) < 1.0e-4f,
               juce::String (mxb.getMasterLevelDb(), 1) + " dB");

        check ("Automation tracks survive the file",
               axb.getNumTracks() == 1
                 && axb.getTracks().front().ownerId == t8b.getLayer (0)->getId()
                 && axb.getTracks().front().target == (int) LayerAutomationTarget::pan,
               juce::String (axb.getNumTracks()) + " track");
    }

    //-- Phase 7: horizontal scrolling and the tool strip ---------------------
    {
        pianoRoll.setScrollBeats (0.0);
        const double timelineAtStart = pianoRoll.getTimelineBeats();

        // Phase 11: the roll no longer stops at 64 beats. Pushing the view past
        // the end extends the timeline itself, one 64-beat chunk at a time —
        // and it keeps extending as long as the user keeps pushing.
        pianoRoll.setScrollBeats (200.0);
        const double atFar      = pianoRoll.getScrollBeats();
        const double grown      = pianoRoll.getTimelineBeats();

        pianoRoll.setScrollBeats (atFar + 300.0);
        const double pushed     = pianoRoll.getScrollBeats();

        check ("The roll scrolls past the default grid and the timeline grows",
               std::abs (timelineAtStart - PianoRollComponent::totalBeats) < 1.0e-9
                 && atFar > 100.0 && grown > PianoRollComponent::totalBeats
                 && pushed > atFar,
               juce::String (atFar, 1) + utf8 (" beats in, timeline ")
                 + juce::String (grown, 0) + utf8 (" -> ") + juce::String (pushed, 1));

        pianoRoll.setScrollBeats (0.0);

        pianoRoll.setTool (PianoRollComponent::Tool::pencil);
        const bool pencil = pianoRoll.getTool() == PianoRollComponent::Tool::pencil;
        pianoRoll.setTool (PianoRollComponent::Tool::eraser);
        const bool eraser = pianoRoll.getTool() == PianoRollComponent::Tool::eraser;
        pianoRoll.setTool (PianoRollComponent::Tool::select);

        check ("The tool strip switches between pencil, eraser and select",
               pencil && eraser && pianoRoll.getTool() == PianoRollComponent::Tool::select,
               "pencil / eraser / select");
    }

    //-- Phase 11: replace an instrument, sends, the endless roll, paint state --
    {
        //-- Replacing the instrument keeps the slot's identity ----------------
        // The slot id is what every layer route, note override and automation
        // track points at, so "replace" must not mint a new one.
        RackModel r11;
        const int slot11 = r11.getSlotIdAt (0);

        r11.setMixerChannel (slot11, 2);
        r11.setLevelDb (slot11, -9.0f);
        r11.getSlot (0)->automationFor (SlotAutomationTarget::level).resetFlat (0.4f, 8.0);
        const float curveBefore = r11.getSlot (0)->automationFor (SlotAutomationTarget::level)
                                     .valueAtBeat (0.0);

        r11.replaceWithNative (slot11, Waveform::square);

        const auto* replaced = r11.findSlot (slot11);

        check ("Replacing a synth's instrument keeps its id, fader, send and curve",
               replaced != nullptr
                 && replaced->id == slot11
                 && replaced->waveform == Waveform::square
                 && std::abs (replaced->levelDb + 9.0f) < 1.0e-5f
                 && replaced->mixerChannel == 2
                 && std::abs (replaced->automationFor (SlotAutomationTarget::level).valueAtBeat (0.0)
                                - curveBefore) < 1.0e-6f,
               "slot " + juce::String (slot11) + " -> " + waveformName (Waveform::square));

        //-- A channel only carries what a synth sends to it -------------------
        LayerStack t11;
        RackModel  r11b;
        MixerModel m11;

        const int slotB11 = r11b.getSlotIdAt (0);
        t11.addLayer ("P11", palette::layer1);
        t11.setLayerOutputSlot (0, slotB11);
        t11.setLayerLevelDb (0, 0.0f);

        const int note11 = t11.getLayer (0)->addNote ({ 0, 60, 0.0, 4.0, 0.9f });
        t11.getLayer (0)->findNote (note11)->envelope = NoteEnvelope::makeFlat();

        PlaybackEngine e11;
        e11.setSampleRate (44100.0);

        auto rms11 = [&] (MixerModel& model)
        {
            e11.publishSchedule (t11, r11b, 120.0, 0.0, &model);
            juce::AudioBuffer<float> b (2, 22050);
            e11.resetMixState();
            e11.renderWindow (0.0, 22050, b, nullptr);
            return b.getRMSLevel (0, 0, 22050);
        };

        const float dry11 = rms11 (m11);

        m11.setInsertType (0, 0, fx::Type::lowPass);
        m11.setInsertParam (0, 0, 0.0f, 0.5f);          // 120 Hz: nearly closed
        const float unconverted = rms11 (m11);

        check ("A channel insert is inaudible until a synth is sent there",
               dry11 > 0.001f
                 && std::abs (unconverted / juce::jmax (1.0e-6f, dry11) - 1.0f) < 0.02f,
               "x" + juce::String (unconverted / juce::jmax (1.0e-6f, dry11), 3));

        r11b.setMixerChannel (slotB11, 0);
        const float routed11 = rms11 (m11);

        check ("Sending the synth to that channel puts its insert in the path",
               routed11 < dry11 * 0.6f,
               "x" + juce::String (routed11 / juce::jmax (1.0e-6f, dry11), 2));

        {
            e11.setPositionBeats (0.0);
            juce::AudioBuffer<float> block (2, 512);
            e11.renderNextBlock (block, 512);

            check ("The channel meter lights only for the channel carrying a synth",
                   e11.getChannelPeak (0) > 0.001f && e11.getChannelPeak (1) == 0.0f,
                   "ch1 " + juce::String (e11.getChannelPeak (0), 3)
                     + " / ch2 " + juce::String (e11.getChannelPeak (1), 3));
        }

        //-- The strips follow the rack, not the layer stack -------------------
        // The demo (loaded above) has three layers and two synths, each sent to
        // its own channel: channel 3 must stay empty even though a third layer
        // exists, because a layer cannot feed a channel.
        check ("Mixer strips are fed by synths, so a third layer adds no channel",
               mixerPanel.slotIdOnChannel (0) == rack.getSlotIdAt (0)
                 && mixerPanel.slotIdOnChannel (1) == rack.getSlotIdAt (1)
                 && mixerPanel.slotIdOnChannel (2) < 0
                 && stack.getNumLayers() >= 3,
               juce::String (stack.getNumLayers()) + utf8 (" layers / ")
                 + juce::String (mixerPanel.sourceCountOnChannel (0)) + "+"
                 + juce::String (mixerPanel.sourceCountOnChannel (1)) + utf8 (" sources"));

        //-- Copy a note's state, draw with it, clear it -----------------------
        LayerStack t12;
        RackModel  r12;
        PianoRollComponent roll (t12, r12);
        roll.setSize (1200, 520);

        t12.addLayer ("Paint", palette::layer1);
        auto* l12 = t12.getLayer (0);

        const int king = l12->addNote ({ 0, 64, 8.0, 2.5, 0.42f });
        auto* source = l12->findNote (king);
        source->modTarget = NoteModTarget::pitch;
        source->envelope = NoteEnvelope::makeFadeOut();
        source->outputSlotOverride = r12.getSlotIdAt (1);

        // Snapshot what the template must carry BEFORE drawing: addNote()
        // push_backs into the same vector, so `source` is not a safe pointer to
        // compare against afterwards.
        const NoteEnvelope sourceEnvelope = source->envelope;
        const int          sourceOverride = source->outputSlotOverride;

        roll.setSelection ({ king });
        const bool copied = roll.copyNoteStateFromSelection();

        check ("Copying a note's state loads the pencil",
               copied && roll.hasPencilState()
                 && std::abs (roll.getPencilStateLengthBeats() - 2.5) < 1.0e-9
                 && std::abs (roll.getPencilStateVelocity() - 0.42f) < 1.0e-5f,
               juce::String (roll.getPencilStateLengthBeats(), 2) + utf8 (" beats / vel ")
                 + juce::String (roll.getPencilStateVelocity(), 2));

        roll.setScrollBeats (200.0);
        const int stamped = roll.addNoteAt (roll.getScrollBeats() + 1.0, 67);
        auto* made = l12->findNote (stamped);

        check ("A pencil note adopts the copied state, and lands past beat 200",
               made != nullptr
                 && std::abs (made->lengthBeats - 2.5) < 1.0e-9
                 && std::abs (made->velocity - 0.42f) < 1.0e-5f
                 && made->envelope.hasSameShapeAs (sourceEnvelope)
                 && made->modTarget == NoteModTarget::pitch
                 && made->outputSlotOverride == sourceOverride
                 && made->pitch == 67
                 && made->startBeat > 200.0,
               made != nullptr ? juce::String (made->startBeat, 1) + utf8 (" beat · ")
                                   + juce::String (made->lengthBeats, 2) + utf8 (" beats")
                               : juce::String ("no note"));

        check ("The roll reaches that far because its timeline grew",
               roll.getTimelineBeats() > 200.0,
               juce::String (roll.getTimelineBeats(), 0) + " beats");

        // Resetting for a new session must not strand the notes that are already
        // in this one: the roll falls back to the default, then grows straight
        // back to whatever the music needs.
        roll.resetTimeline();

        check ("Resetting the timeline still covers the notes already in it",
               roll.getTimelineBeats() >= roll.getContentEndBeat()
                 && roll.getScrollBeats() < 1.0e-6,
               juce::String (roll.getTimelineBeats(), 0) + utf8 (" beats · content ends at ")
                 + juce::String (roll.getContentEndBeat(), 0));

        roll.clearPencilState();
        const int plain = roll.addNoteAt (roll.getScrollBeats() + 6.0, 72);
        auto* plainNote = l12->findNote (plain);

        check ("Clearing the state returns the pencil to a plain default note",
               plainNote != nullptr
                 && std::abs (plainNote->lengthBeats - 1.0) < 1.0e-9
                 && std::abs (plainNote->velocity - 0.8f) < 1.0e-5f
                 && plainNote->modTarget == NoteModTarget::level
                 && plainNote->outputSlotOverride < 0
                 && plainNote->envelope.hasSameShapeAs (NoteEvent{}.envelope),
               plainNote != nullptr ? juce::String (plainNote->lengthBeats, 2) + utf8 (" beats / vel ")
                                          + juce::String (plainNote->velocity, 2)
                                    : juce::String ("no note"));
    }

    //-- Phase 8: the note's tail handle and its inline box -------------------
    {
        LayerStack t9;
        RackModel  r9;
        PianoRollComponent roll (t9, r9);
        roll.setSize (1200, 520);

        t9.addLayer ("Box", palette::layer1);
        auto* l9 = t9.getLayer (0);

        const int longNote  = l9->addNote ({ 0, 72, 0.0, 4.0, 0.9f });
        const int shortNote = l9->addNote ({ 0, 60, 8.0, 0.125, 0.9f });

        auto* wide  = l9->findNote (longNote);
        auto* tiny  = l9->findNote (shortNote);

        const auto wideHandle = roll.getNoteAutomationHandleBounds (*wide);
        const auto tinyHandle = roll.getNoteAutomationHandleBounds (*tiny);

        check ("A note wide enough carries the tail handle, a sliver does not",
               ! wideHandle.isEmpty() && tinyHandle.isEmpty(),
               juce::String (wideHandle.getWidth(), 0) + " px handle");

        roll.openNoteAutomation (0, longNote);
        check ("Clicking the handle opens the box on that note",
               roll.getExpandedNoteId() == longNote && roll.isNoteAutomationOpen(),
               "note " + juce::String (roll.getExpandedNoteId()));

        const auto box = roll.getNoteAutomationBoxBounds (*wide);
        const auto noteArea = roll.getNoteScreenBounds (0, longNote);

        check ("The box is as wide as its note",
               std::abs (box.getWidth() - noteArea.getWidth()) < 0.5f,
               juce::String (box.getWidth(), 1) + " px / note " + juce::String (noteArea.getWidth(), 1));

        check ("The box stays inside the grid",
               roll.getGridBounds().contains (box),
               juce::String (box.getY(), 0) + " .. " + juce::String (box.getBottom(), 0));

        roll.closeNoteAutomation();
        check ("Closing the box forgets the note", ! roll.isNoteAutomationOpen(), "closed");

        // The parameter menu writes the model, which is what the engine reads.
        wide->modTarget = NoteModTarget::pitch;
        NoteEnvelope savedShape = wide->envelope;

        check ("The menu's target is part of the note, not of the editor",
               wide->modTarget == NoteModTarget::pitch
                 && wide->envelope.hasSameShapeAs (savedShape),
               "pitch");
    }

    //-- Phase 10: unsaved changes and rack automation ------------------------
    {
        const juce::String before = captureState();

        check ("An untouched session has nothing to save", ! hasUnsavedChanges(), "clean");

        if (auto* first = stack.getLayer (0))
        {
            first->addNote ({ 0, 60, 100.0, 1.0, 0.6f });
            stack.notifyNotesChanged();
        }

        commitHistory();
        check ("An edit marks the session unsaved", hasUnsavedChanges(), "dirty");

        juce::String saveError;
        const auto tempFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("atomic-dirty-", project::fileExtension, false);

        const bool saved = saveProjectToFile (tempFile, saveError);

        check ("Saving clears the unsaved marker",
               saved && ! hasUnsavedChanges(),
               saved ? juce::String ("clean") : saveError);

        tempFile.deleteFile();
        currentProject = juce::File();


        // A synth curve must produce a row in the automation window, not just a
        // strip inside the card. Drop any row an earlier check already made for
        // this synth, so "creates one" means exactly that.
        const int slotId     = rack.getSlotIdAt (0);
        const int slotTarget = rack.getSlot (0) != nullptr
                                 ? (int) rack.getSlot (0)->automationTarget
                                 : -1;

        if (const int existing = automation.findTrackId (AutomationOwner::slot, slotId, slotTarget);
            existing > 0)
            automation.removeTrack (existing);

        const int tracksBefore = automation.getNumTracks();

        ensureRackAutomationTrack (slotId);


        const int trackId = automation.findTrackId (AutomationOwner::slot, slotId, slotTarget);

        check ("Rack automation creates a row in the automation window",
               slotId > 0 && trackId > 0 && automation.getNumTracks() == tracksBefore + 1,
               juce::String (automation.getNumTracks()) + utf8 (" tracks / target ")
                 + juce::String (slotTarget) + utf8 (" / id ") + juce::String (trackId));

        if (auto* slot = rack.findSlot (slotId))
            check ("Opening rack automation also opens its lane",
                   slot->automationExpanded
                     && slot->automationFor ((SlotAutomationTarget) slotTarget).isEnabled(),
                   slot->automationExpanded ? "expanded" : "collapsed");

        // Put the session back exactly as the screenshot expects it. The history
        // is reset in the same breath: restoring the models without moving
        // historyPos would leave the undo stack pointing at the throwaway edit
        // this block just made, and the next undo would restore *that*.
        cancelPendingUpdate();
        restoreState (before);
        cancelPendingUpdate();

        history.clear();
        history.push_back (captureState());
        historyPos = 0;
        savedStateText = history.front();
        dirtyCache     = false;
        repaint (topBarBounds);
    }

    //-- Phase 8: undo, redo and a fresh project ------------------------------
    {
        const juce::String baseline = captureState();
        const int layersBefore = stack.getNumLayers();
        const int historyBefore = getHistorySize();

        // The screenshots are taken after this function returns, so the history
        // itself is saved and put back: the test must not leave the app as if
        // nothing had ever been edited.
        const auto savedHistory = history;
        const int  savedPos = historyPos;

        stack.addLayer (utf8 ("撤销测试"), palette::layer4);
        commitHistory();          // the AsyncUpdater would do this on the next turn

        check ("An edit becomes one undo step",
               getHistorySize() == historyBefore + 1 && canUndo(),
               juce::String (getHistorySize()) + " states");

        undo();
        check ("Undo restores the previous session", stack.getNumLayers() == layersBefore,
               juce::String (stack.getNumLayers()) + " layers");

        redo();
        check ("Redo puts the edit back", stack.getNumLayers() == layersBefore + 1,
               juce::String (stack.getNumLayers()) + " layers");

        undo();                   // back to the demo state the snapshot needs

        const int notesBefore = stack.getLayer (0) != nullptr
                                  ? (int) stack.getLayer (0)->getNotes().size() : -1;

        newProject();
        check ("New project is one empty layer, one built-in synth, sent to channel 1",
               stack.getNumLayers() == 1
                 && stack.getLayer (0) != nullptr
                 && stack.getLayer (0)->getNotes().empty()
                 && rack.getNumSlots() == 1
                 && ! rack.getSlot (0)->isPlugin()
                 && rack.getSlot (0)->mixerChannel == 0,
               juce::String (stack.getNumLayers()) + " layer / "
                 + juce::String (rack.getNumSlots()) + " synths");

        check ("A brand new project has nothing to save",
               ! hasUnsavedChanges(),
               hasUnsavedChanges() ? "dirty" : "clean");

        // Put the demo back: everything after this point (and the screenshot)
        // expects the project the app booted with.
        cancelPendingUpdate();
        restoreState (baseline);
        cancelPendingUpdate();

        history = savedHistory;
        historyPos = savedPos;
        savedStateText = captureState();      // the restored demo is the baseline again

        check ("The demo is back after the undo tests",
               stack.getNumLayers() == layersBefore
                 && stack.getLayer (0) != nullptr
                 && (int) stack.getLayer (0)->getNotes().size() == notesBefore,
               juce::String (stack.getNumLayers()) + " layers / "
                 + juce::String (stack.getLayer (0) != nullptr
                                   ? (int) stack.getLayer (0)->getNotes().size() : -1)
                 + " notes (expected " + juce::String (notesBefore) + ")");
    }

    //-- Phase 8: the mixer window and its contents ---------------------------
    {
        const auto image = createMixerSnapshot (1180, 300);
        check ("The console renders into its own window",
               image.isValid() && image.getWidth() == 1180 && image.getHeight() == 300,
               juce::String (image.getWidth()) + "x" + juce::String (image.getHeight()));

        toggleMixerWindow();
        const bool opened = isMixerWindowOpen();
        toggleMixerWindow();
        const bool closed = ! isMixerWindowOpen();

        check ("The mixer button opens and closes the window", opened && closed,
               opened ? (closed ? "open / closed" : "open only") : "did not open");
    }

    //-- Phase 8: the gear menu's two settings --------------------------------
    {
        const bool snapDefault = pianoRoll.isSnapEnabled();
        const float ghostDefault = pianoRoll.getGhostAlpha();

        pianoRoll.setSnapEnabled (false);
        const bool off = ! pianoRoll.isSnapEnabled();
        pianoRoll.setSnapEnabled (true);

        pianoRoll.setGhostAlpha (0.6f);
        const bool ghostAlphaSet = std::abs (pianoRoll.getGhostAlpha() - 0.6f) < 0.001f;
        pianoRoll.setGhostAlpha (ghostDefault);

        check ("Settings change snapping and ghost opacity",
               snapDefault && off && ghostAlphaSet && std::abs (pianoRoll.getGhostAlpha() - ghostDefault) < 0.001f,
               juce::String (pianoRoll.getGhostAlpha(), 2) + " alpha");
    }


    {
        LayerStack t8;
        RackModel  r8;
        MixerModel x8;

        t8.addLayer ("Mod", palette::layer1);
        auto* l8 = t8.getLayer (0);
        t8.setLayerOutputSlot (0, r8.getFirstSlotId());
        t8.setLayerLevelDb (0, 0.0f);
        t8.setLayerPan (0, 0.0f);

        const int note8 = l8->addNote ({ 0, 69, 0.0, 4.0, 0.9f });
        auto* n8 = l8->findNote (note8);

        n8->envelope.loadPoints ({ { 0.0f, 1.0f }, { 1.0f, 1.0f } });
        n8->envelope.setEnabled (true);

        PlaybackEngine e8;
        e8.setSampleRate (44100.0);

        auto renderLR = [&] (juce::AudioBuffer<float>& buffer)
        {
            buffer.setSize (2, 44100);
            buffer.clear();       // setSize alone leaves the previous render in place
            e8.publishSchedule (t8, r8, 120.0, 0.0, &x8);
            e8.resetMixState();
            e8.renderWindow (0.25, 44100, buffer, nullptr);
        };

        juce::AudioBuffer<float> levelBuf;
        n8->modTarget = NoteModTarget::level;
        renderLR (levelBuf);

        const float levelL = levelBuf.getRMSLevel (0, 0, 44100);
        const float levelR = levelBuf.getRMSLevel (1, 0, 44100);

        check ("A level curve leaves the note centred",
               levelL > 0.001f && std::abs (levelL - levelR) < 0.02f * levelL,
               juce::String (levelL, 4) + " / " + juce::String (levelR, 4));

        // Same curve, now aimed at pan: a flat 1.0 means "hard right".
        juce::AudioBuffer<float> panBuf;
        n8->modTarget = NoteModTarget::pan;
        renderLR (panBuf);

        const float panL = panBuf.getRMSLevel (0, 0, 44100);
        const float panR = panBuf.getRMSLevel (1, 0, 44100);

        check ("A pan curve moves the note across the stereo field",
               panR > 0.001f && panR > panL * 4.0f,
               "L " + juce::String (panL, 4) + " / R " + juce::String (panR, 4));

        // Same curve, now aimed at pitch: 1.0 at the head bending to 0.0 is one
        // octave down by the tail, so the tail must cross zero far less often.
        juce::AudioBuffer<float> pitchBuf;
        n8->envelope.loadPoints ({ { 0.0f, 1.0f }, { 1.0f, 0.0f } });
        n8->modTarget = NoteModTarget::pitch;
        renderLR (pitchBuf);

        {
            const auto published = e8.getSchedule();

            const int publishedTarget = (published != nullptr && ! published->notes.empty())
                                          ? (int) published->notes.front().modTarget : -1;
            const size_t tableSize = (published != nullptr && ! published->notes.empty())
                                       ? published->notes.front().pitchIntegral.size() : 0;
            const float bendEnd = (published != nullptr && ! published->notes.empty()
                                     && ! published->notes.front().pitchIntegral.empty())
                                    ? published->notes.front().pitchIntegral.back() : 0.0f;

            check ("Publishing a pitch curve builds its bend table",
                   publishedTarget == (int) NoteModTarget::pitch
                     && tableSize == (size_t) ScheduledNote::pitchIntegralSteps + 1
                     && bendEnd < -0.1f,
                   "target " + juce::String (publishedTarget) + " / table "
                     + juce::String ((int) tableSize) + " / bend "
                     + juce::String (bendEnd, 3));
        }

        auto zeroCrossings = [] (const juce::AudioBuffer<float>& b, int from, int to)
        {
            const float* data = b.getReadPointer (0);
            int count = 0;

            for (int i = juce::jmax (1, from); i < juce::jmin (to, b.getNumSamples()); ++i)
                if ((data[i - 1] < 0.0f) != (data[i] < 0.0f))
                    ++count;

            return count;
        };

        // Equal-length windows, or the counts are not comparable: the first
        // quarter of the buffer against the last quarter of the same size.
        const int quarter = pitchBuf.getNumSamples() / 4;
        const int head = zeroCrossings (pitchBuf, 0, quarter);
        const int tail = zeroCrossings (pitchBuf, pitchBuf.getNumSamples() - quarter,
                                        pitchBuf.getNumSamples());

        check ("A pitch curve bends the note instead of changing its level",
               head > 20 && tail > 0 && tail < head * 0.85f,
               juce::String (head) + " crossings at the head / " + juce::String (tail) + " at the tail");
    }

    //==========================================================================
    //  Phase 9: VST3 hosting, the note parameter box, routing focus
    //==========================================================================
    {
        juce::PluginDescription synthDescription, effectDescription;

        for (const auto& entry : plugins.getLibrary())
        {
            if (entry.description.name.containsIgnoreCase ("Test Synth"))
                synthDescription = entry.description;
            else if (entry.description.name.containsIgnoreCase ("Test FX"))
                effectDescription = entry.description;
        }

        check ("A VST3 scan separates instruments from effects",
               synthDescription.name.isNotEmpty() && effectDescription.name.isNotEmpty(),
               juce::String (plugins.countInstruments()) + " instruments / "
                 + juce::String (plugins.countEffects()) + " effects");

        //-- The rack takes a hosted instrument --------------------------------
        const int slotsBefore    = rack.getNumSlots();
        const int activeLayerIdx = stack.getActiveLayerIndex();
        const bool instrumentAdded = addPluginInstrument (synthDescription);

        const SynthSlot* pluginSlot = rack.getSlot (rack.getNumSlots() - 1);
        const int        pluginSlotId = pluginSlot != nullptr ? pluginSlot->id : -1;
        auto*            activeLayer  = stack.getLayer (activeLayerIdx);

        check ("The rack accepts a hosted instrument and points the active layer at it",
               instrumentAdded && rack.getNumSlots() == slotsBefore + 1
                 && pluginSlot != nullptr && pluginSlot->isPlugin()
                 && activeLayer != nullptr && activeLayer->getOutputSlotId() == pluginSlotId,
               pluginSlot != nullptr ? pluginSlot->name : juce::String ("no slot"));

        //-- A hosted instrument renders the notes routed to it ----------------
        {
            LayerStack layers;
            RackModel  hostRack;
            MixerModel mix;
            PluginHost host;

            host.setPlayConfig (44100.0, 512);

            juce::String loadError;
            const int instanceId = host.load (synthDescription, &loadError);

            layers.addLayer ("VST", palette::layer1);
            layers.setLayerOutputSlot (0, hostRack.addPluginSynth (synthDescription, instanceId));
            layers.setLayerLevelDb (0, 0.0f);
            layers.setLayerPan (0, 0.0f);
            layers.getLayer (0)->addNote ({ 0, 60, 0.0, 2.0, 0.9f });

            PlaybackEngine engine;
            engine.setSampleRate (44100.0);
            engine.setPluginHost (&host);
            engine.prepareToPlay (44100.0, 512);

            juce::AudioBuffer<float> buffer;
            buffer.setSize (2, 44100);
            buffer.clear();

            engine.publishSchedule (layers, hostRack, 120.0, 0.0, &mix);
            engine.resetMixState();

            // From 0, not from the middle: a synth is STATEFUL. The oscillator
            // does not care where the window starts (its phase comes from the
            // note's absolute time), but a plugin never receives the note-on if
            // the window opens after it, and simply stays silent.
            engine.renderWindow (0.0, 44100, buffer, nullptr);

            const float hostedRms = buffer.getRMSLevel (0, 0, 44100);

            check ("Notes routed to a hosted VST3 come out of it, not the oscillator",
                   instanceId > 0 && hostedRms > 0.002f,
                   "instance " + juce::String (instanceId) + " / RMS "
                     + juce::String (hostedRms, 4)
                     + (loadError.isNotEmpty() ? " / " + loadError : ""));

            // The plugin writes straight into the master bus, so the one thing
            // that must still hold is that the master fader scales it.
            MixerModel quietMaster;
            quietMaster.setMasterLevelDb (-6.0f);

            auto renderHostedWith = [&] (MixerModel& mixerToUse)
            {
                juce::AudioBuffer<float> out;
                out.setSize (2, 44100);
                out.clear();

                engine.publishSchedule (layers, hostRack, 120.0, 0.0, &mixerToUse);
                engine.resetMixState();
                engine.renderWindow (0.0, 44100, out, nullptr);

                return out.getRMSLevel (0, 0, 44100);
            };

            const float faderRatio = hostedRms > 0.0f ? renderHostedWith (quietMaster) / hostedRms : 0.0f;

            check ("Plugin output still obeys the master fader",
                   faderRatio > 0.40f && faderRatio < 0.60f,
                   "-6 dB master  ->  x" + juce::String (faderRatio, 3));

            //-- ... and a hosted effect sits in a channel chain -----------------
            // Tested with a built-in synth routed to channel 1: a hosted
            // instrument would need its own plugin instance, and this is about
            // the insert chain, not about two plugins talking to each other.
            const int fxInstance = host.load (effectDescription, &loadError);

            LayerStack fxLayers;
            RackModel  fxRack;

            fxLayers.addLayer ("FX", palette::layer1);
            const int fxSynth = fxRack.addSynth();
            fxLayers.setLayerOutputSlot (0, fxSynth);
            fxLayers.setLayerLevelDb (0, 0.0f);
            fxLayers.setLayerPan (0, 0.0f);
            fxLayers.getLayer (0)->addNote ({ 0, 60, 0.0, 4.0, 0.9f });

            // Phase 11: channel inserts process a synth bus, so the synth has to
            // be sent to the channel before the insert can be heard.
            fxRack.setMixerChannel (fxSynth, 0);

            MixerModel dry, wet;
            wet.setInsertPlugin (0, 0, effectDescription, fxInstance);

            auto renderFxWith = [&] (MixerModel& mixerToUse)
            {
                juce::AudioBuffer<float> out;
                out.setSize (2, 44100);
                out.clear();

                engine.publishSchedule (fxLayers, fxRack, 120.0, 0.0, &mixerToUse);
                engine.resetMixState();
                engine.renderWindow (0.25, 44100, out, nullptr);

                return out.getRMSLevel (0, 0, 44100);
            };

            const float dryRms   = renderFxWith (dry);
            const float wetRms   = renderFxWith (wet);
            const float wetRatio = dryRms > 0.0f ? wetRms / dryRms : 0.0f;

            check ("A VST3 insert in a channel processes the channel (gain 0.5 halves it)",
                   fxInstance > 0 && wetRatio > 0.40f && wetRatio < 0.60f,
                   "dry " + juce::String (dryRms, 4) + " / wet " + juce::String (wetRms, 4)
                     + "  ->  x" + juce::String (wetRatio, 3));

            engine.setPluginHost (nullptr);
        }

        //-- The per-note parameter box ----------------------------------------
        {
            auto* layer = stack.getActiveLayer();

            if (layer != nullptr && ! layer->getNotes().empty())
            {
                const auto& note      = layer->getNotes().front();
                const auto  noteWidth = pianoRoll.getNoteScreenBounds (activeLayerIdx, note.id).getWidth();
                const auto  box       = pianoRoll.getNoteAutomationBoxBounds (note);

                check ("The parameter box is exactly as wide as its note",
                       std::abs (box.getWidth() - juce::jmax (noteWidth, 22.0f)) < 1.0f,
                       "note " + juce::String (noteWidth, 1) + " px / box "
                         + juce::String (box.getWidth(), 1) + " px");

                // A 1/16 note is about 8 px wide. Widening the box would break
                // the alignment the whole feature exists for, so it stays 22 px
                // and the label row becomes an icon instead.
                NoteEvent tiny = note;
                tiny.lengthBeats = 0.15;

                const auto tinyBox = pianoRoll.getNoteAutomationBoxBounds (tiny);

                check ("A note too short for a dropdown keeps a 22 px box",
                       std::abs (tinyBox.getWidth() - 22.0f) < 0.5f,
                       juce::String (tinyBox.getWidth(), 1) + " px");
            }
        }

        //-- Selection highlights the synth, clicking another one re-routes it --
        {
            auto* layer = stack.getLayer (activeLayerIdx);

            if (layer != nullptr && ! layer->getNotes().empty() && rack.getNumSlots() >= 2)
            {
                const int noteId    = layer->getNotes().front().id;
                const int layerSlot = layer->getOutputSlotId();

                pianoRoll.setSelection ({ noteId });

                check ("Selecting notes highlights the synth they resolve to",
                       rackPanel.hasRoutingFocus() && rackPanel.getRoutingFocusSlotId() == layerSlot,
                       "focus " + juce::String (rackPanel.getRoutingFocusSlotId())
                         + " / resolves to " + juce::String (pianoRoll.getSelectedRoutingSlotId()));

                int otherSlot = -1;

                for (int i = 0; i < rack.getNumSlots(); ++i)
                    if (rack.getSlotIdAt (i) != layerSlot)
                    {
                        otherSlot = rack.getSlotIdAt (i);
                        break;
                    }

                pianoRoll.routeSelectionTo (otherSlot);

                const auto* moved = stack.getLayer (activeLayerIdx)->findNote (noteId);
                const int   overridesAfterMove = layerPanel.getOverrideCount (activeLayerIdx);

                check ("Choosing another synth moves the selected notes onto it",
                       moved != nullptr && moved->outputSlotOverride == otherSlot
                         && pianoRoll.getSelectedRoutingSlotId() == otherSlot
                         && pianoRoll.selectionUsesOverride()
                         && overridesAfterMove > 0,
                       "note -> slot " + juce::String (moved != nullptr ? moved->outputSlotOverride : -999)
                         + " / " + juce::String (overridesAfterMove) + " overrides");

                resetLayerNoteRouting (activeLayerIdx);

                const auto* restored = stack.getLayer (activeLayerIdx)->findNote (noteId);

                check ("The layer's reset button puts every note back on the layer's synth",
                       restored != nullptr && restored->outputSlotOverride < 0
                         && layerPanel.getOverrideCount (activeLayerIdx) == 0
                         && pianoRoll.getSelectedRoutingSlotId() == layerSlot,
                       juce::String (layerPanel.getOverrideCount (activeLayerIdx)) + " overrides left");
            }
        }

        //-- The settings window ------------------------------------------------
        {
            auto* window = ensureSettingsWindow();

            check ("The settings window has the three tabs it is supposed to have",
                   window != nullptr && window->getNumPages() == 3,
                   juce::String (window != nullptr ? window->getNumPages() : 0) + " tabs");
        }

        //-- The project file keeps the plugins ---------------------------------
        {
            const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("atomic-phase9-roundtrip.atomic");

            juce::String saveError;
            const bool saved = saveProjectToFile (file, saveError);
            const auto text  = file.loadFileAsString();

            MainComponent reloaded;
            juce::String loadError;
            const bool loaded = reloaded.loadProjectFromFile (file, loadError);

            const auto& restoredRack = reloaded.getRack();
            const auto* restoredSlot = restoredRack.getSlot (restoredRack.getNumSlots() - 1);

            check ("A project round trip keeps the hosted instrument",
                   saved && loaded && text.containsIgnoreCase ("<plugin")
                     && restoredSlot != nullptr && restoredSlot->isPlugin(),
                   (saved ? "" : "save: " + saveError + " ")
                     + (loaded ? "" : "load: " + loadError + " ")
                     + (restoredSlot != nullptr ? restoredSlot->name : juce::String ("no slot")));

            file.deleteFile();
        }
    }

    //-- Phase 12: automation rows, the split, and the console's short cuts ----
    {
        newProject();

        check ("A new project opens the automation window empty",
               automation.getNumTracks() == 0,
               juce::String (automation.getNumTracks()) + utf8 (" tracks"));

        auto* layer12 = stack.getLayer (0);
        const int layerId12 = layer12 != nullptr ? layer12->getId() : -1;

        ensureAutomationTrack (AutomationOwner::layer, layerId12, (int) LayerAutomationTarget::volume);

        auto* lane12 = layer12 != nullptr
                         ? &layer12->automationFor (LayerAutomationTarget::volume) : nullptr;

        check ("Choosing a layer parameter creates its automation track",
               automation.getNumTracks() == 1
                 && lane12 != nullptr && lane12->isEnabled()
                 && layer12 != nullptr && layer12->isAutomationExpanded()
                 && automation.getTracks()[0].owner == AutomationOwner::layer
                 && automation.getTracks()[0].ownerId == layerId12,
               juce::String (automation.getNumTracks()) + utf8 (" tracks · lane ")
                 + (lane12 != nullptr && lane12->isEnabled() ? "on" : "off"));

        //-- Rows resize, and the height follows the TRACK ---------------------
        automationWindow.setTrackHeight (0, 96);
        const int shortRow = automationWindow.getTrackHeight (0);

        ensureAutomationTrack (AutomationOwner::layer, layerId12, (int) LayerAutomationTarget::pan);
        automationWindow.setTrackHeight (0, 140);

        const int firstTrackId = automation.getTracks()[0].id;
        const bool moved = automation.moveTrack (firstTrackId, 1);
        const int indexAfter = automation.indexOfTrack (firstTrackId);

        check ("Automation rows resize, and keep their height when reordered",
               shortRow == 96
                 && moved && indexAfter == 1
                 && automationWindow.getTrackHeight (1) == 140,
               juce::String (automationWindow.getTrackHeight (1)) + utf8 (" px · row ")
                 + juce::String (indexAfter) + utf8 (" · was ") + juce::String (shortRow) + " px");

        //-- Dragging the band moves height from one pane to the other ---------
        {
            const int before = pianoRoll.getHeight() + automationWindow.getHeight();
            const int paneBefore = getAutomationPaneHeight();
            const int wanted = juce::jlimit (120, juce::jmax (120, getSplitTravel() - 160),
                                             paneBefore + 70);
            setAutomationPaneHeight (wanted);

            check ("The splitter trades height between the roll and the curves",
                   std::abs (getAutomationPaneHeight() - wanted) <= 1
                     && automationWindow.getHeight() == getAutomationPaneHeight()
                     && pianoRoll.getHeight() + automationWindow.getHeight() == before,
                   juce::String (pianoRoll.getHeight()) + " / "
                     + juce::String (automationWindow.getHeight()) + " px");

            // The band really is between them: a split that only changes the
            // two heights could just as well overlap one of the panes.
            check ("The grab band sits between the roll and the curves",
                   splitterBar.getY() == pianoRoll.getBottom()
                     && automationWindow.getY() == splitterBar.getBottom()
                     && splitterBar.getHeight() == splitterThickness,
                   splitterBar.getBounds().toString());

            // Put the split back: the snapshot photographs the fixture, and a
            // pane left at +70 would look like the default.
            setAutomationPaneHeight (paneBefore);
        }

        //-- A console strip automates the synth it carries --------------------
        {
            const int newSlot = rack.getSlotIdAt (0);
            rack.setMixerChannel (newSlot, 0);

            const int slotOnZero = mixerPanel.slotIdOnChannel (0);
            const int tracksBefore = automation.getNumTracks();

            ensureAutomationTrack (AutomationOwner::slot, slotOnZero, (int) SlotAutomationTarget::level);

            check ("A mixer strip creates the automation of the synth it carries",
                   slotOnZero == newSlot
                     && automation.getNumTracks() == tracksBefore + 1
                     && automation.findTrackId (AutomationOwner::slot, slotOnZero,
                                                (int) SlotAutomationTarget::level) > 0,
                   "ch1 -> " + rack.getSlotName (slotOnZero) + " / "
                     + juce::String (automation.getNumTracks()) + utf8 (" tracks"));

            check ("An empty channel has no synth to automate",
                   mixerPanel.slotIdOnChannel (3) < 0,
                   "ch4 empty");
        }
    }

    //-- Transport glyphs -----------------------------------------------------
    // Phase 13 enlarged the buttons and the icons stayed behind: the play
    // triangle's fixed 12 px insets ate all but 4 px of its height, and the
    // pause bars hung off the same 12 px, which left them sitting below the
    // plate's centre. Both are plain geometry, so both are testable without a
    // display — and the next resize cannot quietly undo the fix.
    {
        const auto playButton  = getPlayButtonBounds().toFloat();
        const auto pauseButton = getPauseButtonBounds().toFloat();
        const auto stopButton  = getStopButtonBounds().toFloat();

        const auto play  = playGlyph (playButton).getBounds();
        const auto bars  = pauseGlyph (pauseButton);
        const auto pause = bars.left.getUnion (bars.right);
        const auto stop  = stopGlyph (stopButton);

        constexpr float tol = 0.5f;

        check ("The play triangle fills its button instead of collapsing to a sliver",
               play.getHeight() > playButton.getHeight() * 0.4f
                 && play.getWidth() > playButton.getWidth() * 0.3f,
               juce::String (play.getWidth(), 1) + " x " + juce::String (play.getHeight(), 1)
                 + utf8 (" px in a ") + juce::String (playButton.getWidth(), 1) + " x "
                 + juce::String (playButton.getHeight(), 1) + " px button");

        check ("The play triangle sits on the plate's centre",
               std::abs (play.getCentreY() - playButton.getCentreY()) < tol
                 && std::abs (play.getCentreX() - playButton.getCentreX()) < play.getWidth() * 0.2f,
               juce::String (play.getCentreX() - playButton.getCentreX(), 2) + " / "
                 + juce::String (play.getCentreY() - playButton.getCentreY(), 2));

        check ("The pause bars sit on the plate's centre, not at the bottom",
               std::abs (pause.getCentreY() - pauseButton.getCentreY()) < tol
                 && std::abs (pause.getCentreX() - pauseButton.getCentreX()) < tol
                 && pause.getHeight() > pauseButton.getHeight() * 0.4f,
               juce::String (pause.getCentreY() - pauseButton.getCentreY(), 2));

        check ("The pause bars are equal, with a gap between them",
               std::abs (bars.left.getWidth() - bars.right.getWidth()) < 0.01f
                 && bars.right.getX() > bars.left.getRight(),
               juce::String (bars.left.getWidth(), 1) + " x " + juce::String (bars.left.getHeight(), 1));

        check ("The stop square is centred and square",
               std::abs (stop.getWidth() - stop.getHeight()) < 0.01f
                 && std::abs (stop.getCentreX() - stopButton.getCentreX()) < tol
                 && std::abs (stop.getCentreY() - stopButton.getCentreY()) < tol,
               juce::String (stop.getWidth(), 1) + " px");
    }

    //-- One gutter for both timelines ----------------------------------------
    // The automation canvas begins where this column ends, so the two numbers
    // are the same number: otherwise every curve is drawn under the wrong beat
    // and the two playheads never line up. They were 140 and 74 before, which
    // put the curves 86 px off the grid.
    check ("The automation track headers are exactly as wide as the piano keys",
           AutomationWindow::listWidth == PianoRollComponent::keyboardWidth,
           juce::String (AutomationWindow::listWidth) + " / "
             + juce::String (PianoRollComponent::keyboardWidth) + " px");

    //-- Phase 16: note groups ------------------------------------------------
    // A local fixture, like the mixer checks above: grouping touches routing
    // and rendering, and both deserve a session this test owns outright.
    {
        LayerStack gs;
        RackModel  gr;

        const int slotA = gr.getFirstSlotId();
        const int slotB = gr.getSlotIdAt (1);

        gs.addLayer (utf8 ("组测试"), palette::layer1);
        auto* layer = gs.getLayer (0);
        gs.setLayerOutputSlot (0, slotA);

        const int n1 = layer->addNote ({ 0, 60, 0.0, 4.0, 0.8f });
        const int n2 = layer->addNote ({ 0, 64, 4.0, 4.0, 0.8f });
        const int n3 = layer->addNote ({ 0, 67, 8.0, 4.0, 0.8f });

        const auto original = *layer->findNote (n1);

        const int groupId = layer->addGroup (groupPalette::defaultName (0), groupPalette::group1);
        layer->assignNotesToGroup ({ n1, n2 }, groupId);

        // Phase 20: give the group a pan curve, so the v9 field has something
        // to be lost by. Level stays flat — the two lanes are saved separately.
        if (auto* curveGroup = layer->findGroup (groupId))
        {
            curveGroup->getPanAutomation().resetFlat (0.5f, 32.0);
            curveGroup->getPanAutomation().addPoint (8.0, 0.9f, 32.0);
            curveGroup->getPanAutomation().setEnabled (true);
        }

        const auto* afterGrouping = layer->findNote (n1);

        check ("A group holds exactly the notes assigned to it",
               layer->countNotesInGroup (groupId) == 2
                 && layer->getGroupNoteIds (groupId).size() == 2,
               juce::String (layer->countNotesInGroup (groupId)) + utf8 (" 个成员"));

        // Grouping is an address, not a container: the notes must not move,
        // change length, or lose their envelopes.
        check ("Grouping leaves every note exactly where it was",
               afterGrouping != nullptr
                 && afterGrouping->startBeat == original.startBeat
                 && afterGrouping->lengthBeats == original.lengthBeats
                 && afterGrouping->velocity == original.velocity,
               juce::String (afterGrouping != nullptr ? afterGrouping->startBeat : -1.0, 2));

        layer->findGroup (groupId)->setOutputSlotId (slotB);

        const auto inGroup  = routing::resolve (*layer, *layer->findNote (n2), gr);
        const auto outGroup = routing::resolve (*layer, *layer->findNote (n3), gr);

        check ("A group mount beats the layer default, and only inside the group",
               inGroup.slotId == slotB && inGroup.viaGroup && ! inGroup.overridden
                 && outGroup.slotId == slotA && ! outGroup.viaGroup,
               "in " + juce::String (inGroup.slotId) + " / out " + juce::String (outGroup.slotId));

        layer->findNote (n1)->outputSlotOverride = slotA;

        const auto overridden = routing::resolve (*layer, *layer->findNote (n1), gr);

        check ("A note-level override still wins over its group",
               overridden.slotId == slotA && overridden.overridden && ! overridden.viaGroup,
               "slot " + juce::String (overridden.slotId));

        //-- The group curve really is heard ----------------------------------
        {
            LayerStack as;
            RackModel  ar;
            PlaybackEngine engine;

            const int aSlot = ar.getFirstSlotId();
            as.addLayer (utf8 ("曲线"), palette::layer1);
            as.setLayerOutputSlot (0, aSlot);
            as.setLayerLevelDb (0, 0.0f);

            auto* audioLayer = as.getLayer (0);
            const int noteId = audioLayer->addNote ({ 0, 57, 0.0, 4.0, 0.9f });
            const int curveGroup = audioLayer->addGroup (utf8 ("组 C"), groupPalette::group2);
            audioLayer->assignNotesToGroup ({ noteId }, curveGroup);

            auto* group = audioLayer->findGroup (curveGroup);

            auto peakWith = [&] (bool enabled, float value)
            {
                juce::AudioBuffer<float> out;
                out.setSize (2, 44100);
                out.clear();

                group->getLevelAutomation().loadPoints ({ { 0.0, value }, { 64.0, value } });
                group->getLevelAutomation().setEnabled (enabled);

                engine.publishSchedule (as, ar, 120.0, 0.0, nullptr);
                engine.resetMixState();
                engine.renderWindow (0.30, 44100, out, nullptr);

                return out.getMagnitude (0, 0, 44100);
            };

            const float full   = peakWith (false, 1.0f);
            const float halved = peakWith (true, 0.5f);
            const float ratio  = full > 0.0f ? halved / full : 0.0f;

            check ("A group's level curve multiplies its notes (0.5 halves the peak)",
                   ratio > 0.40f && ratio < 0.60f,
                   "full " + juce::String (full, 4) + " / halved " + juce::String (halved, 4)
                     + "  ->  x" + juce::String (ratio, 3));
        }

        //-- Round trip --------------------------------------------------------
        {
            juce::String xml = project::toXmlText (gs, gr, 120.0, nullptr, nullptr);

            LayerStack loaded;
            RackModel  loadedRack;
            double     loadedBpm = 0.0;
            juce::String error;

            const bool ok = project::fromXmlText (xml, loaded, loadedRack, loadedBpm, error);

            auto* restored = loaded.getLayer (0);
            const auto* restoredGroup = restored != nullptr && restored->getGroups().size() == 1
                                          ? &restored->getGroups().front() : nullptr;

            check ("A project round trip keeps the group, its mount and its members",
                   ok && restoredGroup != nullptr
                     && restoredGroup->getOutputSlotId() == slotB
                     && restored->countNotesInGroup (restoredGroup->getId()) == 2,
                   error.isNotEmpty() ? error
                                      : juce::String (restored != nullptr
                                                        ? restored->getGroups().size() : 0)
                                          + utf8 (" 个组"));

            // Phase 20: the pan lane is a second <Automation> child on the same
            // <Group>; a reader that assumed "one child per group" would drop it.
            check ("The group's pan curve survives the round trip",
                   restoredGroup != nullptr
                     && restoredGroup->getPanAutomation().isEnabled()
                     && restoredGroup->getPanAutomation().getPoints().size() == 3
                     && std::abs (restoredGroup->getPanAutomation()
                                    .valueAtBeat (8.0) - 0.9f) < 1.0e-4f,
                   restoredGroup != nullptr
                     ? juce::String (restoredGroup->getPanAutomation().getPoints().size())
                         + utf8 (" 个点 / ")
                         + juce::String (restoredGroup->getPanAutomation().valueAtBeat (8.0), 3)
                     : juce::String ("no group"));
        }
    }

    //-- Phase 16: grouping and the spotlight, through the real UI ------------
    {
        // The checks above end on a brand-new, empty project (that is the point
        // of the "new project" test), so give the session a handful of notes to
        // group. loadDemoProject() at the end of this function throws them away.
        if (auto* layer = stack.getActiveLayer())
        {
            if (layer->getNotes().size() < 3)
                for (int i = 0; i < 4; ++i)
                    layer->addNote ({ 0, 60 + i * 4, i * 2.0, 1.5, 0.8f });

            stack.notifyNotesChanged();
            pianoRoll.setScrollBeats (0.0);


            std::vector<int> members;

            for (int i = 0; i < 3; ++i)
                members.push_back (layer->getNotes()[(size_t) i].id);

            pianoRoll.setSelection (members);

            const int groupId = pianoRoll.groupSelection();

            check ("The group button turns the selection into a group",
                   groupId > 0 && layer->countNotesInGroup (groupId) == 3,
                   juce::String (layer->countNotesInGroup (groupId)) + utf8 (" 个成员"));

            check ("The group reports itself as the selection's group",
                   pianoRoll.getSelectedGroupId() == groupId,
                   juce::String (pianoRoll.getSelectedGroupId()));

            const auto label = pianoRoll.getGroupLabelBounds (stack.getActiveLayerIndex(), groupId);

            check ("A group's label is drawn and is a real click target",
                   ! label.isEmpty() && label.getWidth() > 20.0f,
                   juce::String (label.getWidth(), 1) + " x " + juce::String (label.getHeight(), 1));

            pianoRoll.selectGroup (groupId);

            check ("Clicking the label selects every member",
                   (int) pianoRoll.getSelection().size() == 3,
                   juce::String ((int) pianoRoll.getSelection().size()) + utf8 (" 个音符"));

            // The spotlight: focusing a synth has to light up its notes, and
            // the layer list has to agree about which sheets feed it.
            const int resolved = pianoRoll.getSelectedRoutingSlotId();
            focusSynth (resolved);

            check ("Focusing a synth counts the notes that play on it",
                   resolved >= 0 && getFocusedSlotId() == resolved
                     && pianoRoll.getFocusedNoteCount() >= 3,
                   juce::String (pianoRoll.getFocusedNoteCount()) + utf8 (" 个音符"));

            check ("The sheet list marks the layers that feed the focused synth",
                   layerPanel.isLayerAttachedToFocus (stack.getActiveLayerIndex()),
                   juce::String (layerPanel.getFocusSlotId()));

            // Mounting the group: one call moves every member, and the same
            // resolver the engine uses has to agree about it.
            const int mountTarget = rack.getSlotIdAt (rack.getNumSlots() - 1);
            const bool mounted = pianoRoll.setGroupOutputForSelection (mountTarget);
            const auto mountedRoute = routing::resolve (*layer, *layer->findNote (members.front()), rack);

            check ("Mounting moves every member of the group onto that synth",
                   mounted && mountedRoute.slotId == mountTarget && mountedRoute.viaGroup,
                   "slot " + juce::String (mountedRoute.slotId) + " / target "
                     + juce::String (mountTarget));

            pianoRoll.setGroupOutputForSelection (-1);

            // A group curve is a row in the automation window, like any other.
            ensureAutomationTrack (AutomationOwner::group, groupId,
                                   (int) GroupAutomationTarget::level);

            check ("A group gets a curve row of its own",
                   automation.findTrackId (AutomationOwner::group, groupId, 0) > 0,
                   juce::String (automation.getNumTracks()) + utf8 (" 轨"));

            // Phase 20: and a second row for the pan lane, on the same group,
            // both live at the same time — the same rule the layer's two lanes
            // have followed since Phase 7.
            ensureAutomationTrack (AutomationOwner::group, groupId,
                                   (int) GroupAutomationTarget::pan);

            check ("The same group can carry level and pan curves at once",
                   automation.findTrackId (AutomationOwner::group, groupId, 1) > 0
                     && layer->findGroup (groupId) != nullptr
                     && layer->findGroup (groupId)->getPanAutomation().isEnabled(),
                   juce::String (automation.getNumTracks()) + utf8 (" 轨"));

            pianoRoll.disbandGroup (stack.getActiveLayerIndex(), groupId);

            const auto* detached = layer->findNote (members.front());

            check ("Disbanding clears the membership, the group and its row",
                   layer->findGroup (groupId) == nullptr
                     && detached != nullptr && detached->groupId < 0
                     && automation.findTrackId (AutomationOwner::group, groupId, 0) < 0
                     && automation.findTrackId (AutomationOwner::group, groupId, 1) < 0,
                   juce::String (layer->getGroups().size()) + utf8 (" 个组剩余"));
        }
    }

    //-- Phase 17: clips, folding, scrolling, and the group list -------------
    {
        loadDemoProject();      // a known fixture, whatever the checks above left

        //-- A clip is a whole number of lanes, whatever the UI scale is -----
        int clipLayer = -1;
        const SampleEvent* clip = nullptr;

        for (int i = 0; i < stack.getNumLayers() && clip == nullptr; ++i)
        {
            auto* l = stack.getLayer (i);

            if (l != nullptr && ! l->getSamples().empty())
            {
                clipLayer = i;
                clip = &l->getSamples().front();
            }
        }

        check ("The demo carries a decoded clip",
               clip != nullptr && clip->isLoaded() && clip->audio->numFrames() > 0,
               clip != nullptr
                 ? juce::String (clip->name) + " / "
                     + juce::String (clip->audio->numFrames()) + " frames"
                 : juce::String ("no clip"));

        // Copied out before anything can add a clip: `getSamples()` returns a
        // reference into a vector that the very next add reallocates.
        const int    clipId     = clip != nullptr ? clip->id : -1;
        const int    clipSpan   = clip != nullptr ? clip->laneSpan : 0;
        const double clipLength = clip != nullptr ? clip->lengthBeats : 0.0;

        if (clip != nullptr)
        {
            const auto bounds = pianoRoll.getSampleScreenBounds (clipLayer, clipId);

            // The height must be exactly laneSpan lanes: this is the "occupies a
            // whole number of note rows" promise, measured, not assumed.
            const float expectedH = (float) clipSpan * PianoRollComponent::keyHeight;
            const float expectedW = (float) (clipLength * pianoRoll.getBeatWidth());

            check ("A clip covers a whole number of note rows",
                   std::abs (bounds.getHeight() - expectedH) < 0.51f
                     && bounds.getHeight() > PianoRollComponent::keyHeight,
                   juce::String (bounds.getHeight(), 1) + " px / "
                     + juce::String (expectedH, 1) + " px expected");

            // The width is beats x beatWidth, which is why zooming scales it
            // without a single line of zoom code in the clip renderer.
            check ("A clip's width is its length times the beat width",
                   std::abs (bounds.getWidth() - expectedW) < 0.51f,
                   juce::String (bounds.getWidth(), 1) + " px / "
                     + juce::String (expectedW, 1) + " px expected");

            // Both edges sit on lane boundaries measured from the grid's own
            // origin — the roll is scrolled, so "pixel y is a multiple of the
            // lane height" would only be true at scroll 0.
            const float lane    = PianoRollComponent::keyHeight;
            const float originY = pianoRoll.getGridBounds().getY();
            const float relTop  = bounds.getY() - originY;

            check ("A clip's edges land on lane boundaries",
                   std::abs (relTop - std::round (relTop / lane) * lane) < 0.51f,
                   juce::String (relTop, 1) + " px from the grid top");

            //-- Zoom ------------------------------------------------------
            // "The clip's length scales with the roll" is a claim about a
            // ratio, so it is checked as one: double the beat width, double the
            // block. Nothing in the clip code was told about the zoom.
            const float widthBefore = bounds.getWidth();

            pianoRoll.setBeatWidth (pianoRoll.getBeatWidth() * 2.0f);
            const float widthAfter = pianoRoll.getSampleScreenBounds (clipLayer, clipId).getWidth();
            pianoRoll.setBeatWidth (PianoRollComponent::defaultBeatWidth);

            check ("Zooming the roll scales a clip in proportion",
                   std::abs (widthAfter - widthBefore * 2.0f) < 1.0f,
                   juce::String (widthBefore, 1) + " -> " + juce::String (widthAfter, 1) + " px");
        }

        //-- Dropping a file adds a block to the active sheet ----------------
        {
            auto* activeLayer = stack.getActiveLayer();
            const int before  = pianoRoll.getActiveLayerSampleCount();
            const int newClip = pianoRoll.addSampleAt (48.0, 72, getDemoSampleFile());

            check ("Dropping a WAV on the grid creates a clip on the active sheet",
                   newClip > 0 && pianoRoll.getActiveLayerSampleCount() == before + 1
                     && activeLayer != nullptr && activeLayer->findSample (newClip) != nullptr,
                   juce::String (before) + " -> "
                     + juce::String (pianoRoll.getActiveLayerSampleCount()));

            // A file that is not a WAV is refused, and says so.
            const auto bogus = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                   .getChildFile ("atomic-not-a-sample.txt");
            bogus.replaceWithText ("nope");

            juce::String complaint;
            auto previousHandler = pianoRoll.onSampleLoadFailed;
            pianoRoll.onSampleLoadFailed = [&complaint] (const juce::String& m) { complaint = m; };

            const int refused = pianoRoll.addSampleAt (48.0, 72, bogus);
            pianoRoll.onSampleLoadFailed = previousHandler;

            check ("A file the decoder cannot read is refused with a message",
                   refused < 0 && complaint.isNotEmpty(),
                   complaint.isNotEmpty() ? complaint : juce::String ("no message"));

            bogus.deleteFile();

            if (newClip > 0 && activeLayer != nullptr)
                activeLayer->removeSample (newClip);
        }

        //-- The clip survives a save / load ---------------------------------
        {
            const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("atomic-phase17-clips.atomic");

            juce::String saveError;
            const bool saved = saveProjectToFile (file, saveError);

            MainComponent reloaded;
            juce::String loadError;
            const bool loaded = reloaded.loadProjectFromFile (file, loadError);

            int restoredClips = 0;
            double restoredLength = 0.0;
            bool   restoredAudio = false;

            for (int i = 0; i < reloaded.stack.getNumLayers(); ++i)
            {
                auto* l = reloaded.stack.getLayer (i);

                if (l == nullptr)
                    continue;

                for (const auto& s : l->getSamples())
                {
                    ++restoredClips;
                    restoredLength = s.lengthBeats;
                    restoredAudio  = s.isLoaded();
                }
            }

            check ("A clip round trips through the project file, audio included",
                   saved && loaded && restoredClips > 0 && restoredAudio
                     && std::abs (restoredLength - clipLength) < 1.0e-6,
                   juce::String (restoredClips) + utf8 (" 块 / ")
                     + juce::String (restoredLength, 3) + utf8 (" 拍 / 解码: ")
                     + (restoredAudio ? "yes" : "no")
                     + (saved ? "" : " save: " + saveError)
                     + (loaded ? "" : " load: " + loadError));

            file.deleteFile();
        }

        //-- Folding: three lists, one rule ----------------------------------
        if (auto* l = stack.getLayer (0))
        {
            const int before = layerPanel.getContentHeight();
            l->setCollapsed (true);
            const int folded = layerPanel.getContentHeight();
            l->setCollapsed (false);

            check ("Folding a layer shrinks the list by exactly one row",
                   folded < before && before - folded > 0,
                   juce::String (before) + " -> " + juce::String (folded) + " px");
        }

        if (auto* slot = rack.getSlot (0))
        {
            const int before = rackPanel.getContentHeight();
            rack.setCollapsed (slot->id, true);
            const int folded = rackPanel.getContentHeight();
            rack.setCollapsed (slot->id, false);

            check ("Folding a synth shrinks the rack by exactly one card",
                   folded < before && before - folded > 0,
                   juce::String (before) + " -> " + juce::String (folded) + " px");
        }

        // Every group in the session has to appear in the panel: the list is a
        // projection of the model, not a copy that can drift.
        int groupsInModel = 0;

        for (int i = 0; i < stack.getNumLayers(); ++i)
            if (auto* l = stack.getLayer (i))
                groupsInModel += (int) l->getGroups().size();

        check ("The group panel lists every group in the session",
               groupPanel.getNumRows() == groupsInModel && groupsInModel >= 2,
               juce::String (groupPanel.getNumRows()) + utf8 (" 行 / ")
                 + juce::String (groupsInModel) + utf8 (" 个组"));

        if (const auto rows = groupPanel.getRows(); ! rows.empty())
        {
            auto* l = stack.getLayer (rows.front().layerIndex);
            auto* g = l != nullptr ? l->findGroup (rows.front().groupId) : nullptr;

            if (g != nullptr)
            {
                const int before = groupPanel.getContentHeight();
                g->setCollapsed (true);
                const int folded = groupPanel.getContentHeight();
                g->setCollapsed (false);

                check ("Folding a group shrinks the group list",
                       folded < before,
                       juce::String (before) + " -> " + juce::String (folded) + " px");
            }

            //-- Panels scroll, and clamp -----------------------------------
            // Shrink the list on purpose: with three layers nothing overflows,
            // and a scroll test that never overflows proves nothing.
            layerPanel.setBounds (0, 0, LayerPanel::panelWidth, ui::px (120));
            const bool overflows = layerPanel.getContentHeight() > layerPanel.getViewHeight();

            layerPanel.setScrollY (1000000);
            const int clamped = layerPanel.getScrollY();
            const int maxScroll = juce::jmax (0, layerPanel.getContentHeight() - layerPanel.getViewHeight());

            check ("A list that overflows scrolls, and stops at its end",
                   overflows && clamped == maxScroll && clamped > 0,
                   juce::String (layerPanel.getContentHeight()) + utf8 (" 内容 / ")
                     + juce::String (layerPanel.getViewHeight()) + utf8 (" 可视 / 停在 ")
                     + juce::String (clamped));

            layerPanel.setScrollY (0);
            resized();      // give the panel back the height the layout gave it
        }
    }

    //-- Phase 18: pointer, snap, ruler, console strips, master automation -----
    {
        //-- One snap switch, two editors --------------------------------------
        const bool snapBefore = pianoRoll.isSnapEnabled();

        pianoRoll.setSnapEnabled (! snapBefore);
        const bool flipped = automationWindow.isSnapEnabled();

        pianoRoll.setSnapEnabled (snapBefore);
        const bool restored = automationWindow.isSnapEnabled();

        check ("The snap button drives the automation window too",
               flipped == ! snapBefore && restored == snapBefore,
               juce::String (snapBefore ? "on" : "off") + " -> "
                 + juce::String (flipped ? "on" : "off") + " -> "
                 + juce::String (restored ? "on" : "off"));

        //-- The ruler is the transport's own strip ----------------------------
        pianoRoll.onLocateRequested (6.5);
        const double located = getTransportBeats();

        pianoRoll.onLocateRequested (0.0);

        check ("Clicking the ruler moves the transport",
               std::abs (located - 6.5) < 1.0e-9,
               juce::String (located, 3) + utf8 (" 拍"));

        // The zoom the ruler drag performs is the same beatWidth the clips read,
        // so measuring a clip is measuring the zoom.
        {
            const float before = pianoRoll.getBeatWidth();
            pianoRoll.setBeatWidth (before * 2.0f);
            const float after = pianoRoll.getBeatWidth();
            pianoRoll.setBeatWidth (before);

            check ("The ruler's zoom is a beat width, and clips follow it",
                   std::abs (after - before * 2.0f) < 0.01f
                     && std::abs (pianoRoll.getBeatWidth() - before) < 0.01f,
                   juce::String (before, 1) + " -> " + juce::String (after, 1) + " px");
        }

        //-- The console grows, shrinks and resets ------------------------------
        const int countBefore = mixer.getChannelCount();

        while (mixer.getChannelCount() < MixerModel::maxChannels)
            mixer.addChannel();

        const bool refusedAboveCeiling = ! mixer.addChannel();
        mixer.removeChannel();
        const int shrunk = mixer.getChannelCount();
        mixer.addChannel();

        check ("The console refuses a 9th strip and accepts 1..8",
               countBefore == MixerModel::maxChannels
                 && refusedAboveCeiling
                 && shrunk == MixerModel::maxChannels - 1
                 && mixer.getChannelCount() == MixerModel::maxChannels,
               juce::String (countBefore) + " / " + juce::String (shrunk) + " / "
                 + juce::String (mixer.getChannelCount()));

        // Reset means "factory", not "unplugged": the fader goes back to unity,
        // the inserts are cleared, and whatever was routed stays routed.
        {
            const int slotId = mixerPanel.slotIdOnChannel (0);
            bool routedStayed = slotId < 0;

            if (slotId >= 0)
            {
                rack.setLevelDb (slotId, -18.0f);
                mixer.setInsertType (0, 0, fx::Type::none);

                mixerPanel.resetStrip (0);

                const auto* slot = rack.findSlot (slotId);
                routedStayed = slot != nullptr && slot->mixerChannel == 0
                                 && std::abs (slot->levelDb) < 0.01f;
            }

            check ("Resetting a strip returns it to unity and keeps its routing",
                   routedStayed,
                   slotId >= 0 ? juce::String (rack.findSlot (slotId) != nullptr
                                                 ? rack.findSlot (slotId)->levelDb : 0.0f, 2) + " dB"
                               : juce::String ("no source, skipped"));
        }

        //-- The master curve really is heard ---------------------------------
        {
            LayerStack as;
            RackModel  ar;
            MixerModel am;
            PlaybackEngine engine;

            const int aSlot = ar.getFirstSlotId();
            as.addLayer (utf8 ("主曲线"), palette::layer1);
            as.setLayerOutputSlot (0, aSlot);
            as.setLayerLevelDb (0, 0.0f);
            as.getLayer (0)->addNote ({ 0, 57, 0.0, 4.0, 0.9f });

            auto peakWith = [&] (bool enabled, float value)
            {
                auto& lane = am.automationFor (0);
                lane.loadPoints ({ { 0.0, value }, { 64.0, value } });
                lane.setEnabled (enabled);

                juce::AudioBuffer<float> out;
                out.setSize (2, 44100);
                out.clear();

                engine.publishSchedule (as, ar, 120.0, 0.0, &am);
                engine.resetMixState();
                engine.renderWindow (0.30, 44100, out, nullptr);

                return out.getMagnitude (0, 0, 44100);
            };

            const float full   = peakWith (false, 1.0f);
            const float halved = peakWith (true, 0.5f);
            const float ratio  = full > 0.0f ? halved / full : 0.0f;

            check ("The master volume curve multiplies the master fader (0.5 halves the peak)",
                   ratio > 0.40f && ratio < 0.60f,
                   "full " + juce::String (full, 4) + " / halved " + juce::String (halved, 4)
                     + "  ->  x" + juce::String (ratio, 3));

            //-- Round trip ---------------------------------------------------
            am.automationFor (0).setEnabled (true);
            am.setChannelCount (5);

            juce::String xml = project::toXmlText (as, ar, 120.0, &am, nullptr);

            LayerStack loaded;
            RackModel  loadedRack;
            MixerModel loadedMixer;
            double     loadedBpm = 0.0;
            juce::String error;

            const bool ok = project::fromXmlText (xml, loaded, loadedRack, loadedBpm, error,
                                                  &loadedMixer, nullptr);

            check ("Master automation and the strip count survive a project round trip",
                   ok && loadedMixer.getChannelCount() == 5
                     && loadedMixer.automationFor (0).isEnabled()
                     && std::abs (loadedMixer.automationFor (0).valueAtBeat (2.0) - 0.5f) < 0.01f,
                   error.isNotEmpty() ? error
                                      : juce::String (loadedMixer.getChannelCount()) + utf8 (" 个通道 / ")
                                          + juce::String (loadedMixer.automationFor (0).valueAtBeat (2.0), 2));
        }
    }

    //-- Phase 19: the tail button, both resize ends, the audition voice ------
    {
        //-- The tail button no longer sits on the resize edge ------------------
        {
            LayerStack t;
            RackModel  r;
            PianoRollComponent roll (t, r);
            roll.setSize (1200, 520);

            t.addLayer (utf8 ("柄"), palette::layer1);

            const int id = t.getLayer (0)->addNote ({ 0, 72, 0.0, 2.0, 0.9f });

            if (auto* note = t.getLayer (0)->findNote (id))
            {
                const auto handle = roll.getNoteAutomationHandleBounds (*note);
                const auto bounds = roll.getNoteScreenBounds (0, id);

                // The button has to stop short of BOTH ends, or the one gesture
                // a note advertises (make me longer) is the one it blocks.
                const float band = ui::pf (6.0f);

                check ("The tail button clears the note's resize bands",
                       ! handle.isEmpty()
                         && handle.getRight() <= bounds.getRight() - band + 0.01f
                         && handle.getX() > bounds.getX() + band,
                       juce::String (handle.getX() - bounds.getX(), 1) + " .. "
                         + juce::String (bounds.getRight() - handle.getRight(), 1) + utf8 (" px air"));
            }
        }

        //-- A short note hides the button --------------------------------------
        {
            LayerStack t;
            RackModel  r;
            PianoRollComponent roll (t, r);
            roll.setSize (1200, 520);

            t.addLayer (utf8 ("短"), palette::layer1);

            const int quarter = t.getLayer (0)->addNote ({ 0, 60, 0.0, 0.25, 0.9f });
            const int whole   = t.getLayer (0)->addNote ({ 0, 64, 4.0, 1.0, 0.9f });

            const auto* shortNote = t.getLayer (0)->findNote (quarter);
            const auto* longNote  = t.getLayer (0)->findNote (whole);

            check ("A note too short for the tail button simply does not draw one",
                   shortNote != nullptr && longNote != nullptr
                     && roll.getNoteAutomationHandleBounds (*shortNote).isEmpty()
                     && ! roll.getNoteAutomationHandleBounds (*longNote).isEmpty(),
                   shortNote != nullptr
                     ? juce::String (roll.getNoteScreenBounds (0, quarter).getWidth(), 0)
                         + " px -> " + juce::String (roll.getNoteAutomationHandleBounds (*shortNote).isEmpty()
                                                       ? "hidden" : "shown")
                     : juce::String ("no note"));
        }

        //-- Phase 20: the toolbar has to fit eight buttons --------------------
        {
            LayerStack t;
            RackModel  r;
            PianoRollComponent roll (t, r);
            roll.setSize (1400, 520);

            t.addLayer (utf8 ("栏"), palette::layer1);

            bool overlap = false;

            for (int i = 0; i + 1 < PianoRollComponent::numToolButtons; ++i)
                if (roll.getToolButtonBounds (i).getRight() > roll.getToolButtonBounds (i + 1).getX())
                    overlap = true;

            check ("Every toolbar button owns its own pixels (magnet and ungroup included)",
                   ! overlap
                     && PianoRollComponent::numToolButtons == 8
                     && roll.getToolbarArea().contains (roll.getToolButtonBounds (7)),
                   juce::String (PianoRollComponent::numToolButtons) + utf8 (" 个按钮，右端 ")
                     + juce::String (roll.getToolButtonBounds (7).getRight())
                     + " / 栏宽 " + juce::String (roll.getToolbarArea().getWidth()));
        }

        //-- The preview voice -------------------------------------------------
        {
            LayerStack as;
            RackModel  ar;
            PlaybackEngine engine;

            const int aSlot = ar.getFirstSlotId();
            as.addLayer (utf8 ("试听"), palette::layer1);
            as.setLayerOutputSlot (0, aSlot);

            engine.publishSchedule (as, ar, 120.0, 0.0, nullptr);
            engine.resetMixState();

            check ("Nothing sounds until the editor asks for a preview",
                   ! engine.hasAudition(), "idle");

            engine.auditionNote (69, 0.9f, aSlot, true);

            juce::AudioBuffer<float> quiet;
            quiet.setSize (2, 4096);
            quiet.clear();

            engine.renderAudition (quiet, 4096);
            const float previewPeak = quiet.getMagnitude (0, 0, 4096);

            check ("Drawing a note plays it, with the transport stopped",
                   engine.hasAudition() && previewPeak > 0.001f,
                   "peak " + juce::String (previewPeak, 4));

            // A drag sends one request per new pitch. Rendering between them is
            // how the real audio thread sees the gesture, so twelve semitones is
            // twelve blocks — and the voice must still be sounding at the end of
            // it rather than have died or been restarted from silence.
            juce::AudioBuffer<float> glide;
            glide.setSize (2, 512);

            float quietest = 1.0f;

            for (int step = 0; step < 12; ++step)
            {
                engine.auditionNote (69 + step, 0.9f, aSlot, false);

                glide.clear();
                engine.renderAudition (glide, 512);

                quietest = juce::jmin (quietest, glide.getMagnitude (0, 0, 512));
            }

            check ("A continuous pitch drag keeps the preview sounding across twelve semitones",
                   engine.hasAudition() && quietest > 0.001f && quietest > previewPeak * 0.5f,
                   "quietest " + juce::String (quietest, 4) + " vs first " + juce::String (previewPeak, 4));

            // And it releases: a preview left alone must not drone forever.
            juce::AudioBuffer<float> tail;
            tail.setSize (2, 44100 * 2);
            tail.clear();

            for (int block = 0; block < 40; ++block)
                engine.renderAudition (tail, 4410);

            check ("A preview releases on its own",
                   ! engine.hasAudition(),
                   engine.hasAudition() ? juce::String ("still sounding") : juce::String ("released"));
        }

        //-- Phase 20: the sample preview --------------------------------------
        {
            // Two made-up buffers rather than files: the decoder is not what is
            // being tested here, and a buffer is what the engine actually sees.
            auto makeBuffer = [] (double seconds, double frequency, float amplitude)
            {
                auto buffer = std::make_shared<SampleBuffer>();
                buffer->sampleRate = 44100.0;
                buffer->channels   = 1;

                const int frames = juce::jmax (1, (int) (seconds * buffer->sampleRate));
                buffer->frames.resize ((size_t) frames, 0.0f);

                for (int i = 0; i < frames; ++i)
                    buffer->frames[(size_t) i] = (float) (amplitude
                        * std::sin (2.0 * juce::MathConstants<double>::pi * frequency
                                      * (double) i / buffer->sampleRate));

                return buffer;
            };

            auto sine  = makeBuffer (3.0, 330.0, 0.8f);
            auto silent = makeBuffer (3.0, 330.0, 0.0f);

            PlaybackEngine pe;
            pe.setSampleRate (44100.0);

            check ("No sample preview until one is asked for",
                   ! pe.hasSamplePreview(), "idle");

            pe.previewSample (sine, 3.0);

            juce::AudioBuffer<float> block (2, 4096);
            block.clear();
            pe.renderSamplePreview (block, 4096);

            const float firstPeak = block.getMagnitude (0, 0, 4096);

            check ("Clicking a file plays it with the transport stopped",
                   pe.hasSamplePreview() && firstPeak > 0.001f,
                   "peak " + juce::String (firstPeak, 4));

            // The "no overlap" rule: the second click replaces the first, even
            // though the first had two and a half seconds left. A silent buffer
            // makes the assertion unambiguous — if the old preview were still
            // running, this block would not be silent.
            pe.previewSample (silent, 3.0);

            juce::AudioBuffer<float> after;
            after.setSize (2, 4096);
            after.clear();
            pe.renderSamplePreview (after, 4096);

            check ("A second click replaces the first preview instead of stacking",
                   after.getMagnitude (0, 0, 4096) < 1.0e-5f,
                   "residual " + juce::String (after.getMagnitude (0, 0, 4096), 6));

            // And the three seconds are honoured: a long file asked for 0.2 s
            // must be quiet well before its own end.
            pe.previewSample (sine, 0.2);

            juce::AudioBuffer<float> window (2, 22050);
            window.clear();
            pe.renderSamplePreview (window, 22050);

            const float head = window.getRMSLevel (0, 0, 4000);
            const float tail = window.getRMSLevel (0, 20000, 2000);

            check ("The preview stops at the length the browser asked for",
                   head > 0.01f && tail < head * 0.02f,
                   "head " + juce::String (head, 4) + " / tail " + juce::String (tail, 4));

            check ("The preview releases by itself",
                   ! pe.hasSamplePreview(),
                   pe.hasSamplePreview() ? juce::String ("still engaged") : juce::String ("released"));
        }
    }

    report << "\n  " << (failures == 0 ? "ALL CHECKS PASSED" : juce::String (failures) + " CHECK(S) FAILED") << "\n";

    // Put the fixture back before leaving: the checks edit the live session
    // (they scroll the roll, send synths around, add layers), so without this
    // the snapshot would photograph the leftovers of the last check instead of
    // the demo.
    loadDemoProject();

    return report;
}
} // namespace atomic
