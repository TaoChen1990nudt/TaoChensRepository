//==============================================================================
//  SelfTest.cpp — MainComponent::runSelfTest lives here, not in MainComponent.cpp.
//
//  The check edits the live session (it scrolls the roll, sends synths around,
//  adds layers, mounts samplers), so it is a member of MainComponent and needs
//  nothing but MainComponent.h. Moving it out of MainComponent.cpp drops that
//  file from ~13,900 lines to ~6,200: the product code and the check code stop
//  sharing one compilation unit and one 7,700-line function frame.
//==============================================================================

#include "MainComponent.h"
#include "Util/SharedRules.h"

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

        // ATOMIC_TRACE=1 echoes every line as it happens. The whole report is
        // written to the log at the very END of the run, so a check that takes
        // the harness down with it would otherwise leave nothing behind —
        // "which check was it" is worth an env var.
        static const bool echo = juce::SystemStats::getEnvironmentVariable ("ATOMIC_TRACE", "") == "1";

        if (echo)
            std::cout << (passed ? "[PASS] " : "[FAIL] ") << name << std::endl;
    };

    //-- Phase 34: the cascade deletions ask, and the answers are scripted -----
    // Two things about the question are swapped out for the whole run:
    //   * the dialogs — a headless run cannot click "删除", so the answer is a
    //     variable the checks can set, and the call is counted so "did it ask?"
    //     is testable at all;
    //   * the deferral — the real app asks from the message queue (the sweep is
    //     often inside a drag), and there is no loop to pump in here.
    // Both are restored at the very end, after the demo has been rebuilt.
    int  samplerPromptCount  = 0;
    bool samplerPromptAnswer = true;

    deferSamplerPrompt = false;
    askSamplerCascade = [&samplerPromptCount, &samplerPromptAnswer]
                        (const juce::String&, const juce::String&,
                         std::function<void (bool)> onAnswer)
    {
        ++samplerPromptCount;

        if (onAnswer != nullptr)
            onAnswer (samplerPromptAnswer);
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

        //-- Play / stop -------------------------------------------------------
        // play(false): the state machine without taking the sound card.
        AudioEngine transport (t4, r4);

        transport.play (false);
        check ("Play starts the transport",
               transport.isPlaying() && transport.isTransportEngaged(), "playing");

        transport.setPositionBeats (8.0);
        transport.stopTransport();
        check ("The engine's own stop rewinds to the top",
               transport.getTransportState() == TransportState::stopped
                 && transport.getPositionBeats() == 0.0,
               "stopped at beat 0");

        // Phase 33: the STOP button, as one pure function. Running -> stop and
        // rewind to zero. Idle -> nothing at all, because the playhead is also
        // the "play from here" control and a reflex stop must not rewind it.
        const auto whileRunning = stopDecision (true);
        const auto whileIdle    = stopDecision (false);

        check ("Stop while running ends the run at zero",
               whileRunning.stop && whileRunning.beat == 0.0,
               juce::String (whileRunning.beat, 1));

        check ("Stop while already stopped does nothing (the playhead is a choice)",
               ! whileIdle.stop,
               whileIdle.stop ? "it rewound" : "left alone");

        // Phase 33: the space bar. Idle -> play, rolling -> stop. Phase 32 had
        // it stop-only, which is why the keyboard could not start playback.
        check ("Space plays when idle and stops while rolling",
               spaceStartsPlayback (false) && ! spaceStartsPlayback (true),
               "idle=play / rolling=stop");
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
                 && std::abs (mxb.getInsert (2, 3).params[(size_t) 1] - 0.6f) < 1.0e-4f
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
            const int hostedNoteId = layers.getLayer (0)->addNote ({ 0, 60, 0.0, 2.0, 0.9f });

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

            //-- Phase 24: a curve really moves a hosted parameter ---------------
            // End to end, through the same three pieces the editor uses: the
            // plugin's own parameter list, the note's `modPluginParam`, and the
            // published curve. The fixture synth's one parameter is the
            // amplitude, so "did the automation happen" is audible — half the
            // note is rendered with the curve pulled to zero.
            auto* instance = host.getProcessorForEditor (instanceId);
            const int paramCount = instance != nullptr ? instance->getParameters().size() : 0;
            const juce::String paramName = paramCount > 0 ? instance->getParameters()[0]->getName (64)
                                                          : juce::String();

            auto* hostedNote = layers.getLayer (0)->findNote (hostedNoteId);
            float openHalf = 0.0f, closedHalf = 0.0f, dipRatio = 1.0f;

            if (hostedNote != nullptr)
            {
                NoteEnvelope dip;
                // Down to zero a third of the way in, so the drop is over well
                // before the half-way mark: the parameter is written once per
                // render chunk, and a dip that straddled the boundary would
                // measure the chunk size instead of the automation.
                dip.loadPoints ({ { 0.0f, 1.0f }, { 0.25f, 1.0f }, { 0.35f, 0.0f }, { 1.0f, 0.0f } });
                dip.setEnabled (true);

                hostedNote->envelope       = dip;
                hostedNote->modPluginParam = 0;

                juce::AudioBuffer<float> autoBuffer;
                autoBuffer.setSize (2, 44100);
                autoBuffer.clear();

                engine.publishSchedule (layers, hostRack, 120.0, 0.0, &mix);
                engine.resetMixState();
                engine.renderWindow (0.0, 44100, autoBuffer, nullptr);

                openHalf  = autoBuffer.getRMSLevel (0, 0, 22050);
                closedHalf = autoBuffer.getRMSLevel (0, 22050, 22050);
                dipRatio  = openHalf > 1.0e-4f ? closedHalf / openHalf : 1.0f;

                hostedNote->modPluginParam = -1;
                hostedNote->envelope = NoteEnvelope::makeFlat();
            }

            check ("A note's curve moves a hosted instrument's parameter",
                   hostedNote != nullptr && paramCount >= 1 && paramName == "Level"
                     && openHalf > 0.002f && dipRatio < 0.15f,
                   juce::String (paramCount) + " param(s), [0] = " + paramName
                     + " / first half " + juce::String (openHalf, 4)
                     + " -> second " + juce::String (closedHalf, 4)
                     + "  (x" + juce::String (dipRatio, 3) + ")");

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

                focusSynth (-1);          // the state a fresh session is in
                pianoRoll.setSelection ({ noteId });

                // Phase 30: the selection no longer spotlights anything. It used
                // to, and that is how the grid lit up (and, while the veil still
                // existed, dimmed) on a click nobody had asked to be a mode. The
                // rack still has to hear that something IS selected — that is
                // what arms the card's plain click — so both halves of the old
                // rule are checked, one inverted.
                check ("Selecting notes does not switch the synth highlight on",
                       ! rackPanel.hasRoutingFocus()
                         && pianoRoll.getFocusSlotId() < 0
                         && pianoRoll.getSelectedRoutingSlotId() == layerSlot,
                       "focus " + juce::String (rackPanel.getRoutingFocusSlotId())
                         + " / resolves to " + juce::String (pianoRoll.getSelectedRoutingSlotId()));

                // Phase 23: the same selection arms the card's plain click. The
                // two facts are separate on purpose — a spotlit synth is not a
                // reason to re-route anything.
                const bool armed = rackPanel.isSelectionActive();

                pianoRoll.setSelection ({});
                const bool disarmed = ! rackPanel.isSelectionActive();

                pianoRoll.setSelection ({ noteId });

                check ("A live selection arms the rack's click-to-mount",
                       armed && disarmed && rackPanel.isSelectionActive(),
                       armed ? "armed with a selection, disarmed without" : "stayed cold");

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
    // triangle's fixed 12 px insets ate all but 4 px of its height. The glyph is
    // plain geometry, so it is testable without a display — and the next resize
    // cannot quietly undo the fix. Phase 32 removed the pause plate from this
    // set; what is left is measured here, plus the gap it left behind.
    {
        const auto playButton = getPlayButtonBounds().toFloat();
        const auto stopButton = getStopButtonBounds().toFloat();

        const auto play = playGlyph (playButton).getBounds();
        const auto stop = stopGlyph (stopButton);

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

        check ("The stop square is centred and square",
               std::abs (stop.getWidth() - stop.getHeight()) < 0.01f
                 && std::abs (stop.getCentreX() - stopButton.getCentreX()) < tol
                 && std::abs (stop.getCentreY() - stopButton.getCentreY()) < tol,
               juce::String (stop.getWidth(), 1) + " px");

        // Phase 32: two plates, 4 px apart, no orphan gap where the pause plate
        // used to be. A hole in a transport cluster reads as a missing button.
        const float gap = stopButton.getX() - playButton.getRight();

        check ("Play and stop sit side by side with no hole between them",
               gap > 0.0f && gap <= ui::pf (6)
                 && std::abs (stopButton.getWidth() - (float) ui::px (30)) < 0.01f
                 && playButton.getRight() > (float) ui::px (200),
               juce::String (gap, 1) + utf8 (" px 间隙, stop x=")
                 + juce::String (stopButton.getX(), 1));
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
        // Phase 25: the ceiling is 127 strips and a fresh session opens with
        // eight, so those are two different numbers now. The old check asserted
        // they were the same, which read as "a 9th strip is refused".
        const int countBefore = mixer.getChannelCount();
        const int ceiling     = MixerModel::maxChannels;

        while (mixer.getChannelCount() < ceiling)
            mixer.addChannel();

        const bool refusedAboveCeiling = ! mixer.addChannel();
        mixer.removeChannel();
        const int shrunk = mixer.getChannelCount();
        mixer.addChannel();

        check ("The console refuses a 128th strip and accepts 1..127",
               countBefore == MixerModel::defaultChannels
                 && ceiling == 127
                 && refusedAboveCeiling
                 && shrunk == ceiling - 1
                 && mixer.getChannelCount() == ceiling,
               juce::String (countBefore) + " / " + juce::String (shrunk) + " / "
                 + juce::String (mixer.getChannelCount()));

        // Back to the eight the rest of the frame photographs.
        mixer.setChannelCount (countBefore);

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

            // Phase 21: 编组 / 解组 are one pair, so the magnet had to move past
            // them. The order is the whole request, so the order is the check.
            const auto ungroup = roll.getToolButtonBounds (6);
            const auto magnet  = roll.getToolButtonBounds (7);

            check ("Ungroup sits directly left of the magnet now",
                   ungroup.getRight() <= magnet.getX()
                     && magnet.getRight() <= roll.getToolbarArea().getRight(),
                   juce::String (ungroup.getX()) + ".." + juce::String (magnet.getRight())
                     + " / 栏宽 " + juce::String (roll.getToolbarArea().getWidth()));
        }

        //-- Phase 21: both side columns split ---------------------------------
        {
            resized();

            const bool leftStacked  = leftSplitter.getY() <= layerPanel.getBottom()
                                        && filePanel.getY() == leftSplitter.getBottom();
            const bool rightStacked = rightSplitter.getY() <= rackPanel.getBottom()
                                        && groupPanel.getY() == rightSplitter.getBottom();

            const int fileBefore  = filePanel.getHeight();
            const int groupBefore = groupPanel.getHeight();

            leftSplitter.simulateDrag (-ui::px (40));      // drag up = taller list
            rightSplitter.simulateDrag (-ui::px (30));

            const int fileAfter  = filePanel.getHeight();
            const int groupAfter = groupPanel.getHeight();

            check ("Dragging the left band makes the file tree taller",
                   leftStacked && fileAfter > fileBefore && layerPanel.getHeight() > ui::px (100),
                   juce::String (fileBefore) + " -> " + juce::String (fileAfter) + " px");

            check ("Dragging the right band makes the group list taller",
                   rightStacked && groupAfter > groupBefore && rackPanel.getHeight() > ui::px (100),
                   juce::String (groupBefore) + " -> " + juce::String (groupAfter) + " px");

            // Phase 25: the band must keep up with the pointer. 40 px of travel
            // has to be 40 px of height, and the check below is what the old
            // incremental callback could not pass: it clamped the overshoot into
            // oblivion, so pulling back down moved the band only after the
            // pointer had retraced the whole lost distance.
            check ("The left band tracks the pointer one for one",
                   fileAfter - fileBefore == ui::px (40),
                   juce::String (fileAfter - fileBefore) + " px for 40 px of travel");

            // One continuous gesture: 40 px up, far past the floor, then back to
            // 40 px of travel. Absolute travel has to land on exactly the height
            // that 40 px means — no dead zone from the overshoot.
            leftSplitter.simulateDrag (ui::px (10000), true);      // still the same drag
            const int atFloor = filePanel.getHeight();
            leftSplitter.simulateDrag (-ui::px (40), true);

            check ("A band dragged past its limit comes back with the pointer",
                   atFloor < fileAfter && filePanel.getHeight() == fileAfter,
                   juce::String (atFloor) + " px at the limit -> "
                     + juce::String (filePanel.getHeight()) + utf8 (" px at 40 px of travel"));

            // Put the columns back the way the rest of the frame expects them.
            leftSplitter.simulateDrag (ui::px (40));
            rightSplitter.simulateDrag (ui::px (30));
        }

        //-- Phase 21: a new group is a row in the list straight away ----------
        {
            if (auto* sheet = stack.getActiveLayer())
            {
                std::vector<int> detached;

                for (const auto& note : sheet->getNotes())
                    if (note.groupId < 0)
                        detached.push_back (note.id);

                if (detached.size() >= 2)
                {
                    const int before = groupPanel.getNumRows();

                    pianoRoll.setSelection ({ detached[0], detached[1] });
                    const int gid   = pianoRoll.groupSelection();
                    const int after = groupPanel.getNumRows();

                    check ("A group made in the roll is a row in the list right away",
                           gid >= 0 && after == before + 1,
                           juce::String (before) + " -> " + juce::String (after) + utf8 (" 行"));

                    pianoRoll.ungroupSelection();
                    pianoRoll.setSelection ({});
                }
            }
        }

        //-- Phase 21: the group's inline curve box ----------------------------
        {
            LayerStack t;
            RackModel  r;

            t.addLayer (utf8 ("组"), palette::layer1);

            auto* layer = t.getLayer (0);

            // Around middle C: a fresh roll centres its vertical scroll there,
            // and the box is anchored to notes that are actually on screen.
            const int a   = layer->addNote ({ 0, 72, 1.0, 2.0, 0.9f });
            const int b   = layer->addNote ({ 0, 76, 4.0, 2.0, 0.9f });
            const int gid = layer->addGroup (utf8 ("G"), palette::layer1);
            layer->assignNotesToGroup ({ a, b }, gid);

            PianoRollComponent roll (t, r);
            roll.setSize (1400, 520);

            const auto handle = roll.getGroupAutomationHandleBounds (0, gid);

            roll.openGroupAutomation (0, gid, 0);

            const auto* group  = layer->findGroup (gid);
            const auto  box    = roll.getGroupAutomationBoxBounds (0, gid);
            const bool  laneOn = group != nullptr
                                   && group->automationFor (GroupAutomationTarget::level).isEnabled();

            // Members span beats 1..6, so the box must be exactly that wide.
            const float expected = (float) ((6.0 - 1.0) * roll.getBeatWidth());

            check ("A group's curve box is as wide as the group",
                   ! box.isEmpty()
                     && std::abs (box.getWidth() - juce::jmax (expected, 22.0f)) < 1.5f,
                   juce::String (box.getWidth(), 1) + " px vs " + juce::String (expected, 1)
                     + " px" + (handle.isEmpty() ? utf8 (" · 无手柄") : utf8 (" · 有手柄")));

            check ("The pill's curve handle exists and sits inside the pill",
                   ! handle.isEmpty(),
                   handle.toString() + " | 成员 " + juce::String (layer->countNotesInGroup (gid)));

            check ("Opening the group box switches its lane on",
                   roll.isGroupAutomationOpen() && roll.getExpandedGroupId() == gid && laneOn,
                   laneOn ? "lane on" : "lane still off");

            roll.openNoteAutomation (0, a);

            check ("Only one inline box is open at a time",
                   roll.isNoteAutomationOpen() && ! roll.isGroupAutomationOpen(),
                   roll.isNoteAutomationOpen() ? "note box won" : "neither box opened");
        }

        //-- Phase 22: a multi-note drag is rigid ------------------------------
        {
            LayerStack t;
            RackModel  r;

            t.addLayer (utf8 ("拖动"), palette::layer1);

            auto* layer = t.getLayer (0);

            layer->addNote ({ 0, 60, 1.0, 1.0, 0.9f });
            layer->addNote ({ 0, 64, 2.0, 1.0, 0.9f });
            layer->addNote ({ 0, 67, 4.0, 1.0, 0.9f });

            std::vector<NoteEvent> originals (layer->getNotes().begin(), layer->getNotes().end());

            PianoRollComponent roll (t, r);
            roll.setSize (1400, 520);

            // Everybody wants to go 3 beats left; there is 1 beat of travel
            // before the leftmost note reaches beat 0.
            const auto farLeft = roll.clampMove (originals, -3.0, 0, false);

            // ... and a drag off the top must stop the block, not stack it on 127.
            const auto up = roll.clampMove (originals, 0.0, 200, false);

            check ("The leftmost note pins the whole block at beat 0",
                   std::abs (farLeft.beats + 1.0) < 1.0e-9,
                   "delta " + juce::String (farLeft.beats, 3) + " beats");

            check ("A block dragged past the top keeps its intervals",
                   up.pitch == 60 && (67 + up.pitch) == 127,
                   "pitch delta " + juce::String (up.pitch));
        }

        //-- Phase 22: the group box and its curve share both edges ------------
        {
            LayerStack t;
            RackModel  r;

            t.addLayer (utf8 ("对齐"), palette::layer1);

            auto* layer = t.getLayer (0);

            const int a   = layer->addNote ({ 0, 72, 1.0, 1.0, 0.9f });
            const int b   = layer->addNote ({ 0, 76, 5.0, 1.0, 0.9f });
            const int gid = layer->addGroup (utf8 ("G"), palette::layer1);
            layer->assignNotesToGroup ({ a, b }, gid);

            PianoRollComponent roll (t, r);
            roll.setSize (1400, 520);
            roll.openGroupAutomation (0, gid, 0);

            const auto box    = roll.getGroupAutomationBoxBounds (0, gid);
            const auto curve  = roll.getGroupCurveBounds();
            const auto first  = roll.getNoteScreenBounds (0, a);
            const auto last   = roll.getNoteScreenBounds (0, b);

            // The note block is drawn 1.5 px narrower than its beat span (the
            // gap between neighbours), so the box's right edge sits inside that
            // gap — the tolerance says so instead of pretending it is exact.
            check ("The group box spans exactly the group's first and last note",
                   ! box.isEmpty() && std::abs (box.getX() - first.getX()) < 1.0f
                     && box.getRight() >= last.getRight()
                     && box.getRight() - last.getRight() < 2.0f,
                   box.toString() + " vs note " + first.toString() + " .. "
                     + juce::String (last.getRight(), 1));

            check ("The curve fills the box edge to edge",
                   ! curve.isEmpty() && std::abs (curve.getX() - box.getX()) < 0.01f
                     && std::abs (curve.getRight() - box.getRight()) < 0.01f,
                   "curve " + juce::String (curve.getX(), 1) + ".." + juce::String (curve.getRight(), 1)
                     + " / box " + juce::String (box.getX(), 1) + ".." + juce::String (box.getRight(), 1));
        }

        //-- Phase 22: copying a curve from one box to another -----------------
        {
            LayerStack t;
            RackModel  r;

            t.addLayer (utf8 ("曲线"), palette::layer1);

            auto* layer = t.getLayer (0);

            // Three groups: the source (5 beats), a shorter one (3) and a longer
            // one (9). Stretch, truncation and the zero tail all get a target.
            const int n1 = layer->addNote ({ 0, 72,  1.0, 1.0, 0.9f });
            const int n2 = layer->addNote ({ 0, 76,  5.0, 1.0, 0.9f });
            const int n3 = layer->addNote ({ 0, 79, 10.0, 1.0, 0.9f });
            const int n4 = layer->addNote ({ 0, 81, 12.0, 1.0, 0.9f });
            const int n5 = layer->addNote ({ 0, 84, 20.0, 1.0, 0.9f });
            const int n6 = layer->addNote ({ 0, 86, 28.0, 1.0, 0.9f });

            const int g1 = layer->addGroup (utf8 ("源"), palette::layer1);
            const int g2 = layer->addGroup (utf8 ("短"), palette::layer1);
            const int g3 = layer->addGroup (utf8 ("长"), palette::layer1);

            layer->assignNotesToGroup ({ n1, n2 }, g1);
            layer->assignNotesToGroup ({ n3, n4 }, g2);
            layer->assignNotesToGroup ({ n5, n6 }, g3);

            PianoRollComponent roll (t, r);
            roll.setSize (1400, 520);

            auto* source = layer->findGroup (g1);
            source->automationFor (GroupAutomationTarget::level)
                  .loadPoints ({ { 1.0, 0.2f }, { 3.0, 0.9f }, { 6.0, 0.4f } });
            source->automationFor (GroupAutomationTarget::level).setEnabled (true);

            roll.openGroupAutomation (0, g1, 0);
            const bool copied = roll.copyAutomationCurve();

            //-- stretch into the 3-beat group
            roll.openGroupAutomation (0, g2, 0);
            const bool stretched = roll.pasteAutomationCurve (true);
            const float stretchedEnd = layer->findGroup (g2)
                                         ->automationFor (GroupAutomationTarget::level)
                                         .valueAtBeat (13.0);

            check ("A copied curve knows its own length",
                   copied && roll.hasCurveClipboard()
                     && std::abs (roll.getCurveClipboardLengthBeats() - 5.0) < 1.0e-6
                     && roll.getCurveClipboardPointCount() == 3,
                   juce::String (roll.getCurveClipboardPointCount()) + " points over "
                     + juce::String (roll.getCurveClipboardLengthBeats(), 2) + " beats");

            check ("Pasting stretched rescales the curve onto the target",
                   stretched && std::abs ((double) stretchedEnd - 0.4) < 0.02,
                   "end value " + juce::String (stretchedEnd, 3));

            //-- no stretch into the same short group: the tail is cut off
            roll.pasteAutomationCurve (false);
            const float cutEnd = layer->findGroup (g2)
                                   ->automationFor (GroupAutomationTarget::level)
                                   .valueAtBeat (13.0);

            check ("Pasting unstretched onto a shorter group truncates the curve",
                   std::abs ((double) cutEnd - 0.9) < 0.02,
                   "value at the group's end " + juce::String (cutEnd, 3));

            //-- no stretch into the 9-beat group: the rest is 0
            roll.openGroupAutomation (0, g3, 0);
            roll.pasteAutomationCurve (false);

            const auto& longLane = layer->findGroup (g3)->automationFor (GroupAutomationTarget::level);

            check ("Pasting unstretched onto a longer group leaves a zero tail",
                   std::abs ((double) longLane.valueAtBeat (25.0) - 0.4) < 0.02
                     && longLane.valueAtBeat (29.0) < 1.0e-6f,
                   "at beat 25 " + juce::String (longLane.valueAtBeat (25.0), 3)
                     + " / at 29 " + juce::String (longLane.valueAtBeat (29.0), 3));

            //-- a note's curve onto a group: one representation, two boxes
            roll.openNoteAutomation (0, n6);
            const bool noteCopied = roll.copyAutomationCurve();

            roll.openGroupAutomation (0, g2, 0);
            const bool crossed = roll.pasteAutomationCurve (true);

            const float notePeak = layer->findGroup (g2)
                                     ->automationFor (GroupAutomationTarget::level)
                                     .valueAtBeat (10.24);

            check ("A note's envelope can be pasted onto a group's lane",
                   noteCopied && crossed && notePeak > 0.95f,
                   "peak where the ADSR peaked: " + juce::String (notePeak, 3));
        }

        //-- Phase 22: the piano column plays the layer's synth ----------------
        {
            LayerStack t;
            RackModel  r;

            const int slot = r.getFirstSlotId();

            t.addLayer (utf8 ("键"), palette::layer1);
            t.setLayerOutputSlot (0, slot);

            PianoRollComponent roll (t, r);
            roll.setSize (1400, 520);

            int   gotSlot  = -1;
            int   gotPitch = -1;
            float gotVel   = 0.0f;

            roll.onAuditionRequested = [&] (int s, int p, float v, bool)
            {
                gotSlot  = s;
                gotPitch = p;
                gotVel   = v;
            };

            roll.auditionPitchOnActiveLayer (72);

            check ("A piano key sounds the synth its layer is routed to",
                   gotSlot == slot && gotPitch == 72 && gotVel > 0.0f,
                   "slot " + juce::String (gotSlot) + " / pitch " + juce::String (gotPitch)
                     + " / vel " + juce::String (gotVel, 2));
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

    //-- Phase 23: strip-to-strip routing --------------------------------------
    {
        MixerModel m;

        bool allToMaster = true;

        for (int c = 0; c < m.getChannelCount(); ++c)
            allToMaster = allToMaster && m.getChannelOutput (c) == MixerModel::masterChannel;

        check ("Every strip starts plugged into the master",
               allToMaster && MixerModel::masterChannel == MixerModel::numChannels
                 && m.getChannelCount() == MixerModel::defaultChannels,
               juce::String (m.getChannelCount()) + " strips -> master");

        const bool chained  = m.setChannelOutput (0, 1) && m.setChannelOutput (1, 2);
        const bool noLoops  = ! m.setChannelOutput (2, 0)      // 0 -> 1 -> 2 -> 0
                                && ! m.setChannelOutput (1, 1); // a strip into itself

        check ("A strip can feed another strip, but a loop is refused",
               chained && noLoops && m.getChannelOutput (0) == 1 && m.getChannelOutput (1) == 2,
               "0->1->2, 2->0 rejected");

        // Shrinking the console must not leave a route pointing at a strip that
        // no longer exists: the signal would land in a bus nobody reads.
        m.setChannelCount (2);

        check ("Shrinking the console re-plugs a route whose strip is gone",
               m.getChannelOutput (0) == 1 && m.getChannelOutput (1) == MixerModel::masterChannel,
               "1 -> 2 became 1 -> master");
    }

    //-- ... and the renderer actually sums that way ---------------------------
    // The important half: routing a strip into another strip MOVES its signal.
    // A test that only checked "the destination is audible" would pass just as
    // well if the signal had been copied to both places.
    {
        LayerStack t;
        RackModel  r;
        MixerModel m;

        const int slot = r.getSlotIdAt (0);
        t.addLayer (utf8 ("链"), palette::layer1);
        t.setLayerOutputSlot (0, slot);
        t.setLayerLevelDb (0, 0.0f);

        const int noteId = t.getLayer (0)->addNote ({ 0, 60, 0.0, 4.0, 0.9f });
        t.getLayer (0)->findNote (noteId)->envelope = NoteEnvelope::makeFlat();
        r.setMixerChannel (slot, 0);

        PlaybackEngine engine;
        engine.setSampleRate (44100.0);

        auto render = [&] (MixerModel& model) -> RenderStats
        {
            engine.publishSchedule (t, r, 120.0, 0.0, &model);
            engine.resetMixState();
            return engine.renderOffline (1.0);
        };

        const auto flat = render (m);

        m.setChannelOutput (0, 1);
        const auto chained = render (m);

        const float ratio = chained.peak / juce::jmax (1.0e-6f, flat.peak);

        check ("Sending a strip into another strip moves its signal there",
               flat.peakForChannel (1) == 0.0f
                 && chained.peakForChannel (0) > 0.001f
                 && chained.peakForChannel (1) > 0.001f,
               juce::String (flat.peakForChannel (1), 3) + " -> "
                 + juce::String (chained.peakForChannel (1), 3));

        check ("The destination sums it once, so the master is unchanged",
               std::abs (ratio - 1.0f) < 1.0e-3f, "x" + juce::String (ratio, 4));
    }

    //-- Phase 23: clicking a synth mounts the selection there ----------------
    {
        LayerStack t;
        RackModel  r;

        const int slotA = r.getSlotIdAt (0);
        const int slotB = r.getSlotIdAt (1);

        t.addLayer (utf8 ("挂载"), palette::layer1);
        t.setLayerOutputSlot (0, slotA);

        auto* layer = t.getLayer (0);
        const int n1 = layer->addNote ({ 0, 60, 0.0, 2.0, 0.9f });
        const int n2 = layer->addNote ({ 0, 64, 2.0, 2.0, 0.9f });
        const int n3 = layer->addNote ({ 0, 67, 4.0, 2.0, 0.9f });

        PianoRollComponent roll (t, r);
        roll.setSize (1200, 520);

        roll.setSelection ({ n1 });
        const bool looseMounted = roll.mountSelectionTo (slotB);

        check ("A loose selection takes a per-note override",
               looseMounted && layer->findNote (n1) != nullptr
                 && layer->findNote (n1)->outputSlotOverride == slotB,
               "slot " + juce::String (layer->findNote (n1)->outputSlotOverride));

        roll.setSelection ({ n2, n3 });
        const int groupId = roll.groupSelection();

        roll.setSelection (layer->getGroupNoteIds (groupId));
        const bool groupMounted = roll.mountSelectionTo (slotA);

        const auto* group = layer->findGroup (groupId);

        check ("A group selection mounts the GROUP, not twelve overrides",
               groupMounted && group != nullptr && group->getOutputSlotId() == slotA
                 && layer->findNote (n2)->outputSlotOverride == -1
                 && layer->findNote (n3)->outputSlotOverride == -1,
               group != nullptr ? "group -> " + juce::String (group->getOutputSlotId())
                                : juce::String ("no group"));
    }

    //-- Phase 23: the console keeps its shape --------------------------------
    {
        const int wideWidth  = ui::px (1180);
        const int wideHeight = ui::px (300);

        mixerPanel.setSize (wideWidth, wideHeight);

        const bool masterFirst = mixerPanel.displaySlotFor (MixerModel::masterChannel) == 0
                                   && mixerPanel.channelForDisplaySlot (0) == MixerModel::masterChannel
                                   && mixerPanel.displaySlotFor (0) == 1;

        check ("The master strip is display slot 0 and is not a channel",
               masterFirst && mixerPanel.slotIdOnChannel (MixerModel::masterChannel) < 0,
               "master at slot 0");

        check ("A strip is a fixed width, so the row grows instead of squeezing",
               mixerPanel.getTotalStripWidth() == MixerPanel::stripWidth * mixerPanel.getStripCount()
                 && ! mixerPanel.isStripScrollNeeded()
                 && mixerPanel.getMaxStripScroll() == 0,
               juce::String (mixerPanel.getStripCount()) + " strips x "
                 + juce::String (MixerPanel::stripWidth) + " px");

        // Narrower than the row: the scrollbar appears and the scroll clamps.
        mixerPanel.setSize (ui::px (700), wideHeight);

        const bool needsBar = mixerPanel.isStripScrollNeeded()
                                && mixerPanel.getMaxStripScroll() > 0;
        const int  travel   = mixerPanel.getMaxStripScroll();

        mixerPanel.setStripScrollX (100000);
        const bool clampsHigh = mixerPanel.getStripScrollX() == travel;

        mixerPanel.setSize (wideWidth, wideHeight);
        const bool clampsBack = mixerPanel.getStripScrollX() == 0
                                  && ! mixerPanel.isStripScrollNeeded();

        check ("A console that does not fit gets a horizontal scrollbar",
               needsBar && clampsHigh && clampsBack,
               needsBar ? juce::String (mixerPanel.getTotalStripWidth()) + " px of strips, "
                            + juce::String (travel) + " px of travel"
                        : juce::String ("no scrollbar needed"));
    }

    //-- Phase 23: the route survives a save / load ---------------------------
    {
        LayerStack savedStack;
        RackModel  savedRack;
        MixerModel savedMixer;

        savedStack.addLayer (utf8 ("存"), palette::layer1);

        const int slot = savedRack.getSlotIdAt (0);
        savedStack.setLayerOutputSlot (0, slot);

        savedMixer.setChannelCount (4);
        savedMixer.setChannelOutput (0, 1);
        savedMixer.setChannelOutput (1, 2);

        const juce::String xml = project::toXmlText (savedStack, savedRack, 120.0, &savedMixer, nullptr);

        LayerStack loadedStack;
        RackModel  loadedRack;
        MixerModel loadedMixer;
        double     loadedBpm = 0.0;
        juce::String error;

        const bool parsed = project::fromXmlText (xml, loadedStack, loadedRack, loadedBpm, error,
                                                  &loadedMixer, nullptr);

        check ("A channel's output survives save and load",
               parsed
                 && loadedMixer.getChannelCount() == 4
                 && loadedMixer.getChannelOutput (0) == 1
                 && loadedMixer.getChannelOutput (1) == 2
                 && loadedMixer.getChannelOutput (2) == MixerModel::masterChannel,
               error.isNotEmpty() ? error : juce::String ("2 strips chained, 2 on the master"));
    }

    //-- Phase 24: one note out of a mounted group ---------------------------
    // Each of this phase's blocks is an immediately-invoked lambda rather than
    // a bare scope. runSelfTest is one function, so a bare scope does NOT free
    // its locals — they all live in the same (already enormous) frame, and
    // Phase 24's four extra LayerStack / PianoRollComponent / PlaybackEngine
    // sets pushed it past the 1 MB stack. A lambda is its own frame, so the
    // cost is the largest block instead of the sum of all of them.
    [&] {
        LayerStack t;
        RackModel  r;

        const int slotA = r.getSlotIdAt (0);
        const int slotB = r.getSlotIdAt (1);

        t.addLayer (utf8 ("局部"), palette::layer2);
        t.setLayerOutputSlot (0, slotA);

        auto* layer = t.getLayer (0);
        const int n1 = layer->addNote ({ 0, 60, 0.0, 1.0, 0.9f });
        const int n2 = layer->addNote ({ 0, 62, 1.0, 1.0, 0.9f });
        const int n3 = layer->addNote ({ 0, 64, 2.0, 1.0, 0.9f });

        PianoRollComponent roll (t, r);
        roll.setSize (1200, 520);

        roll.setSelection ({ n1, n2, n3 });
        const int groupId = roll.groupSelection();

        // The whole group moves to synth B...
        roll.setSelection (layer->getGroupNoteIds (groupId));
        const bool mounted = roll.mountSelectionTo (slotB);

        // ...then ONE member is pulled back to synth A.
        roll.setSelection ({ n2 });
        const bool single = roll.mountSelectionTo (slotA);

        const auto* group = layer->findGroup (groupId);
        const auto r1 = routing::resolve (*layer, *layer->findNote (n1), r);
        const auto r2 = routing::resolve (*layer, *layer->findNote (n2), r);
        const auto r3 = routing::resolve (*layer, *layer->findNote (n3), r);

        check ("One note can leave its group's synth without dragging the group along",
               mounted && single && group != nullptr && group->getOutputSlotId() == slotB
                 && r1.slotId == slotB && r3.slotId == slotB
                 && r2.slotId == slotA && r2.overridden && ! r2.viaGroup,
               "group -> " + juce::String (group != nullptr ? group->getOutputSlotId() : -1)
                 + " / lone note -> " + juce::String (r2.slotId)
                 + (r2.overridden ? " (override)" : " (NOT an override)"));
    } ();

    //-- Phase 24: a curve driving a hosted instrument's parameter ------------
    [&] {
        LayerStack t;
        RackModel  r;

        const int externalSlot = r.getSlotIdAt (0);
        const int internalSlot = r.getSlotIdAt (1);

        // A fake instance id: publish only needs `pluginId > 0` to decide that a
        // synth is hosted, and the headless harness has no plugins to load.
        // Nothing renders here, so no missing instance is ever asked for audio.
        r.getSlot (0)->pluginId   = 9001;
        r.getSlot (0)->pluginName = "FakeSynth";

        t.addLayer (utf8 ("外部"), palette::layer3);
        t.setLayerOutputSlot (0, externalSlot);

        auto* layer = t.getLayer (0);
        const int noteId = layer->addNote ({ 0, 60, 0.0, 4.0, 0.9f });

        auto* note = layer->findNote (noteId);
        note->modPluginParam = 7;

        NoteEnvelope curve;
        curve.loadPoints ({ { 0.0f, 0.2f }, { 0.5f, 1.0f }, { 1.0f, 0.1f } });
        curve.setEnabled (true);
        note->envelope = curve;

        const int groupId = layer->addGroup (utf8 ("外部组"), groupPalette::group2);
        layer->assignNotesToGroup ({ noteId }, groupId);

        auto* group = layer->findGroup (groupId);
        group->setOutputSlotId (externalSlot);
        group->setPluginParam (3);
        group->getPluginAutomation().resetFlat (0.5f, 64.0);
        group->getPluginAutomation().setEnabled (true);

        PianoRollComponent roll (t, r);
        roll.setSize (1200, 520);

        roll.setPluginParamLister ([externalSlot] (int slotId) -> std::vector<PluginParamInfo>
        {
            if (slotId != externalSlot)
                return {};

            return { { 0, "Cutoff", 0.5f }, { 3, "Resonance", 0.2f }, { 7, "Drive", 0.0f } };
        });

        PlaybackEngine engine;
        engine.setSampleRate (44100.0);
        engine.publishSchedule (t, r, 120.0);

        const auto schedule = engine.getSchedule();

        const bool twoCurves = schedule != nullptr && schedule->pluginParams.size() == 2;

        // Group first (priority 1), note last (priority 2): ascending order is
        // the whole conflict rule, so the ordering itself is the assertion.
        const auto& groupCurve = twoCurves ? schedule->pluginParams[0] : PluginParamCurve {};
        const auto& noteCurve  = twoCurves ? schedule->pluginParams[1] : PluginParamCurve {};

        check ("A note's envelope drives a hosted parameter, in absolute beats",
               twoCurves
                 && noteCurve.pluginId == 9001 && noteCurve.paramIndex == 7
                 && noteCurve.priority == 2
                 && noteCurve.points.size() == 3
                 && std::abs (noteCurve.startBeat - 0.0) < 1.0e-9
                 && std::abs (noteCurve.endBeat - 4.0) < 1.0e-9
                 && std::abs (noteCurve.points[1].beat - 2.0) < 1.0e-6
                 && std::abs (noteCurve.points[1].value - 1.0f) < 1.0e-6f,
               juce::String ((int) schedule->pluginParams.size()) + " curve(s), note at param "
                 + juce::String (noteCurve.paramIndex) + " over "
                 + juce::String (noteCurve.endBeat - noteCurve.startBeat, 2) + " beats");

        check ("A group's third lane drives a hosted parameter over the group's span",
               twoCurves
                 && groupCurve.pluginId == 9001 && groupCurve.paramIndex == 3
                 && groupCurve.priority == 1
                 && std::abs (groupCurve.startBeat - 0.0) < 1.0e-9
                 && std::abs (groupCurve.endBeat - 4.0) < 1.0e-9,
               "group at param " + juce::String (groupCurve.paramIndex)
                 + ", priority " + juce::String (groupCurve.priority));

        check ("The parameter list comes from the synth, and an internal one has none",
               (int) roll.pluginParamsForSlot (externalSlot).size() == 3
                 && roll.pluginParamName (externalSlot, 3) == "Resonance"
                 && roll.pluginParamsForSlot (internalSlot).empty()
                 && roll.resolvedSlotForNote (*note) == externalSlot,
               roll.noteTargetLabel (*note));

        // An internal synth has three things to drive and no parameter list, so
        // a leftover `modPluginParam` has to mean "no curve at all" — not
        // "drive parameter 7 of nothing". Both the note and the group have to
        // be moved: the group's mount outranks the layer, and the note is in it.
        group->setOutputSlotId (internalSlot);
        layer->setOutputSlotId (internalSlot);
        engine.publishSchedule (t, r, 120.0);

        const auto internalSchedule = engine.getSchedule();

        check ("A curve aimed at an internal synth publishes nothing",
               internalSchedule != nullptr && internalSchedule->pluginParams.empty(),
               juce::String (internalSchedule != nullptr ? (int) internalSchedule->pluginParams.size()
                                                         : -1)
                 + " plugin curve(s)");

        group->setOutputSlotId (externalSlot);
        layer->setOutputSlotId (externalSlot);
    } ();

    //-- Phase 24: copying a group copies the GROUP --------------------------
    [&] {
        LayerStack t;
        RackModel  r;

        const int slotA = r.getSlotIdAt (0);
        const int slotB = r.getSlotIdAt (1);

        t.addLayer (utf8 ("复制组"), palette::layer4);
        t.setLayerOutputSlot (0, slotA);

        auto* layer = t.getLayer (0);
        const int n1 = layer->addNote ({ 0, 60, 0.0, 1.0, 0.9f });
        const int n2 = layer->addNote ({ 0, 64, 1.0, 1.0, 0.9f });
        const int n3 = layer->addNote ({ 0, 67, 2.0, 1.0, 0.9f });

        PianoRollComponent roll (t, r);
        roll.setSize (1200, 520);

        roll.setSelection ({ n1, n2, n3 });
        const int groupId = roll.groupSelection();

        auto* group = layer->findGroup (groupId);
        group->setOutputSlotId (slotB);
        group->setLevelDb (-6.0f);
        group->getLevelAutomation().resetFlat (0.25f, 64.0);
        group->getLevelAutomation().setEnabled (true);

        const auto originalName = group->getName();

        roll.selectGroup (groupId);
        const bool copied   = roll.copyNotesToClipboard();
        const bool groupish = roll.isGroupClipboard();
        const int  before   = (int) layer->getGroups().size();

        const bool pasted = roll.pasteNotesFromClipboard();
        const int  after  = (int) layer->getGroups().size();

        const NoteGroup* copy = nullptr;

        for (const auto& candidate : layer->getGroups())
            if (candidate.getId() != groupId)
                copy = &candidate;

        const bool shiftedCurve = copy != nullptr
                                    && copy->getLevelAutomation().getPoints().size() >= 2
                                    && std::abs (copy->getLevelAutomation().getPoints().front().beat - 3.0) < 1.0e-6;

        check ("Copying a group pastes a second group, not its notes",
               copied && groupish && pasted && after == before + 1 && copy != nullptr
                 && copy->getName() != originalName
                 && copy->getOutputSlotId() == slotB
                 && std::abs (copy->getLevelDb() + 6.0f) < 1.0e-4f
                 && copy->getLevelAutomation().isEnabled()
                 && layer->countNotesInGroup (groupId) == 3
                 && layer->countNotesInGroup (copy->getId()) == 3,
               juce::String (before) + " -> " + juce::String (after) + " groups, copy = "
                 + (copy != nullptr ? copy->getName() : juce::String ("none")));

        check ("A pasted group's absolute-beat curve moves with it",
               shiftedCurve,
               copy != nullptr && ! copy->getLevelAutomation().getPoints().empty()
                 ? "first point at beat " + juce::String (copy->getLevelAutomation().getPoints().front().beat, 2)
                 : juce::String ("no curve on the copy"));

        //-- Phase 24: the box's grid is the roll's grid, and it snaps --------
        roll.openGroupAutomation (0, groupId, 0);

        const auto curve = roll.getGroupCurveBounds();
        const auto grid  = roll.getGridBounds();
        const double scroll = roll.getScrollBeats();
        const double bw     = (double) roll.getBeatWidth();

        // The group spans beats 0..3, so its curve area must start at beat 0's
        // x and be exactly three beats wide. That equality is what makes a grid
        // drawn through screenXForBeat() land ON the roll's own grid lines —
        // a decorative quarter-marker grid could never pass this.
        const float expectX = (float) (grid.getX() + (0.0 - scroll) * bw);

        roll.setSnapEnabled (true);
        const double snapped = roll.snapCurveBeat (1.37, { 0.0, 3.0 });
        const double clamped = roll.snapCurveBeat (9.0, { 0.0, 3.0 });

        roll.setSnapEnabled (false);
        const double raw = roll.snapCurveBeat (1.37, { 0.0, 3.0 });

        roll.setSnapEnabled (true);

        check ("The group's curve area is measured in the roll's own beats",
               std::abs (curve.getX() - expectX) < 0.6f
                 && std::abs (curve.getWidth() - 3.0f * (float) bw) < 0.6f,
               "x " + juce::String (curve.getX(), 1) + " vs " + juce::String (expectX, 1)
                 + ", width " + juce::String (curve.getWidth(), 1)
                 + " vs " + juce::String (3.0f * (float) bw, 1));

        check ("A curve point snaps to the grid, and stays inside the group",
               std::abs (snapped - 1.25) < 1.0e-9
                 && std::abs (clamped - 3.0) < 1.0e-9
                 && std::abs (raw - 1.37) < 1.0e-9,
               "1.37 -> " + juce::String (snapped, 2)
                 + ", past the end -> " + juce::String (clamped, 2)
                 + ", snap off -> " + juce::String (raw, 2));
    } ();

    //-- Phase 25: the group is an object, and resizing is per note -----------
    // Two separate complaints, one block: the bracket around a group is what
    // says "this is one thing" (so grabbing it must move the group), and the
    // tail of one note is NOT the tail of its neighbours (so resizing must not
    // touch them). Both are tested on the same two-note group.
    [&]
    {
        LayerStack t;
        RackModel  r;
        PianoRollComponent roll (t, r);
        roll.setSize (1200, 520);

        t.addLayer (utf8 ("组拖"), palette::layer1);

        auto* layer = t.getLayer (0);

        // Around middle C on purpose: the frame hit test only answers for what
        // is on screen (a note scrolled out of view has no bracket to grab), and
        // a default roll opens with the middle of the pitch range in view.
        const int left  = layer->addNote ({ 0, 80, 4.0, 1.0, 0.9f });
        const int right = layer->addNote ({ 0, 78, 8.0, 1.0, 0.9f });

        roll.setSelection ({ left, right });

        const int groupId = roll.groupSelection();
        const auto* group = layer->findGroup (groupId);

        // A point on the bracket but NOT on a note: above the two of them, in
        // the middle. Before this the click fell through to the marquee and
        // quietly deselected the group, which is why the group could not be
        // moved.
        const auto leftBounds  = roll.getNoteScreenBounds (0, left);
        const auto rightBounds = roll.getNoteScreenBounds (0, right);
        const auto framePoint  = juce::Point<float> ((leftBounds.getRight() + rightBounds.getX()) * 0.5f,
                                                     leftBounds.getY() + leftBounds.getHeight() * 0.5f);

        check ("The bracket around a group answers for the group",
               group != nullptr
                 && roll.getGroupFrameAt (framePoint) == groupId
                 && roll.getEditableNoteIdAt (framePoint) == -1,
               juce::String (groupId) + utf8 (" at ") + framePoint.toString()
                 + " / frame -> " + juce::String (roll.getGroupFrameAt (framePoint))
                 + " / note -> " + juce::String (roll.getEditableNoteIdAt (framePoint))
                 + " / group " + (group != nullptr ? juce::String ("found") : juce::String ("null")));

        // A point outside the group must NOT answer for it, or the bracket
        // would swallow the whole canvas.
        check ("The bracket does not extend past its own group",
               roll.getGroupFrameAt (juce::Point<float> (rightBounds.getRight() + ui::pf (40),
                                                        rightBounds.getBottom() + ui::pf (40))) == -1,
               "outside -> -1");

        //-- Resizing one note leaves its neighbour alone ----------------------
        roll.setSelection ({ left, right });

        const double rightLengthBefore = layer->findNote (right)->lengthBeats;
        const double leftLengthBefore  = layer->findNote (left)->lengthBeats;

        const bool resized = roll.simulateNoteResize (right, 11.0, true);

        const double rightAfter = layer->findNote (right)->lengthBeats;
        const double leftAfter  = layer->findNote (left)->lengthBeats;

        check ("Dragging one tail resizes only the note that was grabbed",
               resized
                 && std::abs (rightAfter - 3.0) < 1.0e-9      // 8..11
                 && std::abs (leftAfter - leftLengthBefore) < 1.0e-9
                 && std::abs (rightLengthBefore - 1.0) < 1.0e-9,
               "right " + juce::String (rightAfter, 2) + utf8 (" 拍 / left ")
                 + juce::String (leftAfter, 2) + utf8 (" 拍"));

        // The same rule at the other end: trimming the front of one member does
        // not move its neighbour's head.
        const double leftStartBefore = layer->findNote (left)->startBeat;
        roll.simulateNoteResize (right, 9.0, false);

        check ("Trimming one head leaves the neighbour's head where it was",
               std::abs (layer->findNote (left)->startBeat - leftStartBefore) < 1.0e-9
                 && std::abs (layer->findNote (right)->startBeat - 9.0) < 1.0e-9,
               "left stays at " + juce::String (layer->findNote (left)->startBeat, 2)
                 + ", right trimmed to " + juce::String (layer->findNote (right)->startBeat, 2));
    } ();

    //-- Phase 25: mono monitoring is a listen control, not a mix control -----
    [&]
    {
        MixerModel m;

        check ("A fresh strip listens in stereo",
               ! m.isChannelMono (0) && ! m.isChannelMono (MixerModel::defaultChannels - 1),
               "off");

        m.setChannelMono (0, true);
        const bool hiddenIgnored = (m.setChannelMono (MixerModel::maxChannels - 1, true),
                                    ! m.isChannelMono (MixerModel::maxChannels - 1));

        check ("Mono is per strip, and a strip that does not exist is ignored",
               m.isChannelMono (0) && ! m.isChannelMono (1) && hiddenIgnored,
               "0 on / 1 off / out-of-range off");

        m.resetToDefault();

        check ("Reset puts every strip back to stereo",
               ! m.isChannelMono (0),
               m.isChannelMono (0) ? juce::String ("still mono") : juce::String ("stereo"));

        // The button has to live somewhere the strip is not already using: the
        // readout row was the only slack left, and the check says so rather than
        // trusting a comment.
        check ("The mono button has pixels of its own",
               mixerPanel.monoButtonHasOwnPixels(),
               mixerPanel.monoButtonHasOwnPixels() ? juce::String ("clear")
                                                   : juce::String ("overlapping"));
    } ();

    //-- Phase 25: a mono strip really puts the left channel on the right -----
    // The number that proves it: a strip panned hard left is silent in the
    // right output, and mono monitoring has to break that silence.
    [&]
    {
        LayerStack t;
        RackModel  r;
        MixerModel m;
        PlaybackEngine engine;

        const int slot = r.getSlotIdAt (0);
        t.addLayer (utf8 ("单声道"), palette::layer1);
        t.setLayerOutputSlot (0, slot);
        t.setLayerLevelDb (0, 0.0f);
        t.setLayerPan (0, -1.0f);        // hard left

        const int noteId = t.getLayer (0)->addNote ({ 0, 60, 0.0, 4.0, 0.9f });
        t.getLayer (0)->findNote (noteId)->envelope = NoteEnvelope::makeFlat();

        r.setMixerChannel (slot, 0);

        auto rightChannelPeak = [&] (bool mono)
        {
            m.setChannelMono (0, mono);

            juce::AudioBuffer<float> out;
            out.setSize (2, 22050);
            out.clear();

            engine.publishSchedule (t, r, 120.0, 0.0, &m);
            engine.resetMixState();
            engine.renderWindow (0.10, 22050, out, nullptr);

            return out.getMagnitude (1, 0, 22050);
        };

        const float stereoRight = rightChannelPeak (false);
        const float monoRight   = rightChannelPeak (true);

        m.setChannelMono (0, false);

        check ("Mono monitoring moves a hard-left strip into the right speaker",
               stereoRight < 0.01f && monoRight > 0.05f,
               "right peak " + juce::String (stereoRight, 3) + " -> " + juce::String (monoRight, 3));
    } ();

    //-- Phase 26: the console keeps eight strips, and ANY strip can go -------
    [&]
    {
        MixerModel m;

        check ("A fresh console cannot be deleted below the shipped eight",
               m.getChannelCount() == MixerModel::deletionFloor
                 && ! m.canRemoveChannel() && ! m.removeChannel (),
               juce::String (m.getChannelCount()) + utf8 (" 条，删除被拒绝"));

        m.addChannel();                       // 9

        check ("One strip above the floor, the delete controls come alive",
               m.canRemoveChannel() && m.removeChannel()
                 && m.getChannelCount() == MixerModel::deletionFloor
                 && ! m.canRemoveChannel(),
               "9 -> 8" + juce::String (utf8 ("，到 8 条后又锁死")));

        // A middle strip is not a truncation: the row closes up, and everything
        // that pointed at a strip above the hole follows it down one number.
        m.addChannel(); m.addChannel(); m.addChannel();       // 11
        m.setInsertType (3, 0, fx::Type::drive);              // the strip that dies
        m.setChannelMono (3, true);
        m.setInsertType (4, 0, fx::Type::delay);              // the strip that slides down
        m.setChannelOutput (5, 3);                            // feeds the doomed strip
        m.setChannelOutput (7, 6);                            // feeds a strip above it

        const bool removedMiddle = m.removeChannelAt (3);

        check ("Deleting a middle strip closes the gap and renumbers the routes",
               removedMiddle && m.getChannelCount() == 10
                 && m.getInsert (3, 0).type == fx::Type::delay
                 && ! m.isChannelMono (3)
                 && m.getChannelOutput (4) == MixerModel::masterChannel
                 && m.getChannelOutput (6) == 5,
               "count " + juce::String (m.getChannelCount())
                 + ", ins3=" + juce::String ((int) m.getInsert (3, 0).type)
                 + " (delay=" + juce::String ((int) fx::Type::delay) + ")"
                 + ", mono3=" + juce::String ((int) m.isChannelMono (3))
                 + ", out4=" + juce::String (m.getChannelOutput (4))
                 + ", out6=" + juce::String (m.getChannelOutput (6)));
    } ();

    //-- Phase 26: deleting a strip unplugs the synths that were sending there -
    [&]
    {
        LayerStack t;
        RackModel  r;
        MixerModel m;
        MixerPanel panel (t, r, m);

        const int senderId = r.getSlotIdAt (0);
        const int otherId  = r.getSlotIdAt (1);

        m.addChannel();                       // 9
        m.addChannel();                       // 10
        r.setMixerChannel (senderId, 4);
        r.setMixerChannel (otherId, 6);

        const bool removed = panel.deleteStrip (4);

        check ("Deleting a strip unplugs the synths that were sending to it",
               removed && m.getChannelCount() == 9
                 && r.getSlot (0) != nullptr && r.getSlot (0)->mixerChannel == -1
                 && r.getSlot (1) != nullptr && r.getSlot (1)->mixerChannel == 5,
               "sender -> " + juce::String (r.getSlot (0) != nullptr ? r.getSlot (0)->mixerChannel : -99)
                 + ", the one above it follows the renumber -> "
                 + juce::String (r.getSlot (1) != nullptr ? r.getSlot (1)->mixerChannel : -99));

        m.setChannelCount (MixerModel::deletionFloor);

        check ("At the floor the panel refuses even a valid index",
               ! panel.deleteStrip (0) && m.getChannelCount() == MixerModel::deletionFloor,
               juce::String (m.getChannelCount()) + utf8 (" 条，拒绝删除"));
    } ();

    //-- Phase 26: the master has its own mono button --------------------------
    [&]
    {
        LayerStack t;
        RackModel  r;
        MixerModel m;
        PlaybackEngine engine;

        const int slot = r.getSlotIdAt (0);
        t.addLayer (utf8 ("主单声道"), palette::layer1);
        t.setLayerOutputSlot (0, slot);
        t.setLayerLevelDb (0, 0.0f);
        t.setLayerPan (0, -1.0f);            // hard left, through a real strip

        const int noteId = t.getLayer (0)->addNote ({ 0, 60, 0.0, 4.0, 0.9f });
        t.getLayer (0)->findNote (noteId)->envelope = NoteEnvelope::makeFlat();

        r.setMixerChannel (slot, 0);

        auto rightOfMaster = [&] (bool masterMono)
        {
            m.setMasterMono (masterMono);

            juce::AudioBuffer<float> out;
            out.setSize (2, 22050);
            out.clear();

            engine.publishSchedule (t, r, 120.0, 0.0, &m);
            engine.resetMixState();
            engine.renderWindow (0.10, 22050, out, nullptr);

            return out.getMagnitude (1, 0, 22050);
        };

        const float stereoRight = rightOfMaster (false);
        const float monoRight   = rightOfMaster (true);

        m.setMasterMono (false);

        // The strip is untouched, so its own meter still says "hard left" — the
        // fold happens at the very end of the master chain, not on the way in.
        check ("The master's mono button folds the whole mix, after the inserts",
               stereoRight < 0.01f && monoRight > 0.05f
                 && ! m.isChannelMono (0),
               "master right peak " + juce::String (stereoRight, 3)
                 + " -> " + juce::String (monoRight, 3));

        check ("The meter is two bars per strip, master included",
               mixerPanel.stereoMeterHasOwnPixels(),
               mixerPanel.stereoMeterHasOwnPixels() ? juce::String ("two bars")
                                                    : mixerPanel.describeMeterGeometry());
    } ();

    //-- Phase 26: v13 remembers how the master was being listened to ----------
    [&]
    {
        LayerStack s1;
        RackModel  r1;
        MixerModel m1;

        s1.addLayer (utf8 ("往返"), palette::layer1);
        s1.getLayer (0)->addNote ({ 0, 60, 0.0, 1.0, 0.8f });
        m1.setMasterMono (true);
        m1.setChannelMono (1, true);

        const auto xml = project::toXmlText (s1, r1, 120.0, &m1, nullptr);

        LayerStack s2;
        RackModel  r2;
        MixerModel m2;
        double bpm2 = 0.0;
        juce::String error;

        const bool loaded = project::fromXmlText (xml, s2, r2, bpm2, error, &m2, nullptr, nullptr);

        check ("A project remembers the master's mono button (v13)",
               loaded && m2.isMasterMono() && m2.isChannelMono (1) && ! m2.isChannelMono (0),
               loaded ? juce::String ("master mono ") + (m2.isMasterMono() ? "on" : "off")
                      : error);
    } ();

    //-- Phase 27: the fader gives up half its row to the stereo meter --------
    [&]
    {
        check ("The fader takes half the row and the meter takes the rest",
               mixerPanel.faderYieldedHalfTheRow(),
               mixerPanel.describeControlGeometry());
    } ();

    //-- Phase 27: dropping a file grows a sampler for it ---------------------
    const auto phase27Dir = juce::File::getSpecialLocation (juce::File::tempDirectory);
    const auto phase27A   = phase27Dir.getChildFile ("atomic-phase27-a.wav");
    const auto phase27B   = phase27Dir.getChildFile ("atomic-phase27-b.wav");

    if (! phase27A.existsAsFile() || phase27A.getSize() < 1024)
        writeTestSampleFile (phase27A, 1.0, 44100.0, 2, 330.0);

    if (! phase27B.existsAsFile() || phase27B.getSize() < 1024)
        writeTestSampleFile (phase27B, 2.0, 44100.0, 2, 165.0);

    [&]
    {
        const int before = (int) rack.getSamplerSlotIds().size();
        const int clipId = pianoRoll.addSampleAt (40.0, 40, phase27A);

        const auto ids = rack.getSamplerSlotIds();
        const auto* clip = clipId > 0 ? stack.getActiveLayer()->findSample (clipId) : nullptr;
        const auto* grown = ids.empty() ? nullptr : rack.findSlot (ids.back());

        check ("A dropped clip grows a sampler and is mounted on it",
               clipId > 0 && (int) ids.size() == before + 1
                 && clip != nullptr && grown != nullptr && grown->isSampler()
                 && clip->outputSlotId == grown->id,
               "clips " + juce::String (clipId) + ", samplers "
                 + juce::String (before) + " -> " + juce::String ((int) ids.size())
                 + ", mount " + juce::String (clip != nullptr ? clip->outputSlotId : -99));

        // The same file again must not leave a twin behind: the second drop
        // simply points at the sampler the first one created.
        const int again = pianoRoll.addSampleAt (44.0, 40, phase27A);

        check ("Dropping the same file twice reuses its sampler",
               again > 0 && (int) rack.getSamplerSlotIds().size() == before + 1,
               juce::String ((int) rack.getSamplerSlotIds().size()) + utf8 (" 个采样音源"));
    } ();

    //-- Phase 27: the mount, not the clip, decides what you hear -------------
    [&]
    {
        LayerStack     t;
        RackModel      r;
        MixerModel     m;
        PlaybackEngine engine;

        t.addLayer (utf8 ("采样切换"), palette::layer1);
        t.setLayerLevelDb (0, 0.0f);

        const auto loadA = loadSampleFile (phase27A);
        const auto loadB = loadSampleFile (phase27B);

        const int samplerA = r.addSampler (loadA.buffer, phase27A.getFullPathName(), "A");
        const int samplerB = r.addSampler (loadB.buffer, phase27B.getFullPathName(), "B");

        SampleEvent clip;
        clip.name        = "A";
        clip.path        = phase27A.getFullPathName();
        clip.audio       = loadA.buffer;
        clip.startBeat   = 0.0;
        clip.lengthBeats = 8.0;               // 4 s at 120 bpm: longer than both files
        clip.pitch       = 60;
        clip.laneSpan    = 4;
        clip.outputSlotId = samplerA;

        const int clipId = t.getLayer (0)->addSample (clip);

        const auto peakAt = [&] (double startSeconds)
        {
            juce::AudioBuffer<float> out;
            out.setSize (2, 22050);
            out.clear();

            engine.publishSchedule (t, r, 120.0, 0.0, &m);
            engine.resetMixState();
            engine.renderWindow (startSeconds, 22050, out, nullptr);

            return out.getMagnitude (0, 0, 22050);
        };

        const float earlyA = peakAt (0.10);   // both files are still playing

        const float lateA = peakAt (1.40);    // A is 1 s long: silence by now

        t.getLayer (0)->findSample (clipId)->outputSlotId = samplerB;

        const float lateB = peakAt (1.40);    // B is 2 s long: still sounding

        check ("A clip plays the sampler it is mounted on",
               earlyA > 0.01f && lateA < 0.005f && lateB > 0.01f,
               "0.1s " + juce::String (earlyA, 4)
                 + ", 1.4s on A " + juce::String (lateA, 4)
                 + ", 1.4s on B " + juce::String (lateB, 4));

        check ("The two samplers stayed two separate slots",
               samplerA != samplerB
                 && r.getSamplerSlotIds().size() == 2
                 && r.findSlot (samplerA)->sampleBuffer != r.findSlot (samplerB)->sampleBuffer,
               juce::String ((int) r.getSamplerSlotIds().size()) + utf8 (" 个采样音源"));
    } ();

    //-- Phase 27: a clip can sit in a group and keeps its own sampler --------
    [&]
    {
        auto* layer = stack.getActiveLayer();

        if (layer == nullptr)
        {
            check ("A clip can join a group of notes", false, "no active layer");
            return;
        }

        const auto loaded = loadSampleFile (phase27A);
        const int samplerId = rack.addSampler (loaded.buffer, phase27A.getFullPathName(), "P27");

        const int n1 = layer->addNote ({ 0, 80, 1.0, 2.0, 0.9f });
        const int n2 = layer->addNote ({ 0, 82, 5.0, 2.0, 0.9f });
        const int gid = layer->addGroup (utf8 ("P27 组"), groupPalette::group1);
        layer->assignNotesToGroup ({ n1, n2 }, gid);

        SampleEvent clip;
        clip.name        = "P27";
        clip.path        = phase27A.getFullPathName();
        clip.audio       = loaded.buffer;
        clip.startBeat   = 5.0;
        clip.lengthBeats = 2.0;
        clip.pitch       = 78;
        clip.laneSpan    = 2;
        clip.outputSlotId = samplerId;

        const int clipId = layer->addSample (clip);
        layer->assignSamplesToGroup ({ clipId }, gid);

        check ("A clip can join a group of notes",
               layer->countSamplesInGroup (gid) == 1
                 && layer->getGroupIdOfSample (clipId) == gid,
               juce::String (layer->countSamplesInGroup (gid)) + utf8 (" 个采样在组内"));

        const int layerIndex = stack.getActiveLayerIndex();
        const auto frame    = pianoRoll.groupFrameOf (layerIndex, gid);
        const auto clipBox  = pianoRoll.getSampleScreenBounds (layerIndex, clipId);

        check ("The group frame reaches around the clip, not just the notes",
               ! frame.isEmpty() && ! clipBox.isEmpty() && frame.contains (clipBox),
               "frame " + frame.toString() + " clip " + clipBox.toString());

        // Moving the group moves the clip with it. The clip's own rectangle has
        // to land in the same place a note would: same delta, no separate rule.
        const double noteBefore = layer->findNote (n1)->startBeat;
        const double clipBefore = layer->findSample (clipId)->startBeat;

        const bool moved = pianoRoll.simulateGroupMove (layerIndex, gid, 2.0, 0);

        check ("Dragging the group drags its clip",
               moved && juce::approximatelyEqual (layer->findSample (clipId)->startBeat,
                                                 clipBefore + 2.0)
                 && juce::approximatelyEqual (layer->findNote (n1)->startBeat, noteBefore + 2.0),
               "clip " + juce::String (clipBefore) + " -> "
                 + juce::String (layer->findSample (clipId)->startBeat));

        // Re-mounting the group must NOT re-point the clip. This is the whole
        // promise of "the group is a statement about its notes".
        auto* group = layer->findGroup (gid);

        if (group == nullptr)
        {
            check ("Re-mounting a group leaves its clips alone", false, "group vanished");
            return;
        }

        const int layerDefault = routing::layerSlotId (*layer, rack);
        const int mountTo = (layerDefault == rack.getSlotIdAt (0)) ? rack.getSlotIdAt (1)
                                                                  : rack.getSlotIdAt (0);

        const int clipIsoBefore = routing::resolveSample (*layer, *layer->findSample (clipId), rack).slotId;
        const int noteIsoBefore = routing::resolve (*layer, *layer->findNote (n1), rack).slotId;

        group->setOutputSlotId (mountTo);

        const int clipIsoAfter = routing::resolveSample (*layer, *layer->findSample (clipId), rack).slotId;
        const int noteIsoAfter = routing::resolve (*layer, *layer->findNote (n1), rack).slotId;

        check ("Re-mounting a group moves its notes and leaves its clip alone",
               noteIsoAfter == mountTo && noteIsoBefore == layerDefault
                 && clipIsoBefore == samplerId && clipIsoAfter == samplerId,
               "note " + juce::String (noteIsoBefore) + " -> " + juce::String (noteIsoAfter)
                 + ", clip " + juce::String (clipIsoBefore) + " -> " + juce::String (clipIsoAfter));

        group->setOutputSlotId (-1);
    } ();

    //-- Phase 27: what the block DRAWS follows the mount too -----------------
    [&]
    {
        auto* layer = stack.getActiveLayer();

        if (layer == nullptr)
        {
            check ("The block's waveform follows its mount", false, "no active layer");
            return;
        }

        const auto loadA = loadSampleFile (phase27A);
        const auto loadB = loadSampleFile (phase27B);

        const int slotA = rack.addSampler (loadA.buffer, phase27A.getFullPathName(), "WA");
        const int slotB = rack.addSampler (loadB.buffer, phase27B.getFullPathName(), "WB");

        SampleEvent clip;
        clip.name        = "W";
        clip.path        = phase27A.getFullPathName();
        clip.audio       = loadA.buffer;
        clip.startBeat   = 0.0;
        clip.lengthBeats = 2.0;
        clip.pitch       = 60;
        clip.laneSpan    = 2;
        clip.outputSlotId = slotA;

        const int clipId = layer->addSample (clip);

        const auto first = pianoRoll.clipAudio (*layer, *layer->findSample (clipId));

        layer->findSample (clipId)->outputSlotId = slotB;

        const auto second = pianoRoll.clipAudio (*layer, *layer->findSample (clipId));

        // Both answers come from routing::resolveSample, which is also what the
        // engine publishes. Drawing the clip's own buffer while the engine read
        // the sampler's would be one waveform on screen and another in the air.
        //
        // Compared against the SLOT's buffer, not against this call's freshly
        // decoded one: addSampler reuses the sampler that already holds that
        // path, so the buffer by then is the one from the earlier load — same
        // file, different shared_ptr. "Which file is this" is the question, and
        // the slot is what answers it.
        const auto slotABuffer = rack.findSlot (slotA) != nullptr ? rack.findSlot (slotA)->sampleBuffer
                                                                  : std::shared_ptr<const SampleBuffer>();
        const auto slotBBuffer = rack.findSlot (slotB) != nullptr ? rack.findSlot (slotB)->sampleBuffer
                                                                  : std::shared_ptr<const SampleBuffer>();

        check ("The block's waveform follows its mount",
               first != nullptr && slotABuffer != nullptr && slotBBuffer != nullptr
                 && first == slotABuffer && second == slotBBuffer && first != second,
               juce::String (first == slotABuffer ? "A" : "?")
                 + " -> " + juce::String (second == slotBBuffer ? "B" : "?"));
    } ();

    //-- Phase 27: v14 keeps samplers, mounts and clip membership -------------
    [&]
    {
        LayerStack s1;
        RackModel  r1;

        s1.addLayer (utf8 ("往返"), palette::layer1);

        const auto loaded = loadSampleFile (phase27A);
        const int samplerId = r1.addSampler (loaded.buffer, phase27A.getFullPathName(), "往返采样");

        SampleEvent clip;
        clip.name        = "往返";
        clip.path        = phase27A.getFullPathName();
        clip.audio       = loaded.buffer;
        clip.startBeat   = 2.0;
        clip.lengthBeats = 3.0;
        clip.pitch       = 60;
        clip.laneSpan    = 2;
        clip.outputSlotId = samplerId;

        const int clipId = s1.getLayer (0)->addSample (clip);
        const int gid = s1.getLayer (0)->addGroup (utf8 ("G"), groupPalette::group1);
        s1.getLayer (0)->assignSamplesToGroup ({ clipId }, gid);

        const auto xml = project::toXmlText (s1, r1, 120.0);

        LayerStack s2;
        RackModel  r2;
        double bpm2 = 0.0;
        juce::String error;

        const bool ok = project::fromXmlText (xml, s2, r2, bpm2, error, nullptr, nullptr, nullptr);

        const auto* restored = (s2.getLayer (0) != nullptr && s2.getLayer (0)->getSampleCount() == 1)
                                 ? &s2.getLayer (0)->getSamples().front() : nullptr;

        const auto ids = r2.getSamplerSlotIds();
        const auto* restoredSlot = ids.empty() ? nullptr : r2.findSlot (ids.front());

        check ("A project keeps samplers, clip mounts and clip membership (v14)",
               ok && ids.size() == 1 && restoredSlot != nullptr
                 && restoredSlot->isSampler() && restoredSlot->sampleBuffer != nullptr
                 && restored != nullptr && restored->outputSlotId == ids.front()
                 && restored->groupId == gid,
               ok ? juce::String ("samplers ") + juce::String ((int) ids.size())
                      + ", mount " + juce::String (restored != nullptr ? restored->outputSlotId : -99)
                      + ", group " + juce::String (restored != nullptr ? restored->groupId : -99)
                  : error);
    } ();

    //-- Phase 28: the browser is a tree, and scanning does not block ---------
    [&]
    {
        const auto root = phase27Dir.getChildFile ("atomic-phase28-tree");

        // A fresh tree every run: a leftover from the last one would make "the
        // folder is new" false and the check would fail for the wrong reason.
        root.deleteRecursively();

        const auto first  = root.getChildFile ("First");
        const auto nested = first.getChildFile ("Nested");
        const auto deeper = nested.getChildFile ("Deeper");

        for (auto* dir : { &first, &nested, &deeper })
            dir->createDirectory();

        writeTestSampleFile (root.getChildFile ("top.wav"), 0.2, 44100.0, 1, 440.0);
        writeTestSampleFile (first.getChildFile ("one.wav"), 0.2, 44100.0, 1, 440.0);
        writeTestSampleFile (deeper.getChildFile ("deep.wav"), 0.2, 44100.0, 1, 440.0);

        // Unregister a leftover from an earlier run, so addFolder is really
        // adding rather than silently ignoring a duplicate.
        for (int i = filePanel.getNumFolders() - 1; i >= 0; --i)
            if (filePanel.getFolder (i) == root)
                filePanel.removeFolder (i);

        // Drain the folders the store file contributed at construction time:
        // otherwise "progress starts at zero" would be reading THEIR progress.
        filePanel.waitForPendingScans();

        const int foldersBefore = filePanel.getNumFolders();

        filePanel.addFolder (root);

        // The point of the async scan: addFolder returns at once and the list
        // is not filled yet, so the click that added a huge library costs a
        // click, not a freeze.
        const float startProgress = filePanel.getScanProgress();

        check ("Adding a folder starts a background scan instead of blocking",
               filePanel.isScanning() && filePanel.getNumFolders() == foldersBefore + 1,
               juce::String (filePanel.isScanning() ? "scanning" : "idle")
                 + ", folders " + juce::String (filePanel.getNumFolders()));

        const bool finished = filePanel.waitForPendingScans (15000);

        check ("The scan finishes and reports it is done",
               finished && ! filePanel.isScanning() && filePanel.getScanProgress() > 0.99f,
               finished ? juce::String (filePanel.getScanProgress() * 100.0f, 1) + "%"
                        : juce::String ("timeout"));

        // 0 at the start and 1 at the end is what makes the header line a
        // progress report rather than decoration.
        check ("Scan progress runs from zero to one",
               startProgress < 0.01f && filePanel.getScanProgress() > 0.99f,
               juce::String (startProgress * 100.0f, 1) + "% -> "
                 + juce::String (filePanel.getScanProgress() * 100.0f, 1) + "%");

        // Phase 33: a scanned tree arrives FOLDED — that is the new default and
        // the check below is about nesting, so the two branches are opened
        // first. `persist = false`: this fixture's temp paths have no business
        // in the user's store file.
        filePanel.setSubFolderExpanded (first, true, false);
        filePanel.setSubFolderExpanded (nested, true, false);
        filePanel.setSubFolderExpanded (deeper, true, false);   // the leaf branch holds deep.wav

        auto rows = filePanel.buildRows();

        int rootRow = -1, nestedRow = -1, deepRow = -1, deepestIndent = 0;

        for (int i = 0; i < (int) rows.size(); ++i)
        {
            const auto& row = rows[(size_t) i];

            if (row.kind == FileBrowserPanel::Row::Kind::rootFolder && row.file == root)
                rootRow = i;
            else if (row.kind == FileBrowserPanel::Row::Kind::subFolder && row.label == "Nested")
                nestedRow = i;
            else if (row.kind == FileBrowserPanel::Row::Kind::file
                       && row.file.getFileName() == "deep.wav")
                deepRow = i;

            if (row.kind == FileBrowserPanel::Row::Kind::file)
                deepestIndent = juce::jmax (deepestIndent, row.indent);
        }

        // root(0) -> First(1) -> Nested(2) -> Deeper(3) -> deep.wav(4).
        check ("Nested folders are drawn as nested rows",
               rootRow >= 0 && nestedRow >= 0 && deepRow >= 0 && deepestIndent == 4,
               "root " + juce::String (rootRow) + ", nested " + juce::String (nestedRow)
                 + ", deep " + juce::String (deepRow) + ", deepest indent "
                 + juce::String (deepestIndent));

        const int rowsBeforeFold = (int) rows.size();

        filePanel.setSubFolderExpanded (nested, false, false);

        const auto folded = filePanel.buildRows();
        bool nestedStillThere = false, deepHidden = true;

        for (const auto& row : folded)
        {
            if (row.kind == FileBrowserPanel::Row::Kind::subFolder && row.label == "Nested")
                nestedStillThere = true;
            else if (row.kind == FileBrowserPanel::Row::Kind::file
                       && row.file.getFileName() == "deep.wav")
                deepHidden = false;
        }

        check ("Folding a branch keeps its folder row and hides everything under it",
               nestedStillThere && deepHidden && (int) folded.size() < rowsBeforeFold,
               juce::String ((int) folded.size()) + " / " + juce::String (rowsBeforeFold)
                 + utf8 (" 行"));

        filePanel.setSubFolderExpanded (nested, true, false);

        check ("Unfolding brings the branch back",
               (int) filePanel.buildRows().size() == rowsBeforeFold,
               juce::String ((int) filePanel.buildRows().size()));

        // Leave the store file as we found it, and take the fixture with us.
        for (int i = filePanel.getNumFolders() - 1; i >= 0; --i)
            if (filePanel.getFolder (i) == root)
                filePanel.removeFolder (i);

        root.deleteRecursively();
    } ();

    //-- Phase 28: clips are selectable, and selection means movable ----------
    [&]
    {
        auto* layer = stack.getActiveLayer();

        const int a = pianoRoll.addSampleAt (60.0, 60, phase27A);
        const int b = pianoRoll.addSampleAt (70.0, 70, phase27A);

        pianoRoll.setSampleSelection ({ a, b });

        check ("Selecting clips lets go of the notes",
               a > 0 && b > 0 && pianoRoll.getSelection().empty()
                 && pianoRoll.getSampleSelection().size() == 2,
               "clips " + juce::String (a) + "/" + juce::String (b)
                 + ", notes " + juce::String ((int) pianoRoll.getSelection().size()));

        const auto* clipA = layer->findSample (a);
        const auto* clipB = layer->findSample (b);

        if (clipA == nullptr || clipB == nullptr)
        {
            check ("Phase 28 clip fixtures exist", false, "addSampleAt failed");
            return;
        }

        const double aStart = clipA->startBeat, bStart = clipB->startBeat;
        const int    aPitch = clipA->pitch,     bPitch = clipB->pitch;

        const bool moved = pianoRoll.simulateSampleMove (2.0, 3);

        check ("Two selected clips drag as one rigid block",
               moved
                 && std::abs (clipA->startBeat - (aStart + 2.0)) < 1.0e-9
                 && std::abs (clipB->startBeat - (bStart + 2.0)) < 1.0e-9
                 && clipA->pitch == aPitch + 3 && clipB->pitch == bPitch + 3,
               "d beat " + juce::String (clipA->startBeat - aStart, 3)
                 + " / " + juce::String (clipB->startBeat - bStart, 3)
                 + ", d pitch " + juce::String (clipA->pitch - aPitch)
                 + " / " + juce::String (clipB->pitch - bPitch));

        const int beforeDup = layer->getSampleCount();
        const auto copies   = pianoRoll.duplicateSelectedSamples();

        check ("Duplicate copies every selected block and selects the copies",
               (int) copies.size() == 2 && layer->getSampleCount() == beforeDup + 2
                 && pianoRoll.getSampleSelection() == copies,
               juce::String ((int) copies.size()) + utf8 (" 份，共 ")
                 + juce::String (layer->getSampleCount()) + utf8 (" 块"));

        const int beforeDelete = layer->getSampleCount();
        const bool deleted     = pianoRoll.deleteSelectedSamples();

        check ("Delete removes exactly the selected blocks",
               deleted && layer->getSampleCount() == beforeDelete - 2
                 && pianoRoll.getSampleSelection().empty(),
               juce::String (beforeDelete) + " -> " + juce::String (layer->getSampleCount()));

        pianoRoll.setSampleSelection ({ a });

        const bool copied      = pianoRoll.copySamplesToClipboard();
        const int  beforePaste = layer->getSampleCount();
        const bool pasted      = pianoRoll.pasteSamplesFromClipboard();

        check ("Clipboard copy/paste round-trips a block",
               copied && pasted && layer->getSampleCount() == beforePaste + 1
                 && pianoRoll.getSampleSelection().size() == 1,
               juce::String (beforePaste) + " -> " + juce::String (layer->getSampleCount()));

        // Take this check's own blocks back out; the demo fixture may keep its.
        std::vector<int> mine { a, b };

        for (int id : pianoRoll.getSampleSelection())
            mine.push_back (id);

        pianoRoll.setSampleSelection (mine);
        pianoRoll.deleteSelectedSamples();
    } ();

    //-- Phase 37: a paste snaps, and a repeat steps past the last one -------
    // Two promises, both broken by the same two lines of the old paste: the
    // anchor was snapped and THEN pushed by the clipboard length, so (a) a
    // block whose length is not whole grid steps handed the next paste a beat
    // off the grid, and (b) every Ctrl+V read the same anchor, so the second
    // paste landed exactly on the first and the earlier pastes looked like they
    // had been dragged away.
    [&]
    {
        auto* layer = stack.getActiveLayer();

        if (layer == nullptr)
        {
            check ("Phase 37 needs a layer", false, "no active layer");
            return;
        }

        const bool savedSnap = pianoRoll.isSnapEnabled();
        pianoRoll.setSnapEnabled (true);

        // 0.375 beats = 1.5 grid steps at the 1/16 grid. Half a step is exactly
        // the amount that used to fall through.
        const int src     = pianoRoll.addSampleAt (400.0, 45, phase27A);
        auto*     srcClip = layer->findSample (src);

        if (srcClip == nullptr)
        {
            check ("Phase 37 fixture exists", false, "addSampleAt failed");
            pianoRoll.setSnapEnabled (savedSnap);
            return;
        }

        srcClip->lengthBeats = 0.375;

        //-- (a) an off-grid playhead still produces an on-grid landing ------
        pianoRoll.setSampleSelection ({ src });
        pianoRoll.copySamplesToClipboard();
        pianoRoll.setPlayheadBeat (401.1);
        pianoRoll.pasteSamplesFromClipboard();

        const auto   first     = pianoRoll.getSampleSelection();
        const auto*  firstClip = first.empty() ? nullptr : layer->findSample (first.front());
        const double firstBeat = firstClip != nullptr ? firstClip->startBeat : -1.0;
        const double firstStep = firstBeat / PianoRollComponent::gridDivision;

        check ("With snap on, a paste lands on a grid line",
               firstClip != nullptr && std::abs (firstBeat - 401.0) < 1.0e-9
                 && std::abs (firstStep - std::round (firstStep)) < 1.0e-9,
               "landed on " + juce::String (firstBeat, 4));

        //-- (b) the second Ctrl+V starts after the first, and leaves it alone -
        pianoRoll.pasteSamplesFromClipboard();

        const auto  second      = pianoRoll.getSampleSelection();
        const auto* secondClip  = second.empty() ? nullptr : layer->findSample (second.front());
        const auto* firstAgain  = first.empty() ? nullptr : layer->findSample (first.front());
        const double secondBeat = secondClip != nullptr ? secondClip->startBeat : -1.0;

        check ("A second paste starts after the first and leaves it where it was",
               secondClip != nullptr
                 && std::abs (secondBeat - 401.5) < 1.0e-9
                 && firstAgain != nullptr
                 && std::abs (firstAgain->startBeat - firstBeat) < 1.0e-9,
               "first " + juce::String (firstBeat, 4) + ", second " + juce::String (secondBeat, 4));

        //-- (c) a paste on top of its source moves off it, on the grid too ---
        // 400 + 0.375 = 400.375 would be the old answer; the grid makes it 400.5.
        pianoRoll.setSampleSelection ({ src });
        pianoRoll.setPlayheadBeat (400.0);
        pianoRoll.copySamplesToClipboard();          // a fresh copy, a fresh streak
        pianoRoll.pasteSamplesFromClipboard();

        const auto  shifted      = pianoRoll.getSampleSelection();
        const auto* shiftedClip  = shifted.empty() ? nullptr : layer->findSample (shifted.front());
        const double shiftedBeat = shiftedClip != nullptr ? shiftedClip->startBeat : -1.0;

        check ("A paste on top of its source moves past it, and lands on the grid",
               shiftedClip != nullptr && std::abs (shiftedBeat - 400.5) < 1.0e-9,
               "landed on " + juce::String (shiftedBeat, 4));

        //-- (d) moving the playhead ends the streak -------------------------
        pianoRoll.setPlayheadBeat (420.0);
        pianoRoll.pasteSamplesFromClipboard();

        const auto  moved     = pianoRoll.getSampleSelection();
        const auto* movedClip = moved.empty() ? nullptr : layer->findSample (moved.front());

        check ("Moving the playhead ends the streak",
               movedClip != nullptr && std::abs (movedClip->startBeat - 420.0) < 1.0e-9,
               "landed on " + juce::String (movedClip != nullptr ? movedClip->startBeat : -1.0, 4));

        // Take out everything this block created, source included.
        std::vector<int> mine { src };

        for (int id : first)   mine.push_back (id);
        for (int id : second)  mine.push_back (id);
        for (int id : shifted) mine.push_back (id);
        for (int id : moved)   mine.push_back (id);

        pianoRoll.setSampleSelection (mine);
        pianoRoll.deleteSelectedSamples();
        pianoRoll.setPlayheadBeat (-1.0);
        pianoRoll.setSnapEnabled (savedSnap);
    } ();

    //-- Phase 38: the 编组 button sees clips; a VST3 shows its name and loads -
    [&]
    {
        auto* layer = stack.getActiveLayer();

        if (layer == nullptr)
        {
            check ("Phase 38 needs a layer", false, "no active layer");
            return;
        }

        //-- (a) a clip-only selection is a selection -------------------------
        // Phase 35 made groupSelection() accept clips, and the G key kept up.
        // The toolbar button did not: it asked `! selection.empty()`, which is
        // a question about NOTES, so a selected clip left 编组 greyed out and
        // clicking it did nothing at all.
        const int clipId = pianoRoll.addSampleAt (500.0, 45, phase27A);

        pianoRoll.setSelection ({});
        pianoRoll.setSampleSelection ({ clipId });

        check (utf8 ("A clip-only selection satisfies the 编组 button's predicate"),
               pianoRoll.hasAnySelection() && pianoRoll.getSelection().empty()
                 && (int) pianoRoll.getSampleSelection().size() == 1,
               "notes " + juce::String ((int) pianoRoll.getSelection().size())
                 + ", clips " + juce::String ((int) pianoRoll.getSampleSelection().size()));

        const int grouped  = pianoRoll.groupSelection();
        const auto* clipIn = layer->findSample (clipId);

        check ("...and the clip really lands in a group",
               grouped > 0 && clipIn != nullptr && clipIn->groupId == grouped,
               "groupId " + juce::String (clipIn != nullptr ? clipIn->groupId : -99));

        pianoRoll.setSampleSelection ({ clipId });
        pianoRoll.deleteSelectedSamples();

        //-- (b) replacing an instrument with a VST3 --------------------------
        // No real bundle: `load` needs a scanned library and a native VST3,
        // neither of which a headless run can be asked to provide. The id is
        // what everything downstream keys on, so an id that resolves to nothing
        // is a faithful test of every step except the plugin's own DSP.
        const int slotId = rack.addSynth();
        auto*     slot   = rack.findSlot (slotId);

        if (slot == nullptr)
        {
            check ("Phase 38 needs a rack slot", false, "addSynth failed");
            return;
        }

        const juce::String before = slot->getDisplayName();

        juce::PluginDescription description;
        description.name             = "Atomic Test Synth";
        description.manufacturerName = "Atomic";
        description.pluginFormatName = "VST3";

        rack.replaceWithPlugin (slotId, description, 4242);
        slot = rack.findSlot (slotId);

        check ("Replacing an instrument with a VST3 changes what the slot IS",
               slot != nullptr && slot->isPlugin() && slot->pluginId == 4242,
               slot != nullptr ? juce::String ("pluginId ") + juce::String (slot->pluginId)
                               : juce::String ("slot gone"));

        // The card title used to stay on "Synth A" and only the small pill
        // changed, which is exactly what "替换没有成功" looks like from the
        // outside. The name is now derived from what the slot holds.
        check (utf8 ("...and the card names the plugin instead of the old Synth A"),
               slot != nullptr && slot->getDisplayName() == "Atomic Test Synth"
                 && rack.getSlotName (slotId) == "Atomic Test Synth",
               "card \"" + (slot != nullptr ? slot->getDisplayName() : juce::String()) + "\""
                 + utf8 ("（原 ") + before + "）");

        // The audio thread may not read the rack: the id has to reach the
        // published slot or the notes keep playing the oscillator.
        {
            auto published   = audio.getPlayback().getSchedule();
            int  publishedId = -1;

            if (published != nullptr)
                for (const auto& mix : published->slots)
                    if (mix.slotId == slotId)
                        publishedId = mix.pluginId;

            check ("...and the plugin id reaches the renderer, not just the card",
                   publishedId == 4242,
                   "published " + juce::String (publishedId));
        }

        //-- (c) and back again ----------------------------------------------
        // Derived naming, not a stored one: an overwritten `name` could never
        // give this back.
        rack.replaceWithNative (slotId, Waveform::square);
        slot = rack.findSlot (slotId);

        check ("Replacing it back with an oscillator restores the slot's own name",
               slot != nullptr && ! slot->isPlugin() && slot->pluginId == 0
                 && slot->getDisplayName() == before,
               "card \"" + (slot != nullptr ? slot->getDisplayName() : juce::String()) + "\"");

        //-- (d) and once for real -------------------------------------------
        // The headless harness ships Atomic Test Synth, so at least one VST3
        // can be loaded here without touching the user's library. This is the
        // whole "替换没有成功" path end to end: load, mount, name, and the
        // condition 打开插件界面 is enabled on.
        juce::PluginDescription realSynth;

        for (const auto& entry : plugins.getLibrary())
            if (entry.isInstrument && entry.description.name.containsIgnoreCase ("Test Synth"))
                realSynth = entry.description;

        if (realSynth.name.isNotEmpty())
        {
            juce::String loadError;
            const int    realId = plugins.load (realSynth, &loadError);

            rack.replaceWithPlugin (slotId, realSynth, realId);
            slot = rack.findSlot (slotId);

            check (utf8 ("A real VST3 can be loaded and mounted by 替换音源"),
                   realId > 0 && slot != nullptr && slot->pluginId == realId
                     && plugins.isLoaded (realId),
                   "instance " + juce::String (realId)
                     + (loadError.isNotEmpty() ? juce::String (" / ") + loadError : juce::String()));

            check (utf8 ("...so 打开插件界面 has an editor to open"),
                   plugins.getProcessorForEditor (realId) != nullptr,
                   slot != nullptr ? slot->getDisplayName() : juce::String ("no slot"));
        }

        rack.replaceWithNative (slotId, Waveform::saw);
        reconcilePlugins();            // the instance we loaded has no owner now
        rack.removeSlot (slotId);
        focusSynth (-1);
    } ();

    //-- Phase 39: a keyboard plays the synth --------------------------------
    [&]
    {
        //-- The FL Studio layout -----------------------------------------
        // Read back from the ONE table the key handler, the settings diagram
        // and this check all use. "The picture matches what plays" is then not
        // a claim, it is the same data twice.
        bool everyKeyAgrees = true;

        for (const auto& binding : typing::bindings())
        {
            const int upper = (int) juce::CharacterFunctions::toUpperCase (binding.key);

            everyKeyAgrees = everyKeyAgrees
                               && typing::noteForKeyCode (upper)
                                    == typing::lowestNote + binding.semitone;
        }

        check ("Every FL Studio key plays the note it is drawn on",
               everyKeyAgrees && (int) typing::bindings().size() == 24,
               "24 keys, Z = MIDI " + juce::String (typing::noteForKeyCode ('Z')));

        // The piano pattern, spot-checked where it actually lives: the gaps in
        // the black-key rows are E-F and B-C, and nothing else.
        check ("The letter gaps are the piano's gaps: C# D# then F# G# A#",
               typing::noteForKeyCode ('S') == 61 && typing::noteForKeyCode ('D') == 63
                 && typing::noteForKeyCode ('G') == 66 && typing::noteForKeyCode ('H') == 68
                 && typing::noteForKeyCode ('J') == 70,
               "S " + juce::String (typing::noteForKeyCode ('S'))
                 + " D " + juce::String (typing::noteForKeyCode ('D'))
                 + " G " + juce::String (typing::noteForKeyCode ('G')));

        check ("The upper octave starts one octave above where the lower one ends",
               typing::noteForKeyCode ('M') == 71 && typing::noteForKeyCode ('Q') == 72
                 && typing::noteForKeyCode ('U') == 83,
               "M " + juce::String (typing::noteForKeyCode ('M'))
                 + " / Q " + juce::String (typing::noteForKeyCode ('Q'))
                 + " / U " + juce::String (typing::noteForKeyCode ('U')));

        check ("The diagram is built from the same table it describes",
               typing::rowKeys (typing::Row::lowerWhite) == "Z X C V B N M"
                 && typing::rowNotes (typing::Row::lowerWhite) == "C4 D4 E4 F4 G4 A4 B4"
                 && typing::rowKeys (typing::Row::upperBlack) == "2 3 5 6 7",
               typing::rowKeys (typing::Row::lowerWhite) + " = "
                 + typing::rowNotes (typing::Row::lowerWhite));

        check ("Octave shift moves the whole layout, and clamps instead of inventing notes",
               typing::noteForKeyCode ('Z', 1) == 72
                 && typing::noteForKeyCode ('Z', -1) == 48
                 && typing::noteForKeyCode ('U', 4) == -1
                 && typing::noteForKeyCode ('!') == -1,
               "Z+1 " + juce::String (typing::noteForKeyCode ('Z', 1)));

        //-- The voice: a note with a real end -----------------------------
        {
            LayerStack as;
            RackModel  ar;
            PlaybackEngine engine;

            const int aSlot = ar.getFirstSlotId();
            as.addLayer (utf8 ("演奏"), palette::layer1);
            as.setLayerOutputSlot (0, aSlot);

            engine.publishSchedule (as, ar, 120.0, 0.0, nullptr);
            engine.resetMixState();

            check ("Nothing is held before a key is pressed",
                   ! engine.hasLiveNotes() && engine.getLiveVoiceCount() == 0, "idle");

            engine.noteOnLive (aSlot, 69, 0.9f);
            engine.beginLiveBlock();

            juce::AudioBuffer<float> held;
            held.setSize (2, 4096);
            held.clear();
            engine.renderLive (held, 4096);

            const float heldPeak = held.getMagnitude (0, 0, 4096);

            check ("A held key sounds its synth with the transport stopped",
                   engine.hasLiveNotes() && engine.getLiveVoiceCount() == 1
                     && heldPeak > 0.001f,
                   "peak " + juce::String (heldPeak, 4));

            // Two keys held at once is the whole difference between a keyboard
            // and a monophonic preview: one more voice, and more sound.
            engine.noteOnLive (aSlot, 76, 0.9f);
            engine.beginLiveBlock();

            juce::AudioBuffer<float> chord;
            chord.setSize (2, 4096);
            chord.clear();
            engine.renderLive (chord, 4096);

            const float chordRms = chord.getRMSLevel (0, 0, 4096);
            const float heldRms  = held.getRMSLevel (0, 0, 4096);

            check ("Two keys are two voices, and a chord is louder than one note",
                   engine.getLiveVoiceCount() == 2 && chordRms > heldRms * 1.15f,
                   "voices " + juce::String (engine.getLiveVoiceCount())
                     + " / rms " + juce::String (heldRms, 4) + " → " + juce::String (chordRms, 4));

            //-- And it lets go -------------------------------------------
            engine.noteOffLive (69);
            engine.noteOffLive (76);
            engine.beginLiveBlock();

            juce::AudioBuffer<float> tail;
            tail.setSize (2, 4096);

            for (int block = 0; block < 40; ++block)
            {
                tail.clear();
                engine.renderLive (tail, 4096);
            }

            check ("A released key stops: no voice is left and the tail is silent",
                   engine.getLiveVoiceCount() == 0 && ! engine.hasLiveNotes()
                     && tail.getMagnitude (0, 0, 4096) < 1.0e-4f,
                   "voices " + juce::String (engine.getLiveVoiceCount())
                     + " / tail " + juce::String (tail.getMagnitude (0, 0, 4096), 6));

            //-- "Let go of everything" is one queued event --------------------
            // It has to be: the callers are a settings click and a source
            // switch, both on the message thread, and the voice pool belongs to
            // the audio thread.
            engine.noteOnLive (aSlot, 60, 0.9f);
            engine.noteOnLive (aSlot, 67, 0.9f);
            engine.beginLiveBlock();

            juce::AudioBuffer<float> scratch;
            scratch.setSize (2, 4096);
            scratch.clear();
            engine.renderLive (scratch, 4096);

            const int beforeAllOff = engine.getLiveVoiceCount();

            engine.allLiveNotesOff();
            engine.beginLiveBlock();

            for (int block = 0; block < 20; ++block)
            {
                scratch.clear();
                engine.renderLive (scratch, 4096);
            }

            check ("\"Let go of everything\" releases voices it never looked at",
                   beforeAllOff == 2 && engine.getLiveVoiceCount() == 0
                     && ! engine.hasLiveNotes(),
                   "before " + juce::String (beforeAllOff)
                     + " / after " + juce::String (engine.getLiveVoiceCount()));
        }

        //-- Which synth a key goes to --------------------------------------
        const int synthSlot = rack.addSynth();

        if (synthSlot <= 0)
        {
            check ("Phase 39 needs a rack slot", false, "addSynth failed");
            return;
        }

        focusSynth (synthSlot);

        check ("A played key goes to the spotlit synth",
               getLiveTargetSlot() == synthSlot,
               "slot " + juce::String (getLiveTargetSlot())
                 + utf8 (" / ") + getLiveTargetName());

        //-- Phase 40: two switches, and the keyboard is on out of the box ----
        // This has to come FIRST: every line below flips a switch, and "out of
        // the box" is a property of the session as it was built.
        check ("Out of the box the computer keyboard already plays",
               getMidiSources().typingKeyboard && ! getMidiSources().externalController,
               midiSourcesName (getMidiSources()));

        setMidiSources ({ false, false });

        check ("Both switches off is silence, and the letters go back to the editor",
               ! getMidiSources().any() && ! typingKeyboardLive() && typingHeld.empty()
                 && ! handleTypingKeyDown (juce::KeyPress ('Z', juce::ModifierKeys(), 'z')),
               midiSourcesName (getMidiSources()));

        setMidiSources ({ true, false });

        const bool claimed = handleTypingKeyDown (juce::KeyPress ('Z', juce::ModifierKeys(), 'z'));

        check ("With the keyboard on, Z is a note and not a shortcut",
               claimed && typingHeld.size() == 1
                 && typingHeld.front().second == typing::noteForKeyCode ('Z', getTypingOctave()),
               "held " + juce::String ((int) typingHeld.size())
                 + " / note " + juce::String (typingHeld.empty() ? -1 : typingHeld.front().second));

        // Numpad +/- move the whole layout, and the moved layout is what plays.
        handleTypingKeyDown (juce::KeyPress (juce::KeyPress::numberPadAdd,
                                            juce::ModifierKeys(), 0));

        const bool shifted = handleTypingKeyDown (juce::KeyPress ('X', juce::ModifierKeys(), 'x'));

        check ("Numpad + shifts an octave, and X now plays the note one octave up",
               shifted && getTypingOctave() == 1 && typingHeld.size() == 2
                 && typingHeld.back().second == 74,
               "octave " + juce::String (getTypingOctave())
                 + " / X = " + juce::String (typingHeld.empty() ? -1 : typingHeld.back().second));

        check ("Ctrl+Z is still undo, not a note",
               ! handleTypingKeyDown (juce::KeyPress ('Z', juce::ModifierKeys::ctrlModifier, 'z')),
               "unclaimed");

        //-- Phase 40: the two sources coexist --------------------------------
        // The switch that turns the controller ON must not disturb what the
        // computer keyboard is holding. Phase 39's radio could not express this
        // at all: choosing one switched the other off, and released its notes.
        const int heldBeforeController = (int) typingHeld.size();

        setMidiSources ({ true, true });

        check ("Turning the controller on does not interrupt the keys already down",
               typingKeyboardLive() && getMidiSources().externalController
                 && (int) typingHeld.size() == heldBeforeController,
               "held " + juce::String ((int) typingHeld.size())
                 + " / " + midiSourcesName (getMidiSources()));

        const bool alsoPlays = handleTypingKeyDown (juce::KeyPress ('C', juce::ModifierKeys(), 'c'));

        check ("With both on, the computer keyboard still plays",
               alsoPlays && typingHeld.size() == (size_t) heldBeforeController + 1,
               "held " + juce::String ((int) typingHeld.size()));

        check ("The pair has a name of its own, not just the two parts",
               midiSourcesName ({ true, true }) == utf8 ("电脑键盘 + 外部 MIDI 控制器"),
               midiSourcesName ({ true, true }));

        // The other direction: the keyboard switch owns only its own keys.
        setMidiSources ({ false, true });

        check ("Turning the keyboard off leaves the controller switch alone",
               ! typingKeyboardLive() && getMidiSources().externalController
                 && typingHeld.empty()
                 && ! handleTypingKeyDown (juce::KeyPress ('Z', juce::ModifierKeys(), 'z')),
               midiSourcesName (getMidiSources()));

        // And turning the last switch off has to let go of the voices this side
        // cannot name. The engine-level proof that one queued "all off" really
        // silences them is the check further up; what is checked here is that
        // the switch queues it and clears the keyboard bookkeeping.
        setMidiSources ({ false, false });
        audio.getPlayback().beginLiveBlock();          // apply the all-off

        check ("Turning both switches off stops the keyboard and lets go of everything",
               ! getMidiSources().any() && ! typingKeyboardLive() && typingHeld.empty(),
               "held " + juce::String ((int) typingHeld.size())
                 + " / " + midiSourcesName (getMidiSources()));

        setMidiSources ({ true, false });              // back to the default
        audio.getPlayback().beginLiveBlock();

        setTypingOctave (0);
        rack.removeSlot (synthSlot);
        focusSynth (-1);
    } ();

    //-- Phase 41: a released key lets go of a hosted instrument ---------------
    // The report: 选中外部音源后，电脑键盘输入的 MIDI 会一直持续，音源一直响.
    //
    // The engine kept a pitch → synth table so a note-off — which carries
    // nothing but a pitch — could find the instrument that started it. That
    // table was only written at the bottom of the oscillator branch, and a
    // hosted instrument returns long before reaching it, so its releases
    // arrived with no slot id and were dropped. Every key left the VST holding
    // its note; nothing but a restart could stop it.
    [&]
    {
        juce::PluginDescription testSynth;

        for (const auto& entry : plugins.getLibrary())
            if (entry.isInstrument && entry.description.name.containsIgnoreCase ("Test Synth"))
                testSynth = entry.description;

        if (testSynth.name.isEmpty())
        {
            check (utf8 ("Phase 41 需要内置的 Test Synth"), false, utf8 ("没找到"));
            return;
        }

        // "选中的是一块 VST 卡片" has to mean "the keys go there". Otherwise the
        // spotlight is decoration and the keys play whatever the layer points
        // at, which looks exactly like the wrong synth answering.
        {
            const int vstSlot = rack.addSynth();

            rack.replaceWithPlugin (vstSlot, testSynth, 4242);
            focusSynth (vstSlot);

            const int target = getLiveTargetSlot();

            rack.replaceWithNative (vstSlot, Waveform::saw);
            reconcilePlugins();                 // instance 4242 has no owner now
            rack.removeSlot (vstSlot);
            focusSynth (-1);

            check (utf8 ("选中的 VST 卡片就是键盘打向的那块音源"),
                   target == vstSlot,
                   "target " + juce::String (target) + utf8 (" / 卡片 ")
                     + juce::String (vstSlot));
        }

        //-- And the release really reaches the plugin ------------------------
        // With the bundled instrument, not a fake id: "the note-off arrived"
        // is only true if the synth's own gate closed, which is audible.
        PluginHost host;
        host.setPlayConfig (44100.0, 512);

        juce::String loadError;
        const int instance = host.load (testSynth, &loadError);

        LayerStack layers;
        RackModel  hostRack;

        layers.addLayer (utf8 ("外部音源"), palette::layer1);
        const int liveSlot = hostRack.addPluginSynth (testSynth, instance);

        layers.setLayerOutputSlot (0, liveSlot);
        layers.setLayerLevelDb (0, 0.0f);
        layers.setLayerPan (0, 0.0f);

        PlaybackEngine engine;
        engine.setSampleRate (44100.0);
        engine.setPluginHost (&host);
        engine.prepareToPlay (44100.0, 512);
        engine.publishSchedule (layers, hostRack, 120.0, 0.0, nullptr);
        engine.resetMixState();

        auto renderLiveBlocks = [&engine] (int blocks)
        {
            juce::AudioBuffer<float> out;
            out.setSize (2, 4096);

            float peak = 0.0f;

            for (int i = 0; i < blocks; ++i)
            {
                out.clear();
                engine.renderLive (out, 4096);
                peak = juce::jmax (peak, out.getMagnitude (0, 0, 4096));
            }

            return peak;
        };

        engine.noteOnLive (liveSlot, 69, 0.9f);
        engine.beginLiveBlock();

        const float heldPeak = renderLiveBlocks (1);

        check (utf8 ("按下的键让 VST 音源发声"),
               instance > 0 && heldPeak > 0.01f,
               "instance " + juce::String (instance) + utf8 (" / peak ")
                 + juce::String (heldPeak, 4)
                 + (loadError.isNotEmpty() ? " / " + loadError : juce::String()));

        // The fixture closes its gate on the note-off in the same block, so
        // "silent immediately" is the sharpest possible statement of "the
        // note-off arrived at all".
        engine.noteOffLive (69);
        engine.beginLiveBlock();

        const float releasedPeak = renderLiveBlocks (1);

        check (utf8 ("松开按键，同一个 block 之内 VST 就闭嘴了"),
               releasedPeak < 1.0e-5f,
               "peak " + juce::String (releasedPeak, 6));

        // And the instrument is handed back when its tail is over. A held list
        // that still names the key would keep it engaged for ever — which is
        // the same drone, one layer down (6 s of tail at 44.1 kHz, then the
        // slot must go back in the pool).
        const float afterTailPeak = renderLiveBlocks (70);

        check (utf8 ("...而且尾音结束后音源被交还，引擎不再认为自己按着东西"),
               afterTailPeak < 1.0e-5f && ! engine.hasLiveNotes(),
               "peak " + juce::String (afterTailPeak, 6)
                 + (engine.hasLiveNotes() ? utf8 (" / 仍然占着") : utf8 (" / 已交还")));

        engine.setPluginHost (nullptr);
    } ();

    //-- Phase 42: four reports -------------------------------------------------
    [&]
    {
        juce::PluginDescription testSynth;

        for (const auto& entry : plugins.getLibrary())
            if (entry.isInstrument && entry.description.name.containsIgnoreCase ("Test Synth"))
                testSynth = entry.description;

        //-- (1) 插件窗口在前台时，空格和电脑键盘还得能弹 ---------------------
        // The window is a separate top-level component: without a forwarder on
        // it, every key the user presses "in the synth" is the synth's alone.
        if (testSynth.name.isNotEmpty())
        {
            const int instanceId = plugins.load (testSynth);

            openPluginEditor (instanceId, "Atomic Test Synth");

            auto window = pluginWindows.find (instanceId);
            const bool opened = instanceId > 0 && window != pluginWindows.end()
                                  && window->second != nullptr;

            const bool forwards = opened && window->second->hasKeyForwarder();

            const bool wasEngaged = audio.isTransportEngaged();
            const bool claimed = pluginForwarder.keyPressed (
                                     juce::KeyPress (juce::KeyPress::spaceKey),
                                     opened ? window->second.get() : nullptr);

            const bool moved = audio.isTransportEngaged() != wasEngaged;

            if (audio.isTransportEngaged())
                stopTransport();               // leave the session as we found it

            check (utf8 ("插件窗口开着时，空格仍然开始/停止走带"),
                   opened && forwards && claimed && moved,
                   opened ? (forwards ? utf8 ("窗口已接管按键") : utf8 ("窗口没有按键转发"))
                          : utf8 ("窗口没打开（本次会话没有桌面？）"));

            if (window != pluginWindows.end())
                pluginWindows.erase (window);

            reconcilePlugins();
        }
        else
        {
            check (utf8 ("Phase 42 需要内置的 Test Synth"), false, utf8 ("没找到"));
        }

        //-- (2)+(3) 创建自动化：一个母菜单，里面才有音源参数 -----------------
        // The ids are the contract between the menu and the click handler, so
        // this exercises the handler with the exact ids the submenu hands out.
        const int menuSlot = rack.addSynth();
        auto*     menuCard = rack.findSlot (menuSlot);

        if (menuCard == nullptr)
        {
            check (utf8 ("Phase 42 需要一个机架槽"), false, "addSynth failed");
            return;
        }

        const bool pluginItem = applySlotAutomationChoice (1 + kSlotAutoParamBase, menuSlot, 1);
        menuCard = rack.findSlot (menuSlot);

        check (utf8 ("「创建自动化 ▸ 音源参数」真的建出一行音源参数曲线"),
               pluginItem && menuCard != nullptr && menuCard->drivesPluginParam()
                 && menuCard->getPluginParam() == 0
                 && menuCard->automationTarget == SlotAutomationTarget::plugin
                 && menuCard->getPluginAutomation().isEnabled()
                 && automation.contains (AutomationOwner::slot, menuSlot,
                                         (int) SlotAutomationTarget::plugin),
               menuCard != nullptr ? (utf8 ("参数 ") + juce::String (menuCard->getPluginParam())
                                        + (menuCard->getPluginAutomation().isEnabled() ? utf8 (" / 曲线已开")
                                                                                       : utf8 (" / 曲线没开")))
                                   : juce::String ("卡没了"));

        // The two rows that were there before are inside the same submenu now,
        // under different ids — the dispatch has to still find them.
        const bool filterItem = applySlotAutomationChoice (1, menuSlot, 1);
        const bool levelItem  = applySlotAutomationChoice (2, menuSlot, 1);

        check (utf8 ("母菜单里的滤波器 / 音量还走原来那条路"),
               filterItem && levelItem
                 && automation.contains (AutomationOwner::slot, menuSlot,
                                         (int) SlotAutomationTarget::filter)
                 && automation.contains (AutomationOwner::slot, menuSlot,
                                         (int) SlotAutomationTarget::level),
               utf8 ("轨道数 ") + juce::String (automation.getNumTracks()));

        // The rack card builds the same submenu with its own id base (30), so
        // the same parameter has to be reachable there too.
        const bool cardItem = applySlotAutomationChoice (30 + kSlotAutoParamBase, menuSlot, 30);

        check (utf8 ("音源卡片右键用另一套 id 也能建同样的曲线"), cardItem,
               utf8 ("id ") + juce::String (30 + kSlotAutoParamBase));

        automation.removeTracksForOwner (AutomationOwner::slot, menuSlot);
        rack.removeSlot (menuSlot);

        //-- (3a) 它还得能存进工程文件 ----------------------------------------
        // A curve that only exists until the next save is not automation. v18
        // adds the third lane and the slot's `pluginparam`; a v17 file has
        // neither and must still open (tested by the older round-trip above).
        {
            LayerStack ts;
            RackModel  rs;
            const int  sid = rs.getFirstSlotId();
            auto*      saved = rs.findSlot (sid);

            if (saved == nullptr)
            {
                check (utf8 ("Phase 42 需要一个机架槽"), false, "no slot");
                return;
            }

            // A project with no layer is not a project: the parser refuses one,
            // and the point here is the slot's third lane, not that rule.
            ts.addLayer (utf8 ("参数"), palette::layer1);
            ts.setLayerOutputSlot (0, sid);

            saved->setPluginParam (3);
            rs.setAutomationTarget (sid, SlotAutomationTarget::plugin);
            rs.setAutomationExpanded (sid, true);

            auto& savedLane = saved->getPluginAutomation();
            savedLane.resetFlat (0.4f, 8.0);
            savedLane.addPoint (5.0, 0.9f, 8.0);
            savedLane.setEnabled (true);

            const auto xml = project::toXmlText (ts, rs, 120.0, nullptr, nullptr);

            LayerStack ts2;
            RackModel  rs2;
            double     bpmBack = 0.0;
            juce::String error;

            const bool parsed = project::fromXmlText (xml, ts2, rs2, bpmBack, error);
            const auto* restored = rs2.findSlot (sid);

            check (utf8 ("音源参数曲线存得进工程文件，也读得回来"),
                   parsed && restored != nullptr
                     && restored->getPluginParam() == 3
                     && restored->automationTarget == SlotAutomationTarget::plugin
                     && restored->automationExpanded
                     && restored->getPluginAutomation().isEnabled()
                     && restored->getPluginAutomation().hasSameShapeAs (savedLane),
                   utf8 ("参数 ") + juce::String (restored != nullptr ? restored->getPluginParam() : -99)
                     + utf8 (" / ") + juce::String (restored != nullptr
                                                      ? (int) restored->getPluginAutomation().getPoints().size() : 0)
                     + utf8 (" 点") + (error.isNotEmpty() ? " / " + error : juce::String()));
        }

        //-- (3) 这条曲线要真的驱动插件，不是只存在模型里 ----------------------
        if (testSynth.name.isEmpty())
            return;

        PluginHost host;
        host.setPlayConfig (44100.0, 512);

        const int instance = host.load (testSynth);

        LayerStack t;
        RackModel  r;
        const int slotId = r.addPluginSynth (testSynth, instance);

        t.addLayer (utf8 ("音源参数"), palette::layer1);
        t.setLayerOutputSlot (0, slotId);
        t.setLayerLevelDb (0, 0.0f);
        t.setLayerPan (0, 0.0f);
        t.getLayer (0)->addNote ({ 0, 60, 0.0, 8.0, 0.9f });

        auto* slot = r.findSlot (slotId);
        slot->setPluginParam (0);                    // the fixture's one parameter
        slot->getPluginAutomation().resetFlat (1.0f, 8.0);
        slot->getPluginAutomation().addPoint (4.0, 0.0f, 8.0);

        PlaybackEngine engine;
        engine.setSampleRate (44100.0);
        engine.setPluginHost (&host);
        engine.prepareToPlay (44100.0, 512);
        engine.resetMixState();

        // Two renders of the same two seconds: one with the slot's curve on, one
        // without. "The parameter moved" is not audible; "the second half is
        // quieter than the first" is.
        auto renderHalves = [&] (bool laneOn)
        {
            slot->getPluginAutomation().setEnabled (laneOn);
            engine.publishSchedule (t, r, 120.0, 0.0, nullptr);
            engine.resetMixState();

            juce::AudioBuffer<float> buffer;
            buffer.setSize (2, 44100 * 2);
            buffer.clear();

            engine.renderWindow (0.0, 44100 * 2, buffer, nullptr);

            return std::pair<float, float> { buffer.getRMSLevel (0, 0, 44100),
                                             buffer.getRMSLevel (0, 44100, 44100) };
        };

        const auto flat  = renderHalves (false);
        const auto swept = renderHalves (true);

        engine.publishSchedule (t, r, 120.0, 0.0, nullptr);

        auto published = engine.getSchedule();
        int  publishedParam = -2, publishedPlugin = 0;

        if (published != nullptr)
            for (const auto& curve : published->pluginParams)
                if (curve.pluginId == instance)
                {
                    publishedParam  = curve.paramIndex;
                    publishedPlugin = curve.pluginId;
                }

        const float flatRatio  = flat.first  > 1.0e-5f ? flat.second  / flat.first  : 0.0f;
        const float sweptRatio = swept.first > 1.0e-5f ? swept.second / swept.first : 1.0f;

        check (utf8 ("音源参数曲线被发布到引擎，并且真的把插件参数压下去了"),
               instance > 0 && publishedParam == 0 && publishedPlugin == instance
                 && flatRatio > 0.8f && sweptRatio < 0.6f,
               utf8 ("发布参数 ") + juce::String (publishedParam)
                 + utf8 (" / 前后半比 ") + juce::String (flatRatio, 2)
                 + utf8 (" → ") + juce::String (sweptRatio, 2));

        engine.setPluginHost (nullptr);

        //-- (4) 电平计要自己落回零 ------------------------------------------
        // The bar used to be a maximum that never came down: every render path
        // stored peaks with `jmax`, and the only decay was the idle branch of
        // the device callback — which a rolling-but-silent transport never
        // reached. The fall is now per block and expressed in dB per second, so
        // it is the same fall at any buffer size.
        {
            LayerStack mt;
            RackModel  mr;
            const int meterSlot = mr.getFirstSlotId();

            mt.addLayer (utf8 ("电平"), palette::layer1);
            mt.setLayerOutputSlot (0, meterSlot);
            mt.getLayer (0)->addNote ({ 0, 60, 0.0, 4.0, 0.9f });

            PlaybackEngine me;
            me.setSampleRate (44100.0);
            me.prepareToPlay (44100.0, 512);
            me.publishSchedule (mt, mr, 120.0, 0.0, nullptr);

            auto arm = [&]
            {
                // A block, not a window: the block path is the one that feeds the
                // meters (a window is an offline probe, not a listening session).
                me.resetMixState();
                me.setPositionBeats (0.5);

                juce::AudioBuffer<float> buffer (2, 512);
                buffer.clear();

                me.renderNextBlock (buffer, 512);

                return juce::jmax (me.getMasterPeak (0), me.getMasterPeak (1));
            };

            const float loud = arm();

            // One second of fall, measured in two different block sizes. The
            // law is -50 dB/s, so both should land on about 0.32 % of the peak.
            const auto fallOneSecond = [&] (int blockSize)
            {
                const float before = arm();
                const int   blocks = (int) std::lround (44100.0 / (double) blockSize);

                for (int i = 0; i < blocks; ++i)
                    me.decayMeters (blockSize);

                const float after = me.getMasterPeak (0);
                return before > 1.0e-6f ? after / before : 0.0f;
            };

            const float fall512 = fallOneSecond (512);
            const float fall256 = fallOneSecond (256);
            const float expected = (float) std::pow (10.0, -2.5);

            check (utf8 ("电平每秒落 50 dB，且与缓冲大小无关"),
                   loud > 0.01f
                     && std::abs (fall512 - expected) < expected * 0.10f
                     && std::abs (fall256 - fall512) < expected * 0.05f,
                   utf8 ("峰值 ") + juce::String (loud, 4)
                     + utf8 (" / 512 ") + juce::String (fall512, 5)
                     + utf8 (" / 256 ") + juce::String (fall256, 5)
                     + utf8 (" / 应为 ") + juce::String (expected, 5));

            // And it really reaches zero, which is what "不归零" was about.
            arm();

            for (int i = 0; i < 400; ++i)          // ~4.6 s of 512-frame blocks
                me.decayMeters (512);

            check (utf8 ("安静下来之后电平回到零，窗口也可以停止重绘"),
                   me.getMasterPeak (0) < 1.0e-4f && ! me.hasMeters(),
                   utf8 ("peak ") + juce::String (me.getMasterPeak (0), 6));
        }
    } ();

    //-- Phase 43: five reports -------------------------------------------------
    [&]
    {
        //-- (1) 混音台在前台时，空格还得能开始/停止走带 ----------------------
        // Same hole Phase 42 plugged for a plugin's editor: the console is its
        // own top-level window, so its keys never reach this component.
        if (! isMixerWindowOpen())
            toggleMixerWindow();

        const bool mixerOpen = isMixerWindowOpen() && mixerWindow != nullptr;
        const bool forwards  = mixerOpen && mixerWindow->hasKeyForwarder();

        const bool wasEngaged = audio.isTransportEngaged();
        const bool claimed    = forwards && pluginForwarder.keyPressed (
                                            juce::KeyPress (juce::KeyPress::spaceKey),
                                            mixerWindow.get());
        const bool moved      = audio.isTransportEngaged() != wasEngaged;

        if (audio.isTransportEngaged())
            stopTransport();

        if (isMixerWindowOpen())
            toggleMixerWindow();               // leave the session as we found it

        check (utf8 ("混音台窗口在前台时，空格仍然开始/停止走带"),
               mixerOpen && forwards && claimed && moved,
               ! mixerOpen ? utf8 ("窗口没打开")
                           : (forwards ? utf8 ("窗口已接管按键") : utf8 ("窗口没有按键转发")));

        //-- (2) 文件树上沿的起始高度 -----------------------------------------
        // 250 design px showed a header and two rows. The check is on the stored
        // height, not on the component: a component's height depends on the
        // window it happens to be in, this number IS the setting.
        check (utf8 ("文件树上沿的起始高度抬高了，图层列表仍留着地板"),
               getFilePaneHeight() >= ui::px (370)
                 && filePanel.getHeight() >= ui::px (330)
                 && layerPanel.getHeight() >= ui::px (150),
               utf8 ("文件栏 ") + juce::String (getFilePaneHeight())
                 + utf8 (" / 图上 ") + juce::String (filePanel.getHeight())
                 + utf8 (" / 图层 ") + juce::String (layerPanel.getHeight()));

        //-- (3) 琴键和电脑键盘必须打向同一台音源 -----------------------------
        {
            auto* layer = stack.getActiveLayer();

            if (layer == nullptr)
            {
                check (utf8 ("Phase 43 需要一个活动图层"), false, "no layer");
                return;
            }

            const int layerSlot = routing::layerSlotId (*layer, rack);
            const int spotSlot  = rack.addSynth();      // a second instrument

            if (spotSlot < 0)
            {
                check (utf8 ("Phase 43 需要一个第二音源"), false, "addSynth failed");
                return;
            }

            focusSynth (spotSlot);

            int  heard = -1;
            auto saved = pianoRoll.onAuditionRequested;

            pianoRoll.onAuditionRequested = [&heard] (int slotId, int, float, bool)
            {
                heard = slotId;
            };

            pianoRoll.auditionPitchOnActiveLayer (60, false);
            pianoRoll.onAuditionRequested = saved;

            const int keyboardTarget = getLiveTargetSlot();

            focusSynth (-1);

            check (utf8 ("选中另一台音源时，点钢琴窗琴键和敲电脑键盘打向同一台音源"),
                   spotSlot != layerSlot && heard == spotSlot && keyboardTarget == spotSlot,
                   utf8 ("琴键 ") + juce::String (heard)
                     + utf8 (" / 键盘 ") + juce::String (keyboardTarget)
                     + utf8 (" / 图层默认 ") + juce::String (layerSlot));

            rack.removeSlot (spotSlot);
        }

        //-- (4) 效果器可以换位置，换的是真正的处理顺序 -----------------------
        {
            MixerModel mm;
            LayerStack t;
            RackModel  r;

            const int sid = r.getFirstSlotId();

            t.addLayer (utf8 ("效果器顺序"), palette::layer1);
            t.setLayerOutputSlot (0, sid);
            t.getLayer (0)->addNote ({ 0, 60, 0.0, 8.0, 0.9f });

            // Into channel 1, so the inserts have something to process: a synth
            // that bypasses the console never meets an insert.
            r.setMixerChannel (sid, 0);

            PlaybackEngine e;
            e.setSampleRate (44100.0);
            e.prepareToPlay (44100.0, 512);

            auto renderRms = [&]
            {
                e.publishSchedule (t, r, 120.0, 0.0, &mm);
                e.resetMixState();

                juce::AudioBuffer<float> buffer (2, 88200);
                buffer.clear();
                e.renderWindow (0.0, 88200, buffer, nullptr);

                return buffer.getRMSLevel (0, 0, 88200);
            };

            // 驱动 → 低通 and 低通 → 驱动 are two different sounds from the same
            // two boxes. If moving one did nothing, these numbers would match.
            mm.setInsertType (0, 0, fx::Type::drive);
            mm.setInsertType (0, 1, fx::Type::lowPass);
            mm.setInsertParam (0, 0, 0.95f, 0.0f);
            mm.setInsertParam (0, 1, 0.25f, 0.0f);

            const float driveFirst = renderRms();

            const bool chainMoved = mm.moveInsert (0, 0, 1);

            const float filterFirst = renderRms();

            const float biggest = juce::jmax (driveFirst, filterFirst);
            const float diff    = biggest > 1.0e-6f ? std::abs (driveFirst - filterFirst) / biggest : 0.0f;

            check (utf8 ("效果器换位置以后，处理链的顺序也换了（声音跟着变）"),
                   chainMoved
                     && mm.getInsert (0, 0).type == fx::Type::lowPass
                     && mm.getInsert (0, 1).type == fx::Type::drive
                     && driveFirst > 1.0e-4f && filterFirst > 1.0e-4f
                     && diff > 0.02f,
                   utf8 ("驱动在前 ") + juce::String (driveFirst, 4)
                     + utf8 (" / 低通在前 ") + juce::String (filterFirst, 4)
                     + utf8 (" / 差 ") + juce::String (diff * 100.0f, 1) + "%");
        }

        //-- (5) 四个控件各管自己的自动化，不再挂通道菜单 ---------------------
        {
            const int sid = rack.getFirstInstrumentSlotId();

            if (sid < 0)
            {
                check (utf8 ("Phase 43 需要一个音源"), false, "no instrument");
                return;
            }

            rack.setMixerChannel (sid, 0);

            struct MenuSummary
            {
                juce::StringArray texts;
                bool hasSubmenu = false;
                bool offTopic   = false;
            };

            auto describe = [&] (MixerPanel::StripControl control)
            {
                juce::PopupMenu menu;
                mixerPanel.fillControlMenu (menu, 0, control);

                MenuSummary summary;

                for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
                {
                    const auto& item = it.getItem();

                    if (item.isSeparator)
                        continue;

                    if (item.subMenu != nullptr)
                        summary.hasSubmenu = true;

                    summary.texts.add (item.text);

                    // The channel menu items that used to live in here.
                    if (item.text.contains (utf8 ("通道")) || item.text.contains (utf8 ("删除"))
                        || item.text.contains (utf8 ("重置")))
                        summary.offTopic = true;
                }

                return summary;
            };

            const auto fader = describe (MixerPanel::StripControl::fader);
            const auto pan   = describe (MixerPanel::StripControl::pan);
            const auto mute  = describe (MixerPanel::StripControl::mute);
            const auto solo  = describe (MixerPanel::StripControl::solo);

            const bool eachHasItsOwn =
                   fader.texts.joinIntoString ("|").contains (utf8 ("创建音量自动化"))
                && pan.texts.joinIntoString ("|").contains (utf8 ("创建声相自动化"))
                && mute.texts.joinIntoString ("|").contains (utf8 ("创建静音自动化"))
                && solo.texts.joinIntoString ("|").contains (utf8 ("创建独奏自动化"));

            const bool clean = ! (fader.hasSubmenu || pan.hasSubmenu || mute.hasSubmenu
                                   || solo.hasSubmenu || fader.offTopic || pan.offTopic
                                   || mute.offTopic || solo.offTopic);

            check (utf8 ("音量 / 声相 / 静音 / 独奏 的右键菜单各只有自己那一行自动化，不再挂通道菜单"),
                   eachHasItsOwn && clean,
                   fader.texts.joinIntoString (utf8 (" / ")));

            // And the line really works: same table the strip and the card use.
            applyStripAutomationChoice (0, MixerPanel::StripControl::mute, 100);

            const bool muteTrack = automation.contains (AutomationOwner::slot, sid,
                                                        (int) SlotAutomationTarget::mute);

            applyStripAutomationChoice (0, MixerPanel::StripControl::solo, 100);

            check (utf8 ("静音 / 独奏 的自动化真的能建出轨道"),
                   muteTrack
                     && automation.contains (AutomationOwner::slot, sid,
                                             (int) SlotAutomationTarget::solo),
                   utf8 ("轨道数 ") + juce::String (automation.getNumTracks()));

            automation.removeTracksForOwner (AutomationOwner::slot, sid);
            rack.findSlot (sid)->automationFor (SlotAutomationTarget::mute).setEnabled (false);
            rack.findSlot (sid)->automationFor (SlotAutomationTarget::solo).setEnabled (false);
        }

        //-- (6) 静音是一道闸 / (7) 声相是加法 / (8) 独奏让别的槽闭嘴 ----------
        {
            LayerStack t;
            RackModel  r;

            const int sid = r.getFirstSlotId();

            t.addLayer (utf8 ("静音"), palette::layer1);
            t.setLayerOutputSlot (0, sid);
            t.getLayer (0)->addNote ({ 0, 60, 0.0, 8.0, 0.9f });

            auto* slot = r.findSlot (sid);

            if (slot == nullptr)
            {
                check (utf8 ("Phase 43 需要一个槽"), false, "no slot");
                return;
            }

            PlaybackEngine e;
            e.setSampleRate (44100.0);
            e.prepareToPlay (44100.0, 512);

            auto renderPeak = [&] (int side)
            {
                e.publishSchedule (t, r, 120.0, 0.0, nullptr);
                e.resetMixState();

                juce::AudioBuffer<float> buffer (2, 88200);
                buffer.clear();
                e.renderWindow (0.0, 88200, buffer, nullptr);

                return buffer.getMagnitude (side, 0, 88200);
            };

            const float openPeak = renderPeak (0);

            //-- (6) mute: a gate at 0.5, not a second volume --------------------
            auto& muteLane = slot->automationFor (SlotAutomationTarget::mute);
            muteLane.resetFlat (0.0f, 8.0);
            muteLane.setEnabled (true);

            const float mutedPeak = renderPeak (0);

            muteLane.setEnabled (false);

            check (utf8 ("静音曲线拉到底就是真的没声音（闸，不是第二个音量）"),
                   openPeak > 0.01f && mutedPeak < 1.0e-4f,
                   utf8 ("开 ") + juce::String (openPeak, 4)
                     + utf8 (" / 关 ") + juce::String (mutedPeak, 6));

            //-- (7) pan: added to the strip's pan, -1..+1 ----------------------
            auto& panLane = slot->automationFor (SlotAutomationTarget::pan);
            panLane.resetFlat (1.0f, 8.0);         // curve 1.0 = hard right
            panLane.setEnabled (true);

            const float leftPeak  = renderPeak (0);
            const float rightPeak = renderPeak (1);

            panLane.setEnabled (false);

            check (utf8 ("声相曲线是加法：拉到右端，左声道就没有了"),
                   rightPeak > 0.01f && leftPeak < rightPeak * 0.2f,
                   utf8 ("左 ") + juce::String (leftPeak, 4)
                     + utf8 (" / 右 ") + juce::String (rightPeak, 4));
        }

        //-- (8) solo ----------------------------------------------------------
        {
            LayerStack t;
            RackModel  r;

            const int a = r.getFirstSlotId();
            const int b = r.addSynth();

            if (b < 0)
            {
                check (utf8 ("Phase 43 需要第二个音源"), false, "addSynth failed");
                return;
            }

            // Two identical sheets on two identical synths: summed, they are one
            // signal at twice the amplitude. Solo one of them and the peak halves
            // — a ratio, not an amplitude, so the oscillator's own level drops out.
            t.addLayer (utf8 ("A"), palette::layer1);
            t.addLayer (utf8 ("B"), palette::layer1);
            t.setLayerOutputSlot (0, a);
            t.setLayerOutputSlot (1, b);
            t.getLayer (0)->addNote ({ 0, 60, 0.0, 8.0, 0.9f });
            t.getLayer (1)->addNote ({ 0, 60, 0.0, 8.0, 0.9f });

            auto* slotA = r.findSlot (a);

            if (slotA == nullptr)
            {
                check (utf8 ("Phase 43 需要一个槽"), false, "no slot");
                return;
            }

            PlaybackEngine e;
            e.setSampleRate (44100.0);
            e.prepareToPlay (44100.0, 512);

            auto renderPeak = [&]
            {
                e.publishSchedule (t, r, 120.0, 0.0, nullptr);
                e.resetMixState();

                juce::AudioBuffer<float> buffer (2, 44100);
                buffer.clear();
                e.renderWindow (0.0, 44100, buffer, nullptr);

                return buffer.getMagnitude (0, 0, 44100);
            };

            const float both = renderPeak();

            auto& soloLane = slotA->automationFor (SlotAutomationTarget::solo);
            soloLane.resetFlat (1.0f, 8.0);
            soloLane.setEnabled (true);

            const float soloed = renderPeak();

            soloLane.setEnabled (false);

            const float ratio = both > 1.0e-5f ? soloed / both : 0.0f;

            check (utf8 ("独奏曲线打开以后，没被独奏的音源闭嘴（峰值减半）"),
                   both > 0.01f && ratio > 0.40f && ratio < 0.60f,
                   utf8 ("两台 ") + juce::String (both, 4)
                     + utf8 (" / 独奏一台 ") + juce::String (soloed, 4)
                     + utf8 (" / 比 ") + juce::String (ratio, 2));
        }
    } ();

    //-- Phase 29: a block is a window — trim both ends, fade both ends ------
    [&]
    {
        auto* layer = stack.getActiveLayer();

        if (layer == nullptr)
        {
            check ("Phase 29 needs a layer", false, "no active layer");
            return;
        }

        const int index = stack.getActiveLayerIndex();
        const int a = pianoRoll.addSampleAt (80.0, 55, phase27A);
        const int b = pianoRoll.addSampleAt (86.0, 62, phase27A);

        auto* clip = layer->findSample (a);

        if (clip == nullptr || b <= 0)
        {
            check ("Phase 29 clip fixtures exist", false, "addSampleAt failed");
            return;
        }

        const double natural = clip->lengthBeats;     // 1.0 s at 120 BPM = 2.0 beats
        const double start   = clip->startBeat;

        //-- 3: the tail stops at the end of the file -------------------------
        pianoRoll.simulateSampleTrim (a, start + 12.0, true);
        const double afterLonger = clip->lengthBeats;

        check ("A block cannot be dragged longer than the file it plays",
               std::abs (afterLonger - natural) < 1.0e-6,
               utf8 ("自然 ") + juce::String (natural, 3) + utf8 (" 拖到 12 拍后 → ")
                 + juce::String (afterLonger, 3));

        pianoRoll.simulateSampleTrim (a, start + natural * 0.5, true);
        const double afterShorter = clip->lengthBeats;

        pianoRoll.simulateSampleTrim (a, start + 12.0, true);
        const double backOut = clip->lengthBeats;

        check ("...but a block at full length can still be shortened, then back out to the limit",
               afterShorter < natural - 1.0e-6 && std::abs (backOut - natural) < 1.0e-6,
               juce::String (natural, 3) + utf8 (" → ") + juce::String (afterShorter, 3)
                 + utf8 (" → ") + juce::String (backOut, 3));

        //-- 3: the head trims the WINDOW, not the start of the file ---------
        const double endBefore    = clip->endBeat();
        const double offsetBefore = clip->sourceOffsetSeconds;

        pianoRoll.simulateSampleTrim (a, start + 0.5, false);

        const double expectedOffset = 0.5 * 60.0 / juce::jmax (1.0, audio.getBpm());

        check ("Trimming the head moves the window and leaves the tail where it was",
               std::abs (clip->sourceOffsetSeconds - expectedOffset) < 1.0e-6
                 && std::abs (clip->endBeat() - endBefore) < 1.0e-6
                 && std::abs (clip->startBeat - (start + 0.5)) < 1.0e-6,
               "offset " + juce::String (offsetBefore, 4) + utf8 (" → ")
                 + juce::String (clip->sourceOffsetSeconds, 4) + utf8 (" 秒, 尾 ")
                 + juce::String (clip->endBeat(), 3));

        pianoRoll.simulateSampleTrim (a, start - 8.0, false);

        check ("The head cannot be dragged back past the first sample of the file",
               std::abs (clip->sourceOffsetSeconds) < 1.0e-6
                 && std::abs (clip->startBeat - start) < 1.0e-6,
               "offset " + juce::String (clip->sourceOffsetSeconds, 4) + utf8 (", 起点 ")
                 + juce::String (clip->startBeat, 3));

        //-- 4: two independent fades ----------------------------------------
        pianoRoll.simulateSampleFade (a, false, 0.5);
        pianoRoll.simulateSampleFade (a, true, 1.0);

        check ("Both fades can be set independently",
               std::abs (clip->fadeInBeats - 0.5) < 1.0e-9
                 && std::abs (clip->fadeOutBeats - 1.0) < 1.0e-9,
               utf8 ("淡入 ") + juce::String (clip->fadeInBeats, 3)
                 + utf8 (", 淡出 ") + juce::String (clip->fadeOutBeats, 3));

        pianoRoll.simulateSampleFade (a, false, 99.0);
        const double clampedFade = clip->fadeInBeats;
        pianoRoll.simulateSampleFade (a, false, 0.5);

        check ("A fade cannot be longer than the block it lives in",
               std::abs (clampedFade - clip->lengthBeats) < 1.0e-9,
               juce::String (clampedFade, 3) + utf8 (" vs 块长 ")
                 + juce::String (clip->lengthBeats, 3));

        // The handle is a pixel position derived from the fade: if these two
        // disagree, the thing you grab is not the thing you set.
        const auto rect    = pianoRoll.getSampleScreenBounds (index, a);
        const float beatW  = (float) (rect.getWidth() / juce::jmax (1.0e-9, clip->lengthBeats));
        const float xIn    = pianoRoll.getSampleFadeHandleX (index, a, false);
        const float xOut   = pianoRoll.getSampleFadeHandleX (index, a, true);

        check ("Each fade handle sits on the fade it sets",
               std::abs (xIn  - (rect.getX() + 0.5f * beatW)) < 0.75f
                 && std::abs (xOut - (rect.getRight() - 1.0f * beatW)) < 0.75f
                 && xIn < xOut,
               "in " + juce::String (xIn, 1) + "/" + juce::String (rect.getX() + 0.5f * beatW, 1)
                 + ", out " + juce::String (xOut, 1) + "/" + juce::String (rect.getRight() - beatW, 1));

        //-- 2: one sweep, both kinds ----------------------------------------
        const int noteId = pianoRoll.addNoteAt (86.0, 62);
        const auto noteRect = pianoRoll.getNoteScreenBounds (index, noteId);
        const auto clipRect = pianoRoll.getSampleScreenBounds (index, b);

        if (noteId <= 0 || noteRect.isEmpty() || clipRect.isEmpty())
        {
            check ("Phase 29 marquee fixtures exist", false, "note or clip has no rectangle");
            return;
        }

        const auto sweep = clipRect.getUnion (noteRect).expanded (2.0f);
        const bool swept = pianoRoll.simulateMarquee (sweep);

        const auto& noteHits = pianoRoll.getSelection();
        const auto& clipHits = pianoRoll.getSampleSelection();

        check ("A marquee grabs notes and clips in one sweep",
               swept
                 && std::find (noteHits.begin(), noteHits.end(), noteId) != noteHits.end()
                 && std::find (clipHits.begin(), clipHits.end(), b) != clipHits.end(),
               utf8 ("音符 ") + juce::String ((int) noteHits.size())
                 + utf8 (", 采样 ") + juce::String ((int) clipHits.size()));

        // Delete has to take the WHOLE mixed selection: keeping half of it would
        // be the worst kind of surprise, because the half left behind is the
        // half you were not looking at.
        const int notesBefore = (int) layer->getNotes().size();
        const int clipsBefore = layer->getSampleCount();

        pianoRoll.keyPressed (juce::KeyPress (juce::KeyPress::deleteKey));

        check ("Delete takes the whole mixed selection, not half of it",
               (int) layer->getNotes().size() == notesBefore - (int) noteHits.size()
                 && layer->getSampleCount() == clipsBefore - (int) clipHits.size()
                 && pianoRoll.getSelection().empty()
                 && pianoRoll.getSampleSelection().empty(),
               utf8 ("音符 ") + juce::String (notesBefore) + "→"
                 + juce::String ((int) layer->getNotes().size())
                 + utf8 (", 采样 ") + juce::String (clipsBefore) + "→"
                 + juce::String (layer->getSampleCount()));

        //-- 1: the greyed grid is not a dead end ----------------------------
        const int slotId = rack.getFirstInstrumentSlotId();
        focusSynth (slotId);

        const bool chipShown = ! pianoRoll.getSpotlightExitChipBounds().isEmpty();
        const bool pressed   = pianoRoll.keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));

        check ("Esc clears the spotlight, so the highlight can always come back off",
               slotId >= 0 && chipShown && pressed
                 && focusedSlotId < 0 && pianoRoll.getFocusSlotId() < 0
                 && pianoRoll.getSpotlightExitChipBounds().isEmpty(),
               juce::String (chipShown ? utf8 ("有") : utf8 ("无")) + utf8 (" chip, 清除后 focus ")
                 + juce::String (pianoRoll.getFocusSlotId()));

        // The rack's own item was a one-way door: it set the spotlight and
        // nothing could unset it from there either.
        applyRackCardChoice (34, slotId);
        const bool lit = focusedSlotId == slotId;
        applyRackCardChoice (34, slotId);

        check (utf8 ("The rack's 高亮 item is a toggle now"),
               lit && focusedSlotId < 0,
               juce::String (lit ? utf8 ("亮起") : utf8 ("没亮")) + utf8 (", 再点一次 → focus ")
                 + juce::String (focusedSlotId));
    } ();

    //-- Phase 29: v15 keeps the window offset and the fades ------------------
    [&]
    {
        LayerStack s1;
        RackModel  r1;

        s1.addLayer (utf8 ("窗口"), palette::layer1);

        SampleEvent clip;
        clip.name        = "window";
        clip.startBeat   = 2.0;
        clip.lengthBeats = 1.5;
        clip.pitch       = 60;
        clip.laneSpan    = 2;
        clip.sourceOffsetSeconds = 0.375;
        clip.fadeInBeats  = 0.25;
        clip.fadeOutBeats = 0.5;

        s1.getLayer (0)->addSample (clip);

        const auto xml = project::toXmlText (s1, r1, 120.0);

        LayerStack s2;
        RackModel  r2;
        double bpm2 = 0.0;
        juce::String error;

        const bool ok = project::fromXmlText (xml, s2, r2, bpm2, error, nullptr, nullptr, nullptr);

        const auto* back = (s2.getLayer (0) != nullptr && s2.getLayer (0)->getSampleCount() == 1)
                             ? &s2.getLayer (0)->getSamples().front() : nullptr;

        check ("A v15 project keeps the clip window and its fades",
               ok && back != nullptr
                 && std::abs (back->sourceOffsetSeconds - 0.375) < 1.0e-9
                 && std::abs (back->fadeInBeats - 0.25) < 1.0e-9
                 && std::abs (back->fadeOutBeats - 0.5) < 1.0e-9,
               ok && back != nullptr
                 ? "offset " + juce::String (back->sourceOffsetSeconds, 4)
                     + ", fade " + juce::String (back->fadeInBeats, 3) + "/"
                     + juce::String (back->fadeOutBeats, 3)
                 : error);
    } ();

    //-- Phase 30: the waveform is CUT by a trim, never rescaled ---------------
    [&]
    {
        auto* layer = stack.getActiveLayer();

        if (layer == nullptr)
        {
            check ("Phase 30 needs a layer", false, "no active layer");
            return;
        }

        const int index = stack.getActiveLayerIndex();
        const int id    = pianoRoll.addSampleAt (92.0, 50, phase27A);   // 1 s @ 120 BPM = 2 beats

        auto* clip = layer->findSample (id);

        if (clip == nullptr)
        {
            check ("Phase 30 clip fixture exists", false, "addSampleAt failed");
            return;
        }

        const double fileBeats   = SampleEvent::beatsForSeconds (1.0, audio.getBpm());
        const double startBefore = clip->startBeat;

        // A block that outruns its file: exactly what dropping a short file onto
        // the grid produces, because the drop rounds the length UP to the grid.
        clip->lengthBeats = fileBeats + 1.5;

        const float widthBefore = pianoRoll.getSampleScreenBounds (index, id).getWidth();
        const int   colsBefore  = pianoRoll.sampleWaveColumnCount (id);

        check ("A block longer than its file shows a wave that stops at the file",
               colsBefore > 0 && (float) colsBefore < widthBefore - 1.0f,
               utf8 ("波 ") + juce::String (colsBefore) + utf8 (" px / 块 ")
                 + juce::String (widthBefore, 1) + " px");

        //-- 3: the trim cuts the picture, it does not redraw it --------------
        pianoRoll.simulateSampleTrim (id, startBefore + fileBeats + 0.5, true);

        const float widthAfter = pianoRoll.getSampleScreenBounds (index, id).getWidth();
        const int   colsAfter  = pianoRoll.sampleWaveColumnCount (id);

        // The block got narrower; the wave did NOT. That is the difference
        // between "cut" and "squeeze", and it is the whole point: the same audio
        // has to keep the same pixels so a trim cannot be mistaken for a
        // different sound.
        check ("Trimming the tail cuts the waveform instead of squeezing it",
               widthAfter < widthBefore - 1.0f && colsAfter == colsBefore,
               utf8 ("块 ") + juce::String (widthBefore, 1) + utf8 ("→")
                 + juce::String (widthAfter, 1) + utf8 (" px, 波 ")
                 + juce::String (colsBefore) + utf8 ("→") + juce::String (colsAfter) + " px");

        //-- 2: a trim is reversible ------------------------------------------
        pianoRoll.simulateSampleTrim (id, startBefore + 0.5, false);   // head in by half a beat

        const double trimmedStart = clip->startBeat;
        const bool   restored     = pianoRoll.restoreSampleLength (id);

        const float widthRestored = pianoRoll.getSampleScreenBounds (index, id).getWidth();
        const int   colsRestored  = pianoRoll.sampleWaveColumnCount (id);

        check (utf8 ("恢复原始长度 puts the window back on the whole file"),
               restored
                 && std::abs (clip->sourceOffsetSeconds) < 1.0e-9
                 && std::abs (clip->lengthBeats - fileBeats) < 1.0e-6
                 && std::abs (clip->startBeat - startBefore) < 1.0e-6,
               utf8 ("起点 ") + juce::String (startBefore, 3) + utf8 ("→")
                 + juce::String (trimmedStart, 3) + utf8 ("→") + juce::String (clip->startBeat, 3)
                 + utf8 (", offset ") + juce::String (clip->sourceOffsetSeconds, 4)
                 + utf8 (", 长 ") + juce::String (clip->lengthBeats, 3));

        // ...and the restored block is exactly the file: the wave fills it edge
        // to edge, which is the 1:1 case the fixed scale has to hit dead on.
        check ("A restored block is exactly as wide as its audio",
               colsRestored > 0 && std::abs ((float) colsRestored - widthRestored) <= 1.0f,
               utf8 ("波 ") + juce::String (colsRestored) + utf8 (" px / 块 ")
                 + juce::String (widthRestored, 1) + " px");

        pianoRoll.setSampleSelection ({ id });
        pianoRoll.deleteSelectedSamples();
    } ();

    //-- Phase 30: the spotlight highlights, it does not veil -----------------
    [&]
    {
        const int slotId = rack.getFirstInstrumentSlotId();

        if (slotId < 0)
        {
            check ("Phase 30 needs an instrument to focus", false, "no instrument slot");
            return;
        }

        // Photographed, not asserted about a flag: the claim is about brightness,
        // so the check measures brightness. Same component, same size, same
        // state except the one variable under test.
        const auto previousBounds = pianoRoll.getBounds();
        pianoRoll.setSize (ui::px (900), ui::px (420));

        auto averageBrightness = [&] (const juce::Image& img)
        {
            // The whole frame, not getGridArea() (private): every part of the
            // roll that is not a note is constant between the two shots, so an
            // average over all of it still isolates the one thing that changed.
            const auto area = img.isValid() ? img.getBounds().reduced (ui::px (4))
                                            : juce::Rectangle<int>();

            if (area.isEmpty())
                return 0.0;

            juce::Image::BitmapData px (img, juce::Image::BitmapData::readOnly);

            double sum = 0.0;
            int    n   = 0;

            for (int y = area.getY(); y < area.getBottom(); y += 3)
                for (int x = area.getX(); x < area.getRight(); x += 3)
                {
                    sum += px.getPixelColour (x, y).getBrightness();
                    ++n;
                }

            return n > 0 ? sum / (double) n : 0.0;
        };

        focusSynth (-1);
        const auto  plainShot = pianoRoll.createComponentSnapshot (pianoRoll.getLocalBounds(), true);
        const double plainAvg = averageBrightness (plainShot);

        focusSynth (slotId);
        const auto  litShot = pianoRoll.createComponentSnapshot (pianoRoll.getLocalBounds(), true);
        const double litAvg = averageBrightness (litShot);

        check ("Focusing a synth no longer darkens the piano roll",
               plainAvg > 0.02 && litAvg > plainAvg * 0.92,
               utf8 ("亮度 ") + juce::String (plainAvg, 3) + utf8 (" → ")
                 + juce::String (litAvg, 3));

        //-- 1: and the grid itself is the way out ----------------------------
        const bool dismissed = pianoRoll.dismissSpotlight();

        check ("Clicking the empty grid ends the spotlight",
               dismissed && focusedSlotId < 0 && pianoRoll.getFocusSlotId() < 0,
               juce::String (dismissed ? utf8 ("已退出") : utf8 ("没反应"))
                 + utf8 (", focus ") + juce::String (pianoRoll.getFocusSlotId()));

        pianoRoll.setBounds (previousBounds);
        focusSynth (-1);
    } ();

    //-- Phase 31: the ruler zooms around the point you pressed --------------
    [&]
    {
        const auto   grid           = pianoRoll.getGridBounds();
        const float  anchorX        = grid.getX() + grid.getWidth() * 0.62f;
        const float  elsewhereX     = grid.getX() + grid.getWidth() * 0.22f;
        const float  previousWidth  = pianoRoll.getBeatWidth();
        const double previousScroll = pianoRoll.getScrollBeats();

        // Park the view where it can scroll both ways: a clamped scroll is the
        // one case where the anchor legitimately cannot be honoured, and a test
        // that sat on it would pass for the wrong reason.
        pianoRoll.setScrollBeats (8.0);

        const auto beatAt = [&] (float x)
        {
            return pianoRoll.getScrollBeats()
                     + (double) (x - grid.getX()) / (double) pianoRoll.getBeatWidth();
        };

        const double anchorBefore    = beatAt (anchorX);
        const double elsewhereBefore = beatAt (elsewhereX);

        const bool   zoomedIn = pianoRoll.simulateRulerZoom (anchorX, 140.0f);
        const double anchorIn = beatAt (anchorX);
        const double elsewhereIn = beatAt (elsewhereX);
        const float  widthIn  = pianoRoll.getBeatWidth();

        // Zoom back out, but not past the point where the scroll would have to
        // go negative: there is no music left of beat 0, and that is the one
        // case where the anchor legitimately gives way.
        const bool   zoomedOut = pianoRoll.simulateRulerZoom (anchorX, -80.0f);
        const double anchorOut = beatAt (anchorX);
        const float  widthOut  = pianoRoll.getBeatWidth();

        check ("Right-dragging the ruler zooms around the point you pressed",
               zoomedIn && zoomedOut && widthIn > previousWidth * 1.5f && widthOut < widthIn
                 && std::abs (anchorIn  - anchorBefore) < 1.0e-6
                 && std::abs (anchorOut - anchorBefore) < 1.0e-6,
               utf8 ("锚点拍 ") + juce::String (anchorBefore, 4) + utf8 (" → ")
                 + juce::String (anchorIn, 4) + utf8 (" → ") + juce::String (anchorOut, 4)
                 + utf8 (", 宽度 ") + juce::String (previousWidth, 1) + utf8 (" → ")
                 + juce::String (widthIn, 1) + utf8 (" → ") + juce::String (widthOut, 1));

        // The other half of the claim: it is a ZOOM, not a pan. A beat away from
        // the anchor has to move, or "the anchor stays put" would be satisfied
        // by freezing the whole view.
        check ("...and everything away from that point moves",
               std::abs (elsewhereIn - elsewhereBefore) > 0.5,
               juce::String (elsewhereBefore, 3) + utf8 (" → ") + juce::String (elsewhereIn, 3));

        pianoRoll.setBeatWidth (previousWidth);
        pianoRoll.setScrollBeats (previousScroll);
    } ();

    //-- Phase 31: "恢复原始长度" is exact, and snapping cannot spoil it ------
    const auto phase31Dir = juce::File::getSpecialLocation (juce::File::tempDirectory);
    const auto phase31A   = phase31Dir.getChildFile ("atomic-phase31-a.wav");

    if (! phase31A.existsAsFile() || phase31A.getSize() < 1024)
    {
        // 1.37 s on purpose: at 120 BPM that is 2.74 beats, which is NOT on the
        // 1/16 grid. A length that snapping could reach would prove nothing.
        writeTestSampleFile (phase31A, 1.37, 44100.0, 2, 330.0);
    }

    [&]
    {
        auto* layer = stack.getActiveLayer();

        if (layer == nullptr)
        {
            check ("Phase 31 needs a layer", false, "no active layer");
            return;
        }

        const int index = stack.getActiveLayerIndex();
        const int id    = pianoRoll.addSampleAt (100.0, 45, phase31A);

        auto* clip = layer->findSample (id);

        if (clip == nullptr || clip->audio == nullptr)
        {
            check ("Phase 31 clip fixture exists", false, "addSampleAt failed");
            return;
        }

        const double exactBeats = SampleEvent::beatsForSeconds (clip->audio->lengthSeconds(),
                                                                audio.getBpm());
        const double startBeat  = clip->startBeat;

        check ("A dropped block is exactly as long as its file, not rounded to the grid",
               std::abs (clip->lengthBeats - exactBeats) < 1.0e-9,
               utf8 ("块 ") + juce::String (clip->lengthBeats, 4) + utf8 (" / 文件 ")
                 + juce::String (exactBeats, 4) + utf8 (" 拍"));

        // Shorten it with snapping ON (the drag always snaps), then put it back:
        // the restored length is the file's, to the last sample — a value the
        // 1/16 grid can never produce, which is the whole point.
        pianoRoll.simulateSampleTrim (id, startBeat + 0.75, true);
        const double trimmed = clip->lengthBeats;

        pianoRoll.setSampleSelection ({ id });
        const int restored = pianoRoll.restoreSelectedSampleLengths();

        check (utf8 ("恢复原始长度 gives the file's exact length, snapping or not"),
               restored == 1
                 && std::abs (clip->lengthBeats - exactBeats) < 1.0e-12
                 && std::abs (clip->startBeat - startBeat) < 1.0e-12
                 && std::abs (clip->sourceOffsetSeconds) < 1.0e-12,
               utf8 ("长 ") + juce::String (trimmed, 4) + utf8 (" → ")
                 + juce::String (clip->lengthBeats, 6) + utf8 ("（文件 ")
                 + juce::String (exactBeats, 6) + utf8 ("）"));

        // ...and the same end is reachable by dragging the tail out, even though
        // no grid point lands on it.
        pianoRoll.simulateSampleTrim (id, startBeat + 0.5, true);
        pianoRoll.simulateSampleTrim (id, startBeat + exactBeats + 0.03, true);

        check ("...and the tail can be dragged back to exactly the end of the file",
               std::abs (clip->lengthBeats - exactBeats) < 1.0e-9,
               juce::String (clip->lengthBeats, 6) + utf8 (" / ") + juce::String (exactBeats, 6));

        //-- Phase 31: a squeezed block shows its whole file -------------------
        pianoRoll.setSampleStretchMode (id, StretchMode::tempo);

        const int   columns = pianoRoll.sampleWaveColumnCount (id);
        const float width   = pianoRoll.getSampleScreenBounds (index, id).getWidth();

        check ("A squeezed block draws its whole file, edge to edge",
               columns > 0 && std::abs ((float) columns - width) <= 1.0f,
               utf8 ("波 ") + juce::String (columns) + utf8 (" px / 块 ")
                 + juce::String (width, 1) + " px");

        //-- Phase 31: the engine publishes the RATIO, not just the mode -------
        // The DSP above is tested as a pure function; this closes the loop the
        // pure test cannot reach — that a clip's length really does become the
        // two numbers the renderer reads.
        clip->stretchMode = StretchMode::tempo;
        clip->lengthBeats = exactBeats * 0.5;          // half as long → ratio 2
        audio.getPlayback().publishSchedule (stack, rack, audio.getBpm(), 0.0, &mixer);

        const auto schedule = audio.getPlayback().getSchedule();
        const ScheduledClip* frozen = nullptr;

        if (schedule != nullptr)
            for (const auto& scheduled : schedule->clips)
                if (scheduled.clipId == id)
                    frozen = &scheduled;

        check ("The engine freezes a clip's stretch ratio, not just its mode",
               frozen != nullptr
                 && std::abs (frozen->stretch.speed - 2.0) < 0.01
                 && std::abs (frozen->stretch.pitch - 1.0) < 1.0e-9,
               frozen != nullptr ? utf8 ("speed ") + juce::String (frozen->stretch.speed, 4)
                                     + utf8 (", pitch ") + juce::String (frozen->stretch.pitch, 4)
                                 : juce::String ("clip not published"));

        pianoRoll.setSampleSelection ({ id });
        pianoRoll.deleteSelectedSamples();
    } ();

    //-- Phase 31: the three stretch modes, measured on a sine ----------------
    [&]
    {
        SampleBuffer sine;
        sine.sampleRate = 44100.0;
        sine.channels   = 1;

        const int frames = 44100;               // exactly one second of 440 Hz
        sine.frames.resize ((size_t) frames);

        for (int i = 0; i < frames; ++i)
            sine.frames[(size_t) i] = (float) std::sin (juce::MathConstants<double>::twoPi
                                                          * 440.0 * (double) i / 44100.0);

        constexpr double rate = 44100.0;

        // Two properties of the SOUND, which is the only honest way to test a
        // DSP change: how often the waveform crosses zero (pitch) and how loud
        // it still is in a window (whether the source has run out yet).
        const auto measure = [&] (const clipstretch::Playback& pb,
                                  double from, double to, double& hz) -> double
        {
            double peak = 0.0;
            int    crossings = 0;
            double previous = clipstretch::read (sine, pb, 0.0, from).left;

            for (int i = (int) (from * rate); i < (int) (to * rate); ++i)
            {
                const auto frame = clipstretch::read (sine, pb, 0.0, (double) i / rate);

                peak = juce::jmax (peak, (double) std::abs (frame.left));

                if (previous <= 0.0 && frame.left > 0.0)
                    ++crossings;

                previous = frame.left;
            }

            hz = (to > from) ? (double) crossings / (to - from) : 0.0;
            return peak;
        };

        double hzFlat = 0.0, hzTape = 0.0, hzTempo = 0.0, hzPitch = 0.0;
        const double peakFlat  = measure (clipstretch::Playback::make (StretchMode::none, 2.0),
                                          0.10, 0.40, hzFlat);
        const double peakTape  = measure (clipstretch::Playback::make (StretchMode::resample, 2.0),
                                          0.10, 0.40, hzTape);
        const double peakTempo = measure (clipstretch::Playback::make (StretchMode::tempo, 2.0),
                                          0.10, 0.40, hzTempo);
        const double peakPitch = measure (clipstretch::Playback::make (StretchMode::pitch, 2.0),
                                          0.10, 0.40, hzPitch);

        double ignored = 0.0;
        const double tailTape  = measure (clipstretch::Playback::make (StretchMode::resample, 2.0),
                                          0.55, 0.62, ignored);
        const double tailTempo = measure (clipstretch::Playback::make (StretchMode::tempo, 2.0),
                                          0.55, 0.62, ignored);
        const double tailPitch = measure (clipstretch::Playback::make (StretchMode::pitch, 2.0),
                                          0.55, 0.62, ignored);

        check (utf8 ("原速窗口 neither speeds up nor transposes, whatever the ratio"),
               peakFlat > 0.9 && std::abs (hzFlat - 440.0) < 4.0,
               juce::String (hzFlat, 1) + " Hz");

        check (utf8 ("变速变调: twice the speed is also an octave up"),
               peakTape > 0.5 && std::abs (hzTape - 880.0) < 12.0,
               juce::String (hzTape, 1) + " Hz (440 × 2)");

        check (utf8 ("只变速不变调: the file is squeezed, the pitch does not move"),
               peakTempo > 0.5 && std::abs (hzTempo - 440.0) < 8.0,
               juce::String (hzTempo, 1) + " Hz");

        check (utf8 ("不变速只变调: the pitch moves, the file still passes at its own speed"),
               peakPitch > 0.5 && std::abs (hzPitch - 880.0) < 12.0 && tailPitch > 0.5,
               juce::String (hzPitch, 1) + utf8 (" Hz, 0.55 s 处 ")
                 + juce::String (tailPitch, 2));

        // The two modes that consume the whole file are DONE by 0.55 s; the one
        // that reads at the file's own speed is still going. Without this the
        // pitch test above would also pass for a plain resample.
        check ("...and the two whole-file modes have run out by then",
               tailTape < 0.02 && tailTempo < 0.02,
               juce::String (tailTape, 3) + " / " + juce::String (tailTempo, 3));

        // 1:1 is bit-exact: a block that was never stretched still IS the file.
        // Measured over 256 samples rather than one, because 440 Hz is exactly
        // at a zero crossing on every half second and a single sample there
        // would compare two zeroes and prove nothing.
        const auto identity = clipstretch::Playback::make (StretchMode::none, 2.0);
        double worst = 0.0;
        double loudest = 0.0;

        for (int i = 0; i < 256; ++i)
        {
            const int idx = (int) (0.5 * rate) + i;
            const auto frame = clipstretch::read (sine, identity, 0.0, (double) idx / rate);

            worst   = juce::jmax (worst, (double) std::abs (frame.left - sine.frames[(size_t) idx]));
            loudest = juce::jmax (loudest, (double) std::abs (sine.frames[(size_t) idx]));
        }

        check ("An unstretched block is bit-exact with its source",
               loudest > 0.5 && worst < 1.0e-6,
               utf8 ("最大偏差 ") + juce::String (worst, 9) + utf8 ("（峰值 ")
                 + juce::String (loudest, 3) + utf8 ("）"));
    } ();

    //-- Phase 31: v16 keeps the stretch mode ---------------------------------
    [&]
    {
        LayerStack s1;
        RackModel  r1;

        s1.addLayer (utf8 ("拉伸"), palette::layer1);

        SampleEvent clip;
        clip.name        = "stretch";
        clip.startBeat   = 1.0;
        clip.lengthBeats = 2.0;
        clip.pitch       = 60;
        clip.laneSpan    = 2;
        clip.stretchMode = StretchMode::pitch;

        s1.getLayer (0)->addSample (clip);

        const auto xml = project::toXmlText (s1, r1, 120.0);

        LayerStack s2;
        RackModel  r2;
        double bpm2 = 0.0;
        juce::String error;

        const bool ok = project::fromXmlText (xml, s2, r2, bpm2, error, nullptr, nullptr, nullptr);

        const auto* back = (s2.getLayer (0) != nullptr && s2.getLayer (0)->getSampleCount() == 1)
                             ? &s2.getLayer (0)->getSamples().front() : nullptr;

        check ("A v16 project keeps a clip's stretch mode",
               ok && back != nullptr && back->stretchMode == StretchMode::pitch,
               back != nullptr ? juce::String ((int) back->stretchMode) : error);
    } ();

    //-- Phase 32: a clip and its sampler are two views of one thing ----------
    // The reference rule runs BOTH ways here, and the mount carries the file
    // with it: drop, select, switch, notice the orphan, delete the card.
    [&]
    {
        auto* layer = stack.getActiveLayer();

        if (layer == nullptr)
        {
            check ("Phase 32 needs a layer", false, "no active layer");
            return;
        }

        focusSynth (-1);                    // start from "nothing is highlighted"

        const int a = pianoRoll.addSampleAt (120.0, 45, phase31A);
        auto*     clipA = layer->findSample (a);

        if (clipA == nullptr || clipA->outputSlotId < 0)
        {
            check ("Phase 32 clip fixture exists", false, "addSampleAt failed");
            return;
        }

        const int slotA = clipA->outputSlotId;

        check ("A dropped clip grows a sampler and names it",
               rack.findSlot (slotA) != nullptr && rack.findSlot (slotA)->isSampler(),
               juce::String ("clip -> slot ") + juce::String (slotA));

        //-- Selecting the block lights the card that holds the file ----------
        pianoRoll.setSampleSelection ({ a });
        updateRackRoutingFocus();

        check ("Selecting a clip highlights the sampler it is mounted on",
               pianoRoll.selectedSamplerSlotId() == slotA && getFocusedSlotId() == slotA,
               juce::String (slotA) + " / " + juce::String (getFocusedSlotId()));

        //-- ...and letting go takes only THAT highlight away -----------------
        pianoRoll.setSampleSelection ({});
        updateRackRoutingFocus();

        check ("Clearing the block selection clears the highlight it caused",
               getFocusedSlotId() < 0,
               juce::String (getFocusedSlotId()));

        //-- Switching sampler hands the block that sampler's FILE ------------
        const int b = pianoRoll.addSampleAt (124.0, 45, phase27B);   // 2 s, other file
        auto*     clipB = layer->findSample (b);

        if (clipB == nullptr || clipB->outputSlotId == slotA)
        {
            check ("Phase 32 second clip exists", false, "addSampleAt failed");
            return;
        }

        const int    slotB     = clipB->outputSlotId;
        const double startBeat = clipA->startBeat;
        const auto*  targetA   = rack.findSlot (slotB);
        const double lengthB   = targetA != nullptr && targetA->sampleBuffer != nullptr
                                   ? SampleEvent::beatsForSeconds (targetA->sampleBuffer->lengthSeconds(),
                                                                   audio.getBpm())
                                   : 0.0;

        pianoRoll.setSampleSelection ({ a });
        pianoRoll.mountSelectedSamplesToSlot (slotB);

        clipA = layer->findSample (a);

        check ("Pointing a clip at another sampler gives it that sampler's sample",
               clipA != nullptr && lengthB > 0.0
                 && clipA->outputSlotId == slotB
                 && clipA->audio == targetA->sampleBuffer
                 && clipA->name == targetA->name
                 && std::abs (clipA->lengthBeats - lengthB) < 1.0e-9
                 && std::abs (clipA->startBeat - startBeat) < 1.0e-9
                 && std::abs (clipA->sourceOffsetSeconds) < 1.0e-12,
               clipA != nullptr ? utf8 ("长 ") + juce::String (clipA->lengthBeats, 4)
                                    + utf8 (" 拍（文件 ") + juce::String (lengthB, 4)
                                    + utf8 ("），起点 ") + juce::String (clipA->startBeat, 3)
                                : juce::String ("clip gone"));

        //-- Re-picking the card it is already on must not undo a trim --------
        // "Show me that synth" is a click people make constantly; if it handed
        // the block its file again it would silently restore the whole length
        // and turn a one-click inspection into a lost edit.
        pianoRoll.simulateSampleTrim (a, startBeat + 1.0, true);
        const double trimmedBeats = clipA->lengthBeats;

        pianoRoll.setSampleSelection ({ a });
        pianoRoll.mountSelectedSamplesToSlot (slotB);       // the same sampler

        clipA = layer->findSample (a);

        check ("Re-picking the sampler a block already plays does not undo its trim",
               clipA != nullptr && std::abs (clipA->lengthBeats - trimmedBeats) < 1.0e-12,
               clipA != nullptr ? juce::String (trimmedBeats, 4) + utf8 (" → ")
                                    + juce::String (clipA->lengthBeats, 4)
                                : juce::String ("clip gone"));

        //-- The sampler nobody names any more is gone ------------------------
        pruneOrphanSamplerSlots();

        check ("A sampler no clip names any more leaves the rack",
               rack.findSlot (slotA) == nullptr && rack.findSlot (slotB) != nullptr,
               juce::String ("slot ") + juce::String (slotA) + utf8 (" 已移除，余 ")
                 + juce::String (rack.getNumSlots()));

        //-- The other direction: removing the card deletes its blocks --------
        removeSlotAndItsClips (slotB);

        check ("Deleting a sampler deletes the clips that played through it",
               rack.findSlot (slotB) == nullptr
                 && layer->findSample (a) == nullptr
                 && layer->findSample (b) == nullptr,
               utf8 ("卡 ") + juce::String (slotB)
                 + utf8 ("，剩余块 ")
                 + juce::String ((layer->findSample (a) != nullptr ? 1 : 0)
                                  + (layer->findSample (b) != nullptr ? 1 : 0)));

        pianoRoll.setSampleSelection ({});
        focusSynth (-1);
    } ();

    //-- Phase 33: a drop selects, and no block can vanish orphaned -----------
    [&]
    {
        auto* layer = stack.getActiveLayer();

        if (layer == nullptr)
        {
            check ("Phase 33 needs a layer", false, "no active layer");
            return;
        }

        focusSynth (-1);
        pianoRoll.setSampleSelection ({});

        //-- A dropped block IS the selection --------------------------------
        const int dropped = pianoRoll.addSampleAt (200.0, 45, phase31A);
        auto*     clip    = layer->findSample (dropped);

        if (clip == nullptr)
        {
            check ("Phase 33 clip fixture exists", false, "addSampleAt failed");
            return;
        }

        const int  droppedSlot = clip->outputSlotId;
        const auto droppedSel  = pianoRoll.getSampleSelection();

        check ("A dropped block arrives selected",
               droppedSel.size() == 1 && droppedSel.front() == dropped,
               juce::String ((int) droppedSel.size()) + utf8 (" 个选中"));

        check ("...and the sampler that now holds its file is highlighted",
               droppedSlot >= 0 && getFocusedSlotId() == droppedSlot,
               juce::String ("slot ") + juce::String (droppedSlot) + " / "
                 + juce::String (getFocusedSlotId()));

        //-- The sweep hangs off the MODEL, not off one delete command --------
        // This is the case that was broken: remove the block the way anything
        // else in the program would (a direct model edit plus a notification),
        // never through PianoRollComponent::deleteSelectedSamples, and the
        // sampler still has to go.
        layer->removeSample (dropped);
        stack.notifyNotesChanged();

        check ("A block removed by any other path still takes its sampler with it",
               rack.findSlot (droppedSlot) == nullptr,
               rack.findSlot (droppedSlot) == nullptr
                 ? juce::String ("slot ") + juce::String (droppedSlot) + utf8 (" 已移除")
                 : juce::String ("slot ") + juce::String (droppedSlot) + utf8 (" 还在"));

        //-- Cut then paste is a MOVE, not a block pointed at nothing ---------
        const int cutId = pianoRoll.addSampleAt (210.0, 45, phase27B);
        auto*     cutClip = layer->findSample (cutId);

        if (cutClip == nullptr)
        {
            check ("Phase 33 second fixture exists", false, "addSampleAt failed");
            return;
        }

        const int cutSlot = cutClip->outputSlotId;

        pianoRoll.setSampleSelection ({ cutId });
        const bool cut     = pianoRoll.cutSamplesToClipboard();
        const bool swept   = rack.findSlot (cutSlot) == nullptr;
        const bool pasted  = pianoRoll.pasteSamplesFromClipboard();

        const auto pastedSel = pianoRoll.getSampleSelection();
        const auto* moved    = pastedSel.empty() ? nullptr : layer->findSample (pastedSel.front());

        check ("Cutting a block sweeps its sampler, and pasting mounts a fresh one",
               cut && swept && pasted && moved != nullptr
                 && moved->outputSlotId >= 0
                 && rack.findSlot (moved->outputSlotId) != nullptr,
               moved != nullptr ? juce::String (cutSlot) + utf8 (" → ")
                                    + juce::String (moved->outputSlotId)
                                : juce::String ("nothing pasted"));

        pianoRoll.setSampleSelection ({});
        focusSynth (-1);
    } ();

    //-- Phase 34: both cascade deletions ask first ---------------------------
    // Phase 32 made the two directions automatic. That is right about the RULE
    // and wrong about the moment: the sweep fires on a mouse move, and the rack
    // × is one pixel away from "inspect". Neither may delete music silently.
    [&]
    {
        auto* layer = stack.getActiveLayer();

        if (layer == nullptr)
        {
            check ("Phase 34 needs a layer", false, "no active layer");
            return;
        }

        focusSynth (-1);
        pianoRoll.setSampleSelection ({});

        const int clipId = pianoRoll.addSampleAt (300.0, 45, phase31A);
        auto*     clip   = layer->findSample (clipId);

        if (clip == nullptr || clip->outputSlotId < 0)
        {
            check ("Phase 34 clip fixture exists", false, "addSampleAt failed");
            return;
        }

        const int slotId = clip->outputSlotId;

        //-- The roll lost its last block: ask, and honour "keep it" ----------
        samplerPromptAnswer = false;
        const int asksBefore = samplerPromptCount;

        // Removed the way anything else in the program does it — a model edit
        // plus a notification — so the sweep, not the caller, is what notices.
        layer->removeSample (clipId);
        stack.notifyNotesChanged();

        check ("The sweep asks before deleting a sampler nothing plays",
               samplerPromptCount == asksBefore + 1,
               utf8 ("询问 ") + juce::String (samplerPromptCount - asksBefore) + utf8 (" 次"));

        check ("...and saying no keeps the sampler",
               rack.findSlot (slotId) != nullptr,
               juce::String ("slot ") + juce::String (slotId)
                 + (rack.findSlot (slotId) != nullptr ? utf8 (" 还在") : utf8 (" 没了")));

        //-- ...without coming back on the next mouse move --------------------
        // The sweep runs on every notesChanged, so an answer that is not
        // remembered is not an answer, it is a nag.
        stack.notifyNotesChanged();
        pruneOrphanSamplerSlots();          // and once more, straight up

        check ("A 'keep it' answer is remembered instead of re-asked",
               samplerPromptCount == asksBefore + 1 && rack.findSlot (slotId) != nullptr,
               utf8 ("询问共 ") + juce::String (samplerPromptCount - asksBefore) + utf8 (" 次"));

        //-- Naming it again makes it a question again ------------------------
        const int again   = pianoRoll.addSampleAt (304.0, 45, phase31A);  // same file → same slot
        auto*     clipTwo = layer->findSample (again);

        check ("A block that names the sampler again retires the 'keep it' answer",
               clipTwo != nullptr && clipTwo->outputSlotId == slotId,
               clipTwo != nullptr ? juce::String ("slot ") + juce::String (clipTwo->outputSlotId)
                                  : juce::String ("no clip"));

        samplerPromptAnswer = true;
        const int asksBeforeYes = samplerPromptCount;

        layer->removeSample (again);
        stack.notifyNotesChanged();

        check ("Answering yes deletes the sampler nothing plays",
               rack.findSlot (slotId) == nullptr && samplerPromptCount == asksBeforeYes + 1,
               juce::String ("slot ") + juce::String (slotId)
                 + (rack.findSlot (slotId) == nullptr ? utf8 (" 已移除") : utf8 (" 还在")));

        //-- The other direction: the card asks before taking blocks ----------
        const int mounted   = pianoRoll.addSampleAt (310.0, 45, phase27B);
        auto*     clipM     = layer->findSample (mounted);
        const int mountedOn = clipM != nullptr ? clipM->outputSlotId : -1;

        if (mountedOn < 0)
        {
            check ("Phase 34 mounted fixture exists", false, "addSampleAt failed");
            return;
        }

        const int asksBeforeCard = samplerPromptCount;

        samplerPromptAnswer = false;
        requestRemoveSlotAndItsClips (mountedOn);

        check ("Deleting a card that holds blocks asks first, and no means no",
               samplerPromptCount == asksBeforeCard + 1
                 && rack.findSlot (mountedOn) != nullptr
                 && layer->findSample (mounted) != nullptr,
               juce::String ("slot ") + juce::String (mountedOn)
                 + utf8 ("，块 ") + juce::String (layer->findSample (mounted) != nullptr ? 1 : 0));

        samplerPromptAnswer = true;
        requestRemoveSlotAndItsClips (mountedOn);

        // Exactly one question: deleting the blocks notifies, and a sweep
        // inside that notification would see the card it is about to remove as
        // an orphan and ask about it again.
        check ("...and yes deletes the card and its blocks together",
               rack.findSlot (mountedOn) == nullptr && layer->findSample (mounted) == nullptr
                 && samplerPromptCount == asksBeforeCard + 2,
               juce::String ("slot ") + juce::String (mountedOn)
                 + utf8 ("，余块 ") + juce::String (layer->findSample (mounted) != nullptr ? 1 : 0)
                 + utf8 ("，询问 ") + juce::String (samplerPromptCount - asksBeforeCard) + utf8 (" 次"));

        //-- A card with nothing on the grid is not worth a question ----------
        // Built directly on the rack, so no clip ever names it: this is the
        // state the prompt must stay quiet about.
        std::shared_ptr<const SampleBuffer> bareBuffer;

        for (int i = 0; i < rack.getNumSlots() && bareBuffer == nullptr; ++i)
            if (const auto* s = rack.getSlot (i); s != nullptr && s->isSampler())
                bareBuffer = s->sampleBuffer;

        const int bare     = rack.addSampler (bareBuffer, {}, utf8 ("裸采样音源"));
        const int asksBare = samplerPromptCount;

        requestRemoveSlotAndItsClips (bare);

        check ("A sampler nothing is mounted on is deleted without asking",
               rack.findSlot (bare) == nullptr && samplerPromptCount == asksBare,
               juce::String ("slot ") + juce::String (bare)
                 + utf8 ("，询问 ") + juce::String (samplerPromptCount - asksBare) + utf8 (" 次"));

        pianoRoll.setSampleSelection ({});
        focusSynth (-1);
        samplerPromptAnswer = true;
    } ();

    //-- Phase 33: a scanned folder tree opens FOLDED -------------------------
    // The tree used to arrive fully open, which for a real sample library is a
    // thousand-row wall the instant the scan finishes. Folded is now the
    // default and the store remembers the branches that were OPENED.
    [&]
    {
        const auto root  = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile ("atomic-tree-collapse");
        const auto inner = root.getChildFile ("Inner");

        if (! inner.isDirectory())
            inner.createDirectory();

        writeTestSampleFile (inner.getChildFile ("inside.wav"), 0.10, 44100.0, 1, 220.0);
        writeTestSampleFile (root.getChildFile ("top.wav"), 0.10, 44100.0, 1, 330.0);

        filePanel.clearFoldersForSnapshot();      // view-only; nothing is saved
        filePanel.addFolder (root, false);
        filePanel.waitForPendingScans();

        check ("A freshly scanned folder opens folded",
               filePanel.isFolderExpanded (0)
                 && ! filePanel.isSubFolderExpanded (inner),
               utf8 ("根 ") + (filePanel.isFolderExpanded (0) ? "open" : "folded")
                 + utf8 ("，子目录 ") + (filePanel.isSubFolderExpanded (inner) ? "open" : "folded"));

        filePanel.setSubFolderExpanded (inner, true, false);

        check ("Opening one branch leaves the rest of the tree folded",
               filePanel.isSubFolderExpanded (inner),
               "Inner open");

        filePanel.setSubFolderExpanded (inner, false, false);

        check ("...and it folds again", ! filePanel.isSubFolderExpanded (inner), "Inner folded");
    } ();

    //-- Phase 35: a group is "these members", and a member can be a block -----
    // Four requirements, and they are one idea seen from three sides: a block
    // can MAKE a group, a group's label SELECTS its blocks, and a group's strip
    // REACHES their audio. Membership already existed on the model (Phase 27);
    // what was missing was a way in, a way to see it, and a reason to care.
    [&]
    {
        auto* layer = stack.getActiveLayer();

        if (layer == nullptr)
        {
            check ("Phase 35 needs an active layer", false, "no active layer");
            return;
        }

        const auto loaded  = loadSampleFile (phase27B);   // 2 s, 165 Hz
        const int  sampler = rack.addSampler (loaded.buffer, phase27B.getFullPathName(), "P35");

        SampleEvent fixture;
        fixture.name        = "P35";
        fixture.path        = phase27B.getFullPathName();
        fixture.audio       = loaded.buffer;
        fixture.startBeat   = 60.0;
        fixture.lengthBeats = 4.0;
        fixture.pitch       = 84;
        fixture.laneSpan    = 2;
        fixture.outputSlotId = sampler;

        const int clipId = layer->addSample (fixture);

        if (clipId <= 0)
        {
            check ("Phase 35 clip fixture exists", false, "addSample failed");
            return;
        }

        const int groupsBefore = (int) layer->getGroups().size();

        //-- Requirement 1: a block, on its own, is enough to make a group ----
        pianoRoll.setSampleSelection ({ clipId });

        const int gid = pianoRoll.groupSelection();

        check ("A sample selection makes a group with no notes in it",
               gid > 0
                 && layer->countSamplesInGroup (gid) == 1
                 && layer->countNotesInGroup (gid) == 0
                 && (int) layer->getGroups().size() == groupsBefore + 1,
               "gid " + juce::String (gid) + utf8 ("，组内 ")
                 + juce::String (layer->countSamplesInGroup (gid)) + utf8 (" 个采样"));

        check ("...and the selection reports that group",
               pianoRoll.getSelectedGroupId() == gid,
               juce::String (pianoRoll.getSelectedGroupId()));

        //-- Requirement 4: a MIXED group selects both kinds ------------------
        // Two notes join the same group. This is the case that used to be
        // impossible to express: "select the whole group" was a question about
        // notes, and the mixed selection it now writes is what setMixedSelection
        // exists for.
        const int n1 = layer->addNote ({ 0, 88, 60.0, 2.0, 0.9f });
        const int n2 = layer->addNote ({ 0, 90, 64.0, 2.0, 0.9f });
        layer->assignNotesToGroup ({ n1, n2 }, gid);
        stack.notifyNotesChanged();

        pianoRoll.selectGroup (gid);

        check ("Selecting a group takes its samples as well as its notes",
               (int) pianoRoll.getSampleSelection().size() == 1
                 && pianoRoll.isSampleSelected (clipId),
               juce::String ((int) pianoRoll.getSampleSelection().size())
                 + utf8 (" 个采样被选中"));

        check ("A mixed group selects both kinds in one gesture",
               (int) pianoRoll.getSelection().size() == 2
                 && (int) pianoRoll.getSampleSelection().size() == 1,
               juce::String ((int) pianoRoll.getSelection().size()) + utf8 (" 音符 + ")
                 + juce::String ((int) pianoRoll.getSampleSelection().size()) + utf8 (" 采样"));

        check ("...and the whole group still reads as one group",
               pianoRoll.getSelectedGroupId() == gid && pianoRoll.isWholeGroupSelected (gid),
               juce::String (pianoRoll.getSelectedGroupId()));

        // Dragging a mixed group has to take the block with it — the reason a
        // clip is in a group at all.
        const double clipBeatBefore = layer->findSample (clipId)->startBeat;

        pianoRoll.simulateGroupMove (stack.getActiveLayerIndex(), gid, 4.0, 1);

        check ("A group move carries its sample block",
               std::abs (layer->findSample (clipId)->startBeat - (clipBeatBefore + 4.0)) < 1.0e-9,
               "beat " + juce::String (clipBeatBefore, 2) + " -> "
                 + juce::String (layer->findSample (clipId)->startBeat, 2));

        //-- Shift+G over the mixed selection empties the group --------------
        pianoRoll.selectGroup (gid);

        const bool detached = pianoRoll.ungroupSelection();

        check ("Ungrouping a mixed selection takes the blocks out too",
               detached
                 && layer->findSample (clipId)->groupId < 0
                 && layer->findNote (n1)->groupId < 0
                 && (int) layer->getGroups().size() == groupsBefore,
               utf8 ("组数 ") + juce::String ((int) layer->getGroups().size())
                 + utf8 ("，采样归属 ") + juce::String (layer->findSample (clipId)->groupId));
    } ();

    //-- Phase 35: the group's fader, pan and mute reach a clip's audio -------
    // The one requirement that cannot be checked on the model: "有效" means
    // audible. A self-contained session, because a measurement that shares a
    // bus with the demo project measures the demo project.
    [&]
    {
        LayerStack     t;
        RackModel      r;
        MixerModel     m;
        PlaybackEngine e;

        e.setSampleRate (48000.0);
        t.addLayer (utf8 ("组内采样"), palette::layer1);
        t.setLayerLevelDb (0, 0.0f);
        t.setLayerPan (0, 0.0f);

        const int synth = r.addSynth();
        t.setLayerOutputSlot (0, synth);

        const auto loaded  = loadSampleFile (phase27B);
        const int  sampler = r.addSampler (loaded.buffer, phase27B.getFullPathName(), "P35 bus");

        auto* layer = t.getLayer (0);

        SampleEvent clip;
        clip.name         = "P35";
        clip.path         = phase27B.getFullPathName();
        clip.audio        = loaded.buffer;
        clip.startBeat    = 0.0;                  // 0.0 – 1.0 s at 120 bpm
        clip.lengthBeats  = 2.0;
        clip.pitch        = 60;
        clip.laneSpan     = 2;
        clip.outputSlotId = sampler;

        const int clipId = layer->addSample (clip);

        // A note that shares the group but not the clock: between beats 8 and
        // 12 (4.0 – 6.0 s), so the clip's window and the note's window can be
        // measured without one hiding the other.
        const int noteId = layer->addNote ({ 0, 57, 8.0, 4.0, 0.8f });

        if (auto* note = layer->findNote (noteId))
            note->envelope = NoteEnvelope::makeFlat();

        const int gid = layer->addGroup (utf8 ("P35 组"), groupPalette::group1);
        layer->assignSamplesToGroup ({ clipId }, gid);
        layer->assignNotesToGroup ({ noteId }, gid);

        auto renderSides = [&] (double from, double span, float& left, float& right)
        {
            const int n = juce::jmax (1, (int) std::llround (span * 48000.0));
            juce::AudioBuffer<float> b (2, n);

            e.resetMixState();
            e.publishSchedule (t, r, 120.0, 0.0, &m);
            e.renderWindow (from, n, b, nullptr);

            left  = b.getMagnitude (0, 0, n);
            right = b.getMagnitude (1, 0, n);
        };

        auto clipPeak = [&] (double from)
        {
            float l = 0.0f, rr = 0.0f;
            renderSides (from, 0.2, l, rr);
            return juce::jmax (l, rr);
        };

        float unityL = 0.0f, unityR = 0.0f;
        renderSides (0.10, 0.2, unityL, unityR);

        check ("A clip in a group still sounds, and sounds the same",
               juce::jmax (unityL, unityR) > 0.01f,
               "L " + juce::String (unityL, 4) + " / R " + juce::String (unityR, 4));

        //-- Volume -----------------------------------------------------------
        layer->findGroup (gid)->setLevelDb (-6.0f);

        const float minus6 = clipPeak (0.10);

        check ("A -6 dB group fader halves the clip",
               std::abs (minus6 / juce::jmax (1.0e-6f, juce::jmax (unityL, unityR)) - 0.501f) < 0.06f,
               "x" + juce::String (minus6 / juce::jmax (1.0e-6f, juce::jmax (unityL, unityR)), 3));

        layer->findGroup (gid)->setLevelDb (0.0f);

        //-- Pan ---------------------------------------------------------------
        // A clip's pan is a BALANCE (a stereo file already has an image), so
        // "hard left" must empty the right channel while leaving the left one
        // where it was.
        layer->findGroup (gid)->setPan (-1.0f);

        float panL = 0.0f, panR = 0.0f;
        renderSides (0.10, 0.2, panL, panR);

        check ("Hard left on the group empties the clip's right channel",
               panL > 0.01f && panR < panL * 0.02f,
               "L " + juce::String (panL, 3) + " / R " + juce::String (panR, 3));

        layer->findGroup (gid)->setPan (0.0f);

        //-- Mute --------------------------------------------------------------
        // One flag, applied by GroupMix::gainAt, so the note and the clip cannot
        // disagree about what "the group is muted" means.
        layer->findGroup (gid)->setMuted (true);

        const float mutedClip = clipPeak (0.10);

        float noteL = 0.0f, noteR = 0.0f;
        renderSides (4.20, 0.2, noteL, noteR);

        check ("Muting a group silences its sample block",
               mutedClip < 1.0e-6f, "clip " + juce::String (mutedClip, 6));

        check ("Muting a group silences its notes too",
               juce::jmax (noteL, noteR) < 1.0e-6f,
               "note " + juce::String (juce::jmax (noteL, noteR), 6));

        layer->findGroup (gid)->setMuted (false);

        // The other half of a mute: it has to be reversible without touching the
        // fader, or "mute" is just "drag the level and remember the old value".
        float backL = 0.0f, backR = 0.0f;
        renderSides (4.20, 0.2, backL, backR);

        check ("Unmuting brings the group back at its fader value",
               juce::jmax (backL, backR) > 0.01f,
               "L " + juce::String (backL, 4) + " / R " + juce::String (backR, 4));

        //-- And the mute survives a save -------------------------------------
        // A mute that a reload forgets is worse than no mute: the project would
        // come back sounding different from the take the user signed off on.
        layer->findGroup (gid)->setMuted (true);

        LayerStack  reloaded;
        RackModel   reloadedRack;
        juce::String error;
        double      bpmBack = 0.0;

        const auto  xml = project::toXmlText (t, r, 120.0);
        const bool  read = project::fromXmlText (xml, reloaded, reloadedRack, bpmBack, error);

        const auto* reloadedLayer = reloaded.getLayer (0);
        const auto* reloadedGroup = reloadedLayer != nullptr ? reloadedLayer->findGroup (gid) : nullptr;

        check ("A group's mute is written to the project file",
               read && reloadedGroup != nullptr && reloadedGroup->isMuted(),
               reloadedGroup != nullptr ? (reloadedGroup->isMuted() ? "muted" : "not muted")
                                        : juce::String ("no group after load"));
    } ();


    //-- Phase 36: three automation layers, one law ---------------------------
    // Before this phase the level multiply was spelled out inline in the voice
    // loop and again in the clip loop, and a hosted parameter was written by
    // whichever curve had the higher priority. These checks hold the law itself
    // and both callers to the same numbers, so "the overlay says -18 dB" and
    // "the renderer produced -18 dB" cannot drift apart.
    [&]
    {
        LayerStack t;
        RackModel  r;

        const int slot = r.getSlotIdAt (0);
        t.addLayer (utf8 ("叠加"), palette::layer1);
        t.setLayerOutputSlot (0, slot);
        t.setLayerLevelDb (0, 0.0f);

        auto* layer = t.getLayer (0);
        const int noteId = layer->addNote ({ 0, 60, 0.0, 4.0, 1.0f });
        auto* note = layer->findNote (noteId);

        // The note sits in a group from the start: every check below turns one
        // layer on at a time, and membership must not be what changes.
        const int groupId = layer->addGroup (utf8 ("层叠"), groupPalette::group2);
        layer->assignNotesToGroup ({ noteId }, groupId);

        auto* group = layer->findGroup (groupId);

        if (note == nullptr || group == nullptr)
        {
            check ("Phase 36 needs a note and a group", false, "fixture missing");
            return;
        }

        // Three curves that all say 0.5, prepared switched OFF. One number for
        // all three means the product is the only thing that can produce 0.125.
        auto& layerLane = layer->automationFor (LayerAutomationTarget::volume);
        layerLane.resetFlat (0.5f, 64.0);
        layerLane.setEnabled (false);

        auto& slotLane = r.getSlot (0)->automationFor (SlotAutomationTarget::level);
        slotLane.resetFlat (0.5f, 64.0);
        slotLane.setEnabled (false);

        auto& groupLane = group->getLevelAutomation();
        groupLane.resetFlat (0.5f, 64.0);
        groupLane.setEnabled (false);

        NoteEnvelope halfCurve;
        halfCurve.loadPoints ({ { 0.0f, 0.5f }, { 1.0f, 0.5f } });
        halfCurve.setEnabled (false);
        note->envelope = halfCurve;
        note->modTarget = NoteModTarget::level;

        PlaybackEngine engine;
        engine.setSampleRate (44100.0);

        auto peakNow = [&] () -> float
        {
            engine.publishSchedule (t, r, 120.0);
            engine.resetMixState();
            return engine.renderOffline (1.0).peak;
        };

        auto renderSides = [&] (float& left, float& right)
        {
            engine.publishSchedule (t, r, 120.0);
            engine.resetMixState();

            juce::AudioBuffer<float> buffer (2, 22050);
            buffer.clear();
            engine.renderWindow (0.30, 22050, buffer, nullptr);

            left  = buffer.getMagnitude (0, 0, 22050);
            right = buffer.getMagnitude (1, 0, 22050);
        };

        //-- The baseline: three lanes off, the note is untouched -------------
        const float plain = peakNow();

        check ("With all three layers off the note is at its fader",
               plain > 0.01f, "peak " + juce::String (plain, 4));

        //-- One layer at a time -----------------------------------------------
        // Each of the three alone must give exactly half. This is what proves the
        // layers are independent: a phase that only tested the product could be
        // passing because one layer was silently doing all the work.
        layerLane.setEnabled (true);
        const float macroOnly = peakNow() / plain;
        layerLane.setEnabled (false);

        check ("The macro layer alone halves the note",
               std::abs (macroOnly - 0.5f) < 0.02f, "x" + juce::String (macroOnly, 3));

        slotLane.setEnabled (true);
        const float synthOnly = peakNow() / plain;
        slotLane.setEnabled (false);

        check ("The synth's own lane is part of the same macro layer",
               std::abs (synthOnly - 0.5f) < 0.02f, "x" + juce::String (synthOnly, 3));

        groupLane.setEnabled (true);
        const float groupOnly = peakNow() / plain;
        groupLane.setEnabled (false);

        check ("The group layer alone halves the note",
               std::abs (groupOnly - 0.5f) < 0.02f, "x" + juce::String (groupOnly, 3));

        note->envelope.setEnabled (true);
        const float noteOnly = peakNow() / plain;
        note->envelope.setEnabled (false);

        check ("The note's own curve alone halves the note",
               std::abs (noteOnly - 0.5f) < 0.02f, "x" + juce::String (noteOnly, 3));

        //-- All three at once -------------------------------------------------
        // 0.5 x 0.5 x 0.5. Not the minimum, not the last one written: the
        // product. A mixer that let one curve win would land on 0.5 here, so the
        // margin has to be tight enough to tell those apart.
        layerLane.setEnabled (true);
        groupLane.setEnabled (true);
        note->envelope.setEnabled (true);

        const float allThree = peakNow() / plain;

        check ("All three layers multiply: 0.5 x 0.5 x 0.5 = 0.125",
               std::abs (allThree - 0.125f) < 0.01f, "x" + juce::String (allThree, 4));

        //-- And the law itself, called directly -------------------------------
        // The renderer is one caller; the overlay painter is another. Pinning
        // the function is cheaper than pinning every picture of it.
        check ("The three-layer law is multiply, add, multiply",
               std::abs (automation::stackLevel (0.5f, 0.5f, 0.5f) - 0.125f) < 1.0e-6f
                 && std::abs (automation::stackPan (0.3f, 0.3f, 0.3f) - 0.9f) < 1.0e-6f
                 && std::abs (automation::stackPan (0.8f, 0.8f, 0.0f) - 1.0f) < 1.0e-6f
                 && std::abs (automation::stackPluginParam (0.5f, 0.5f) - 0.25f) < 1.0e-6f
                 && std::abs (automation::stackPluginParam (1.0f, 0.6f) - 0.6f) < 1.0e-6f,
               "level " + juce::String (automation::stackLevel (0.5f, 0.5f, 0.5f), 3)
                 + " pan " + juce::String (automation::stackPan (0.8f, 0.8f, 0.0f), 2)
                 + " param " + juce::String (automation::stackPluginParam (0.5f, 0.5f), 3));

        //-- Pan: three angles added, not three gains multiplied ---------------
        layerLane.setEnabled (false);
        groupLane.setEnabled (false);
        note->envelope.setEnabled (false);

        layer->setPan (0.3f);
        group->setPan (0.3f);

        float panL = 0.0f, panR = 0.0f;
        renderSides (panL, panR);

        // 0.3 + 0.3 = 0.6 of centre, which is (0.6 + 1) * pi/4 of angle.
        const float expectedRatio = std::tan ((0.6f + 1.0f) * juce::MathConstants<float>::pi * 0.25f);
        const float ratio = panR / juce::jmax (1.0e-6f, panL);

        check ("The layer and group pans add up, they do not multiply",
               std::abs (ratio - expectedRatio) < 0.25f,
               "R/L " + juce::String (ratio, 3) + utf8 ("，应为 ")
                 + juce::String (expectedRatio, 3));

        // Clamped once, at the end: 0.8 + 0.8 would be 1.6 without the clamp,
        // and a pan past hard right is not a louder right, it is a bug.
        layer->setPan (0.8f);
        group->setPan (0.8f);
        renderSides (panL, panR);

        check ("Three pans past hard right clamp to hard right",
               panR > 0.01f && panL < panR * 0.05f,
               "L " + juce::String (panL, 4) + " / R " + juce::String (panR, 4));

        layer->setPan (0.0f);
        group->setPan (0.0f);

        //-- The note's pan curve joins the same sum ---------------------------
        note->modTarget = NoteModTarget::pan;
        note->envelope.setEnabled (true);
        note->envelope.loadPoints ({ { 0.0f, 1.0f }, { 1.0f, 1.0f } });   // +1 offset

        renderSides (panL, panR);

        check ("A note's pan curve is the third term of the same sum",
               panR > 0.01f && panL < panR * 0.05f,
               "L " + juce::String (panL, 4) + " / R " + juce::String (panR, 4));

        //-- A level curve that is not a level curve --------------------------
        // Back on pan, and the overlay must stop counting it as a level factor.
        // Otherwise the picture would claim a 0.5 level change that the ear does
        // not get, because the engine is sweeping pan with that curve.
        {
            NoteEnvelopePanel panel (t, r);
            panel.setSize (NoteEnvelopePanel::panelWidth, NoteEnvelopePanel::panelHeight);
            panel.openFor (0, noteId);

            const auto& overlay = panel.getOverlay();

            check ("A pan curve is drawn as pan, not folded into the level product",
                   overlay.noteLive == false,
                   overlay.noteLive ? juce::String (utf8 ("被当成电平曲线"))
                                    : juce::String (utf8 ("不计入电平")));

            panel.close();
        }

        //-- The overlay's product is the renderer's product -------------------
        note->modTarget = NoteModTarget::level;
        note->envelope.loadPoints ({ { 0.0f, 0.5f }, { 1.0f, 0.5f } });
        note->envelope.setEnabled (true);

        layerLane.setEnabled (true);
        groupLane.setEnabled (true);

        note->modTarget = NoteModTarget::level;

        {
            NoteEnvelopePanel panel (t, r);
            panel.setSize (NoteEnvelopePanel::panelWidth, NoteEnvelopePanel::panelHeight);
            panel.openFor (0, noteId);

            const auto& overlay = panel.getOverlay();

            const float drawn = automation::stackLevel (overlay.macroAt (0.5f),
                                                        overlay.groupAt (0.5f),
                                                        note->envelope.valueAt (0.5f));

            const float rendered = peakNow() / plain;

            check ("The picture's product and the renderer's product are one number",
                   overlay.macroLive && overlay.groupLive && overlay.noteLive
                     && std::abs (drawn - rendered) < 0.01f,
                   "drawn " + juce::String (drawn, 4)
                     + " / rendered " + juce::String (rendered, 4));

            panel.close();
        }
    } ();

    // Put the fixture back before leaving: the checks edit the live session
    // (they scroll the roll, send synths around, add layers), so without this
    // the snapshot would photograph the leftovers of the last check instead of
    // the demo.
    loadDemoProject();

    //-- Phase 35: the demo itself carries a mixed group ----------------------
    // Checked AFTER the rebuild, because the point is what ships to the user:
    // the snapshot's group pill, its bracket and its count all read off this
    // state, and a demo that quietly lost the block from the group would make
    // every picture of this phase a picture of nothing.
    [&]
    {
        auto* layer = stack.getActiveLayer();

        if (layer == nullptr || layer->getSamples().empty())
        {
            check ("The demo has a block to put in a group", false, "no clip");
            return;
        }

        const auto& clip = layer->getSamples().front();
        const auto* group = layer->findGroup (clip.groupId);

        check ("The demo ships a group holding notes AND a block",
               group != nullptr
                 && group->getId() == clip.groupId
                 && layer->countSamplesInGroup (clip.groupId) == 1
                 && layer->countNotesInGroup (clip.groupId) > 0,
               group != nullptr ? group->getName() + utf8 ("：")
                                    + juce::String (layer->countNotesInGroup (group->getId()))
                                    + utf8 (" 音符 + ")
                                    + juce::String (layer->countSamplesInGroup (group->getId()))
                                    + utf8 (" 采样")
                                : juce::String (utf8 ("块不在任何组里")));

        if (group == nullptr)
            return;

        const int  layerIndex = stack.getActiveLayerIndex();
        const auto frame      = pianoRoll.groupFrameOf (layerIndex, group->getId());
        const auto clipBox    = pianoRoll.getSampleScreenBounds (layerIndex, clip.id);

        check ("...and the bracket reaches around the block",
               ! frame.isEmpty() && ! clipBox.isEmpty() && frame.contains (clipBox),
               "frame " + frame.toString() + " clip " + clipBox.toString());
    } ();


    //-- Phase 46: 混音链三件套 — EQ / 压缩 / 混响 --------------------------
    {
        constexpr double sr = 44100.0;
        constexpr int    n  = (int) (sr * 0.5);

        auto rmsRange = [] (const juce::AudioBuffer<float>& b, int from, int to)
        {
            double acc = 0.0;
            const int count = juce::jmax (1, to - from);
            for (int i = from; i < to; ++i)
                acc += (double) b.getSample (0, i) * (double) b.getSample (0, i);
            return (float) std::sqrt (acc / (double) count);
        };

        //-- EQ：三段互相独立，没动的频段必须透明 --------------------------
        {
            // 80 Hz + 6 kHz, equal amplitude. Boosting ONE band must move only
            // that band's share; a band left at 0 dB must be bit-transparent,
            // not "about the same" — the shelf bypass exists for exactly that.
            auto eqRender = [sr, n] (const std::array<float, fx::maxParams>& params)
            {
                juce::AudioBuffer<float> b (2, n);
                b.clear();   // AudioBuffer's (ch, samples) ctor mallocs WITHOUT zeroing
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < n; ++i)
                        b.setSample (ch, i,
                            (float) (0.4 * std::sin (2.0 * juce::MathConstants<double>::pi * 80.0
                                                       * (double) i / sr)
                                   + 0.4 * std::sin (2.0 * juce::MathConstants<double>::pi * 6000.0
                                                       * (double) i / sr)));

                fx::ChainState st;
                st.prepare (sr);
                fx::Settings s;
                s.type   = fx::Type::eq;
                s.params = params;
                st.processSlot (0, s, b, n, sr);
                return b;
            };

            auto bandRatio = [n] (const juce::AudioBuffer<float>& b)
            {
                constexpr double sr = 44100.0;
                const float a = 1.0f - std::exp (-2.0f * juce::MathConstants<float>::pi
                                                    * 1000.0f / (float) sr);
                double lo = 0.0, hi = 0.0;
                float lp = 0.0f;
                for (int i = 0; i < n; ++i)
                {
                    const float x = b.getSample (0, i);
                    lp += a * (x - lp);
                    lo += (double) lp * lp;
                    hi += (double) (x - lp) * (x - lp);
                }
                return (float) std::sqrt (hi / juce::jmax (1.0e-12, lo));
            };

            const auto flatParams = []
            { std::array<float, fx::maxParams> a; a.fill (0.5f); return a; }();

            const float ratioFlat = bandRatio (eqRender (flatParams));

            // 透明基准：同一段信号不过任何效果器
            juce::AudioBuffer<float> untouched (2, n);
            untouched.clear();
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < n; ++i)
                    untouched.setSample (ch, i,
                        (float) (0.4 * std::sin (2.0 * juce::MathConstants<double>::pi * 80.0
                                                   * (double) i / sr)
                               + 0.4 * std::sin (2.0 * juce::MathConstants<double>::pi * 6000.0
                                                   * (double) i / sr)));
            const float ratioRaw = bandRatio (untouched);

            auto lowUp  = flatParams;  lowUp[(size_t) 3]  = 1.0f;   // 低频增益 +12 dB
            auto highUp = flatParams;  highUp[(size_t) 5] = 1.0f;   // 高频增益 +12 dB

            const float ratioLowBoost  = bandRatio (eqRender (lowUp));
            const float ratioHighBoost = bandRatio (eqRender (highUp));

            check (utf8 ("三段 EQ：全平时透明，只提低频/只提高频各动各的频段"),
                   std::abs (ratioFlat - ratioRaw) < 0.02f * juce::jmax (0.01f, ratioRaw)
                     && ratioLowBoost  < ratioFlat * 0.45f
                     && ratioHighBoost > ratioFlat * 2.0f,
                   utf8 ("全平 ") + juce::String (ratioFlat, 3)
                     + utf8(" / 原始 ") + juce::String (ratioRaw, 3)
                     + utf8(" / 提低 ") + juce::String (ratioLowBoost, 3)
                     + utf8(" / 提高 ") + juce::String (ratioHighBoost, 3));

            // Q 值：中频同样 +12 dB，窄 Q 只动峰边一小圈，宽 Q 连 3 kHz 一起抬
            auto midBoost = [] (float q)
            {
                std::array<float, fx::maxParams> a;
                a.fill (0.5f);
                a[(size_t) 1] = 0.5f;   // 中频频率 0.5 = 1000 Hz
                a[(size_t) 4] = 1.0f;   // 中频增益 +12 dB
                a[(size_t) 7] = q;
                return a;
            };

            const float rmsNarrowQ = rmsRange (eqRender (midBoost (1.0f)), n / 4, n);
            const float rmsWideQ   = rmsRange (eqRender (midBoost (0.0f)), n / 4, n);

            check (utf8 ("中频 Q 值真的到达 DSP：宽 Q 把 3 kHz 也抬了起来，窄 Q 没有"),
                   rmsWideQ > rmsNarrowQ * 1.04f,
                   utf8 ("窄 Q ") + juce::String (rmsNarrowQ, 4)
                     + utf8(" / 宽 Q ") + juce::String (rmsWideQ, 4));
        }

        //-- 压缩：阈值/比率/拐点，各是各的旋钮 -----------------------------
        {
            auto compSettings = [] (float threshold, float ratio, float makeup, float knee)
            {
                fx::Settings s;
                s.type = fx::Type::comp;
                s.params.fill (0.5f);
                s.params[(size_t) 0] = threshold;   // -40 .. 0 dB
                s.params[(size_t) 1] = ratio;       // 1 .. 20
                s.params[(size_t) 2] = makeup;      // 0 .. +18 dB
                s.params[(size_t) 5] = knee;        // 0 .. 24 dB
                return s;
            };

            auto render = [sr, n] (const fx::Settings& s, float amp)
            {
                juce::AudioBuffer<float> in (2, n), out (2, n);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < n; ++i)
                        in.setSample (ch, i,
                            (float) (amp * std::sin (2.0 * juce::MathConstants<double>::pi * 440.0
                                                       * (double) i / sr)));
                out = in;
                fx::ChainState st;
                st.prepare (sr);
                st.processSlot (0, s, out, n, sr);
                return std::pair (in, out);
            };

            auto inOutGain = [rmsRange, n] (const std::pair<juce::AudioBuffer<float>,
                                                            juce::AudioBuffer<float>>& io)
            {
                return rmsRange (io.second, n / 2, n)
                     / juce::jmax (1.0e-9f, rmsRange (io.first, n / 2, n));
            };

            const auto thrasher = compSettings (0.0f, 1.0f, 0.0f, 0.5f);
                                                                  // 阈值 -40 dB，20:1，无补偿
            const float gainTiny = inOutGain (render (thrasher, 0.004f));   // -48 dB：阈值之下
            const float gainLoud = inOutGain (render (thrasher, 0.90f));

            check (utf8 ("压缩：阈值之下的不动，阈值之上的狠压（阈值与比率是真的）"),
                   gainTiny > 0.9f && gainLoud < 0.75f && gainLoud < gainTiny * 0.7f,
                   utf8 ("小声增益 ") + juce::String (gainTiny, 3)
                     + utf8(" / 大声增益 ") + juce::String (gainLoud, 3));

            // 拐点：比阈值略低的电平上，硬拐点完全不压，软拐点已经开始压
            const float gainHard = inOutGain (render (compSettings (0.0f, 1.0f, 0.0f, 0.0f), 0.0095f));
            const float gainSoft = inOutGain (render (compSettings (0.0f, 1.0f, 0.0f, 1.0f), 0.0095f));

            check (utf8 ("拐点软硬度是真的：同一电平，硬拐点没压，软拐点已开始压"),
                   gainHard > 0.95f && gainSoft < gainHard * 0.9f,
                   utf8 ("硬 ") + juce::String (gainHard, 3)
                     + utf8(" / 软 ") + juce::String (gainSoft, 3));
        }

        //-- 混响：尾巴、长度、低切，各归各 ---------------------------------
        {
            auto flatReverb = []
            { std::array<float, fx::maxParams> a; a.fill (0.5f); return a; }();

            auto impulseTail = [sr, n] (const std::array<float, fx::maxParams>& params)
            {
                juce::AudioBuffer<float> impulse (2, n);
                impulse.clear();   // AudioBuffer's (channels, samples) ctor mallocs
                                   // WITHOUT zeroing — measured, not assumed.
                impulse.setSample (0, 0, 0.8f);
                impulse.setSample (1, 0, 0.8f);

                fx::ChainState st;
                st.prepare (sr);
                fx::Settings s;
                s.type   = fx::Type::reverb;
                s.params = params;
                st.processSlot (0, s, impulse, n, sr);
                return impulse;
            };

            {
                const auto impulse = impulseTail (flatReverb);

                const float direct = impulse.getSample (0, 0);
                const float early  = rmsRange (impulse, (int) (0.10 * sr), (int) (0.30 * sr));
                const float late   = rmsRange (impulse, (int) (0.35 * sr), (int) (0.49 * sr));

                check (utf8 ("混响：冲激直通之后还有可闻的尾巴"),
                       direct > 0.4f && direct < 0.75f && early > 5.0e-4f,
                       utf8 ("直通 ") + juce::String (direct, 3)
                         + utf8(" / 早尾 ") + juce::String (early, 5));

                check (utf8 ("混响尾巴在衰减，不是嗡嗡不停"),
                       late < early * 0.8f,
                       utf8 ("早尾 ") + juce::String (early, 5)
                         + utf8(" / 晚尾 ") + juce::String (late, 5));

                // 长度旋钮：短混响（0.2 s）的晚尾要比长混响（10 s）小得多
                auto shortDecay = flatReverb; shortDecay[(size_t) 0] = 0.0f;
                auto longDecay  = flatReverb; longDecay[(size_t) 0]  = 1.0f;

                const float lateShort = rmsRange (impulseTail (shortDecay),
                                                  (int) (0.35 * sr), (int) (0.49 * sr));
                const float lateLong  = rmsRange (impulseTail (longDecay),
                                                  (int) (0.35 * sr), (int) (0.49 * sr));

                check (utf8 ("混响长度旋钮是真的：短混响的晚尾远小于长混响"),
                       lateLong > lateShort * 3.0f,
                       utf8 ("短 ") + juce::String (lateShort, 5)
                         + utf8(" / 长 ") + juce::String (lateLong, 5));

                // 低切：800 Hz 低切把尾巴里的低频段切掉一大截
                auto cutOff = flatReverb; cutOff[(size_t) 2] = 0.0f;   // 20 Hz ≈ 不切
                auto cutOn  = flatReverb; cutOn[(size_t) 2]  = 1.0f;   // 800 Hz

                auto lowBand = [sr] (const juce::AudioBuffer<float>& b)
                {
                    constexpr double srD = 44100.0;
                    const float a = 1.0f - std::exp (-2.0f * juce::MathConstants<float>::pi
                                                        * 200.0f / (float) srD);
                    double acc = 0.0;
                    float lp = 0.0f;
                    for (int i = (int) (0.05 * sr); i < (int) (0.49 * sr); ++i)
                    {
                        lp += a * (b.getSample (0, i) - lp);
                        acc += (double) lp * lp;
                    }
                    return (float) std::sqrt (acc / juce::jmax (1, (int) (0.44 * sr)));
                };

                const float lowNoCut = lowBand (impulseTail (cutOff));
                const float lowCut   = lowBand (impulseTail (cutOn));

                check (utf8 ("低切频率是真的：800 Hz 低切把尾巴的低频段切掉"),
                       lowCut < lowNoCut * 0.6f && lowNoCut > 1.0e-5f,
                       utf8 ("不切 ") + juce::String (lowNoCut, 6)
                         + utf8(" / 切 ") + juce::String (lowCut, 6));
            }
        }

        //-- 枚举追加不串位：vst=5 被老工程冻结，新三件只能排后面 ------------
        check (utf8 ("九个效果器枚举值经 typeFromInt 往返不串位"),
               fx::numTypes() == 9
                 && fx::typeFromInt (5) == fx::Type::vst
                 && fx::typeFromInt (6) == fx::Type::eq
                 && fx::typeFromInt (7) == fx::Type::comp
                 && fx::typeFromInt (8) == fx::Type::reverb,
               utf8 ("numTypes ") + juce::String (fx::numTypes()));

        //-- 真实通道集成：引擎真的把三件 DSP 接进了处理链 -------------------
        {
            MixerModel mm;
            LayerStack t;
            RackModel  r;

            const int sid = r.getFirstSlotId();

            t.addLayer (utf8 ("混响尾巴"), palette::layer1);
            t.setLayerOutputSlot (0, sid);
            t.getLayer (0)->addNote ({ 0, 60, 0.0, 1.0, 0.9f });   // 一拍 @120bpm = 0.5 s
            r.setMixerChannel (sid, 0);

            PlaybackEngine e;
            e.setSampleRate (44100.0);
            e.prepareToPlay (44100.0, 512);

            const int total = (int) (sr * 3.0);

            auto tailRms = [&]
            {
                e.publishSchedule (t, r, 120.0, 0.0, &mm);
                e.resetMixState();

                juce::AudioBuffer<float> buffer (2, total);
                buffer.clear();
                e.renderWindow (0.0, total, buffer, nullptr);

                // 音符在 0.5 s 结束；0.7 s 之后只剩效果器的尾巴。
                return rmsRange (buffer, (int) (sr * 0.7), (int) (sr * 2.5));
            };

            mm.setInsertType (0, 0, fx::Type::reverb);
            mm.setInsertParam (0, 0, 0, 0.8f);   // 混响长度 0.8 ≈ 5.3 s decay
            const float withVerb = tailRms();

            mm.setInsertEnabled (0, 0, false);
            const float bypassed = tailRms();

            check (utf8 ("挂在真实通道上的混响，音符结束之后尾巴还在响（引擎接上了 DSP）"),
                   withVerb > 5.0e-4f && withVerb > bypassed * 5.0f,
                   utf8 ("混响开 ") + juce::String (withVerb, 5)
                     + utf8(" / 旁通 ") + juce::String (bypassed, 6));
        }
        //-- Phase 47: 效果器编辑窗口——标题有名字、窗口随参数长高、按键有转发 --
        {
            // 参数表是 DSP / 编辑窗口 / 机架三方共用的表，先把表本身验了
            check (utf8 ("三个新效果器的参数表：数量与名字对得上"),
                   fx::numParams (fx::Type::eq) == 9
                     && fx::numParams (fx::Type::comp) == 6
                     && fx::numParams (fx::Type::reverb) == 5
                     && fx::paramName (fx::Type::eq, 0) == utf8 ("低频频率")
                     && fx::paramName (fx::Type::eq, 8) == utf8 ("高频Q值")
                     && fx::paramName (fx::Type::comp, 5) == utf8 ("拐点软硬度")
                     && fx::paramName (fx::Type::reverb, 4) == utf8 ("散射度"),
                   utf8 ("EQ 9 / COMP 6 / REVERB 5"));

            // 编辑窗口：EQ 的窗口要比混响的高，标题不再是空白的“效果器”
            const bool titleNamed = ! InsertEditorContent::effectName (fx::Type::eq).isEmpty()
                                      && InsertEditorContent::effectName (fx::Type::comp)
                                           == utf8 ("压缩");

            check (utf8 ("内置效果器编辑窗口：标题有名字，窗口高度跟着参数数量走"),
                   titleNamed
                     && InsertEditorContent::preferredHeightFor (9)
                          > InsertEditorContent::preferredHeightFor (5),
                   utf8 ("9 行高 ") + juce::String (InsertEditorContent::preferredHeightFor (9))
                     + utf8(" / 5 行高 ") + juce::String (InsertEditorContent::preferredHeightFor (5)));

            // 按键转发：窗口是独立的顶层组件，不装转发器空格和键盘就断
            mixer.setInsertType (0, 1, fx::Type::eq);
            openInsertEditor (0, 1);

            const bool opened = ! insertWindows.empty()
                                  && insertWindows.back() != nullptr
                                  && insertWindows.back()->getChannel() == 0
                                  && insertWindows.back()->getSlot() == 1;

            const bool forwards = opened && insertWindows.back()->hasKeyForwarder();

            check (utf8 ("效果器编辑窗口开着时，按键也被接管（空格走带 / 键盘弹音）"),
                   opened && forwards,
                   opened ? (forwards ? utf8 ("窗口已接管按键") : utf8 ("窗口没有按键转发"))
                          : utf8 ("窗口没打开"));

            if (opened)
                closeInsertEditor (insertWindows.back().get());

            mixer.setInsertType (0, 1, fx::Type::none);   // 把 fixture 还回去
        }
    }

    // Phase 34: hand the real dialog and the message-queue deferral back. A
    // scripted answer left in place would make the running app delete samplers
    // without ever asking — the exact thing this phase exists to stop.
    deferSamplerPrompt = true;
    installSamplerCascadePrompt();

    // The summary MUST come after the last check. It used to sit mid-function
    // (before the fixture restore), and Phase 46's reverb block landed after
    // it — three FAILs printed, summary still said ALL CHECKS PASSED, and the
    // P44 exit-code gate read the stale verdict and exited 0. A summary that
    // does not cover every check is worse than no summary.
    report << "\n  " << (failures == 0 ? "ALL CHECKS PASSED"
                                        : juce::String (failures) + " CHECK(S) FAILED") << "\n";

    return report;
}

} // namespace atomic
