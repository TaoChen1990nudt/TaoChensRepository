#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Util/Metrics.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "Audio/AudioEngine.h"
#include "Audio/MidiInputHub.h"
#include "Audio/PluginHost.h"
#include "Audio/ScopeRender.h"
#include "Audio/AudioExport.h"
#include "Model/AutomationModel.h"
#include "Model/HardwareRack.h"
#include "Model/LayerModel.h"
#include "Model/MixerModel.h"
#include "Model/ProjectIO.h"
#include "UI/AutomationWindow.h"
#include "UI/BpmField.h"
#include "UI/EffectWindows.h"
#include "UI/ExportDialog.h"
#include "UI/FileBrowserPanel.h"
#include "UI/GroupPanel.h"
#include "UI/LayerPanel.h"
#include "UI/MixerPanel.h"
#include "UI/MixerWindow.h"
#include "UI/NoteEnvelopePanel.h"
#include "UI/SampleEnvelopePanel.h"
#include "UI/PianoRollComponent.h"
#include "UI/RackPanel.h"
#include "UI/SettingsWindow.h"
#include "UI/TypingPiano.h"

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

    /** Fired once when the drag starts, before any travel. The host captures
        the height it is about to change here, because the travel below is
        ABSOLUTE — see onDragTravel. */
    std::function<void()> onDragBegin;

    /** Total pointer travel since the drag began, in pixels, positive = down.

        Phase 25: this used to be an incremental delta, which is what made the
        band feel detached. Deltas that the host clamped away were LOST: drag
        past the bottom limit and back, and the band stayed at the limit while
        the pointer was already 200 px above it. Absolute travel means the band
        is under the pointer for every pixel it can move, and leaves the limit
        the instant the pointer comes back. */
    std::function<void (int totalTravel)> onDragTravel;

    /** Test hook: the self-test has no live peer to synthesise a MouseEvent
        from, so it drives the same two callbacks a real drag would. Pass
        `continueGesture` to report travel against the previous drag's base,
        which is how a single gesture is emulated. */
    void simulateDrag (int totalTravel, bool continueGesture = false);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    bool hot      = false;
    bool dragging = false;
    int  dragStartScreenY = 0;
    int  lastTravel       = 0;
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

    /** Phase 39: the only reliable "a key came up" signal. JUCE has no per-key
        up event, so the release side of the typing keyboard is found by asking
        which mapped keys are still down. It walks up the parent chain, so it
        arrives here from whichever panel has focus. */
    bool keyStateChanged (bool isKeyDown) override;

    /** Opens the output device. Returns false when there is no usable output;
        the editor keeps working, it just cannot be heard. */
    bool startAudio();
    bool isAudioRunning() const { return audio.isRunning(); }

    //-- Transport ------------------------------------------------------------
    /** Starts (or resumes) playback from wherever the playhead is. The beat it
        started from is remembered (playStartBeat): Phase 51 makes stop return
        there, so "play from bar 20, stop" parks the head at bar 20 again —
        the run's own beginning, not the top of the timeline. */
    void playTransport();
    /** The one implementation, with the device flag exposed for the headless
        self-test: stop-returns-to-start must be provable without grabbing the
        sound card (the same reason AudioEngine::play takes openDevice). */
    void playTransport (bool openDevice);
    /** Ends a run and returns the playhead to where THIS run started. Pressed
        while nothing is running it does NOTHING — the playhead is also how
        playback is positioned, so a stop by reflex must not undo a click on
        the ruler. Phase 33: there is no pause button; space is a play/stop
        toggle. */
    void stopTransport();
    /** Return: the explicit rewind, from any state. */
    void rewindTransport();
    /** Stops the engine and puts every view's playhead at `target`. The one
        place a transport state change is written out. */
    void parkTransportAt (double target);

    //-- Phase 81: recording ---------------------------------------------------
    /** The red dot. One button for both takes: pressing it starts recording
        (and rolling, if the transport is parked), pressing it again ends the
        take and lays down what was captured. */
    juce::Rectangle<int> getRecordButtonBounds() const;
    void toggleRecording();
    bool startRecording();
    void finishRecording();
    bool isRecording() const noexcept { return recording.load(); }

    /** The whole landing step in one place: WAV + sampler + clip for the audio,
        notes + group + lanes for the MIDI. Shared by the button and the
        self-test so a take is never built two ways. */
    int commitTake (Take take, const juce::AudioBuffer<float>& audioTake, double sampleRate);

    /** Writes the recorded CC onto the group's own lanes. */
    void applyRecordedControls (NoteGroup& group, const Take& take) const;

    /** Where takes are written: `Music/AtomicDAW Recordings`. Created on the
        first take; never inside the project folder, because a take outlives the
        project it was recorded into. */
    juce::File getRecordingsFolder() const;

    /** Redirects takes somewhere else. The self-test uses the temp folder — a
        test run must not leave twenty WAVs in the user's Music library. */
    void setRecordingsFolder (juce::File folder) { recordingsFolder = std::move (folder); }

    int  getLastRecordGroupId() const noexcept { return lastRecordGroupId; }
    int  getLastRecordClipId()  const noexcept { return lastRecordClipId; }

    /** Test-only seams: the same collector and the same commit path the real
        button uses, with the audio injected instead of captured. */
    /** The two event sinks, shared by the MIDI thread's hop and by the tests.
        Beats are captured where the event happened, not where the message
        thread got round to it. */
    void recordNoteAt (int pitch, float velocity, bool on, double beat);
    void recordControlAt (int controller, int value7, double beat);

    void beginTakeForTest (double startBeat);
    void takeAudioForTest (const juce::AudioBuffer<float>& audio, double sampleRate);
    int  endTakeForTest (double endBeat);

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

    /** Phase 78: opens the clip envelope window for a sample block. Positioned
        beside the block, clamped inside the window — same reasoning as the note
        panel above. */
    void openSampleEnvelopePanel (int layerIndex, int sampleId);

    /** P79: rename a synth slot AND the sample blocks that carry its name.

        The rack cannot do this itself — it knows nothing about layers — and the
        roll must not do it either, because a block can live on a sheet the roll
        is not showing. So the host does it, once, for every sheet. */
    void renameSynthSlot (int slotId, const juce::String& newName);

    //-- Phase 80: tempo automation -------------------------------------------
    /** Puts the tempo curve in the automation window and scrolls to it. The one
        entry point, called from the BPM field's right-click menu and from the
        headless demo; asking twice selects the existing row rather than adding a
        second one (AutomationModel::addTrack's own rule). */
    void showTempoAutomation();

    /** True when the window has a tempo row — what the menu item ticks on. */
    bool hasTempoAutomationTrack() const;

    /** Phase 80: the tempo curve, for the self-test (it is the only handle on a
        model that the UI reads only through the window). */
    TempoModel& getTempoModel() noexcept { return tempo; }

    /** Phase 80: the track list and the model behind it, for the self-test. The
        window owns the drawing; these are the two things a check needs to ask
        "is there a tempo row, and which curve does it resolve to". */
    AutomationModel& getAutomationModel() noexcept { return automation; }
    AutomationWindow& getAutomationWindowForTest() noexcept { return automationWindow; }
    const std::vector<AutomationTrack>& getAutomationTracks() const
    {
        return automation.getTracks();
    }

    /** Parks the playhead somewhere visible; used by --snapshot. */
    void setDemoPlayheadBeats (double beats);

    /** P79: opens the in-place rename box on a sheet name; used by --snapshot.
        A rename box is the whole feature and it only exists while a gesture is
        in progress, so a still frame is the only way to show it. */
    void openDemoRename();

    /** Phase 80: puts a drawn tempo curve in the automation window and switches
        it on, so the screenshot shows the row — the one part of this feature
        that has no representation anywhere else. */
    void openDemoTempoAutomation();

    /** Phase 81: one recorded take, in place, for the snapshot. A feature whose
        whole output is "a group you did not draw and a curve you did not write"
        has nothing to look at unless the demo performs the take. */
    void openDemoRecording();

    /** Opens the roll's inline automation box on a long demo note, so the
        screenshot shows the box and its parameter menu. */
    void openDemoNoteAutomation();

    /** Phase 22: selects a few notes so the screenshot shows what "selected"
        looks like next to "not selected". Snapshot only. */
    void showDemoNoteSelection();

    /** Phase 28: selects the demo's audio clip, so the screenshot shows a
        selected BLOCK (white ring + corner brackets) and not just a note.
        Snapshot only. */
    void showDemoSampleSelection();

    /** Phase 28: adds a fixture folder tree to the browser — four levels deep,
        created in the temp directory and never written to the user's folder
        list — so the browser image documents the tree rather than whatever
        happens to be on this machine. Snapshot only. */
    void prepareSnapshotBrowser();

    /** Phase 21: the same for a group — opens the inline group curve box on the
        widest group of the active sheet. Snapshot only. */
    void openDemoGroupAutomation();

    /** Borrows two notes of the active layer onto another synth, so the
        screenshot shows both halves of the routing feature: the highlighted
        synth in the rack and the layer's 复位 pill. Snapshot only. */
    void showDemoRoutingOverride();

    /** Phase 23: chains three strips (1 → 2 → 3 → master) so the mixer
        screenshot shows strip-to-strip routing instead of eight identical
        "主输出" chips. Snapshot only. */
    void showDemoConsoleRouting();

    /** Phase 29: selects the demo's clip AND some of its notes at once, which
        is the state a marquee can now produce. Snapshot only. */
    void showDemoMixedSelection();

    /** Phase 29: the roll zoomed onto the demo clip. The trim bands and the
        block's own edges are a few pixels wide at the default scale, so the
        block needs its own frame to be visible at all. Restores the view. */
    juce::Image createClipCloseupSnapshot (int width = ui::px (900), int height = ui::px (420));

    /** Phase 49: the same zoom, with the clip's volume curve unfolded under it.

        Its own frame because only one floating editor may be open at a time and
        the hero image already carries the note box. The curve is given a shape
        for the shot and put back afterwards: a flat line would leave the reader
        wondering whether the box works, and a demo curve is not the user's. */
    /** Phase 50: `target` chooses which of the clip's two curves the frame
        shows. Two frames instead of one, because the menu row's whole new job is
        naming a target and a single picture cannot show both. */
    juce::Image createClipAutomationSnapshot (int width = ui::px (900), int height = ui::px (520),
                                              SampleAutomationTarget target
                                                  = SampleAutomationTarget::volume);

    /** Phase 30: the roll with one synth highlighted, at normal brightness.

        Own frame and own zoom: the marks are rings and a colour tint on a few
        notes, and they are invisible at the opening scale. */
    juce::Image createSpotlightSnapshot (int width = ui::px (900), int height = ui::px (420));

    /** Phase 31: three copies of the demo file, one per stretch mode, side by
        side.

        The three modes sound different and LOOK identical, so the picture can
        only make the claim the badges make: the same file at three ratios, each
        saying what it is doing. The copy is made and thrown away inside this
        function — the snapshot must not leave three mystery blocks in the
        session it photographed. */
    juce::Image createStretchSnapshot (int width = ui::px (900), int height = ui::px (420));

    /** Phase 53: the two card drops, mid-gesture.

        A drag preview exists for the half second before the button comes up,
        and it is the only place the interface says what a drop will do — so it
        cannot be photographed by a live run at all and has to be staged here.
        `instrumentCard` picks which gesture: a synth hovering a note (挂载) or a
        sampler hovering a block (替换采样). The preview is cleared before
        returning; the session is not changed by either frame. */
    juce::Image createCardDropSnapshot (bool instrumentCard,
                                        int width = ui::px (900),
                                        int height = ui::px (420));

    /** Phase 54: a note selection and the cards it plays on, in one frame.

        The whole point of the mark is that it is NOT the spotlight — the grid
        keeps its brightness while the cards are ringed — and that is a claim
        only a picture can make: both states are "a card with a ring on it" in
        prose. Selects one note per distinct routed slot so the multi-card case
        is what gets photographed, then puts the selection and the spotlight
        back. */
    juce::Image createSelectionMarkSnapshot (bool wholeShell = false, bool spotlit = false);

    /** Phase 35: the demo group, muted, with its mixed members selected — the
        pill reads 静音 and the block inside the bracket is lit at the same time.
        The mute is put back before returning: it exists for the frame. */
    juce::Image createGroupMuteSnapshot (int width = ui::px (900), int height = ui::px (420));

    /** Phase 36: the note inspector with all three automation layers live.

        The overlay only earns its pixels when the three curves disagree — three
        flat rules at 1.0 would look like a working picture of nothing — so this
        frame writes one curve per layer, photographs the panel, and puts the
        demo back exactly as it found it. */
    juce::Image createAutomationStackSnapshot (int width = ui::px (452), int height = ui::px (314));

    /** Phase 42: the automation window with a synth's own parameter as a row.
        The menu that creates it cannot be photographed (a popup belongs to the
        desktop and needs a live message loop), so the picture shows the result:
        a row that says WHICH synth and that it is 音源参数, not the filter.
        Tall enough for seven rows: the new one is the LAST, and a frame that
        cuts its own subject in half documents the wrong thing. */
    juce::Image createSlotParamAutomationSnapshot (int width = ui::px (860), int height = ui::px (380));

    /** Phase 32: the whole window with the demo clip selected.

        One gesture, two panels, and neither half is convincing alone: the block
        wears its selection ring in the roll while the sampler that holds its
        file is outlined in the rack. The frame is the pair. */
    juce::Image createSamplerLinkSnapshot();

    /** Phase 34: the cascade question, rendered. A dialog lives in its own
        window, so the only way to document what the user is being asked is to
        build the window off-screen and photograph it. */
    juce::Image createSamplerPromptSnapshot();

    /** Phase 52: the export dialog, headless. `mp3` picks which format the
        picture shows — the two frames together are the whole control (WAV with
        the bitrate row greyed, MP3 with the depth row greyed). */
    juce::Image createExportDialogSnapshot (bool mp3);

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
    bounce::ExportResult exportAudioToFile (const juce::File& file,
                                         const std::function<bool (double)>& progress = {},
                                         double tailSeconds = 0.05);

    /** Last note's end, in seconds at the current tempo. 0 for an empty project. */
    double getContentSeconds() const;

    juce::File getCurrentProjectFile() const { return currentProject; }

    juce::String getAudioStatus() const { return audio.getStatusText(); }

    LayerStack&  getLayerStack() { return stack; }
    RackModel&   getRack()       { return rack; }

    /** P79: the roll itself, so a test can read what a block actually prints
        on its name strip. Reading the model instead would only prove the model
        changed, not that the words on screen did. */
    PianoRollComponent& pianoRollForTest() { return pianoRoll; }

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
    juce::Image createSettingsSnapshot (int tabIndex, int width = ui::px (820), int height = ui::px (520));

    /** Phase 39/40: the MIDI tab with only the computer keyboard on, so the
        frame shows the FL Studio map and nothing else. Restores the switches
        afterwards. */
    juce::Image createMidiKeyboardSnapshot();

    /** Phase 40: the same tab with BOTH sources on — the state the two switches
        exist for, and the only way to photograph the shared layout. */
    juce::Image createMidiBothSnapshot();

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

    /** Phase 43: how tall the file tree's pane is, in device pixels, as the
        splitter currently has it. The self-test asks this instead of measuring
        components: the number IS the setting, and a component's height also
        depends on the window it happens to be in. */
    int getFilePaneHeight() const noexcept { return filePaneHeight; }

    /** Phase 28: the same for the sample browser, which in the window is only
        a couple of hundred pixels tall — too short to show a four-level tree. */
    juce::Image createFileBrowserSnapshot (int width = ui::px (330), int height = ui::px (430));

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

    //-- Phase 71: an undo step is an ACTION, not a state ---------------------
    /** Brackets one mouse press-and-release.

        Between these two calls the history takes no snapshots at all: dragging
        a note four beats used to fire one notesChanged per mouse move and leave
        four states behind, so "undo" walked the drag back one quarter at a
        time. The gesture's final state is captured once, when the mouse comes
        up, which is what "undo what I just did" has always meant.

        Nesting is ignored on purpose — one press is one gesture. A press that
        never sees its release (the window closed mid-drag) is closed by the
        next press. */
    void beginUndoGesture();
    void endUndoGesture();

    /** True between begin and end. The self-test uses it to prove the bracket
        actually swallows the edits in between. */
    bool isUndoGestureActive() const { return undoGestureActive; }

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

    /** Phase 42: "自动化这个音源的第 N 个参数".

        The generic call cannot do this one honour: a parameter index is an
        index into ONE plugin's list, so it has to be stored on the slot next to
        the lane. Creating the curve also means picking the parameter, and
        picking the parameter is what makes the lane mean anything — one call,
        or the two can disagree.
    */
    void ensureSlotPluginParamTrack (int slotId, int paramIndex);

    /** Phase 42: which parameters a slot's hosted instrument offers. Empty for
        an internal synth, a sampler, or a slot whose plugin is not loaded —
        the menus use the empty list to say "there is nothing to automate here"
        instead of showing an empty submenu. */
    std::vector<PluginParamInfo> pluginParamsForSlot (int slotId);

    /** Phase 42: the menu items every "automate this synth" menu shares —
        滤波器 / 音量 / 音源参数 ▶. `idBase` is the first id the caller has free;
        the two fixed items take idBase+0/1 and the parameters take
        idBase + 100 + index. */
    void addSlotAutomationItems (juce::PopupMenu& menu, int slotId, int idBase);

    /** Phase 51: the sampler card's 拉伸 control, host half. The state is what
        the slot's blocks agree on (StretchMode value / -1 mixed / -2 unused);
        applying writes the mode onto every block playing the slot and notifies
        through the one channel every model edit uses. Public for the
        self-test, which asserts the write reaches the blocks. */
    int  samplerStretchStateFor (int slotId) const;
    void applySamplerStretchToSlot (int slotId, StretchMode mode);

    /** Phase 52: the same pair for the 只变调 range, in semitones. -2 when no
        block plays the slot, -1 when they disagree. */
    int  samplerPitchRangeStateFor (int slotId) const;
    void applySamplerPitchRangeToSlot (int slotId, int semitones);

    /** The other half of the pair: true when `result` was one of the ids
        addSlotAutomationItems() handed out and it has been acted on. One
        function for both menus, because "which id means which parameter" must
        not exist twice. */
    bool applySlotAutomationChoice (int result, int slotId, int idBase);

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

    //-- Phase 22: menu bar internals -----------------------------------------
    /** Filled on demand: a menu whose "enabled" states depend on the session
        has to be built when it is shown, not when the window opens. */
    void fillFileMenu (juce::PopupMenu& menu) const;
    void fillEditMenu (juce::PopupMenu& menu) const;

    /** Phase 54: the tool strip as a menu.

        Not a second implementation of the tools — every item calls the same
        method the strip's click handler calls, with the same "nothing selected
        means nothing happens" guards, so the two entry points cannot drift.
        The point is reach: the strip has nine icon buttons whose states (which
        mode is on, whether 编组 is possible) are only visible if you are
        looking at them. */
    void fillToolsMenu (juce::PopupMenu& menu) const;
    void applyToolsChoice (int itemId);

    void applyFileChoice (int itemId);
    void applyEditChoice (int itemId);
    void showMenuBarMenu (int index);

    /** Bottom-right popup with the two visual preferences plus the version. */
    void showAboutOrReset();

    //-- Phase 9: plugin wiring ----------------------------------------------
    /** Creates the settings window on first use and returns it. Kept separate so
        the headless snapshot can build the pages without showing the window. */
    SettingsWindow* ensureSettingsWindow();

    void showRackAddMenu (juce::Point<int> screenPosition);

    /** Right-click / "→ 输出" on a rack card. Phase 11: replace the instrument
        inside the slot (keeping its id, fader and curves), send the synth to a
        console channel, add its automation, or remove it.

        Phase 21: split in two. `fill…` builds the menu into a caller's
        PopupMenu so a numeric control can hang it as a REAL sub-menu (which
        JUCE opens on hover), `apply…` runs the choice. Neither half knows about
        the other's screen position, so the same code serves the card's own
        right-click and the fader's sub-menu. */
    void fillRackCardMenu (juce::PopupMenu& menu, int slotId);
    void applyRackCardChoice (int result, int slotId);
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

    /** Phase 59: pushes the roll's group selection into the group panel, so the
        row for "the group I am dragging in the piano roll" is the one lit up in
        a list of twenty. The roll owns the selection; the panel only mirrors it. */
    void syncGroupPanelSelection();

    /** Phase 32: removes a rack slot AND the clips that were playing through it.

        The only way to delete a slot from the UI, because the two halves are
        one promise: a clip names a slot, so a slot cannot vanish from under it.
        Reached by the card's × and by the card menu's "删除这个音源". */
    void removeSlotAndItsClips (int slotId);

    /** Phase 34: the × and the menu item land HERE, not on the executor.

        Deleting a sampler takes its blocks with it, and Phase 34's rule is that
        nothing that deletes music happens without being announced first. The
        question is only asked when there is something to lose: a sampler with
        no blocks, or an instrument slot, deletes on the spot. */
    void requestRemoveSlotAndItsClips (int slotId);

    /** Phase 34: the ONE place a cascade deletion asks.

        Both directions arrive here — "the roll has no blocks left" and "the
        rack is losing the card" — so the wording, the "you said keep" memory
        and the executor are shared. Handed a list of sampler ids; answers
        itself by calling removeSamplerSlots() or by remembering the refusal.

        Injected as a std::function because the self-test has to drive both
        answers without a message loop: the real one is a modal AlertWindow and
        cannot return a value. */
    std::function<void (const juce::String& title,
                        const juce::String& body,
                        std::function<void (bool)> onAnswer)> askSamplerCascade;

    /** Phase 34: puts the real AlertWindow back into `askSamplerCascade`.
        Called by the constructor, and by the self-test when it is done
        scripting answers. */
    void installSamplerCascadePrompt();

    /** Phase 34: removes sampler slots with the re-entry guard on, and forgets
        any "keep it" verdict about them. Blocks are not touched — the only
        caller that has blocks to delete goes through removeSlotAndItsClips(). */
    void removeSamplerSlots (const std::vector<int>& slotIds);

    /** Phase 32: deletes every SAMPLER slot that no clip on any layer names.

        The other direction of the same promise: a clip grew a sampler when it
        was dropped, so deleting the last clip leaves that sampler with nothing
        to play and nothing to show. A rack full of orphan samplers is what
        "delete a clip" used to leave behind. */
    void pruneOrphanSamplerSlots();

    /** Phase 34: the sweep's question, asked from the message queue rather than
        from inside the model notification that noticed the orphan. */
    void askToPruneOrphanSamplerSlots();

    /** Phase 34: which sampler slots nothing names any more, minus the ones the
        user said to keep.

        Also forgets a "keep it" verdict the moment some clip names that slot
        again: the answer was about a sampler that had nothing to play, and that
        is no longer the situation. Queried by both the trigger and the prompt,
        so the question is always about the model as it is right now. */
    std::vector<int> orphanSamplerIds();


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

    //-- Phase 39/40: playing the synth from a keyboard -----------------------
    /** Two independent switches — 电脑键盘 and 外部 MIDI 控制器 — and the typing
        keyboard is on out of the box. Turning a source OFF releases the notes it
        was holding; turning one ON disturbs nothing, which is the whole point of
        two switches instead of a choice. */
    void setMidiSources (MidiSources sources);
    MidiSources getMidiSources() const noexcept;

    /** True when the computer keyboard is routed to the synth. The key handler
        asks this on every keystroke, so it stays a one-line read. */
    bool typingKeyboardLive() const noexcept;

    /** Which rack slot a played key goes to. The spotlit card when it is an
        instrument — that is the FL Studio "selected channel" gesture, and the
        card already lights up when you click it — otherwise the active layer's
        synth. -1 when the session has no instrument at all. */
    int getLiveTargetSlot() const;

    /** The same answer as a name, for the settings page ("目标音源：Synth A"). */
    juce::String getLiveTargetName() const;

    /** One played key. `velocity` is 0.8 for the typing keyboard, which has no
        way to say how hard a key was hit. */
    void liveNoteOn (int pitch, float velocity);
    void liveNoteOff (int pitch);

    /** The typing keyboard's octave, in octaves, relative to FL Studio's
        layout (Z = C4). Numpad + / - move it while playing. */
    int  getTypingOctave() const noexcept { return typingOctave; }
    void setTypingOctave (int octaves);

    /** The settings page edits the device list through this; the host owns it
        because the host is what makes the sound. */
    MidiInputHub& getMidiInputHub() noexcept { return *midiHub; }

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

    /** Phase 21: the same menu, as content. See the rack card pair above. */
    void fillAutomationMenu (juce::PopupMenu& menu, AutomationOwner owner, int ownerId);
    void applyAutomationChoice (int result, AutomationOwner owner, int ownerId);

    /** Phase 18: a console strip's menu. Built here, not in MixerPanel, because
        the automation half of it needs the session model. Items are GREYED when
        the channel has no source rather than answered with a dialog: a menu that
        shows what is unavailable is information, a dialog is an interruption. */
    void showChannelMenu (int channel, juce::Point<int> screenPos);

    /** Phase 21: the same menu, as content. */
    void fillChannelMenu (juce::PopupMenu& menu, int channel);
    void applyChannelChoice (int result, int channel);

    /** Phase 43: ONE control of one strip — the fader, the pan, or one of the two
        switches. The panel knows which control was clicked and nothing else, so
        this is where "the fader means the synth's level lane" lives.

        It exists because the old arrangement let a right-click on a fader open
        the whole channel menu: a menu about the strip answering a question about
        a number, with "删除通道条" two rows below "复位". */
    void fillStripAutomationItems (juce::PopupMenu& menu, int channel,
                                   MixerPanel::StripControl control);
    void applyStripAutomationChoice (int channel, MixerPanel::StripControl control, int result);

    //-- File actions (button / shortcut entry points) ------------------------
    /** `onDone` (optional) is told whether a file was actually written, so the
        quit / new / load flows can continue only after a real save. */
    void askToSaveProject (std::function<void (bool saved)> onDone = {});
    void askToLoadProject();
    /** The file dialog itself, with no "save first?" question in front of it. */
    void showLoadChooser();
    void askToExportAudio();

    /** Phase 52: opens the export dialog (format / depth / bitrate / channels)
        and, if the user accepts, runs the chooser and the bounce with those
        settings. The button, the menu item and Ctrl+E all land here, so there
        is exactly one export path. */
    void showExportDialog();
    void beginExport (const ExportSettings& settings);

    void showMessage (const juce::String& title, const juce::String& message, bool isError);
    void exportFinished();

    //-- Phase 50: bounce a group / layer / selection into a sample block -----
    /** Asks for nothing, renders in the background and then puts the file on the
        layer as a block at the first free beat. When the layer is full the user
        is told — a bounce that silently appends to the end of the song is a
        bounce that moved the song. */
    void renderScopeToSample (const PianoRollComponent::RenderScopeRequest& request);

    /** Called on the message thread when the bounce thread finishes. */
    void insertRenderedSample (const juce::File& file, int layerIndex,
                               double lengthBeats, const juce::String& name);

    /** Where a block of `lengthBeats` would fit on this layer, or "nowhere".
        Used twice on purpose: once before rendering (so a full layer does not
        cost four minutes of CPU) and once when the block is about to land (the
        user may have edited the layer while the bounce ran). */
    scope::FreeBeat freeBeatOnLayer (int layerIndex, double lengthBeats) const;

    //-- Phase 22: the menu bar ----------------------------------------------
    /** Three menus, drawn by this component rather than by juce::MenuBarComponent.

        A stock menu bar would be a light grey strip in the middle of a dark
        transport bar, and this app has one look. The cost is that the items,
        their bounds and their hover state are all here — which is also why the
        shortcuts are listed in one table instead of two. */
    static constexpr int numMenus = 3;

    /** The label above each menu, in one place: paint() and the bounds maths
        have to agree about how many there are, and a ternary chain in paint()
        was how the second menu spelled itself in two different places. */
    static juce::String menuTitle (int index);

    juce::Rectangle<int> getMenuBarBounds() const { return menuBarBounds; }
    juce::Rectangle<int> getMenuBarItemBounds (int index) const;

    /** Save, no questions: writes to the current file, or opens the chooser when
        the session has never been saved. This is what 文件 ▸ 保存 and Ctrl+S do;
        `askToSaveProject` always asks for a path, so it is 另存为. */
    void saveProject();

    /** Quits the way the close button does: asks about unsaved work first. */
    void requestQuit();

    //-- Phase 22: Edit menu --------------------------------------------------
    /** Copy / cut / paste route to the curve clipboard when an inline
        automation box is selected, and to the note clipboard otherwise. One
        shortcut set, one rule: the thing in front of you wins. */
    bool canCopyEditSelection() const;
    bool canCutEditSelection() const;
    bool canPasteEditSelection() const;
    void copyEditSelection();
    void cutEditSelection();
    void pasteEditSelection();

    /** The stretch question, then the paste. Separated so the self-test can
        prove the paste maths without a modal dialog. */
    void askHowToPasteCurve();

    /** Where the dialogs start: Documents/AtomicDAW, created on demand. */
    static juce::File getProjectDirectory();

    juce::Rectangle<int> getPlayButtonBounds() const;
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

    /** Phase 51: where the current (or last) run of the transport started.
        Written by playTransport() only, read by stopTransport() — stop returns
        here, not to zero. Zero until the first play, which keeps "stop on a
        fresh session" identical to the old rewind-to-top. */
    double              playStartBeat = 0.0;

    /** VST3 instances. Declared before the engine so it outlives every
        PlaybackEngine that points at it. */
    PluginHost          plugins;
    AutomationModel     automation;
    MixerModel          mixer;

    /** Phase 80: the project's tempo curve. Declared before the engine and the
        automation window because both point at it. */
    TempoModel          tempo;

    AudioEngine         audio { stack, rack };

    LayerPanel          layerPanel       { stack, rack };
    PianoRollComponent  pianoRoll        { stack, rack };
    NoteEnvelopePanel   envelopePanel    { stack, rack };
    SampleEnvelopePanel sampleEnvelopePanel { stack, rack };   // Phase 78
    RackPanel           rackPanel        { rack };
    AutomationWindow    automationWindow { stack, rack, automation, mixer, tempo };
    MixerPanel          mixerPanel       { stack, rack, mixer };
    BpmField            bpmField;

    //-- Phase 81 -------------------------------------------------------------
    TakeCollector takeCollector;

    /** Atomic because the MIDI thread reads it: "is the tape rolling" is asked
        from a real-time-ish callback, and a plain bool there is a data race. */
    std::atomic<bool> recording { false };
    double        recordStartBeat    = 0.0;
    int           recordingCounter   = 0;   // "录音 N"
    int           lastRecordGroupId  = -1;
    int           lastRecordClipId   = -1;

    /** What the self-test feeds instead of a microphone. Kept out of the real
        path: `finishRecording()` reads the engine, `endTakeForTest()` reads this. */
    juce::AudioBuffer<float> testTakeAudio;
    double                   testTakeSampleRate = 44100.0;

    /** Empty means "Music/AtomicDAW Recordings". */
    juce::File recordingsFolder;

    /** Phase 17: the two lists the side columns grew downwards into. Both are
        siblings of the panels above them rather than children: each one owns its
        own scroll position, and the split between them is a fixed pixel height
        so a fold never rescales the other. */
    FileBrowserPanel    filePanel;
    GroupPanel          groupPanel       { stack, rack };

    /** Pixels each lower panel keeps, measured from the bottom of its column.
        Same reasoning as automationPaneHeight: a size the user dragged to is a
        size, not a ratio.

        Phase 43 raised the file pane from 250 to 380 design px. 250 was chosen
        in Phase 17 when the list was three folders tall; a real sample library
        is a tree, and the shipped height showed the header and two rows — the
        one panel whose job is browsing had nothing to browse in. The band is
        still draggable both ways, so this is a starting point and not a rule. */
    int filePaneHeight  = ui::px (380);
    int groupPaneHeight = ui::px (320);

    /** Phase 25: the heights the three bands started their drag from. The bands
        report absolute travel, so the host has to remember what the value was
        when the gesture began — that is what makes the band keep up with the
        pointer instead of drifting away from it. */
    int dragBaseFileHeight       = 0;
    int dragBaseGroupHeight      = 0;
    int dragBaseAutomationHeight = 0;

    /** Phase 12: drag it to rebalance the roll and the automation window. */
    VerticalSplitter    splitterBar;

    /** Phase 21: one per side column. Same band, same gesture, rotated use —
        the user asked to be able to make the file tree (or the group list) taller
        without losing the panel above it. Two more instances rather than making
        the panels resizable themselves: the height belongs to the column, and
        the host is what knows how much room the column has left. */
    VerticalSplitter    leftSplitter;
    VerticalSplitter    rightSplitter;

    /** Pixels given to the automation window. 312 opens tall enough for six
        curve rows, which is exactly what the demo fixture ships (Phase 18 added
        the master bus row to the five that were already there): a default that
        clips the last row it ships with is a bad first impression. The
        splitter changes it from there. Stored in pixels, not as a ratio: the
        user dragged to a size, and that size is what they get back when the
        window comes back. */
    int automationPaneHeight = ui::px (312);

    juce::Rectangle<int> topBarBounds;
    juce::Rectangle<int> menuBarBounds;
    int                  hoveredMenuItem = -1;

    int   demoOverrideLayer = -1;
    int   demoOverrideNote  = -1;

    /** Phase 16: the synth the roll is spotlighting, -1 for none. Kept on the
        host because all three panels have to agree about it. */
    int   focusedSlotId = -1;

    //-- Phase 39: the typing keyboard ---------------------------------------
    /** The spotlit synth, mirrored into an atomic the MIDI thread can read.

        A note that arrives from a controller lands on that thread, and the
        model is not safe to touch there. The mapping decision belongs to the
        message thread; the MIDI thread only gets the answer. */
    std::atomic<int> liveTargetSlot { -1 };

    /** Keys currently held, as (key code, note). The note is stored beside the
        key because the octave can change while a key is down — the release has
        to land on the note that actually started, not on the new mapping. */
    std::vector<std::pair<int, int>> typingHeld;

    /** Octaves, relative to FL Studio's layout. */
    int typingOctave = 0;

    /** Velocity for a key that cannot say how hard it was hit. */
    static constexpr float typingVelocity = 0.8f;

    /** Forwards key events to the typing keyboard BEFORE the focused panel sees
        them. A KeyListener is called ahead of the component's own keyPressed —
        which is what keeps plain G a note instead of "group the selection"
        while the keyboard is set to play. */
    class TypingKeyForwarder final : public juce::KeyListener
    {
    public:
        explicit TypingKeyForwarder (MainComponent& ownerToUse) : owner (ownerToUse) {}

        bool keyPressed (const juce::KeyPress& key, juce::Component*) override
        {
            return owner.handleTypingKeyDown (key);
        }

        bool keyStateChanged (bool, juce::Component*) override
        {
            return owner.releaseTypingKeysThatAreUp();
        }

    private:
        MainComponent& owner;
    };

    TypingKeyForwarder typingForwarder { *this };

    /** Phase 42/43: the keys that must work while ANOTHER top-level window of
        this app is in front.

        A hosted VST's editor and the mixer are separate top-level windows, so a
        key pressed there never walks up to this component's parent chain: 空格
        stopped starting the transport and the computer keyboard stopped playing
        the moment the user clicked into the synth's UI — and again the moment
        the console window got focus. The window's KeyListener closes that hole:
        keys reach here first, and a key we do not claim falls through. */
    class PluginKeyForwarder final : public juce::KeyListener
    {
    public:
        explicit PluginKeyForwarder (MainComponent& ownerToUse) : owner (ownerToUse) {}

        bool keyPressed (const juce::KeyPress& key, juce::Component*) override
        {
            return owner.handleGlobalKeyPress (key);
        }

        bool keyStateChanged (bool, juce::Component*) override
        {
            return owner.releaseTypingKeysThatAreUp();
        }

    private:
        MainComponent& owner;
    };

    /** One instance, attached to every window that is not this one. Two copies
        would be two chances for the key maps to disagree. */
    PluginKeyForwarder pluginForwarder { *this };

    /** The host's key map, applied to a window that is not this one. Refuses
        while the user is typing into whatever has focus — a preset name in the
        plugin's own UI is text, not notes. */
    bool handleGlobalKeyPress (const juce::KeyPress& key);

    bool handleTypingKeyDown (const juce::KeyPress& key);
    bool releaseTypingKeysThatAreUp();
    void releaseAllTypingNotes();

    /** Phase 39: owns the open MIDI inputs. Declared after `audio` on purpose —
        members die in reverse order, and a MidiInput that outlives the engine
        it feeds would call into a destroyed object. */
    std::unique_ptr<MidiInputHub> midiHub;

    /** Re-derives `liveTargetSlot` from the model. Message thread only. */
    void refreshLiveTarget();

    /** Phase 32: true when `focusedSlotId` was set by SELECTING a clip rather
        than by the user. Clearing the selection must take that highlight away
        again, but must never take away one the user asked for. */
    bool  focusFromSampleSelection = false;

    /** Phase 33: suspends the sampler sweep while the layers and the rack are
        deliberately out of step (a session rebuild or a project load). Without
        it, the sweep would delete a sampler whose clip has not been restored
        yet — the notification fires halfway through the swap. */
    bool suspendSamplerPrune = false;

    /** Phase 32: guards the reference sweep against re-entering itself. Adding
        or removing a slot notifies the rack, and that notification is what asks
        for the sweep in the first place. */
    bool pruningSamplers = false;

    /** Phase 34: samplers the user answered "keep it" about.

        Without this the sweep would ask again on the very next note drag — the
        sweep runs on every notesChanged — and a question you answered ten
        seconds ago coming back is not a question, it is a nag. Cleared for a
        slot as soon as some clip names it again: at that point the answer was
        about a different situation. */
    std::set<int> pruneDeclinedSlots;

    /** Phase 34: one prompt at a time. The sweep is reached from every
        notesChanged, so a drag that leaves two orphans would otherwise queue a
        dialog per mouse move. */
    bool prunePromptQueued = false;

    /** Phase 34: true = ask from the message queue (the real app, where the
        sweep is often inside a drag); false = ask on the spot (the self-test,
        which has no message loop to pump).

        A modal dialog opened from inside a mouse-drag notification swallows the
        mouse-up that the drag is waiting for. That is the whole reason for the
        deferral, so it is kept out of the flow the tests walk. */
    bool deferSamplerPrompt = true;

    bool hoveringPlay  = false;
    bool hoveringStop  = false;
    bool hoveringRecord = false;      // Phase 81

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

    /** Phase 71: true from a mouse press until its release; every snapshot
        request in between is dropped. See beginUndoGesture(). */
    bool undoGestureActive = false;

    /** Phase 71: the press/release witness.

        A Desktop-wide listener rather than one per panel, because the gesture
        it has to bracket happens in the roll, in the console window, in a
        plugin's own editor and everywhere in between — and a rule that has to
        be repeated in forty components is a rule that will be missing in one of
        them. It sees a press, it sees a release, and it does nothing else.

        Note the order JUCE uses: a component's own mouseDown/mouseUp runs
        BEFORE its listeners'. That is exactly what this needs — the release
        that finishes a drag has already written its final state by the time
        endUndoGesture() captures it. */
    struct GestureWatcher final : public juce::MouseListener
    {
        explicit GestureWatcher (MainComponent& ownerToUse) : owner (ownerToUse) {}

        void mouseDown (const juce::MouseEvent&) override;
        void mouseUp   (const juce::MouseEvent&) override;

        MainComponent& owner;
    };

    GestureWatcher gestureWatcher { *this };

    //-- Phase 71: previewing a note through a hosted instrument --------------
    /** What the roll's preview is currently holding down on a plugin.

        A hosted instrument is not oscillated at — it is PLAYED: the preview
        sends it the same note-on the computer keyboard does and lets go after
        a moment. `pitch < 0` means nothing is held. */
    struct HostedPreview
    {
        int          slotId      = -1;
        int          pitch       = -1;
        juce::uint32 releaseAtMs = 0;
    };

    HostedPreview hostedPreview;

    /** How long a preview holds a plugin note before letting go. Same shape as
        the oscillator preview (hold ~0.28 s then decay), so both previews end
        when the hand expects them to. */
    static constexpr int hostedPreviewHoldMs = 420;

    /** One entry point for "sound this note now": a hosted instrument gets a
        real note-on, everything else gets the oscillator voice. */
    void auditionPreview (int slotId, int pitch, float velocity, bool retrigger);

    /** Sends the note-off for a held preview, if there is one. */
    void releaseHostedPreview();

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

    /** Phase 52: what the export dialog offered last time. Kept for the
        session so a second bounce is one click, not four — and kept HERE
        rather than on disk because it describes one sitting's intent, not a
        project property. */
    ExportSettings exportSettings;

    /** The dialog can only be open once; a second Ctrl+E while it is up brings
        it forward rather than stacking another. */
    bool exportDialogOpen = false;

    /** Phase 50: how many bounces have been written, used only to give each
        render file a unique name. Never reused, so a second bounce of the same
        group cannot overwrite the audio the first block is playing. */
    int        scopeRenderSerial = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace atomic
