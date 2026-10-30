#include <JuceHeader.h>

#include <cstdlib>
#include <iostream>

#include "MainComponent.h"
#include "Util/Metrics.h"
#include "Util/Theme.h"
#include "Util/UiLookAndFeel.h"

namespace
{
    // The two throwaway VST3s live in the build tree next to this binary. They
    // are registered as extra search paths so "does VST3 actually work on this
    // machine" has an answer even when nothing is installed — which is exactly
    // the machine this is written on. Plugins the user has installed are found
    // by the host itself, through the default VST3 folders.
    void addDevPluginPaths (atomic::MainComponent& component)
    {
        auto directory = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                             .getParentDirectory();

        for (int level = 0; level < 4; ++level)
        {
            for (const auto* name : { "AtomicTestSynth_artefacts", "AtomicTestFX_artefacts" })
            {
                const auto vst3 = directory.getChildFile (name).getChildFile ("Release").getChildFile ("VST3");

                if (vst3.isDirectory())
                    component.getPluginHost().addSearchPath (vst3);
            }

            directory = directory.getParentDirectory();
        }
    }

    // Headless dev harness: renders the UI to a PNG and dumps the rule check.
    // Used to verify the UI without a display attached.
    // `bpm` > 0 overrides the default tempo, so the same snapshot can be taken
    // at two tempos and compared.
    // Returns whether the rule check passed: --snapshot is a gate, so a red
    // run has to be able to fail the process, not just print red text that a
    // human may never read.
    bool renderHeadlessReport (const juce::File& pngFile, double bpm = 0.0)
    {
        atomic::MainComponent component;
        component.setSize (atomic::ui::px (1440), atomic::ui::px (820));

        // Before the self-test, so the plugin checks have a library to look at.
        // Extra paths only: a screenshot run must not enumerate every plugin the
        // machine has installed — that takes minutes and makes the image differ
        // from one machine to the next for no reason.
        addDevPluginPaths (component);
        component.getPluginHost().setScanExtraPathsOnly (true);
        component.ensurePluginScan();

        if (bpm > 0.0)
            component.setBpm (bpm);

        const auto report = component.runSelfTest();

        // Saving here does two things at once: it exercises the real save path
        // on every snapshot, and it puts a project name in the top bar, so the
        // screenshot shows which file is open rather than "untitled".
        {
            juce::String saveError;

            if (! component.saveProjectToFile (pngFile.getSiblingFile ("demo.atomic"), saveError))
                std::cout << "save failed: " << saveError << std::endl;
        }

        // Phase 5 shows the mixer rather than the note inspector: the point of
        // this frame is that the faders, the pans and two automation lanes are
        // live at the same time. Dropping the demo playhead inside the timeline
        // is what lights the meters and both lane playheads.
        component.setDemoPlayheadBeats (6.55);
        component.openDemoNoteAutomation();
        component.showDemoRoutingOverride();

        // Phase 29: the hero shot shows a MIXED selection — a block and notes at
        // once — because that is the state the marquee can now produce, and it
        // is the one thing a still image can prove about it.
        component.prepareSnapshotBrowser();
        component.showDemoMixedSelection();

        // P79: the rename box on a sheet name — the one state of this feature
        // that leaves no trace in the model, so a screenshot is the only proof
        // it lands on the word it is renaming.
        component.openDemoRename();

        // P80: the tempo curve has no card and no row anywhere else, so the
        // still is the only place the row itself can be checked.
        component.openDemoTempoAutomation();

        // P81: the take itself. What recording produces is a block nobody drew
        // and a group nobody grouped, plus the curve the hand wrote into it —
        // three things that only exist after the button was pressed twice.
        component.openDemoRecording();

        pngFile.getParentDirectory().createDirectory();

        // The snapshot REPLACES the file. `createOutputStream` left the previous
        // image in place and appended the new one after it (a stale 18-image
        // pile was what the eye was reading while the code was right), so the
        // file is removed first: one run, one image.
        pngFile.deleteFile();

        if (auto stream = pngFile.createOutputStream())
        {
            juce::PNGImageFormat png;
            png.writeImageToStream (component.createComponentSnapshot (component.getLocalBounds(), true), *stream);
        }



        // Phase 8: the console lives in its own desktop window now, which a
        // headless run cannot photograph. So the *panel* is rendered on its own
        // into a second file — same pixels the window shows.
        // Phase 9 does the same for the settings tabs, for the same reason.
        const auto writePng = [&pngFile] (const juce::String& suffix, const juce::Image& image)
        {
            if (! image.isValid())
            {
                std::cout << "no image for " << suffix << std::endl;
                return;
            }

            const auto target = pngFile.getSiblingFile (pngFile.getFileNameWithoutExtension()
                                                          + suffix + ".png");

            // Same trap the hero image above documents: createOutputStream
            // appends, so a second run left the new frame behind the old one
            // and every reader kept seeing the FIRST run's pixels. Deleting
            // first is the only version that survives a rebuild.
            target.deleteFile();

            if (auto stream = target.createOutputStream())
            {
                juce::PNGImageFormat png;
                png.writeImageToStream (image, *stream);
            }
        };

        // Phase 32: the clip and its sampler, in one frame. Selecting the block
        // is the gesture that lights the card in the rack, and the pair is the
        // only way to photograph the rule that ties them together.
        writePng ("-sampler", component.createSamplerLinkSnapshot());

        // Phase 34: the question, not the answer. Both cascade deletions stop
        // here first, and a dialog is the one thing a screenshot of the shell
        // can never contain — it lives in its own window.
        writePng ("-prompt", component.createSamplerPromptSnapshot());

        // Phase 84: the question a crashed-out session comes back to. Like the
        // prompt above, a modal dialog has no other way into a still.
        writePng ("-recovery", component.createRecoveryPromptSnapshot());

        // Phase 52: the export dialog, in both formats — WAV with the bitrate
        // row greyed and MP3 with the depth row greyed is the whole control,
        // and a modal window cannot be photographed by a headless run.
        writePng ("-export", component.createExportDialogSnapshot (false));
        writePng ("-exportmp3", component.createExportDialogSnapshot (true));

        // Phase 23: 通道 1 → 2 → 3 → 主输出, so the console image documents the
        // strip-to-strip routing instead of eight strips that all say the same.
        component.showDemoConsoleRouting();

        writePng ("-mixer", component.createMixerSnapshot());

        // Phase 23: the same console in a window too narrow to hold it. The
        // strip widths do not change — a scrollbar appears instead — and that
        // is a claim only a second image can make.
        writePng ("-mixer-narrow", component.createMixerSnapshot (atomic::ui::px (700),
                                                                 atomic::ui::px (300)));

        // Phase 28: the browser tree, taller than the shell's split can give it.
        writePng ("-browser", component.createFileBrowserSnapshot());

        // Phase 29: the demo block zoomed in — the trim bands and the block's
        // own edges are the point, and at the opening scale they are a few
        // pixels. Restores the view itself, so the frames below are unaffected.
        writePng ("-clip", component.createClipCloseupSnapshot());

        // Phase 49: the same block with its volume curve unfolded underneath.
        // Its own frame because only one floating editor may be open at a time
        // and the hero image already carries the note box.
        writePng ("-clipauto", component.createClipAutomationSnapshot());

        // Phase 50: same frame, the other curve. The pan lane is where the menu
        // row's new target button earns its keep.
        writePng ("-clippan", component.createClipAutomationSnapshot (atomic::ui::px (900),
                                                                     atomic::ui::px (520),
                                                                     atomic::SampleAutomationTarget::pan));

        // Phase 30: "highlight a synth" with no veil. The grid keeps its normal
        // brightness and the notes that reach the synth are marked in its
        // colour, which is a claim only a picture can make — every other frame
        // here looks identical either way.
        writePng ("-spotlight", component.createSpotlightSnapshot());

        // Phase 31: one file, three stretch modes side by side. The modes are
        // audible and the blocks are not, so this frame exists to show the
        // badges — the only place the mode is written down on screen.
        writePng ("-stretch", component.createStretchSnapshot());

        // Phase 53: the two card drops, caught mid-gesture. A drag preview only
        // exists while a real drag is in flight — no live screenshot can catch
        // it — so the frame that documents "a synth lands on a note / a sampler
        // lands on a block" has to stage the preview.
        writePng ("-dropnote", component.createCardDropSnapshot (true));
        writePng ("-dropsample", component.createCardDropSnapshot (false));

        // Phase 54: the selection mark, next to the spotlight's absence. The
        // grid is at full brightness and the cards still carry a ring, which is
        // the one difference a screenshot can settle. Two frames: the rack on
        // its own (where the ring is legible) and the shell (where the undimmed
        // grid is).
        //
        // Phase 55 put the SAME ring on both states, so the pair
        // {-selectmark, -selectmark-focus} is the evidence: one card, lit by the
        // selection and then by hand, has to look identical apart from the veil.
        writePng ("-selectmark", component.createSelectionMarkSnapshot (false));
        writePng ("-selectmark-shell", component.createSelectionMarkSnapshot (true));
        writePng ("-selectmark-focus", component.createSelectionMarkSnapshot (false, true));

        // Phase 35: a group whose members are two kinds of thing. The pill says
        // 静音 and the block under it is lit by the same selection that lights
        // the notes — a muted group is still an editable group.
        writePng ("-groups", component.createGroupMuteSnapshot());

        // Phase 21: the group's inline curve box. Its own frame, because only
        // one floating editor can be open at a time and the main image above
        // already documents the note box.
        component.openDemoGroupAutomation();
        writePng ("-groupauto", component.createComponentSnapshot (component.getLocalBounds(), true));

        // Phase 36: the three automation layers in one editor — macro, group and
        // the note's own curve — with the product drawn through them. A number
        // in a legend can be quoted in prose; only the picture shows that the
        // three lines were read separately and multiplied.
        writePng ("-stack", component.createAutomationStackSnapshot());

        // Phase 42: a synth's OWN parameter as an automation row. The submenu
        // that creates it cannot be photographed headlessly, so this frame shows
        // what it produced: a row whose parameter column says 音源参数.
        writePng ("-slotparam", component.createSlotParamAutomationSnapshot());

        // The audio page lists device, sample rate and buffer size — none of
        // which exist until something is open. Opening a device here only fills
        // that page in; the main window's pixels are already on disk.
        component.startAudio();

        writePng ("-settings-audio", component.createSettingsSnapshot (0));
        writePng ("-settings-midi", component.createSettingsSnapshot (1));

        // Phase 39/40: the same tab with the keyboard source chosen — the FL
        // Studio map, built from the table the key handler reads.
        writePng ("-midi-keyboard", component.createMidiKeyboardSnapshot());

        // Phase 40: both sources on at once. With two blocks sharing one page
        // the layout is the thing that can go wrong, and only a picture of the
        // pair shows whether the device list still fits above the monitor.
        writePng ("-midi-both", component.createMidiBothSnapshot());
        writePng ("-settings-plugins", component.createSettingsSnapshot (2));

        // Phase 10: what a real double-click launch shows — one empty layer and
        // an empty rack. Photographed from a SECOND component, because the one
        // above is carrying the demo fixture the rest of these images document.
        {
            atomic::MainComponent blank;
            blank.setSize (atomic::ui::px (1440), atomic::ui::px (820));

            writePng ("-blank", blank.createComponentSnapshot (blank.getLocalBounds(), true));
        }

        pngFile.withFileExtension (".log").replaceWithText (report);
        std::cout << report << std::endl;

        return ! report.contains ("CHECK(S) FAILED");
    }
}

//==============================================================================
class AtomicDAWApplication final : public juce::JUCEApplication
{
public:
    AtomicDAWApplication() = default;

    const juce::String getApplicationName() override     { return "AtomicDAW"; }
    const juce::String getApplicationVersion() override  { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override           { return true; }

    void initialise (const juce::String& commandLine) override
    {
        // Phase 13: menus, alerts and tab bars are painted by JUCE, not by the
        // shell, so the scaled font has to be installed before anything can
        // open one. Default, not per-component: a popup menu belongs to the
        // desktop, and would not inherit from the component that opened it.
        juce::LookAndFeel::setDefaultLookAndFeel (&atomic::ui::lookAndFeel());

        const auto args = juce::StringArray::fromTokens (commandLine, true);

        const auto fileArg = [&args] (const char* flag) -> juce::File
        {
            const int index = args.indexOf (flag);

            if (index < 0 || args.size() <= index + 1)
                return {};

            return juce::File::getCurrentWorkingDirectory().getChildFile (args[index + 1]);
        };

        const int snapshotIndex = args.indexOf ("--snapshot");

        // Phase 19: dev harness — render the hand-drawn tool pointers at 8x with
        // a crosshair on the hotspot. "The tip is off the centreline" is a
        // visual claim, so it gets a visual check.
        if (args.contains ("--cursor-preview"))
        {
            const auto target = fileArg ("--cursor-preview");

            if (target != juce::File())
            {
                constexpr int zoom = 8;
                constexpr int cell = 32 * zoom;
                constexpr int gap  = 16;

                juce::Image sheet (juce::Image::ARGB, cell * 2 + gap * 3, cell + gap * 2, true);

                {
                    juce::Graphics g (sheet);

                    g.setColour (atomic::theme::surface4);
                    g.fillAll();

                    const atomic::PianoRollComponent::Tool which[2]
                        { atomic::PianoRollComponent::Tool::pencil,
                          atomic::PianoRollComponent::Tool::eraser };

                    for (int i = 0; i < 2; ++i)
                    {
                        juce::Point<int> hotspot;
                        const auto art = atomic::PianoRollComponent::renderToolCursorImage (which[i], hotspot);

                        const int x = gap + i * (cell + gap);
                        const int y = gap;

                        g.drawImage (art, x, y, cell, cell, 0, 0, art.getWidth(), art.getHeight());

                        // Where the OS will put the pointer: a cross through the
                        // hot pixel, so "on the barrel's centreline" is a thing
                        // you can see rather than a thing you have to trust.
                        const float hx = (float) (x + hotspot.x * zoom) + (float) zoom * 0.5f;
                        const float hy = (float) (y + hotspot.y * zoom) + (float) zoom * 0.5f;

                        g.setColour (juce::Colours::red.withAlpha (0.85f));
                        g.drawLine (hx - 14.0f, hy, hx + 14.0f, hy, 1.0f);
                        g.drawLine (hx, hy - 14.0f, hx, hy + 14.0f, 1.0f);
                    }
                }

                juce::PNGImageFormat png;

                if (auto stream = target.createOutputStream())
                {
                    const bool ok = png.writeImageToStream (sheet, *stream);

                    std::cout << "cursor sheet " << sheet.getWidth() << "x" << sheet.getHeight()
                              << " px(0,0)=" << juce::String::toHexString ((int) sheet.getPixelAt (1, 1).getARGB())
                              << " written=" << (int) ok << std::endl;
                }
            }

            quit();
            return;
        }

        if (snapshotIndex >= 0 && args.size() > snapshotIndex + 1)
        {
            double bpm = 0.0;
            const int bpmIndex = args.indexOf ("--bpm");

            if (bpmIndex >= 0 && args.size() > bpmIndex + 1)
                bpm = args[bpmIndex + 1].getDoubleValue();

            const bool selfTestPassed
                = renderHeadlessReport (juce::File::getCurrentWorkingDirectory().getChildFile (args[snapshotIndex + 1]),
                                        bpm);

            // The gate: 421 checks only guarantee anything if a failing run can
            // fail the command line. The windowed app never touches this, so a
            // normal launch still exits 0.
            setApplicationReturnValue (selfTestPassed ? 0 : 1);
            quit();
            return;
        }

        //-- Plugin scan child ------------------------------------------------
        // Not a user-facing mode: the parent re-launches this executable to do
        // the scanning somewhere a crashing plugin cannot take the editor with
        // it. Writes one PLUGIN element per line, quitting when done.
        if (const int childIndex = args.indexOf ("--plugin-scan-child"); childIndex >= 0)
        {
            const auto outFile = childIndex + 1 < args.size()
                                   ? juce::File (args[childIndex + 1].unquoted())
                                   : juce::File();

            juce::FileSearchPath paths;

            for (int i = childIndex + 2; i < args.size(); ++i)
                paths.add (juce::File (args[i].unquoted()), 0);

            if (outFile != juce::File())
                atomic::PluginHost::scanPathsToFile (paths, outFile);

            quit();
            return;
        }

        // Plugin scan report: what the host actually finds on this machine.
        // Diagnoses "my VST3 is installed but not in the list" without opening
        // the window.
        if (args.contains ("--pluginscan"))
        {
            atomic::MainComponent component;
            addDevPluginPaths (component);

            const auto start = juce::Time::getMillisecondCounterHiRes();
            const int  count = component.getPluginHost().scan();
            const auto elapsed = juce::Time::getMillisecondCounterHiRes() - start;

            std::cout << "scanned " << count << " plugin(s) in "
                      << juce::String (elapsed / 1000.0, 2) << " s  -  "
                      << component.getPluginHost().countInstruments() << " instruments / "
                      << component.getPluginHost().countEffects() << " effects" << std::endl;

            for (const auto& entry : component.getPluginHost().getLibrary())
                std::cout << "  [" << (entry.isInstrument ? "INST" : "FX  ") << "] "
                          << entry.getDisplayName() << std::endl;

            quit();
            return;
        }

        //-- Project save -----------------------------------------------------
        // The same call the Save button makes; the button only adds a dialog.
        if (const auto file = fileArg ("--save"); file != juce::File())
        {
            atomic::MainComponent component;
            juce::String error;

            const bool ok = component.saveProjectToFile (file, error);

            std::cout << (ok ? "saved " : "FAILED to save ") << file.getFullPathName()
                      << (ok ? "" : "  -  " + error) << std::endl;

            quit();
            return;
        }

        //-- Project load -----------------------------------------------------
        if (const auto file = fileArg ("--load"); file != juce::File())
        {
            atomic::MainComponent component;
            juce::String error;

            if (component.loadProjectFromFile (file, error))
                std::cout << "loaded " << file.getFullPathName()
                          << "  -  " << component.getLayerStack().getNumLayers() << " layers, "
                          << component.getRack().getNumSlots() << " synths, "
                          << juce::String (component.getBpm(), 1) << " BPM, "
                          << juce::String (component.getContentSeconds(), 2) << " s of music"
                          << std::endl;
            else
                std::cout << "FAILED to load " << file.getFullPathName() << "  -  " << error << std::endl;

            quit();
            return;
        }

        //-- Offline bounce ---------------------------------------------------
        // `--export out.wav --project song.atomic` bounces a saved song; with no
        // --project it bounces the built-in demo. Same code as the button.
        const auto exportFile = fileArg ("--export") != juce::File() ? fileArg ("--export")
                                                                     : fileArg ("--render");

        if (exportFile != juce::File())
        {
            std::unique_ptr<atomic::MainComponent> component = std::make_unique<atomic::MainComponent>();
            addDevPluginPaths (*component);

            juce::String loadError;

            if (const auto projectFile = fileArg ("--project"); projectFile != juce::File())
            {
                if (! component->loadProjectFromFile (projectFile, loadError))
                {
                    std::cout << "FAILED to load " << projectFile.getFullPathName()
                              << "  -  " << loadError << std::endl;
                    quit();
                    return;
                }
            }
            else
            {
                // No --project: bounce the demo, as documented above. Since
                // Phase 10 the app opens blank, so without this "no file" would
                // export half a second of silence instead of the fixture.
                component->loadDemoProject();
            }

            exportFile.getParentDirectory().createDirectory();

            const auto result = component->exportAudioToFile (exportFile);

            if (result.ok)
                std::cout << "wrote " << exportFile.getFullPathName()
                          << "  (" << juce::String (result.seconds, 2) << " s, "
                          << juce::String (result.numSamples) << " samples, 16-bit stereo 44.1 kHz, peak "
                          << juce::String (juce::Decibels::gainToDecibels (result.peak, -100.0f), 1)
                          << " dBFS)" << std::endl;
            else
                std::cout << "FAILED to export " << exportFile.getFullPathName()
                          << "  -  " << result.message << std::endl;

            quit();
            return;
        }

        // Device smoke test: report whether this machine can actually open one.
        if (args.contains ("--audiotest"))
        {
            atomic::MainComponent component;
            const bool ok = component.startAudio();
            std::cout << "audio device: " << (ok ? "OPEN" : "UNAVAILABLE")
                      << "  -  " << component.getAudioStatus() << std::endl;
            quit();
            return;
        }

        // Real-time transport check: run the actual device callback for one
        // second and see where the playhead got to. The offline self-test
        // proves the maths; this proves the callback is wired up.
        if (args.contains ("--transporttest"))
        {
            atomic::MainComponent component;
            component.startAudio();
            component.setBpm (120.0);
            component.playTransport();

            const double start = component.getTransportBeats();

            juce::Thread::sleep (1000);

            const double end = component.getTransportBeats();
            component.stopTransport();

            std::cout << "transport moved " << juce::String (end - start, 3)
                      << " beats in 1 s at 120 BPM  (expected about 2)"
                      << "  -  " << component.getAudioStatus() << std::endl;
            quit();
            return;
        }

        // Window contract: "opens maximised, keeps the title bar". A screenshot
        // cannot answer that — it shows the content, not the state the window
        // manager thinks the window is in — so the window is asked directly.
        // Run by hand after touching MainWindow.
        if (args.contains ("--windowtest"))
        {
            mainWindow = std::make_unique<MainWindow> (getApplicationName());

            auto* window = mainWindow.get();
            auto* peer   = window->getPeer();

            const auto content = window->getScreenBounds();
            const auto frame   = peer != nullptr ? peer->getFrameSize()
                                                 : juce::BorderSize<int>();

            auto userArea = juce::Rectangle<int> {};
            juce::String displayName;

            if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
            {
                userArea    = display->userArea;
                displayName = juce::String (display->scale) + "x";
            }

            const auto near = [] (int a, int b, int tolerance)
            {
                return std::abs (a - b) <= tolerance;
            };

            // A maximised Windows window sticks its (invisible) resize border a
            // few pixels past the work area, and the border itself comes back in
            // physical pixels while the bounds are logical. Both make an exact
            // comparison lie; 16 px of slack does not hide a real failure — a
            // window that is not maximised is off by hundreds.
            const auto expected = userArea.withTrimmedTop (frame.getTop());

            const bool maximised   = window->isFullScreen();
            const bool hasTitleBar = frame.getTop() >= 20;
            const bool fillsWorkArea = near (content.getWidth(),  expected.getWidth(),  16)
                                        && near (content.getHeight(), expected.getHeight(), 16)
                                        && near (content.getX(),      expected.getX(),      16)
                                        && near (content.getY(),      expected.getY(),      16);

            std::cout << "maximised=" << (int) maximised
                      << "  titleBar=" << frame.getTop()
                      << "  border=" << frame.getLeft() << "/" << frame.getRight()
                                      << "/" << frame.getBottom()
                      << "  content=" << content.toString()
                      << "  userArea=" << userArea.toString()
                      << "  scale=" << displayName
                      << "  ->  " << ((maximised && hasTitleBar && fillsWorkArea) ? "OK" : "MISMATCH")
                      << std::endl;

            // And un-maximising has to come back to a usable window, not to the
            // corner it was created in: that is what proves the frame is a normal
            // frame and not a kiosk one.
            window->setFullScreen (false);

            std::cout << "restored to " << window->getScreenBounds().toString()
                      << "  (asked for " << atomic::ui::px (1440) << " x " << atomic::ui::px (820) << ")"
                      << std::endl;

            quit();
            return;
        }

        mainWindow = std::make_unique<MainWindow> (getApplicationName());
    }

    void shutdown() override
    {
        mainWindow.reset();
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    void anotherInstanceStarted (const juce::String&) override {}

    //==========================================================================
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name,
                              atomic::theme::bgApp,
                              DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new atomic::MainComponent(), true);

            // Only the windowed app touches the sound card; --snapshot must stay
            // headless and deterministic.
            if (auto* content = dynamic_cast<atomic::MainComponent*> (getContentComponent()))
            {
                addDevPluginPaths (*content);
                content->startAudio();

                // Phase 84: recovery is enabled HERE and nowhere else. Every
                // other MainComponent in this file belongs to a headless run,
                // and those save a project before doing anything else — with the
                // feature on by default, a snapshot pass would delete the rescue
                // copy of whatever the human had open.
                content->setRecoveryEnabled (true);

                // Phase 86: same rule, same reason. Opening a project that has
                // lost its audio asks a question; a headless run that asks it
                // does not finish.
                content->setSampleRelinkPromptEnabled (true);
            }
            setResizable (true, false);

            // Phase 13: the minimum is stated in the same design units as the
            // layout itself. At the original 1024 the top bar's edit cluster
            // now runs into the master meter, so the floor has to move with it.
            setResizeLimits (atomic::ui::px (1024), atomic::ui::px (620), 12000, 12000);

            // Opens maximised, and "maximised" means the state the window
            // manager itself is in. Phase 40 replaced a hand-rolled attempt:
            // Phase 18 called setBounds (userArea trimmed by the frame), which
            // is only right on a 100 % display — userArea is in logical units
            // while the frame comes back in physical ones, so at 150 % the
            // window came up a title bar short of the work area and the restore
            // button did not light. The peer's own maximise has no such
            // arithmetic in it, keeps the title bar (it is ShowWindow
            // (SW_SHOWMAXIMIZED), not a borderless fullscreen), and still
            // restores to the size set below.
            centreWithSize (atomic::ui::px (1440), atomic::ui::px (820));

            setVisible (true);
            setFullScreen (true);

            // Phase 84: after the window exists, not during construction. The
            // offer is a modal child of the shell, and a modal child of a window
            // that is not on screen yet has nowhere to appear.
            juce::Component::SafePointer<MainWindow> safe (this);

            juce::MessageManager::callAsync ([safe]
            {
                if (safe == nullptr)
                    return;

                if (auto* content = dynamic_cast<atomic::MainComponent*> (safe->getContentComponent()))
                    content->offerRecoveryIfAny();
            });
        }

        void closeButtonPressed() override
        {
            // Phase 10: closing a session with edits asks first. handleCloseRequest
            // returns false when it has put that question on screen — quitting
            // then is its callback's job, not ours.
            if (auto* content = dynamic_cast<atomic::MainComponent*> (getContentComponent()))
                if (! content->handleCloseRequest())
                    return;

            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

private:
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (AtomicDAWApplication)
