#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Util/Metrics.h"

#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "Audio/AudioEngine.h"
#include "Audio/PluginHost.h"
#include "Audio/WavExport.h"
#include "Model/AutomationModel.h"
#include "Model/HardwareRack.h"
#include "Model/LayerModel.h"
#include "Model/MixerModel.h"
#include "Model/ProjectIO.h"
#include "UI/AutomationWindow.h"
#include "UI/BpmField.h"
#include "UI/EffectWindows.h"
#include "UI/FileBrowserPanel.h"
#include "UI/GroupPanel.h"
#include "UI/LayerPanel.h"
#include "UI/MixerPanel.h"
#include "UI/MixerWindow.h"
#include "UI/NoteEnvelopePanel.h"
#include "UI/PianoRollComponent.h"
#include "UI/RackPanel.h"
#include "UI/SettingsWindow.h"

namespace atomic
{

//==============================================================================
/**
    Phase 12: the grab band between the piano roll and the automation window.

    A separate component rather than hit testing in MainComponent, because both
    neighbours paint right up to the shared edge: a click at y == rollBottom is
    claimed by whichever child the mouse lands on, and the parent never sees it.
    Eight pixels of dedicated real estate is the only reliable splitter.
*/
class VerticalSplitter final : public juce::Component
{
public:
    VerticalSplitter();

    /** Incremental mouse travel, in pixels, since the previous callback. The
        host owns the heights, so it owns the arithmetic too. */
    std::function<void (int deltaY)> onDrag;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    bool hot      = false;
    bool dragging = false;
    int  lastY    = 0;
};

//==============================================================================
/**
    Shell: layers on the left, overlay piano roll in the middle, synth rack on
    the right, transport in the top bar.

    Owns the two models (LayerStack, RackModel) and the one thing that makes
    sound. Nothing else in the app knows they are connected: the AudioEngine
    listens to both models and republishes its schedule, so an edit anywhere is
    audible on the next block.
*/
class MainComponent final : public juce::Component,
                            public juce::DragAndDropContainer,
                            private juce::Timer,
                            private juce::AsyncUpdater,
                            private LayerStack::Listener,
                            private RackModel::Listener,
                            private MixerModel::Listener,
                            private AutomationModel::Listener
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

    /** Opens the output device. Returns false when there is no usable output;
        the editor keeps working, it just cannot be heard. */
    bool startAudio();
    bool isAudioRunning() const { return audio.isRunning(); }

    //-- Transport ------------------------------------------------------------
    void playTransport();
    void pauseTransport();
    void stopTransport();
    /** Space bar: play when stopped or paused, pause when already playing. */
    void togglePlay();

    void setBpm (double newBpm);
    double getBpm() const { return audio.getBpm(); }

    /** Phase 18: clicking the roll's ruler asks the transport to go there. The
        audio engine owns the position, so this is the one place that moves it
        and tells every view about it. */
    void locatePlayhead (double beats);

    /** Raw transport position, for the --transporttest harness. */
    double getTransportBeats() const { return audio.getPositionBeats(); }
    bool   isTransportPlaying() const { return audio.isPlaying(); }

    /** Headless verification of the Phase 1 / 2 / 3 rules.
        Runs in --snapshot mode; returns a human readable report. */
    juce::String runSelfTest();

    /** Opens the note inspector for a note. Positioning is done here because
        only this component knows the full layout. */
    void openEnvelopePanel (int layerIndex, int noteId);

    /** Opens the inspector on the overridden demo note; used by --snapshot. */
    void openDemoEnvelopePanel();

    /** Parks the playhead somewhere visible; used by --snapshot. */
    void setDemoPlayheadBeats (double beats);

    /** Opens the roll's inline automation box on a long demo note, so the
        screenshot shows the box and its parameter menu. */
    void openDemoNoteAutomation();

    /** Borrows two notes of the active layer onto another synth, so the
        screenshot shows both halves of the routing feature: the highlighted
        synth in the rack and the layer's 复位 pill. Snapshot only. */
    void showDemoRoutingOverride();

    //-- Phase 6: project files and offline bounce ----------------------------
    /** Writes the whole session to a .atomic file. Also the path the self-test
        and the command line use — the Save button only adds a file chooser in
        front of it. */
    bool saveProjectToFile (const juce::File& file, juce::String& error);

    /** Replaces the session with the contents of a .atomic file. On failure the
        current session is left untouched. */
    bool loadProjectFromFile (const juce::File& file, juce::String& error);

    /** Renders the project to a 16-bit stereo WAV, from 0 to the last note's
        end. `progress` may be null; returning false cancels. */
    wav::ExportResult exportAudioToFile (const juce::File& file,
                                         const std::function<bool (double)>& progress = {},
                                         double tailSeconds = 0.05);

    /** Last note's end, in seconds at the current tempo. 0 for an empty project. */
    double getContentSeconds() const;

    juce::File getCurrentProjectFile() const { return currentProject; }

    juce::String getAudioStatus() const { return audio.getStatusText(); }

    LayerStack&  getLayerStack() { return stack; }
    RackModel&   getRack()       { return rack; }

    //-- Phase 9: VST3 --------------------------------------------------------
    /** The session's plugin host. Main.cpp adds extra search paths here before
        the window opens; the self-test drives it directly. */
    PluginHost& getPluginHost() { return plugins; }

    /** Scans once. Every entry point that lists plugins calls this first, so
        nobody has to remember to scan at startup. */
    void ensurePluginScan();

    /** 0 = audio, 1 = MIDI, 2 = plugins. */
    void openSettings (int tabIndex = 0);

    /** Renders one settings tab to an image without showing the window. Same
        reason as createMixerSnapshot(): a desktop window cannot be photographed
        by a headless run. */
    juce::Image createSettingsSnapshot (int tabIndex, int width = ui::px (820), int height = ui::px (500));

    /** Adds a plugin from the manager or a menu. Public so the self-test can
        prove the whole path without simulating a mouse. */
    bool addPluginInstrument (const juce::PluginDescription&);
    bool addPluginEffect (const juce::PluginDescription&);

    /** Drops the per-note synth overrides of one layer. */
    void resetLayerNoteRouting (int layerIndex);

    //-- Phase 8: the mixer window -------------------------------------------
    /** Opens or closes the detached console. The panel itself always exists;
        only the window it lives in comes and goes. */
    void toggleMixerWindow();
    void closeMixerWindow();
    bool isMixerWindowOpen() const;

    /** Renders the console to an image without opening a window — the headless
        harness cannot photograph a desktop window. */
    juce::Image createMixerSnapshot (int width = ui::px (1180), int height = ui::px (300));

    //-- Phase 10: blank session, demo fixture, unsaved changes ---------------
    /** Replaces the session with the demo arrangement the self-test and the
        screenshot harness are written against. NOT called at startup any more:
        the app now opens blank. */
    void loadDemoProject();

    /** The WAV the fixture generates for its sample clip. Lives beside the
        plugin library, not in the project: it is test material, and a project
        file that points at it must keep resolving after a restart. */
    static juce::File getDemoSampleFile();

    /** True when the session differs from the state last written to / read from
        disk. Flushes the pending undo capture first, so a gesture still in
        flight is counted. */
    bool hasUnsavedChanges();

    /** Window close button. Returns true when it is safe to quit right now;
        false means a "save your changes?" dialog is up and will quit itself. */
    bool handleCloseRequest();

    /** New-project button / Ctrl+N: asks first when there is something to lose. */
    void requestNewProject();

    //-- Phase 8: project actions on the top bar ------------------------------
    /** Discards the session for an empty one: one layer, an empty rack, no notes. */
    void newProject();

    bool canUndo() const;
    bool canRedo() const;
    void undo();
    void redo();

    /** How many states the history holds, including the current one. The
        self-test uses it to prove an edit actually became undoable. */
    int getHistorySize() const { return (int) history.size(); }
    int getHistoryPosition() const { return historyPos; }

    //-- Phase 12: the roll / automation split --------------------------------
    static constexpr int splitterThickness = ui::px (8);

    /** Height of the automation window in pixels. Dragging the splitter, not a
        stored ratio: the user set that many pixels, and that is what they get
        back when the window regains the same size. */
    int  getAutomationPaneHeight() const { return automationPaneHeight; }
    void setAutomationPaneHeight (int pixels);

    /** Total travel available to the splitter (0 when the window is tiny). */
    int getSplitTravel() const;

    /** The curve target of the note the roll's inline box is open on, or -1.
        Only useful for the self-test / snapshot. */
    int getExpandedNoteId() const { return pianoRoll.getExpandedNoteId(); }

private:
    /** The shared reset behind "new project" (with one empty layer) and the
        demo fixture (with none, because it adds its own three). */
    void buildSession (bool withEmptyLayer);
    void buildBlankProject();
    void buildDemoContent();
    void applyDemoMixState();

    /** Three-button "save before you lose it" dialog. `onProceed` runs only
        after a successful save or an explicit "don't save". */
    void confirmDiscardChanges (const juce::String& action, std::function<void()> onProceed);

    /** Makes sure the automation window has a row for this synth and scrolls to
        it. Phase 14: the caller is the card's context menu, so a curve created
        in the rack is also a curve you can edit in the window. */
    void ensureRackAutomationTrack (int slotId);

    /** Phase 12: the one place that turns "automate this parameter" into a lane,
        a track and a revealed row. All three owner kinds go through here,
        because a layer curve, a synth curve and a group curve differ only in
        which model they live in — never in whether a row exists. */
    void ensureAutomationTrack (AutomationOwner owner, int ownerId, int target);

    void timerCallback() override;

    //-- Phase 8: undo history ------------------------------------------------
    /** AsyncUpdater: a snapshot is taken once per message-loop turn no matter
        how many notifications one gesture produced. */
    void handleAsyncUpdate() override;

    /** Called by every model listener. Cheap: it only schedules a capture. */
    void modelChanged();

    juce::String captureState() const;
    void         restoreState (const juce::String& xml);
    void         commitHistory();
    void         applyHistoryPosition();

    //-- Phase 8: settings popup ---------------------------------------------
    void showSettingsMenu (juce::Point<int> screenPosition);

    /** Bottom-right popup with the two visual preferences plus the version. */
    void showAboutOrReset();

    //-- Phase 9: plugin wiring ----------------------------------------------
    /** Creates the settings window on first use and returns it. Kept separate so
        the headless snapshot can build the pages without showing the window. */
    SettingsWindow* ensureSettingsWindow();

    void showRackAddMenu (juce::Point<int> screenPosition);

    /** Right-click / "→ 输出" on a rack card. Phase 11: replace the instrument
        inside the slot (keeping its id, fader and curves), send the synth to a
        console channel, add its automation, or remove it. */
    void showRackCardMenu (int slotId, juce::Point<int> screenPosition);
    void showInsertPluginMenu (int channel, int slot, juce::Point<int> screenPosition);

    /** Phase 12: open the editor for an insert. A hosted VST3 gets its own
        window; a built-in effect gets the small parameter window. One entry
        point so "click the effect" means the same thing in both cases. */
    void openInsertEditor (int channel, int slot);
    void openPluginEditor (int instanceId, const juce::String& displayName);
    void closePluginEditor (int instanceId);
    void closeInsertEditor (InsertEditorWindow* window);

    /** Unloads every plugin instance no slot or insert points at any more.
        One place, called after any model change, instead of every delete path
        remembering to tell the host. */
    void reconcilePlugins();

    /** Highlights the synth the current note selection plays on. */
    void updateRackRoutingFocus();

    //-- Phase 16: the synth spotlight ---------------------------------------
    /** Makes one synth the spotlight: its notes, its groups and the sheets that
        feed it light up everywhere. -1 clears it. Public so the self-test can
        drive the highlight without clicking. */
    void focusSynth (int slotId);
    int  getFocusedSlotId() const noexcept { return focusedSlotId; }

    /** Pushes the current focus to the three panels. Separate from
        updateRackRoutingFocus() because "the selection moved" and "the user
        picked a card" must not fight: only the former re-derives the focus. */
    void applyFocus();

    /** The menu behind "编组挂载音源…": pick a rack slot, or go back to the
        layer default. */
    void showGroupMountMenu (int layerIndex, int groupId, juce::Point<int> screenPosition);

    //-- Listeners: every model edit feeds the undo history -------------------
    void layersChanged() override;
    void notesChanged() override;
    void rackChanged() override;
    void mixerChanged() override;
    void automationTracksChanged() override;

    /** Rebuilds the audio schedule. One call site for "the model changed",
        so no edit path can forget the mixer. */
    void republish();

    /** Right-click menu for adding an automation track to the window. */
    void showAutomationMenu (AutomationOwner owner, int ownerId, juce::Point<int> screenPos);

    /** Phase 18: a console strip's menu. Built here, not in MixerPanel, because
        the automation half of it needs the session model. Items are GREYED when
        the channel has no source rather than answered with a dialog: a menu that
        shows what is unavailable is information, a dialog is an interruption. */
    void showChannelMenu (int channel, juce::Point<int> screenPos);

    //-- File actions (button / shortcut entry points) ------------------------
    /** `onDone` (optional) is told whether a file was actually written, so the
        quit / new / load flows can continue only after a real save. */
    void askToSaveProject (std::function<void (bool saved)> onDone = {});
    void askToLoadProject();
    /** The file dialog itself, with no "save first?" question in front of it. */
    void showLoadChooser();
    void askToExportAudio();
    void showMessage (const juce::String& title, const juce::String& message, bool isError);
    void exportFinished();

    /** Where the dialogs start: Documents/AtomicDAW, created on demand. */
    static juce::File getProjectDirectory();

    juce::Rectangle<int> getPlayButtonBounds() const;
    juce::Rectangle<int> getPauseButtonBounds() const;
    juce::Rectangle<int> getStopButtonBounds() const;
    juce::Rectangle<int> getBpmFieldBounds() const;
    juce::Rectangle<int> getPositionReadoutBounds() const;
    juce::Rectangle<int> getMasterMeterBounds() const;
    juce::Rectangle<int> getFileClusterBounds() const;
    juce::Rectangle<int> getSaveButtonBounds() const;
    juce::Rectangle<int> getLoadButtonBounds() const;
    juce::Rectangle<int> getExportButtonBounds() const;
    juce::Rectangle<int> getProjectNameBounds() const;

    //-- Phase 8: new / undo / redo / mixer / settings ------------------------
    juce::Rectangle<int> getNewButtonBounds() const;
    juce::Rectangle<int> getUndoButtonBounds() const;
    juce::Rectangle<int> getRedoButtonBounds() const;
    juce::Rectangle<int> getMixerButtonBounds() const;
    juce::Rectangle<int> getSettingsButtonBounds() const;

    /** Shared by paint() and the 60 Hz timer, so only the moving number is
        repainted instead of the whole bar. */
    juce::String formatBarBeat() const;
    juce::String formatTime() const;

    LayerStack          stack;
    RackModel           rack;

    /** VST3 instances. Declared before the engine so it outlives every
        PlaybackEngine that points at it. */
    PluginHost          plugins;
    AutomationModel     automation;
    MixerModel          mixer;
    AudioEngine         audio { stack, rack };

    LayerPanel          layerPanel       { stack, rack };
    PianoRollComponent  pianoRoll        { stack, rack };
    NoteEnvelopePanel   envelopePanel    { stack, rack };
    RackPanel           rackPanel        { rack };
    AutomationWindow    automationWindow { stack, rack, automation, mixer };
    MixerPanel          mixerPanel       { stack, rack, mixer };
    BpmField            bpmField;

    /** Phase 17: the two lists the side columns grew downwards into. Both are
        siblings of the panels above them rather than children: each one owns its
        own scroll position, and the split between them is a fixed pixel height
        so a fold never rescales the other. */
    FileBrowserPanel    filePanel;
    GroupPanel          groupPanel       { stack, rack };

    /** Pixels each lower panel keeps, measured from the bottom of its column.
        Same reasoning as automationPaneHeight: a size the user dragged to is a
        size, not a ratio. */
    int filePaneHeight  = ui::px (250);
    int groupPaneHeight = ui::px (320);

    /** Phase 12: drag it to rebalance the roll and the automation window. */
    VerticalSplitter    splitterBar;

    /** Pixels given to the automation window. 312 opens tall enough for six
        curve rows, which is exactly what the demo fixture ships (Phase 18 added
        the master bus row to the five that were already there): a default that
        clips the last row it ships with is a bad first impression. The
        splitter changes it from there. Stored in pixels, not as a ratio: the
        user dragged to a size, and that size is what they get back when the
        window comes back. */
    int automationPaneHeight = ui::px (312);

    juce::Rectangle<int> topBarBounds;

    int   demoOverrideLayer = -1;
    int   demoOverrideNote  = -1;

    /** Phase 16: the synth the roll is spotlighting, -1 for none. Kept on the
        host because all three panels have to agree about it. */
    int   focusedSlotId = -1;

    bool hoveringPlay  = false;
    bool hoveringPause = false;
    bool hoveringStop  = false;

    bool hoveringSave   = false;
    bool hoveringLoad   = false;
    bool hoveringExport = false;

    //-- Phase 9 --------------------------------------------------------------
    std::unique_ptr<SettingsWindow> settingsWindow;

    /** Set while a plugin-provided menu is about to add an effect, so the
        "first empty slot" rule has one home. */
    int pendingEffectChannel = -1;
    int pendingEffectSlot    = -1;

    //-- Phase 8 --------------------------------------------------------------
    bool hoveringNew      = false;
    bool hoveringUndo     = false;
    bool hoveringRedo     = false;
    bool hoveringMixer    = false;
    bool hoveringSettings = false;

    /** The mixer's desktop window. Null until the first time it is opened, and
        destroyed when it is closed — the panel it shows never is. */
    std::unique_ptr<MixerWindow> mixerWindow;

    /** Phase 12: effect editors, keyed by plugin instance id. One window per
        instance: a processor can only have one editor, so a second "open" has
        to raise the existing window instead of building another one. */
    std::map<int, std::unique_ptr<PluginEditorWindow>> pluginWindows;

    /** The built-in effects' windows, one per (channel, slot). */
    std::vector<std::unique_ptr<InsertEditorWindow>> insertWindows;

    /** Undo history: whole-project snapshots, oldest first. `historyPos` is the
        state the session is currently in. A new edit truncates everything after
        it, which is what makes redo disappear the moment you branch. */
    std::vector<juce::String> history;
    int  historyPos       = 0;
    bool restoringHistory = false;
    static constexpr int maxHistoryStates = 64;

    /** One dialog at a time: a second FileChooser would orphan the first. */
    std::unique_ptr<juce::FileChooser> chooser;

    juce::File currentProject;

    /** The session text last saved to / loaded from disk. Comparing against it
        is what "unsaved changes" means; there is no dirty flag to keep in sync
        with forty edit paths. */
    juce::String savedStateText;

    /** cached `captureState() != savedStateText`, refreshed at every point that
        can change either side. paint() must not run a full XML dump. */
    bool       dirtyCache = false;

    bool       exporting = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace atomic
