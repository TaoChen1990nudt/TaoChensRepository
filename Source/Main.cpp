#include <JuceHeader.h>

#include <iostream>

#include "MainComponent.h"
#include "Util/Metrics.h"
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
    void renderHeadlessReport (const juce::File& pngFile, double bpm = 0.0)
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

        pngFile.getParentDirectory().createDirectory();

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

            if (auto stream = pngFile.getSiblingFile (pngFile.getFileNameWithoutExtension() + suffix + ".png")
                                   .createOutputStream())
            {
                juce::PNGImageFormat png;
                png.writeImageToStream (image, *stream);
            }
        };

        writePng ("-mixer", component.createMixerSnapshot());

        // The audio page lists device, sample rate and buffer size — none of
        // which exist until something is open. Opening a device here only fills
        // that page in; the main window's pixels are already on disk.
        component.startAudio();

        writePng ("-settings-audio", component.createSettingsSnapshot (0));
        writePng ("-settings-midi", component.createSettingsSnapshot (1));
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

                    g.setColour (juce::Colour (0xff2a2d35));
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

            renderHeadlessReport (juce::File::getCurrentWorkingDirectory().getChildFile (args[snapshotIndex + 1]),
                                  bpm);
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
            component.pauseTransport();

            std::cout << "transport moved " << juce::String (end - start, 3)
                      << " beats in 1 s at 120 BPM  (expected about 2)"
                      << "  -  " << component.getAudioStatus() << std::endl;
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
                              juce::Colour (0xff0d0e11),
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
            }
            setResizable (true, false);

            // Phase 13: the minimum is stated in the same design units as the
            // layout itself. At the original 1024 the top bar's edit cluster
            // now runs into the master meter, so the floor has to move with it.
            setResizeLimits (atomic::ui::px (1024), atomic::ui::px (620), 12000, 12000);

            // Opens maximised: three panes side by side plus the automation
            // window is what the shell is for, and a 1440 window no longer
            // holds them at the larger UI scale.
            //
            // Phase 18: setBounds() on a top-level Component takes the CONTENT
            // size and lets the peer grow the frame AROUND it, so
            // setBounds (userArea) pushed the title bar off the top of the
            // screen — a borderless fullscreen, which is not what the user
            // asked for. Subtract the frame first: the window+titlebar then
            // sits inside the work area, which is exactly "maximised".
            setVisible (true);

            if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
            {
                auto area = display->userArea;

                if (auto* peer = getPeer())
                {
                    const auto frame = peer->getFrameSize();

                    area = area.withTrimmedTop (frame.getTop())
                               .withTrimmedLeft (frame.getLeft())
                               .withTrimmedRight (frame.getRight())
                               .withTrimmedBottom (frame.getBottom());
                }

                setBounds (area);
            }
            else
            {
                centreWithSize (atomic::ui::px (1440), atomic::ui::px (820));
            }
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
