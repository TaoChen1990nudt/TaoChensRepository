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
#include "Util/Theme.h"

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

        // Phase 33 + 51: the STOP button, as one pure function. Running ->
        // stop and return to where THIS run started (the caller passes the
        // beat playTransport() recorded; the default 0 keeps no-anchor
        // callers identical to the old rewind-to-top). Idle -> nothing at
        // all, because the playhead is also the "play from here" control and
        // a reflex stop must not rewind it.
        const auto whileRunning = stopDecision (true, 20.5);
        const auto whileIdle    = stopDecision (false, 20.5);

        check ("Stop while running returns to the run's own start",
               whileRunning.stop && whileRunning.beat == 20.5,
               juce::String (whileRunning.beat, 1));

        check ("Stop without a remembered start still lands on zero",
               stopDecision (true).stop && stopDecision (true).beat == 0.0,
               juce::String (stopDecision (true).beat, 1));

        check ("Stop while already stopped does nothing (the playhead is a choice)",
               ! whileIdle.stop,
               whileIdle.stop ? "it moved" : "left alone");

        // Phase 33: the space bar. Idle -> play, rolling -> stop. Phase 32 had
        // it stop-only, which is why the keyboard could not start playback.
        check ("Space plays when idle and stops while rolling",
               spaceStartsPlayback (false) && ! spaceStartsPlayback (true),
               "idle=play / rolling=stop");

        // Phase 51: the real path, headless (no sound card — that is what the
        // openDevice flag is for). Start a run at 20.5, stop it, and the head
        // must be back at 20.5: the run's own beginning. Return stays the
        // explicit rewind to the top.
        audio.setPositionBeats (20.5);
        playTransport (false);

        const bool runStarted = audio.isTransportEngaged();

        stopTransport();

        const double parkedAt = audio.getPositionBeats();

        check ("A run started at 20.5 stops back at 20.5",
               runStarted && ! audio.isTransportEngaged()
                 && std::abs (parkedAt - 20.5) < 1.0e-9,
               juce::String (parkedAt, 2));

        rewindTransport();

        check ("Return still rewinds to the top from anywhere",
               std::abs (audio.getPositionBeats()) < 1.0e-9
                 && ! audio.isTransportEngaged(),
               juce::String (audio.getPositionBeats(), 2));
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
            stage.setSampleRate (bounce::exportSampleRate);
            stage.publishSchedule (tx, rx, 120.0);

            auto schedule = stage.getSchedule();

            check ("The export length is the last note's end, not the 64-beat grid",
                   schedule != nullptr && std::abs (schedule->contentBeats - 8.0) < 1.0e-9,
                   juce::String (schedule == nullptr ? -1.0 : schedule->contentBeats, 3) + " beats");

            const auto unityFile  = testDir.getChildFile ("bounce-unity.wav");
            const auto quietFile  = testDir.getChildFile ("bounce-minus12.wav");

            const auto unity = bounce::renderToFile (unityFile, *schedule, bounce::exportSampleRate, 0.0);
            const auto quiet = [&]
            {
                tx.setLayerLevelDb (lx, -12.0f);
                stage.publishSchedule (tx, rx, 120.0);
                return bounce::renderToFile (quietFile, *stage.getSchedule(), bounce::exportSampleRate, 0.0);
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
                const auto cancelled = bounce::renderToFile (cancelFile, *schedule, bounce::exportSampleRate, 0.0,
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

        //-- Phase 71: one gesture, one undo step --------------------------
        // The shape of a real drag: one press, four mouse-move updates, one
        // release. Before Phase 71 each of the four committed its own state, so
        // "undo" walked a four-beat drag back one beat at a time.
        {
            auto* dragLayer = stack.getLayer (0);

            if (dragLayer != nullptr && ! dragLayer->getNotes().empty())
            {
                const NoteEvent startNote = dragLayer->getNotes().front();
                const double    fromBeat  = startNote.startBeat;

                cancelPendingUpdate();
                history    = savedHistory;
                historyPos = savedPos;

                const int statesBefore = getHistorySize();

                beginUndoGesture();

                for (int step = 1; step <= 4; ++step)
                {
                    if (auto* moved = stack.getLayer (0)->findNote (startNote.id))
                        moved->startBeat = fromBeat + (double) step;

                    stack.notifyNotesChanged();

                    // What the AsyncUpdater does at the end of every message
                    // turn — while the mouse is still down.
                    cancelPendingUpdate();
                    commitHistory();
                }

                const int statesMidDrag = getHistorySize();

                endUndoGesture();

                check (utf8 ("拖动途中不落任何撤销步骤"),
                       statesMidDrag == statesBefore && ! isUndoGestureActive(),
                       juce::String (statesMidDrag - statesBefore) + utf8 (" 步 / 拖拽中"));

                check (utf8 ("一次拖拽（四次更新）只留一个撤销步骤"),
                       getHistorySize() == statesBefore + 1,
                       juce::String (getHistorySize() - statesBefore) + utf8 (" 步"));

                undo();

                auto* afterUndo = stack.getLayer (0) != nullptr
                                    ? stack.getLayer (0)->findNote (startNote.id) : nullptr;

                check (utf8 ("撤销一次就回到拖拽前的位置"),
                       afterUndo != nullptr
                         && juce::approximatelyEqual (afterUndo->startBeat, fromBeat),
                       afterUndo != nullptr ? juce::String (afterUndo->startBeat, 3)
                                            : juce::String (utf8 ("音符没了")));

                // Back to the demo, whichever position this block left it in.
                cancelPendingUpdate();
                restoreState (baseline);
                cancelPendingUpdate();

                history    = savedHistory;
                historyPos = savedPos;
                savedStateText = captureState();
            }
        }

        //-- Phase 71: the preview of a hosted instrument -------------------
        {
            auto* previewSlot = rack.getSlot (0);

            if (previewSlot != nullptr)
            {
                const int slotId    = previewSlot->id;
                const int printedId = previewSlot->pluginId;

                // A fake instance id: publish only needs `pluginId > 0` to
                // decide a slot is hosted, which is the same recipe the plugin
                // automation fixtures above use.
                previewSlot->pluginId = 7788;
                republish();

                auditionPreview (slotId, 60, 0.9f, true);

                check (utf8 ("图层挂的是 VST 时，预览交给插件演奏而不是正弦"),
                       hostedPreview.pitch == 60 && hostedPreview.slotId == slotId
                         && ! audio.getPlayback().hasAudition(),
                       "held " + juce::String (hostedPreview.pitch)
                         + (audio.getPlayback().hasAudition() ? utf8 (" / 还在发正弦")
                                                              : juce::String()));

                auditionPreview (slotId, 64, 0.9f, false);

                check (utf8 ("预览换音高时先放开旧音"),
                       hostedPreview.pitch == 64,
                       juce::String (hostedPreview.pitch));

                releaseHostedPreview();

                check (utf8 ("预览结束会放开按住的音"),
                       hostedPreview.pitch == -1 && hostedPreview.slotId == -1,
                       juce::String (hostedPreview.pitch));

                // The engine sees the events while the slot is STILL hosted, so
                // they land in its live-plugin table exactly as they would in a
                // real session — one note, held and released.
                juce::AudioBuffer<float> drain;
                drain.setSize (2, 512);
                drain.clear();

                audio.getPlayback().beginLiveBlock();

                check (utf8 ("预览的音符真的交给了插件演奏"),
                       audio.getPlayback().getLivePluginSlotCount() == 1,
                       juce::String (audio.getPlayback().getLivePluginSlotCount())
                         + utf8 (" 个槽"));

                // Now the instrument goes away — the slot is deleted, or it is
                // re-mounted as a built-in synth. The table entry that points at
                // it has to go too: it holds one of eight lives the session has,
                // and nothing renders it any more.
                previewSlot->pluginId = printedId;
                republish();

                audio.getPlayback().beginLiveBlock();
                audio.getPlayback().renderLive (drain, 512);

                check (utf8 ("插件预览放下后引擎不再占着现场槽位"),
                       audio.getPlayback().getLivePluginSlotCount() == 0,
                       juce::String (audio.getPlayback().getLivePluginSlotCount())
                         + utf8 (" 个槽 / ")
                         + (audio.getPlayback().hasLiveNotes() ? utf8 ("仍然占着")
                                                               : utf8 ("已交还")));


                // And the fallback: the same slot as a built-in synth sounds
                // through the oscillator voice, with nothing held on a plugin.
                auditionPreview (slotId, 60, 0.9f, true);

                check (utf8 ("内置合成器的预览仍走振荡器"),
                       audio.getPlayback().hasAudition() && hostedPreview.pitch == -1,
                       audio.getPlayback().hasAudition() ? utf8 ("在发正弦")
                                                         : utf8 ("没声音"));

                // Release it again, or the preview keeps a voice up past this
                // block. Twenty blocks of 512 covers the 200 ms the fade-out
                // takes at any sane sample rate.
                audio.getPlayback().stopAudition();

                for (int block = 0; block < 20; ++block)
                {
                    drain.clear();
                    audio.getPlayback().renderAudition (drain, 512);
                }

                check (utf8 ("停止预览后振荡器会自己收干净"),
                       ! audio.getPlayback().hasAudition(),
                       audio.getPlayback().hasAudition() ? utf8 ("还在响") : utf8 ("已停"));
            }
        }
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

        //-- Phase 20: the toolbar has to fit its buttons (9 since Phase 51) ---
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

            check ("Every toolbar button owns its own pixels (knife included now)",
                   ! overlap
                     && PianoRollComponent::numToolButtons == 9
                     && roll.getToolbarArea().contains (roll.getToolButtonBounds (8)),
                   juce::String (PianoRollComponent::numToolButtons) + utf8 (" 个按钮，右端 ")
                     + juce::String (roll.getToolButtonBounds (8).getRight())
                     + " / 栏宽 " + juce::String (roll.getToolbarArea().getWidth()));

            // Phase 21: 编组 / 解组 are one pair, so the magnet moved past
            // them. The order is the whole request, so the order is the check.
            const auto ungroup = roll.getToolButtonBounds (7);
            const auto magnet  = roll.getToolButtonBounds (8);

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

            //-- Phase 70: a stop is a request the voice has to honour --------
            // `stopSamplePreview()` only cleared a flag and the render loop never
            // read it, so an eight-second audition played to the end no matter
            // what the user clicked. A silent block two blocks after the stop is
            // the thing that says it worked.
            {
                auto drone = makeBuffer (8.0, 220.0, 0.8f);

                PlaybackEngine stopper;
                stopper.setSampleRate (44100.0);
                stopper.previewSample (drone, 8.0);

                juce::AudioBuffer<float> stopHead (2, 4096);
                stopHead.clear();
                stopper.renderSamplePreview (stopHead, 4096);

                stopper.stopSamplePreview();

                juce::AudioBuffer<float> stopRamp (2, 4096);   // the 4 ms fade-out
                stopRamp.clear();
                stopper.renderSamplePreview (stopRamp, 4096);

                juce::AudioBuffer<float> stopTail (2, 4096);
                stopTail.clear();
                stopper.renderSamplePreview (stopTail, 4096);

                check (utf8 ("停止试听后预览真的停了，不会一直播到文件结束"),
                       stopHead.getMagnitude (0, 0, 4096) > 0.001f
                         && stopTail.getMagnitude (0, 0, 4096) < 1.0e-4f
                         && ! stopper.hasSamplePreview(),
                       utf8 ("起始 ") + juce::String (stopHead.getMagnitude (0, 0, 4096), 3)
                         + utf8 ("，停止后 ") + juce::String (stopTail.getMagnitude (0, 0, 4096), 6));
            }
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

        //-- Phase 57: a paste goes BACK into the group it was copied from -----
        // It used to mint "复制组 2": one copy of a group became two rows in the
        // group list, two trims to keep in sync, two mounts, two curves. The
        // paste now lends its members to the source group and nothing is made.
        const bool pasted     = roll.pasteNotesFromClipboard();
        const int  after      = (int) layer->getGroups().size();
        const auto* sameGroup = layer->findGroup (groupId);

        check ("Pasting a copied group reuses that group instead of minting one",
               copied && groupish && pasted
                 && after == before
                 && sameGroup != nullptr
                 && sameGroup->getName() == originalName
                 && sameGroup->getOutputSlotId() == slotB
                 && std::abs (sameGroup->getLevelDb() + 6.0f) < 1.0e-4f
                 && sameGroup->getLevelAutomation().isEnabled()
                 && layer->countNotesInGroup (groupId) == 6,
               juce::String (before) + " -> " + juce::String (after) + utf8 (" 组，组内 ")
                 + juce::String ((int) layer->countNotesInGroup (groupId)) + utf8 (" 个音符"));

        //-- Phase 58: the copy is another APPEARANCE, and the curve is ONE ---
        // "复制到后一小节" used to build a second NoteGroup and drag a copy of the
        // absolute-beat lanes onto it. A second appearance shares the group's
        // one curve: that is what "the same group, twice" has to mean, and it is
        // why the counts below are occurrence counts, not group counts.
        const int  groupsBeforeDup = (int) layer->getGroups().size();
        const auto occBeforeDup    = layer->getGroupOccurrences (groupId);

        const auto firstStartOf = [&] (int occ)
        {
            double at = -1.0;

            for (int id : layer->getOccurrenceNoteIds (groupId, occ))
                if (const auto* note = layer->findNote (id))
                    at = at < 0.0 ? note->startBeat : juce::jmin (at, note->startBeat);

            return at;
        };

        const double srcFirstBeforeDup = firstStartOf (0);

        const bool duped = roll.duplicateGroupToNextBar (0, groupId, 0);

        const auto occurrences = layer->getGroupOccurrences (groupId);

        double dstFirst = -1.0;

        if (occurrences.size() > occBeforeDup.size())
            for (int id : layer->getOccurrenceNoteIds (groupId, occurrences.back()))
                if (const auto* note = layer->findNote (id))
                    dstFirst = dstFirst < 0.0 ? note->startBeat
                                              : juce::jmin (dstFirst, note->startBeat);

        const auto* live = layer->findGroup (groupId);

        check (utf8 ("复制到后一小节") + " adds one appearance and leaves the source where it was",
               duped
                 && (int) layer->getGroups().size() == groupsBeforeDup    // no second group
                 && (int) occurrences.size() == (int) occBeforeDup.size() + 1
                 && dstFirst > srcFirstBeforeDup + 1.0
                 && std::abs (firstStartOf (0) - srcFirstBeforeDup) < 1.0e-6
                 && live != nullptr
                 && live->getLevelAutomation().getPoints().size() >= 2
                 && std::abs (live->getLevelAutomation().getPoints().front().beat) < 1.0e-6,
               juce::String ((int) occurrences.size()) + utf8 (" 处，副本起点 ")
                 + juce::String (dstFirst, 2) + utf8 ("，源起点 ")
                 + juce::String (firstStartOf (0), 2) + utf8 ("（应为 ")
                 + juce::String (srcFirstBeforeDup, 2) + utf8 ("）"));
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

    //-- Phase 29: a block is a window — trim both ends, curve both ends -----
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

        //-- 3: Phase 51 — the tail goes wherever the drag goes ---------------
        // The P30 ceiling ("the extra width is silence, so a block cannot be
        // longer than its file") is gone at the user's request: resample and
        // tempo turn the extra width into a real stretch, and even 原速 may
        // reserve visual space past the file's end. The drag lands where the
        // mouse lands.
        pianoRoll.simulateSampleTrim (a, start + 12.0, true);
        const double afterLonger = clip->lengthBeats;

        check ("A block CAN now be dragged longer than the file it plays",
               std::abs (afterLonger - 12.0) < 1.0e-6,
               utf8 ("自然 ") + juce::String (natural, 3) + utf8 (" 拖到 12 拍后 → ")
                 + juce::String (afterLonger, 3));

        pianoRoll.simulateSampleTrim (a, start + natural * 0.5, true);
        const double afterShorter = clip->lengthBeats;

        pianoRoll.simulateSampleTrim (a, start + 12.0, true);
        const double backOut = clip->lengthBeats;

        check ("...and the drag stays symmetrical: shorten, then back out past the file again",
               afterShorter < natural - 1.0e-6 && std::abs (backOut - 12.0) < 1.0e-6,
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

        //-- 4: the block's own volume curve (P49: fades became a curve) -----
        // The idle curve is flat 1.0 and OFF: a fresh block is "the file,
        // untouched", and the first edit is what switches it on — the same
        // path a click in the box runs.
        check ("A fresh block's curve is idle: disabled and flat at unity",
               ! clip->volumeCurve.isEnabled()
                 && clip->volumeCurve.getPoints().size() == 2
                 && std::abs (clip->volumeCurve.valueAt (0.25f) - 1.0f) < 1.0e-6,
               juce::String (clip->volumeCurve.isEnabled() ? 1 : 0) + ", n="
                 + juce::String ((int) clip->volumeCurve.getPoints().size()));

        // Down-ramp then back to unity: two points MOVED onto new values, the
        // gesture path the box's drags take.
        //
        //-- Phase 65: the switch is gone, so the curve is never a lie --------
        // A block saved by an older build can still carry enabled = false. With
        // the switch deleted there is no way back from that state, so opening
        // the box has to be the way back.
        clip->volumeCurve.setEnabled (false);
        pianoRoll.openSampleAutomation (index, a);

        check (utf8 ("采样自动化框没有开关：打开框就把曲线恢复成生效"),
               clip->volumeCurve.isEnabled(),
               clip->volumeCurve.isEnabled() ? "enabled" : "still bypassed");

        // The edits run as their own statement, not inside the check's
        // condition: argument evaluation order is unspecified, and on the
        // first run the message was built BEFORE the edits ran — it printed
        // the flat curve while the assertion read the edited one. A test that
        // reports numbers from a different moment than the one it judged is
        // worse than no numbers.
        const bool curveEdited = pianoRoll.simulateSampleCurveEdit (a, 0.0f, 0.0f)
                                  && pianoRoll.simulateSampleCurveEdit (a, 1.0f, 0.0f)
                                  && pianoRoll.simulateSampleCurveEdit (a, 0.5f, 1.0f)
                                  && pianoRoll.simulateSampleCurveEdit (a, 0.5f, 0.75f);

        check ("The curve edits like the note envelope: move, not rebuild",
               curveEdited
                 && clip->volumeCurve.isEnabled()
                 && std::abs (clip->volumeCurve.valueAt (0.0f)) < 1.0e-6
                 && std::abs (clip->volumeCurve.valueAt (0.5f) - 0.75f) < 1.0e-6
                 && std::abs (clip->volumeCurve.valueAt (1.0f)) < 1.0e-6,
               "f(0)=" + juce::String (clip->volumeCurve.valueAt (0.0f), 3)
                 + " f(0.5)=" + juce::String (clip->volumeCurve.valueAt (0.5f), 3)
                 + " f(1)=" + juce::String (clip->volumeCurve.valueAt (1.0f), 3));

        // The box is the whole point of P49: exactly as wide as the block, at
        // the zoom the assertion runs at. If these drift, the curve you edit
        // no longer lines up with the block you hear.
        // The block sits at beat 80, well right of a fresh view: bring it into
        // the grid before opening the box, so the assertion tests the real
        // alignment and not the off-screen clamp. The scroll goes back after,
        // because the marquee fixtures below measure unclamped rectangles.
        // `start` is the beat the block sits at; `a` is its id — scrolling to
        // an id clamps to zero and quietly leaves the block off-screen, which
        // is exactly the clamp this assertion is trying to rule out.
        const double scrollBefore = pianoRoll.getScrollBeats();
        pianoRoll.setScrollBeats (clip->startBeat - 2.0);

        pianoRoll.openSampleAutomation (index, a);
        const auto blockRect = pianoRoll.getSampleScreenBounds (index, a);
        const auto autoBox   = pianoRoll.getSampleAutomationBoxForTest (index, a);

        // Below the block, or flipped above when the view has no room — the
        // note box's own escape hatch. Either way: same width, same left edge,
        // never overlapping the block it edits.
        check ("The clip's automation box is exactly as wide as the block",
               ! autoBox.isEmpty()
                 && std::abs (autoBox.getWidth() - blockRect.getWidth()) < 0.75f
                 && std::abs (autoBox.getX() - blockRect.getX()) < 0.75f
                 && (autoBox.getY() >= blockRect.getBottom() - 1.0f
                       || autoBox.getBottom() <= blockRect.getY() + 1.0f),
               juce::String (autoBox.getWidth(), 1) + " vs "
                 + juce::String (blockRect.getWidth(), 1)
                 + ", box x/y " + juce::String (autoBox.getX(), 1) + "/"
                 + juce::String (autoBox.getY(), 1)
                 + ", block x " + juce::String (blockRect.getX(), 1)
                 + " top/bottom " + juce::String (blockRect.getY(), 1) + "/"
                 + juce::String (blockRect.getBottom(), 1));

        // Phase 50: and the curve INSIDE the box lines up with the block, to
        // the pixel. P49 inset it by 10% of the box width, which at 48 px/beat
        // put the first point a beat and a half right of the audio it shapes.
        const auto curveArea = pianoRoll.getSampleCurveAreaForTest (index, a);

        check ("The curve's x range IS the block's x range",
               ! curveArea.isEmpty()
                 && std::abs (curveArea.getX() - blockRect.getX()) < 0.75f
                 && std::abs (curveArea.getWidth() - blockRect.getWidth()) < 0.75f,
               "curve x/w " + juce::String (curveArea.getX(), 1) + "/"
                 + juce::String (curveArea.getWidth(), 1)
                 + ", block x/w " + juce::String (blockRect.getX(), 1) + "/"
                 + juce::String (blockRect.getWidth(), 1));

        //-- Phase 65: 开关去掉之后，菜单行和目标按钮必须还是同一个矩形 -----
        // A deleted widget leaves no compile error — it leaves a gap. The row
        // and the target button being identical is what proves the row has no
        // switch-shaped hole at its right end.
        const auto rowArea = pianoRoll.getSampleMenuAreaForTest (index, a);
        const auto tapArea = pianoRoll.getSampleTargetAreaForTest (index, a);

        check (utf8 ("采样自动化框的菜单行整行都是目标按钮（开关已去掉）"),
               ! rowArea.isEmpty()
                 && std::abs (tapArea.getWidth() - rowArea.getWidth()) < 0.01f
                 && std::abs (tapArea.getRight() - rowArea.getRight()) < 0.01f,
               juce::String (tapArea.getWidth(), 1) + utf8 (" vs ")
                 + juce::String (rowArea.getWidth(), 1));

        // One floating editor at a time: opening the clip box must have
        // closed whatever the note box was showing, and the second toggle
        // closes it again.
        pianoRoll.openSampleAutomation (index, b);
        check ("Opening a second clip's box closes the first, toggling closes both",
               pianoRoll.isSampleAutomationOpen()
                 && pianoRoll.getExpandedSampleId() == b,
               "expanded=" + juce::String (pianoRoll.getExpandedSampleId()));
        pianoRoll.closeSampleAutomation();
        check ("...and closeSampleAutomation leaves nothing open",
               ! pianoRoll.isSampleAutomationOpen(), "id still set");
        pianoRoll.setScrollBeats (scrollBefore);

        // The end values survive a whole-struct copy: the curve travels with
        // the clip like the envelope travels with a note.
        {
            const SampleEvent copy = *clip;
            check ("Copying the clip carries the curve",
                   copy.volumeCurve.hasSameShapeAs (clip->volumeCurve), "shape mismatch");
        }

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

    //-- Phase 49: a v20 file's fades migrate onto the volume curve -----------
    [&]
    {
        LayerStack s1;
        RackModel  r1;

        s1.addLayer (utf8 ("迁移"), palette::layer1);

        SampleEvent clip;
        clip.name        = "legacy";
        clip.startBeat   = 2.0;
        clip.lengthBeats = 2.0;
        clip.pitch       = 60;
        clip.laneSpan    = 2;

        s1.getLayer (0)->addSample (clip);

        // Save as v21, then surgically turn the file back into what v20 wrote:
        // no <Envelope> child, and the two fade attributes on the <Sample> tag
        // instead. The reader has to meet the file it used to meet.
        juce::String xml = project::toXmlText (s1, r1, 120.0);

        const int sampleTag = xml.indexOf ("<Sample ");
        const int tagEnd    = xml.indexOfChar (sampleTag, '>');
        const int envStart  = xml.indexOf ("<Envelope");
        const juce::String envClose = "</Envelope>";
        const int envEnd    = xml.indexOf (envStart, envClose) + envClose.length();

        check ("The v21 writer emits an envelope where v20 wrote fades",
               sampleTag >= 0 && tagEnd > sampleTag
                 && envStart > tagEnd && envEnd > envStart,
               "sample " + juce::String (sampleTag) + ", envelope " + juce::String (envStart));

        if (sampleTag < 0 || tagEnd <= sampleTag || envStart <= tagEnd || envEnd <= envStart)
            return;

        juce::String legacy = xml.substring (0, tagEnd)
                                + " fadein=\"0.25\" fadeout=\"0.5\""
                                + xml.substring (tagEnd, envStart)
                                + xml.substring (envEnd);

        LayerStack s2;
        RackModel  r2;
        double bpm2 = 0.0;
        juce::String error;

        const bool ok = project::fromXmlText (legacy, s2, r2, bpm2, error, nullptr, nullptr, nullptr);

        const auto* back = (s2.getLayer (0) != nullptr && s2.getLayer (0)->getSampleCount() == 1)
                             ? &s2.getLayer (0)->getSamples().front() : nullptr;

        // fadein 0.25 of a 2.0-beat block = ramp to full at t=0.125; fadeout
        // 0.5 = fade begins at t=0.75. The migration has to reproduce exactly
        // the shape the old audio multiplied, or an old project changes sound.
        check ("A v20 project's fades open as the same ramp on the curve",
               ok && back != nullptr
                 && back->volumeCurve.isEnabled()
                 && back->volumeCurve.getPoints().size() == 4
                 && std::abs (back->volumeCurve.valueAt (0.0f)) < 1.0e-6
                 && std::abs (back->volumeCurve.valueAt (0.5f) - 1.0f) < 1.0e-6
                 && std::abs (back->volumeCurve.valueAt (1.0f)) < 1.0e-6
                 && std::abs (back->volumeCurve.getPoints()[(size_t) 1].time - 0.125f) < 1.0e-5
                 && std::abs (back->volumeCurve.getPoints()[(size_t) 2].time - 0.75f)  < 1.0e-5,
               ok && back != nullptr
                 ? "n=" + juce::String ((int) back->volumeCurve.getPoints().size())
                     + " t1=" + juce::String (back->volumeCurve.getPoints()[(size_t) 1].time, 4)
                     + " t2=" + juce::String (back->volumeCurve.getPoints()[(size_t) 2].time, 4)
                 : error);
    } ();

    //-- Phase 49: the clip's curve is audible — ramp, and full-down silence --
    [&]
    {
        LayerStack     t;
        RackModel      r;
        PlaybackEngine e;

        e.setSampleRate (48000.0);
        t.addLayer (utf8 ("P49 曲线"), palette::layer1);
        t.setLayerLevelDb (0, 0.0f);
        t.setLayerPan (0, 0.0f);

        const auto loaded  = loadSampleFile (phase27B);
        const int  sampler = r.addSampler (loaded.buffer, phase27B.getFullPathName(), "P49 bus");

        auto* layer = t.getLayer (0);

        SampleEvent clip;
        clip.name         = "P49";
        clip.path         = phase27B.getFullPathName();
        clip.audio        = loaded.buffer;
        clip.startBeat    = 0.0;
        clip.lengthBeats  = 8.0;               // 0.0 - 4.0 s at 120 bpm
        clip.pitch        = 60;
        clip.laneSpan     = 2;
        clip.outputSlotId = sampler;

        const int clipId = layer->addSample (clip);

        // The test file decays as it plays, so the honest measurement is the
        // SAME window rendered twice — curve off vs curve on — and the ratio
        // read against the curve's own value at that window. Any shape error
        // in the seconds conversion lands in these ratios.
        auto renderTo = [&] (juce::AudioBuffer<float>& b)
        {
            b.clear();
            e.resetMixState();
            e.publishSchedule (t, r, 120.0);
            e.renderWindow (0.0, b.getNumSamples(), b, nullptr);
        };

        const double sr = 48000.0;

        auto windowMagnitude = [&] (const juce::AudioBuffer<float>& b, double from, double span)
        {
            const int first = (int) std::llround (from * sr);
            const int count = juce::jmax (1, (int) std::llround (span * sr));
            return b.getMagnitude (0, first, count);
        };

        juce::AudioBuffer<float> withOff (2, (int) (2.0 * sr));
        renderTo (withOff);

        check ("A clip with an idle curve sounds untouched",
               windowMagnitude (withOff, 0.10, 0.2) > 0.01f,
               "peak " + juce::String (windowMagnitude (withOff, 0.10, 0.2), 4));

        // The 0 -> 1 ramp the migration also produces, over a FOUR second block.
        // That length is the whole point: the curve is normalised, the renderer
        // measures seconds, and only a block that is not exactly one second
        // long lets the two disagree. A 1 s block makes the conversion a no-op
        // — which is how a 2x unit error walked straight through the first
        // version of this test.
        if (auto* c = layer->findSample (clipId))
        {
            c->volumeCurve.loadPoints ({ { 0.0f, 0.0f }, { 1.0f, 1.0f } });
            c->volumeCurve.setEnabled (true);
        }

        juce::AudioBuffer<float> withRamp (2, (int) (2.0 * sr));
        renderTo (withRamp);

        // Same window, curve on vs curve off: the file's own decay cancels, so
        // the ratio IS the curve's value there. A linear 0..1 ramp over 4 s
        // reads 0.25 at 1.0 s and 0.40 at 1.6 s; a normalised x would read 1.0
        // at both, and even a plain "quieter early than late" test would pass.
        auto curveAt = [&] (double centre)
        {
            return (double) windowMagnitude (withRamp, centre - 0.05, 0.10)
                     / juce::jmax (1.0e-9f, windowMagnitude (withOff, centre - 0.05, 0.10));
        };

        const double atOne = curveAt (1.0);
        const double atLate = curveAt (1.6);

        check ("The clip's curve is read in seconds, not in normalised time",
               std::abs (atOne - 0.25) < 0.06 && std::abs (atLate - 0.40) < 0.06,
               "1.0 s x" + juce::String (atOne, 3) + utf8 (" (应为 0.250) / 1.6 s x")
                 + juce::String (atLate, 3) + utf8 (" (应为 0.400)"));

        // Full-down: a curve pinned at zero is a mute. The de-click ramp stays,
        // so the block is silent rather than producing a discontinuity spike.
        if (auto* c = layer->findSample (clipId))
        {
            c->volumeCurve.loadPoints ({ { 0.0f, 0.0f }, { 1.0f, 0.0f } });
            c->volumeCurve.setEnabled (true);
        }

        juce::AudioBuffer<float> withMute (2, (int) (2.0 * sr));
        renderTo (withMute);

        const double muteRatio = windowMagnitude (withMute, 0.10, 0.2)
                                   / juce::jmax (1.0e-9f, windowMagnitude (withOff, 0.10, 0.2));

        check ("A curve pinned at zero silences the block",
               muteRatio < 1.0e-3f, "x" + juce::String (muteRatio, 6));

        // Switching the curve off puts the file back, shape kept.
        if (auto* c = layer->findSample (clipId))
            c->volumeCurve.setEnabled (false);

        juce::AudioBuffer<float> restored (2, (int) (2.0 * sr));
        renderTo (restored);

        check ("Bypassing the curve restores the untouched file",
               windowMagnitude (restored, 0.10, 0.2)
                 > windowMagnitude (withOff, 0.10, 0.2) * 0.95f,
               "peak " + juce::String (windowMagnitude (restored, 0.10, 0.2), 4)
                 + " vs " + juce::String (windowMagnitude (withOff, 0.10, 0.2), 4));
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
        // no grid point lands on it. (Phase 51: the tail now goes wherever the
        // drag goes — the file's end is no longer a ceiling, so the drag aims
        // AT the end instead of relying on the old clamp to stop there.)
        pianoRoll.simulateSampleTrim (id, startBeat + 0.5, true);
        pianoRoll.simulateSampleTrim (id, startBeat + exactBeats, true);

        check ("...and the tail can be dragged back to exactly the end of the file",
               std::abs (clip->lengthBeats - exactBeats) < 1.0e-9,
               juce::String (clip->lengthBeats, 6) + utf8 (" / ") + juce::String (exactBeats, 6));

        //-- Phase 51: the tail can be dragged PAST the end of the file -------
        // 原速: the extra width is silence — the window is what it is, and the
        // picture may now be wider than the sound. 变速不变调: the same width
        // is a real time-stretch, which the ratio states (remaining source over
        // length, here clearly below 1 = slower than the file).
        pianoRoll.simulateSampleTrim (id, startBeat + exactBeats + 2.0, true);

        check (utf8 ("原速模式：尾巴可以拖过文件末尾（多出的宽度是静音）"),
               std::abs (clip->lengthBeats - (exactBeats + 2.0)) < 1.0e-9,
               juce::String (clip->lengthBeats, 4) + utf8(" / 文件 ")
                 + juce::String (exactBeats, 4));

        pianoRoll.setSampleStretchMode (id, StretchMode::tempo);

        const double stretchedRatio = clip->stretchRatio (audio.getBpm());

        check (utf8 ("变速不变调：拉长后的块是真实的时间伸缩（比例 < 1）"),
               stretchedRatio > 0.0 && stretchedRatio < 1.0
                 && std::abs (stretchedRatio - exactBeats / (exactBeats + 2.0)) < 1.0e-9,
               juce::String (stretchedRatio, 4));

        // ...and the trim is symmetrical: back to the file's exact end, because
        // the checks below expect the untouched fixture.
        pianoRoll.simulateSampleTrim (id, startBeat + exactBeats, true);
        pianoRoll.setSampleStretchMode (id, StretchMode::none);

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

    //-- Phase 50: pan curve / reverse / cut / scope render -------------------
    [&]
    {
        LayerStack     t;
        RackModel      r;
        PlaybackEngine e;

        e.setSampleRate (48000.0);
        t.addLayer (utf8 ("P50 采样"), palette::layer1);
        t.setLayerLevelDb (0, 0.0f);
        t.setLayerPan (0, 0.0f);

        const auto loaded  = loadSampleFile (phase27B);
        const int  sampler = r.addSampler (loaded.buffer, phase27B.getFullPathName(), "P50 bus");

        auto* layer = t.getLayer (0);

        SampleEvent clip;
        clip.name         = "P50";
        clip.path         = phase27B.getFullPathName();
        clip.audio        = loaded.buffer;
        clip.startBeat    = 0.0;
        clip.lengthBeats  = 8.0;               // 4 s at 120 bpm, not 1 s (P49-B)
        clip.pitch        = 60;
        clip.laneSpan     = 2;
        clip.outputSlotId = sampler;

        const int clipId = layer->addSample (clip);
        const double sr  = 48000.0;

        auto renderTo = [&] (juce::AudioBuffer<float>& b)
        {
            b.clear();
            e.resetMixState();
            e.publishSchedule (t, r, 120.0);
            e.renderWindow (0.0, b.getNumSamples(), b, nullptr);
        };

        auto channelPeak = [] (const juce::AudioBuffer<float>& b, int ch, double from, double span)
        {
            const int first = (int) std::llround (from * 48000.0);
            const int count = juce::jmax (1, (int) std::llround (span * 48000.0));

            return b.getMagnitude (ch, first, count);
        };

        juce::AudioBuffer<float> neutral (2, (int) (4.0 * sr));
        renderTo (neutral);

        const float neutralL = channelPeak (neutral, 0, 0.10, 0.20);
        const float neutralR = channelPeak (neutral, 1, 0.10, 0.20);

        if (auto* c = layer->findSample (clipId))
        {
            c->panCurve.loadPoints ({ { 0.0f, 1.0f }, { 1.0f, 1.0f } });   // hard right
            c->panCurve.setEnabled (true);
            c->automationTarget = SampleAutomationTarget::pan;
        }

        juce::AudioBuffer<float> panned (2, (int) (4.0 * sr));
        renderTo (panned);

        const float pannedL = channelPeak (panned, 0, 0.10, 0.20);
        const float pannedR = channelPeak (panned, 1, 0.10, 0.20);

        // Hard right = left channel silent, right channel untouched (balance
        // law: balL = 1-pan, balR = 1+pan). Compared per channel against the
        // neutral render, so a stereo test file cannot fake the result.
        check ("A clip's pan curve moves the block across the field",
               neutralL > 0.005f && neutralR > 0.005f
                 && pannedL < neutralL * 0.02f
                 && pannedR > neutralR * 0.90f,
               "neutral " + juce::String (neutralL, 4) + "/" + juce::String (neutralR, 4)
                 + utf8 (" → 声相 ") + juce::String (pannedL, 4) + "/" + juce::String (pannedR, 4));

        if (auto* c = layer->findSample (clipId))
            c->panCurve.setEnabled (false);

        //-- Reverse ----------------------------------------------------------
        juce::AudioBuffer<float> forward (2, (int) (4.0 * sr));
        renderTo (forward);

        if (auto* c = layer->findSample (clipId))
            c->reversed = true;

        juce::AudioBuffer<float> backward (2, (int) (4.0 * sr));
        renderTo (backward);

        // Matched windows, not "the start looks different": at t the reversed
        // block reads the source at (length - t), so its head must equal the
        // forward block's tail. The test file decays, which is what makes the
        // two windows distinguishable at all.
        const float forwardHead = channelPeak (forward, 0, 0.03, 0.20);
        const float forwardTail = channelPeak (forward, 0, 3.77, 0.20);
        const float backHead    = channelPeak (backward, 0, 0.03, 0.20);

        check ("Reverse reads the window backwards, not the file",
               forwardHead > forwardTail * 1.5f
                 && std::abs (backHead - forwardTail) < juce::jmax (0.002f, forwardTail * 0.30f),
               utf8 ("正放头/尾 ") + juce::String (forwardHead, 4) + "/" + juce::String (forwardTail, 4)
                 + utf8 ("，反转头 ") + juce::String (backHead, 4));

        if (auto* c = layer->findSample (clipId))
            c->reversed = false;

        //-- The cut ----------------------------------------------------------
        // Its own fixture and its own roll: the split reads the SELECTION and the
        // PLAYHEAD, and both belong to a view.
        LayerStack t2;
        RackModel  r2;

        t2.addLayer (utf8 ("P50 切割"), palette::layer1);

        auto* cutLayer = t2.getLayer (0);

        SampleEvent cutClip;
        cutClip.name                = "cut";
        cutClip.audio               = loaded.buffer;
        cutClip.startBeat           = 1.0;
        cutClip.lengthBeats         = 6.0;
        cutClip.sourceOffsetSeconds = 0.4;
        cutClip.pitch               = 60;
        cutClip.laneSpan            = 2;
        cutClip.volumeCurve.loadPoints ({ { 0.0f, 0.0f }, { 1.0f, 1.0f } });
        cutClip.volumeCurve.setEnabled (true);

        const int cutId = cutLayer->addSample (cutClip);

        PianoRollComponent roll (t2, r2);
        roll.setBpm (120.0);
        roll.setSampleSelection ({ cutId });
        roll.setPlayheadBeat (3.0);            // 2 beats into a 6-beat block

        const int splits = roll.simulateSampleSplitAtPlayhead();

        const auto& pieces = cutLayer->getSamples();
        const SampleEvent* left  = pieces.size() == 2 ? &pieces[0] : nullptr;
        const SampleEvent* right = pieces.size() == 2 ? &pieces[1] : nullptr;

        // 2 beats at 120 bpm = 1 s of source consumed. The right half must pick
        // up there, or a cut block replays its own head.
        check ("Cutting at the playhead hands the right half the consumed audio",
               splits == 1 && left != nullptr && right != nullptr
                 && std::abs (left->lengthBeats - 2.0) < 1.0e-6
                 && std::abs (right->startBeat - 3.0) < 1.0e-6
                 && std::abs (right->lengthBeats - 4.0) < 1.0e-6
                 && std::abs (right->sourceOffsetSeconds - 1.4) < 1.0e-3,
               splits == 1 && left != nullptr && right != nullptr
                 ? "left " + juce::String (left->lengthBeats, 3)
                     + utf8 (" / right @") + juce::String (right->startBeat, 3)
                     + " len " + juce::String (right->lengthBeats, 3)
                     + " off " + juce::String (right->sourceOffsetSeconds, 3)
                 : "splits=" + juce::String (splits) + " n=" + juce::String ((int) pieces.size()));

        // The ramp is split, not restarted: at the seam both halves read the
        // value the original had there (2/6 = 0.333).
        check ("The curve is cut at the seam, not restarted",
               left != nullptr && right != nullptr
                 && std::abs (left->volumeCurve.valueAt (1.0f) - 0.3333f) < 0.01f
                 && std::abs (right->volumeCurve.valueAt (0.0f) - 0.3333f) < 0.01f
                 && std::abs (right->volumeCurve.valueAt (1.0f) - 1.0f) < 0.01f,
               left != nullptr && right != nullptr
                 ? "l(1)=" + juce::String (left->volumeCurve.valueAt (1.0f), 3)
                     + " r(0)=" + juce::String (right->volumeCurve.valueAt (0.0f), 3)
                     + " r(1)=" + juce::String (right->volumeCurve.valueAt (1.0f), 3)
                 : "no pieces");

        //-- Scope render -----------------------------------------------------
        // Two layers with notes 20 beats apart: the layer extract must keep the
        // layer's rows and report the real start, which is what stops a bounce
        // from beginning with ten seconds of silence.
        LayerStack st;
        RackModel  rr;

        st.addLayer (utf8 ("P50 A"), palette::layer1);
        st.addLayer (utf8 ("P50 B"), palette::layer2);

        for (int i = 0; i < 2; ++i)
        {
            NoteEvent note;
            note.id          = 0;
            note.pitch       = 60 + i;
            note.startBeat   = 20.0 + i * 2.0;
            note.lengthBeats = 1.0;
            st.getLayer (0)->addNote (note);
        }

        {
            NoteEvent note;
            note.pitch       = 64;
            note.startBeat   = 1.0;
            note.lengthBeats = 1.0;
            st.getLayer (1)->addNote (note);
        }

        PlaybackEngine probe;
        probe.setSampleRate (48000.0);
        probe.publishSchedule (st, rr, 120.0);

        const auto full = probe.getSchedule();
        scope::Scope asked;
        asked.kind       = scope::Scope::Kind::layer;
        asked.layerIndex = 0;

        const auto extracted = scope::extract (*full, asked);

        check ("Rendering a layer keeps its rows and starts where they start",
               ! extracted.empty
                 && extracted.schedule.notes.size() == 2
                 && std::abs (extracted.startSeconds - 10.0) < 1.0e-6
                 && std::abs (extracted.lengthSeconds - 1.5) < 1.0e-3,
               "n=" + juce::String ((int) extracted.schedule.notes.size())
                 + " start " + juce::String (extracted.startSeconds, 3)
                 + " len " + juce::String (extracted.lengthSeconds, 3));

        // Free-slot search: gaps inside the layer's own span only.
        const auto inGap   = scope::findFreeBeat ({ { 0.0, 2.0 }, { 4.0, 6.0 } }, 1.5, 6.0);
        const auto inTight = scope::findFreeBeat ({ { 0.0, 2.0 }, { 4.0, 6.0 } }, 2.5, 6.0);
        const auto overloaded = scope::findFreeBeat ({ { 0.0, 3.0 }, { 3.0, 6.0 } }, 1.0, 6.0);

        check ("A bounced block lands in a gap, and a full layer says so",
               inGap.found && std::abs (inGap.beat - 2.0) < 1.0e-9
                 && ! inTight.found                       // 2 beats of gap, 2.5 asked
                 && ! overloaded.found,                   // tiled end to end
               "gap " + juce::String (inGap.found ? inGap.beat : -1.0, 2)
                 + " tight " + juce::String (inTight.found ? 1 : 0)
                 + " full " + juce::String (overloaded.found ? 1 : 0));

        //-- The insert half of the bounce ------------------------------------
        // Renders through the real exporter into a real WAV, then hands it to
        // the host's own insert path — the same function the bounce thread calls
        // when it finishes. Only the thread itself is left untested.
        {
            LayerStack rt;
            RackModel  rr2;
            PlaybackEngine re;

            re.setSampleRate (bounce::exportSampleRate);
            rt.addLayer (utf8 ("P50 渲染"), palette::layer2);

            NoteEvent note;
            note.pitch       = 60;
            note.startBeat   = 0.0;
            note.lengthBeats = 2.0;
            rt.getLayer (0)->addNote (note);

            re.publishSchedule (rt, rr2, 120.0);

            scope::Scope only;
            only.kind       = scope::Scope::Kind::layer;
            only.layerIndex = 0;

            const auto kept = scope::extract (*re.getSchedule(), only);
            const auto bounceFile = phase27Dir.getChildFile ("atomic-p50-bounce.wav");

            const auto bounced = bounce::renderToFile (bounceFile, kept.schedule,
                                                    bounce::exportSampleRate, 0.05, {}, nullptr,
                                                    kept.startSeconds);

            // A LOCAL stack and rack, not the live session: a bounce that lands
            // in the demo project would end up in the snapshot AND in
            // docs/demo.atomic, and a test that changes the fixture it runs
            // against is a test that cannot be repeated.
            LayerStack target;
            RackModel  targetRack;

            target.addLayer (utf8 ("P50 渲染结果"), palette::layer3);

            const int slotsBefore = targetRack.getNumSlots();
            const auto emptyFree  = scope::findFreeBeat ({}, 1.0, 0.0);

            check ("The bounce is a real WAV, and an empty layer accepts it at beat 0",
                   bounced.ok && bounced.seconds > 0.1
                     && emptyFree.found && std::abs (emptyFree.beat) < 1.0e-9,
                   "ok " + juce::String ((int) bounced.ok)
                     + " len " + juce::String (bounced.seconds, 2)
                     + " empty-beat " + juce::String (emptyFree.beat, 2));

            const auto inserted = scope::insertSampleBlock (target, targetRack, 0, bounceFile,
                                                            120.0, utf8 ("渲染"));

            const auto* placed = (inserted.ok && target.getLayer (0) != nullptr
                                    && target.getLayer (0)->getSampleCount() == 1)
                                   ? &target.getLayer (0)->getSamples().front() : nullptr;

            check ("A finished bounce becomes a mounted block on the target layer",
                   placed != nullptr
                     && std::abs (placed->startBeat) < 1.0e-6
                     && placed->lengthBeats > 1.0
                     && placed->isLoaded()
                     && placed->outputSlotId >= 0
                     && targetRack.getNumSlots() == slotsBefore + 1
                     && placed->audio->lengthSeconds() > 1.0,
                   placed != nullptr
                     ? "beat " + juce::String (placed->startBeat, 2)
                         + " len " + juce::String (placed->lengthBeats, 2)
                         + " slot " + juce::String (placed->outputSlotId)
                         + " aud " + juce::String (placed->audio->lengthSeconds(), 2)
                     : (inserted.message.isNotEmpty() ? inserted.message
                                                      : juce::String (utf8 ("没有落下来"))));
        }

        //-- Serialisation ----------------------------------------------------
        {
            LayerStack s3;
            RackModel  r3;

            s3.addLayer (utf8 ("P50 存盘"), palette::layer1);

            SampleEvent saved;
            saved.name             = "roundtrip";
            saved.startBeat        = 4.0;
            saved.lengthBeats      = 2.0;
            saved.automationTarget = SampleAutomationTarget::pan;
            saved.reversed         = true;
            saved.panCurve.loadPoints ({ { 0.0f, 0.0f }, { 1.0f, 1.0f } });
            saved.panCurve.setEnabled (true);
            saved.volumeCurve.loadPoints ({ { 0.0f, 0.5f }, { 1.0f, 0.5f } });
            saved.volumeCurve.setEnabled (true);

            s3.getLayer (0)->addSample (saved);

            const juce::String xml = project::toXmlText (s3, r3, 120.0);

            LayerStack s4;
            RackModel  r4;
            double bpm4 = 0.0;
            juce::String error;

            const bool ok = project::fromXmlText (xml, s4, r4, bpm4, error,
                                                  nullptr, nullptr, nullptr);

            const auto* back = (s4.getLayer (0) != nullptr && s4.getLayer (0)->getSampleCount() == 1)
                                 ? &s4.getLayer (0)->getSamples().front() : nullptr;

            check ("v22 keeps the pan curve, the shown target and the reverse flag",
                   ok && back != nullptr
                     && back->automationTarget == SampleAutomationTarget::pan
                     && back->reversed
                     && back->panCurve.isEnabled()
                     && std::abs (back->panCurve.valueAt (1.0f) - 1.0f) < 1.0e-6
                     && std::abs (back->volumeCurve.valueAt (0.5f) - 0.5f) < 1.0e-6,
                   ok && back != nullptr
                     ? "target " + juce::String ((int) back->automationTarget)
                         + " rev " + juce::String ((int) back->reversed)
                         + " pan(1) " + juce::String (back->panCurve.valueAt (1.0f), 3)
                     : error);

            // A v21 file has neither the pan child nor the two attributes. It
            // must open as "volume curve, not reversed, pan untouched".
            juce::String legacy = xml;
            legacy = legacy.replace ("<PanEnvelope", "<Ignored")
                           .replace ("</PanEnvelope>", "</Ignored>")
                           .replace ("autotarget=\"pan\"", "")
                           .replace (" reversed=\"1\"", "");

            LayerStack s5;
            RackModel  r5;
            double bpm5 = 0.0;
            juce::String error5;

            const bool ok5 = project::fromXmlText (legacy, s5, r5, bpm5, error5,
                                                   nullptr, nullptr, nullptr);

            const auto* old = (s5.getLayer (0) != nullptr && s5.getLayer (0)->getSampleCount() == 1)
                                ? &s5.getLayer (0)->getSamples().front() : nullptr;

            check ("A v21 file opens with the volume curve showing and nothing reversed",
                   ok5 && old != nullptr
                     && old->automationTarget == SampleAutomationTarget::volume
                     && ! old->reversed
                     && ! old->panCurve.isEnabled(),
                   ok5 && old != nullptr
                     ? "target " + juce::String ((int) old->automationTarget)
                         + " rev " + juce::String ((int) old->reversed)
                     : error5);
        }
    } ();

    //-- Phase 50: the "render to a block" menu items ------------------------
    // Two verbs, one list: the canvas menu builds them through this function,
    // so reading the function is the same as reading the menu. The interesting
    // half is not that the ids exist but that the selection verb GREYS OUT with
    // nothing selected — the whole point of choosing the scope at click time.
    {
        juce::PopupMenu withSelection, withoutSelection;
        PianoRollComponent::addRenderScopeItems (withSelection, true);
        PianoRollComponent::addRenderScopeItems (withoutSelection, false);

        struct Snapshot { int id; juce::String text; bool enabled; };

        auto read = [] (const juce::PopupMenu& menu)
        {
            std::vector<Snapshot> out;
            juce::PopupMenu::MenuItemIterator it (menu);
            while (it.next())
            {
                const auto& item = it.getItem();
                if (! item.isSeparator)
                    out.push_back ({ item.itemID, item.text, item.isEnabled });
            }
            return out;
        };

        const auto full = read (withSelection);
        const auto bare = read (withoutSelection);

        const bool ok = full.size() == 2 && bare.size() == 2
                         && full[0].id == 7 && bare[0].id == 7
                         && full[0].enabled && ! bare[0].enabled
                         && full[1].id == 8 && bare[1].id == 8
                         && full[1].enabled && bare[1].enabled
                         && full[0].text.contains (utf8 ("选中"))
                         && full[1].text.contains (utf8 ("图层"));

        check ("The render-to-block menu offers selection + layer, and only the first needs a selection",
               ok,
               ok ? full[0].text + " / " + full[1].text
                  : "items " + juce::String ((int) full.size()) + " / " + juce::String ((int) bare.size()));
    }

    //-- Phase 50: tabular figures -------------------------------------------
    // DESIGN.md §5's first hard rule, asserted instead of assumed. Two parts:
    // the default UI face is measured (on this machine its digits happen to be
    // uniform already, so that alone would be a vacuous check), and then a font
    // with genuinely proportional digits is used to prove the cell logic does
    // something. The bite test for this phase replaced the cell with the glyph
    // width and only the second assertion turned red — which is exactly why the
    // second one exists.
    {
        const auto f = ui::font (10.0f);

        const float one = ui::tabularWidth (f, "1");
        const float zero = ui::tabularWidth (f, "0");
        const float eightSeven = ui::tabularWidth (f, "87");
        const float oneTwo = ui::tabularWidth (f, "12");

        check ("Tabular figures: every digit gets the same cell",
               std::abs (one - zero) < 0.01f
                 && std::abs (eightSeven - oneTwo) < 0.01f
                 && zero > 0.5f,
               "1=" + juce::String (one, 3) + " 0=" + juce::String (zero, 3)
                 + " 87=" + juce::String (eightSeven, 3)
                 + " 12=" + juce::String (oneTwo, 3));

        // Asking for a wider family must not collapse the cells back to
        // proportional: the helper has to keep working after a font change.
        check ("Tabular cells follow the font they are measured in",
               ui::tabularWidth (ui::title (14.0f), "87")
                 > ui::tabularWidth (f, "87") * 1.05f,
               juce::String (ui::tabularWidth (ui::title (14.0f), "87"), 2)
                 + " vs " + juce::String (ui::tabularWidth (f, "87"), 2));

        // The real proof: a face whose '1' is narrower than its '0'. Calibri and
        // Georgia ship with Windows and are the usual proportional-figure fonts;
        // if none of them is installed the assertion is skipped rather than
        // faked against a font that was already monospaced.
        juce::String proportional;
        float narrow = 0.0f, wide = 0.0f;

        for (const auto* name : { "Calibri", "Georgia", "Times New Roman", "Cambria" })
        {
            const juce::Font probe (juce::FontOptions (juce::String (name), 14.0f, juce::Font::plain));

            if (! probe.getTypefaceName().equalsIgnoreCase (name))
                continue;                    // not installed

            const float w1 = juce::GlyphArrangement::getStringWidth (probe, "1");
            const float w0 = juce::GlyphArrangement::getStringWidth (probe, "0");

            if (w0 - w1 > 0.2f)
            {
                proportional = name;
                narrow = w1;
                wide = w0;
                break;
            }
        }

        if (proportional.isNotEmpty())
        {
            const juce::Font probe (juce::FontOptions (proportional, 14.0f, juce::Font::plain));

            // Three narrow digits must take three cells, not three narrow
            // advances: that is the whole mechanism, and a proportional face is
            // the only place it is visible.
            check ("Tabular drawing widens a proportional font's narrow digits",
                   std::abs (ui::tabularWidth (probe, "111") - wide * 3.0f) < 0.05f,
                   proportional + " 1=" + juce::String (narrow, 2)
                     + " 0=" + juce::String (wide, 2)
                     + " 111=" + juce::String (ui::tabularWidth (probe, "111"), 2)
                     + utf8 ("（应为 ") + juce::String (wide * 3.0f, 2) + utf8 ("）"));
        }
    }

    // Phase 34: hand the real dialog and the message-queue deferral back. A
    // scripted answer left in place would make the running app delete samplers
    // without ever asking — the exact thing this phase exists to stop.
    deferSamplerPrompt = true;
    installSamplerCascadePrompt();

    //-- Phase 51: stop-to-start / card stretch / knife / chevron -------------
    [&]
    {
        const bool oldSnap = pianoRoll.isSnapEnabled();
        pianoRoll.setSnapEnabled (false);

        auto* layer = stack.getActiveLayer();

        if (layer == nullptr)
        {
            check ("Phase 51 needs a layer", false, "no active layer");
            pianoRoll.setSnapEnabled (oldSnap);
            return;
        }

        //-- the sampler card's stretch control -------------------------------
        const auto loadA  = loadSampleFile (phase27A);
        const int  slotId = rack.addSampler (loadA.buffer, phase27A.getFullPathName(), "P51");

        const int cardA = pianoRoll.addSampleAt (200.0, 50, phase27A);
        const int cardB = pianoRoll.addSampleAt (204.0, 50, phase27A);

        if (auto* a = layer->findSample (cardA)) a->outputSlotId = slotId;
        if (auto* b = layer->findSample (cardB)) b->outputSlotId = slotId;

        check (utf8 ("A slot whose blocks all sit on 原速 reports that state"),
               samplerStretchStateFor (slotId) == (int) StretchMode::none,
               juce::String (samplerStretchStateFor (slotId)));

        applySamplerStretchToSlot (slotId, StretchMode::tempo);

        const auto* writtenA = layer->findSample (cardA);
        const auto* writtenB = layer->findSample (cardB);

        check (utf8 ("Choosing 只变速不变调 on the card reaches every block of the slot"),
               writtenA != nullptr && writtenB != nullptr
                 && writtenA->stretchMode == StretchMode::tempo
                 && writtenB->stretchMode == StretchMode::tempo
                 && samplerStretchStateFor (slotId) == (int) StretchMode::tempo,
               writtenA != nullptr && writtenB != nullptr
                 ? juce::String ((int) writtenA->stretchMode) + "/"
                   + juce::String ((int) writtenB->stretchMode)
                 : "missing clip");

        // Disagreeing blocks are 混合, not one of the modes — a check mark on a
        // menu item would be a lie the user cannot see through.
        if (auto* a = layer->findSample (cardA))
            a->stretchMode = StretchMode::pitch;

        check (utf8 ("Two blocks on different modes read as 混合 (-1)"),
               samplerStretchStateFor (slotId) == -1,
               juce::String (samplerStretchStateFor (slotId)));

        // The card's own geometry: inside the card, on the readout row, right
        // end — the slot the 越级 badge uses on synths.
        int cardIndex = -1;
        for (int i = 0; i < rack.getNumSlots(); ++i)
            if (const auto* s = rack.getSlot (i); s != nullptr && s->id == slotId)
            {
                cardIndex = i;
                break;
            }

        const auto stretchRect = rackPanel.getStretchBounds (cardIndex);
        const auto cardRect    = rackPanel.getCardBoundsForTest (cardIndex);

        check ("The stretch chip sits inside its card, flush to the right edge",
               cardIndex >= 0 && cardRect.contains (stretchRect)
                 && cardRect.getRight() - stretchRect.getRight() <= ui::px (20)
                 && stretchRect.getY() == cardRect.getY() + ui::px (40),
               "x " + juce::String (stretchRect.getX()) + " y "
                 + juce::String (stretchRect.getY()));

        //-- the knife cuts notes ---------------------------------------------
        pianoRoll.setTool (PianoRollComponent::Tool::knife);

        check ("The knife is the toolbar's fifth tool button",
               PianoRollComponent::numToolButtons == 9
                 && pianoRoll.getToolButtonBounds (4).getX()
                      > pianoRoll.getToolButtonBounds (3).getRight()
                 && pianoRoll.getToolButtonBounds (5).getX()
                      > pianoRoll.getToolButtonBounds (4).getRight(),
               "btn4 x " + juce::String (pianoRoll.getToolButtonBounds (4).getX()));

        const int noteId = pianoRoll.addNoteAt (210.0, 62);
        int leftGroup = -2;

        if (auto* note = layer->findNote (noteId))
        {
            note->lengthBeats = 2.0;
            note->envelope.addPoint (0.5f, 0.42f);
            leftGroup = note->groupId;
        }

        const int notesBefore = (int) layer->getNotes().size();

        const int noteSplits = pianoRoll.splitNotesAt ({ noteId }, 210.5);

        const auto* leftNote = layer->findNote (noteId);

        int rightId = -1;
        for (const auto& n : layer->getNotes())
            if (n.groupId == leftGroup && n.id != noteId
                  && n.startBeat > 210.4 && n.startBeat < 210.6)
                rightId = n.id;

        const auto* rightNote = layer->findNote (rightId);

        check ("A knife click on a note makes two notes at the click",
               noteSplits == 1 && (int) layer->getNotes().size() == notesBefore + 1
                 && leftNote != nullptr && rightNote != nullptr
                 && std::abs (leftNote->lengthBeats - 0.5) < 1.0e-9
                 && std::abs (rightNote->lengthBeats - 1.5) < 1.0e-9,
               leftNote != nullptr && rightNote != nullptr
                 ? juce::String (leftNote->lengthBeats, 3) + " + "
                   + juce::String (rightNote->lengthBeats, 3)
                 : "missing half");

        // The curve is split at the seam, not restarted: the left half's END
        // value and the right half's START value are the same number, which is
        // what "the ramp continues" means in numbers.
        check ("The note's curve continues across the cut",
               leftNote != nullptr && rightNote != nullptr
                 && std::abs (leftNote->envelope.valueAt (1.0f)
                                - rightNote->envelope.valueAt (0.0f)) < 1.0e-3,
               leftNote != nullptr && rightNote != nullptr
                 ? juce::String (leftNote->envelope.valueAt (1.0f), 3) + " vs "
                   + juce::String (rightNote->envelope.valueAt (0.0f), 3)
                 : "missing half");

        //-- ...and blocks -----------------------------------------------------
        const int blockId = pianoRoll.addSampleAt (220.0, 50, phase27A);
        const auto* block = layer->findSample (blockId);

        const int blockSplits = block != nullptr
                                  ? pianoRoll.splitSamplesAt ({ blockId },
                                                               block->startBeat + 1.0)
                                  : 0;

        // Re-find AFTER the split: addSample() can grow the layer's vector and
        // move every element, so the pointer taken before the cut is stale.
        const auto* leftHalf  = layer->findSample (blockId);

        int rightHalfId = -1;
        for (const auto& probe : layer->getSamples())
            if (probe.id != blockId && probe.startBeat > 220.9 && probe.startBeat < 221.1)
                rightHalfId = probe.id;

        const auto* rightHalf = layer->findSample (rightHalfId);

        check ("A knife click on a block cuts it with the offset advancing",
               blockSplits == 1 && leftHalf != nullptr && rightHalf != nullptr
                 && std::abs (leftHalf->lengthBeats - 1.0) < 1.0e-9
                 && std::abs (leftHalf->sourceOffsetSeconds) < 1.0e-9
                 && std::abs (rightHalf->lengthBeats - 1.0) < 1.0e-9
                 && std::abs (rightHalf->sourceOffsetSeconds - 0.5) < 1.0e-9,
               leftHalf != nullptr && rightHalf != nullptr
                 ? juce::String (leftHalf->lengthBeats, 3) + "+"
                   + juce::String (rightHalf->lengthBeats, 3) + utf8 (" 拍, offset ")
                   + juce::String (leftHalf->sourceOffsetSeconds, 3) + "/"
                   + juce::String (rightHalf->sourceOffsetSeconds, 3)
                 : "missing half");

        //-- the chevron: top edge, the note's own inset ----------------------
        const auto sampleBounds = pianoRoll.getSampleScreenBounds (
                                      stack.getActiveLayerIndex(), blockId);
        const auto sampleHandle = pianoRoll.getSampleAutomationHandleForTest (blockId);

        const int   someNoteId = pianoRoll.addNoteAt (230.0, 60);

        if (auto* some = layer->findNote (someNoteId))
            some->lengthBeats = 8.0;        // long enough to carry a tail button

        const auto* someNote   = layer->findNote (someNoteId);
        const auto  noteHandle = someNote != nullptr
                                   ? pianoRoll.getNoteAutomationHandleBounds (*someNote)
                                   : juce::Rectangle<float> {};
        const auto  noteBounds = someNote != nullptr
                                   ? pianoRoll.getNoteScreenBounds (
                                         stack.getActiveLayerIndex(), someNoteId)
                                   : juce::Rectangle<float> {};

        const float sampleInset = sampleBounds.getRight() - sampleHandle.getX();
        const float noteInset   = noteBounds.getRight() - noteHandle.getX();

        check ("The block's chevron sits on the block's top edge",
               ! sampleHandle.isEmpty()
                 && std::abs (sampleHandle.getY() - (sampleBounds.getY() + 1.0f)) < 1.5f,
               "y " + juce::String (sampleHandle.getY(), 1) + utf8 (" / 块顶 ")
                 + juce::String (sampleBounds.getY(), 1));

        check ("...and its distance from the right edge is the note's own",
               ! sampleHandle.isEmpty() && ! noteHandle.isEmpty()
                 && std::abs (sampleInset - noteInset) < 1.0e-3,
               juce::String (sampleInset, 2) + " vs " + juce::String (noteInset, 2));

        //-- cleanup ------------------------------------------------------------
        layer->removeSample (cardA);
        layer->removeSample (cardB);
        layer->removeSample (blockId);
        if (rightHalfId > 0)
            layer->removeSample (rightHalfId);

        layer->removeNote (noteId);
        if (rightId > 0)
            layer->removeNote (rightId);
        layer->removeNote (someNoteId);

        rack.removeSlot (slotId);
        stack.notifyNotesChanged();
        pruneOrphanSamplerSlots();

        pianoRoll.setSnapEnabled (oldSnap);
        pianoRoll.setTool (PianoRollComponent::Tool::select);
    } ();

    //-- Phase 52: 只变调 range / card drag / export settings & writers -------
    [&]
    {
        //-- 1. the range, as arithmetic ---------------------------------------
        // 2.0 over 2.0 is exactly +12 semitones, so a 12-semitone ceiling has to
        // leave it alone; 8.0 is +36 and has to stop at the ceiling.
        const auto atCeiling = clipstretch::Playback::make (StretchMode::pitch, 2.0, 12);
        const auto pastCeiling = clipstretch::Playback::make (StretchMode::pitch, 8.0, 12);
        const auto wideOpen = clipstretch::Playback::make (StretchMode::pitch, 8.0, 36);
        const auto otherMode = clipstretch::Playback::make (StretchMode::tempo, 8.0, 1);

        check ("A 12-semitone range transposes by at most an octave",
               std::abs (atCeiling.pitch - 2.0) < 1.0e-9
                 && std::abs (pastCeiling.pitch - 2.0) < 1.0e-9
                 && std::abs (pastCeiling.speed - 1.0) < 1.0e-9,
               juce::String (pastCeiling.pitch, 3) + " / speed "
                 + juce::String (pastCeiling.speed, 3));

        check ("...and a wider range lets the same block go further",
               std::abs (wideOpen.pitch - 8.0) < 1.0e-9,
               juce::String (wideOpen.pitch, 3));

        check (utf8 ("The range belongs to 只变调 only"),
               std::abs (otherMode.pitch - 1.0) < 1.0e-9
                 && std::abs (otherMode.speed - 8.0) < 1.0e-9,
               "pitch " + juce::String (otherMode.pitch, 3));

        //-- 2. the card's half: a real slot, real blocks ----------------------
        auto* layer = stack.getActiveLayer();

        if (layer == nullptr)
        {
            check ("Phase 52 needs a layer", false, "no active layer");
            return;
        }

        const bool oldSnap = pianoRoll.isSnapEnabled();
        pianoRoll.setSnapEnabled (false);

        const auto loadA  = loadSampleFile (phase27A);
        const int  slotId = rack.addSampler (loadA.buffer, phase27A.getFullPathName(), "P52");

        const int clipA = pianoRoll.addSampleAt (240.0, 50, phase27A);

        if (auto* clip = layer->findSample (clipA))
        {
            clip->outputSlotId = slotId;
            clip->stretchMode  = StretchMode::pitch;
        }

        check ("A block starts with the default 24-semitone range",
               layer->findSample (clipA) != nullptr
                 && layer->findSample (clipA)->pitchRangeSemitones == 24,
               juce::String (layer->findSample (clipA) != nullptr
                               ? layer->findSample (clipA)->pitchRangeSemitones : -1));

        applySamplerPitchRangeToSlot (slotId, 5);

        check ("Choosing a range on the card writes every block of the slot",
               layer->findSample (clipA) != nullptr
                 && layer->findSample (clipA)->pitchRangeSemitones == 5
                 && samplerPitchRangeStateFor (slotId) == 5,
               juce::String (samplerPitchRangeStateFor (slotId)));

        // Out of range is clamped, not stored: the UI prints this number.
        applySamplerPitchRangeToSlot (slotId, 99);

        check ("A range past the maximum is clamped, not stored",
               layer->findSample (clipA) != nullptr
                 && layer->findSample (clipA)->pitchRangeSemitones
                      == SampleEvent::maxPitchRangeSemitones,
               juce::String (layer->findSample (clipA) != nullptr
                               ? layer->findSample (clipA)->pitchRangeSemitones : -1));

        //-- 3. the chip: present in 只变调, absent otherwise ------------------
        int cardIndex = -1;
        for (int i = 0; i < rack.getNumSlots(); ++i)
            if (const auto* s = rack.getSlot (i); s != nullptr && s->id == slotId)
                cardIndex = i;

        const auto cardRect  = rackPanel.getCardBoundsForTest (cardIndex);
        const auto stretchRect = rackPanel.getStretchBounds (cardIndex);
        const auto rangeRect   = rackPanel.getPitchRangeBounds (cardIndex);

        check (utf8 ("The 变调范围 chip appears in 只变调, left of the stretch chip"),
               ! rangeRect.isEmpty() && cardRect.contains (rangeRect)
                 && rangeRect.getRight() <= stretchRect.getX()
                 && rangeRect.getY() == cardRect.getY() + ui::px (40),
               "range x " + juce::String (rangeRect.getX()) + ".."
                 + juce::String (rangeRect.getRight()) + " / stretch "
                 + juce::String (stretchRect.getX()));

        applySamplerStretchToSlot (slotId, StretchMode::tempo);

        check (utf8 ("...and is gone as soon as the mode is not 只变调"),
               rackPanel.getPitchRangeBounds (cardIndex).isEmpty(),
               "still there");

        applySamplerStretchToSlot (slotId, StretchMode::pitch);

        //-- 4. the drag payload ----------------------------------------------
        check ("A sampler drag payload round-trips, a file path does not",
               samplerDragSlotId (samplerDragDescription (7)) == 7
                 && samplerDragSlotId ("C:\\loops\\kick.wav") == -1
                 && samplerDragSlotId ("atomic-sampler:") == -1,
               juce::String (samplerDragSlotId (samplerDragDescription (7))));

        //-- 5. dropping the card onto the grid -------------------------------
        const int slotsBefore  = rack.getNumSlots();
        const int droppedId    = pianoRoll.addSampleFromSlot (250.0, 60, slotId);
        const auto* dropped    = layer->findSample (droppedId);

        check ("Dropping a card makes a block on that same slot, with no twin",
               droppedId > 0 && dropped != nullptr && dropped->outputSlotId == slotId
                 && rack.getNumSlots() == slotsBefore,
               juce::String (dropped != nullptr ? dropped->outputSlotId : -1)
                 + " / slots " + juce::String (rack.getNumSlots())
                 + " vs " + juce::String (slotsBefore));

        //-- 6. the export settings describe themselves ------------------------
        ExportSettings wav24;
        wav24.bitDepth = 24;
        wav24.mono     = true;

        ExportSettings mp3Settings;
        mp3Settings.format     = ExportFormat::mp3;
        mp3Settings.mp3Bitrate = 320;

        check ("24-bit mono describes itself as the file it will write",
               wav24.extension() == "wav" && wav24.wildcard() == "*.wav"
                 && wav24.describe().contains ("24-bit")
                 && wav24.describe().contains (utf8 ("单声道")),
               wav24.describe());

        check ("320 kbps MP3 describes itself too",
               mp3Settings.extension() == "mp3" && mp3Settings.wildcard() == "*.mp3"
                 && mp3Settings.describe().contains ("320"),
               mp3Settings.describe());

        //-- the dialog's own geometry -----------------------------------------
        ExportDialog preview;

        const auto formatRow = preview.getFormatBounds (0);
        const auto depthRow  = preview.getBitDepthBounds (0);
        const auto rateRow   = preview.getBitrateBounds (0);
        const auto chanRow   = preview.getChannelBounds (0);

        check ("The export dialog's four rows stack without touching",
               formatRow.getBottom() <= depthRow.getY()
                 && depthRow.getBottom() <= rateRow.getY()
                 && rateRow.getBottom() <= chanRow.getY()
                 && chanRow.getBottom() < preview.getExportButtonBounds().getY(),
               juce::String (formatRow.getBottom()) + "/" + juce::String (depthRow.getY())
                 + "/" + juce::String (chanRow.getBottom()) + "/"
                 + juce::String (preview.getExportButtonBounds().getY()));

        check ("A click on a chip selects the chip that is drawn there",
               ExportDialog::segmentAt (rateRow, 4, rateRow.getCentre()) == 0
                 && ExportDialog::segmentAt (rateRow, 4,
                                             preview.getBitrateBounds (2).getCentre()) == 2
                 && ExportDialog::segmentAt (rateRow, 4,
                                             { preview.getBitrateBounds (3).getRight() + 4,
                                               rateRow.getCentreY() }) == -1,
               "row " + juce::String (rateRow.getWidth()));

        // The rule behind the greyed row: a click on it does nothing, and the
        // same click on the live row does exactly what it looks like.
        preview.chooseFormat (ExportFormat::wav);
        preview.chooseBitrate (192);

        preview.clickAt (preview.getBitrateBounds (3).getCentre());     // greyed
        const int bitrateAfterGreyed = preview.getSettings().mp3Bitrate;

        preview.clickAt (preview.getBitDepthBounds (2).getCentre());    // live
        const int depthAfterLive = preview.getSettings().bitDepth;

        check ("The greyed row of the dialog ignores the click, the live one takes it",
               bitrateAfterGreyed == 192 && depthAfterLive == 32,
               "bitrate " + juce::String (bitrateAfterGreyed) + " / depth "
                 + juce::String (depthAfterLive));

        //-- 7. the writers, measured on the bytes they produced ---------------
        PlaybackEngine probe;
        probe.setSampleRate (bounce::exportSampleRate);
        probe.publishSchedule (stack, rack, audio.getBpm(), 0.0, &mixer);

        const auto schedule = probe.getSchedule();

        if (schedule == nullptr)
        {
            check ("Phase 52 needs a schedule", false, "publish failed");
        }
        else
        {
            const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                               .getChildFile ("atomic-p52");

            dir.createDirectory();

            const auto monoFile   = dir.getChildFile ("mono24.wav");
            const auto floatFile  = dir.getChildFile ("float32.wav");
            const auto mp3File    = dir.getChildFile ("bounce.mp3");

            const auto monoResult = bounce::renderToFile (monoFile, *schedule, wav24,
                                                          bounce::exportSampleRate, 0.0);

            // A WAV header is a fixed layout: format tag at byte 20, channel
            // count at 22, bits per sample at 34. Reading them back is the only
            // way "24-bit mono" is a fact rather than a struct field nobody
            // looked at.
            juce::FileInputStream headerStream (monoFile);

            const auto readShort = [&headerStream] (int offset) -> int
            {
                headerStream.setPosition (offset);
                return (int) headerStream.readShort();
            };

            const int monoTag      = readShort (20);
            const int monoChannels = readShort (22);
            const int monoBits     = readShort (34);

            check ("A mono 24-bit WAV really writes one channel of 24-bit PCM",
                   monoResult.ok && monoTag == 1 && monoChannels == 1 && monoBits == 24
                     && monoResult.numChannels == 1,
                   "tag " + juce::String (monoTag) + " ch " + juce::String (monoChannels)
                     + " bits " + juce::String (monoBits) + " / "
                     + juce::String (monoResult.numChannels) + utf8 (" 声道"));

            ExportSettings floatSettings;
            floatSettings.bitDepth = 32;

            const auto floatResult = bounce::renderToFile (floatFile, *schedule, floatSettings,
                                                            bounce::exportSampleRate, 0.0);

            juce::FileInputStream floatStream (floatFile);

            const auto readFloatShort = [&floatStream] (int offset) -> int
            {
                floatStream.setPosition (offset);
                return (int) floatStream.readShort();
            };

            const int floatTag  = readFloatShort (20);
            const int floatBits = readFloatShort (34);

            check ("A 32-bit WAV writes an IEEE-float data chunk",
                   floatResult.ok && floatTag == 3 && floatBits == 32,
                   "tag " + juce::String (floatTag) + " bits " + juce::String (floatBits));

            //-- 8. MP3, through the platform encoder ---------------------------
            const auto mp3Result = bounce::renderToFile (mp3File, *schedule, mp3Settings,
                                                          bounce::exportSampleRate, 0.0);

            juce::FileInputStream mp3Stream (mp3File);

            const int mp3FirstByte = mp3Stream.openedOk()
                                       ? (int) (juce::uint8) mp3Stream.readByte() : -1;
            const int mp3SecondByte = mp3Stream.openedOk()
                                        ? (int) (juce::uint8) mp3Stream.readByte() : -1;

            // An MP3 starts either with an ID3 tag ('I','D','3') or with a
            // frame sync (0xFF 0xEx/0xFx). Either is proof the bytes are MPEG
            // audio and not, say, PCM that someone renamed.
            const bool looksMpeg = (mp3FirstByte == 'I' && mp3SecondByte == 'D')
                                     || (mp3FirstByte == 0xFF && (mp3SecondByte & 0xE0) == 0xE0);

            check ("An MP3 export produces MPEG audio, not a renamed WAV",
                   mp3Result.ok && mp3File.existsAsFile() && mp3File.getSize() > 1000
                     && looksMpeg,
                   mp3Result.ok ? juce::String (mp3File.getSize()) + utf8 (" 字节, 首字节 ")
                                    + juce::String (mp3FirstByte)
                                : mp3Result.message);

            monoFile.deleteFile();
            floatFile.deleteFile();
            mp3File.deleteFile();
            dir.deleteRecursively();
        }

        //-- cleanup -----------------------------------------------------------
        layer->removeSample (clipA);
        if (droppedId > 0)
            layer->removeSample (droppedId);

        rack.removeSlot (slotId);
        stack.notifyNotesChanged();
        pruneOrphanSamplerSlots();

        pianoRoll.setSnapEnabled (oldSnap);
    } ();

    //-- Phase 53: a synth card on a note / a sampler card on a block -----------
    [&]
    {
        auto* layer = stack.getActiveLayer();

        if (layer == nullptr)
        {
            check ("Phase 53 needs a layer", false, "no active layer");
            return;
        }

        const bool oldSnap = pianoRoll.isSnapEnabled();
        pianoRoll.setSnapEnabled (false);

        //-- 1. the two payloads are two different questions -------------------
        check ("A synth payload round-trips and never reads as a sampler",
               instrumentDragSlotId (instrumentDragDescription (9)) == 9
                 && instrumentDragSlotId (samplerDragDescription (9)) == -1
                 && samplerDragSlotId (instrumentDragDescription (9)) == -1
                 && instrumentDragSlotId ("atomic-instrument:") == -1,
               "inst " + juce::String (instrumentDragSlotId (instrumentDragDescription (9)))
                 + " / cross " + juce::String (instrumentDragSlotId (samplerDragDescription (9))));

        //-- 2. fixtures: two samplers (1 s and 2 s), two synths, two notes ----
        const auto loadA = loadSampleFile (phase27A);
        const auto loadB = loadSampleFile (phase27B);

        if (! loadA.ok() || ! loadB.ok())
        {
            check ("Phase 53 needs the two sample files", false, "decode failed");
            pianoRoll.setSnapEnabled (oldSnap);
            return;
        }

        const int samplerA = rack.addSampler (loadA.buffer, phase27A.getFullPathName(), "P53 A");
        const int samplerB = rack.addSampler (loadB.buffer, phase27B.getFullPathName(), "P53 B");
        const int synthOne = rack.addSynth();
        const int synthTwo = rack.addSynth();

        const auto makeNote = [layer] (int pitch, double beat)
        {
            NoteEvent note;
            note.pitch       = pitch;
            note.startBeat   = beat;
            note.lengthBeats = 1.0;
            return layer->addNote (note);
        };

        const int noteA = makeNote (60, 400.0);
        const int noteB = makeNote (64, 401.0);

        //-- 3. a sampler card carries a file, not a mount ---------------------
        const bool refusedMount = pianoRoll.routeNoteToSlot (noteA, samplerA);

        check ("A sampler card is refused on a note, and the note is untouched",
               ! refusedMount && layer->findNote (noteA) != nullptr
                 && layer->findNote (noteA)->outputSlotOverride < 0,
               "override " + juce::String (layer->findNote (noteA) != nullptr
                                             ? layer->findNote (noteA)->outputSlotOverride
                                             : -999));

        //-- 4. a synth card on a note is that note's OWN mount ----------------
        const bool mounted = pianoRoll.routeNoteToSlot (noteA, synthOne);

        const auto* dropNote = layer->findNote (noteA);
        const auto  resolved = dropNote != nullptr ? routing::resolve (*layer, *dropNote, rack)
                                                   : routing::Resolution();

        check ("A synth dropped on a note becomes that note's own mount",
               mounted && dropNote != nullptr && dropNote->outputSlotOverride == synthOne
                 && resolved.slotId == synthOne && resolved.overridden,
               "slot " + juce::String (resolved.slotId) + " / override "
                 + juce::String (dropNote != nullptr ? dropNote->outputSlotOverride : -999));

        //-- 5. the selection rule: a selected note takes the selection with it,
        //      an unselected one becomes the selection -------------------------
        pianoRoll.setSelection ({ noteA, noteB });
        pianoRoll.routeNoteToSlot (noteB, synthTwo);

        check ("Dropping on a selected note moves the whole selection",
               layer->findNote (noteA) != nullptr && layer->findNote (noteB) != nullptr
                 && layer->findNote (noteA)->outputSlotOverride == synthTwo
                 && layer->findNote (noteB)->outputSlotOverride == synthTwo,
               "A " + juce::String (layer->findNote (noteA)->outputSlotOverride)
                 + " / B " + juce::String (layer->findNote (noteB)->outputSlotOverride));

        pianoRoll.setSelection ({ noteB });
        pianoRoll.routeNoteToSlot (noteA, synthOne);

        check ("...and an unselected note becomes the whole selection",
               pianoRoll.getSelection().size() == 1
                 && pianoRoll.getSelection().front() == noteA
                 && layer->findNote (noteA)->outputSlotOverride == synthOne
                 && layer->findNote (noteB)->outputSlotOverride == synthTwo,
               "selection " + juce::String ((int) pianoRoll.getSelection().size())
                 + " / B " + juce::String (layer->findNote (noteB)->outputSlotOverride));

        //-- 6. a sampler card on a block replaces that block's sample ---------
        const int clipId = pianoRoll.addSampleAt (410.0, 50, phase27A);

        if (auto* clip = layer->findSample (clipId))
        {
            clip->outputSlotId       = samplerA;
            clip->lengthBeats        = 0.75;   // a trim the drop must NOT keep
            clip->sourceOffsetSeconds = 0.25;
        }

        const int  slotsBefore = rack.getNumSlots();
        const bool replaced    = pianoRoll.replaceSamplesWithSlot (clipId, samplerB);

        const auto*  swapped   = layer->findSample (clipId);
        const double wantBeats = SampleEvent::beatsForSeconds (loadB.buffer->lengthSeconds(),
                                                               audio.getBpm());

        check ("A sampler dropped on a block hands over its file",
               replaced && swapped != nullptr
                 && swapped->outputSlotId == samplerB
                 && swapped->audio == rack.findSlot (samplerB)->sampleBuffer
                 && swapped->path == phase27B.getFullPathName()
                 && std::abs (swapped->startBeat - 410.0) < 1.0e-9
                 && swapped->sourceOffsetSeconds == 0.0
                 && std::abs (swapped->lengthBeats - wantBeats) < 1.0e-6,
               swapped == nullptr ? juce::String ("block gone")
                                  : "slot " + juce::String (swapped->outputSlotId)
                                      + " / len " + juce::String (swapped->lengthBeats, 3)
                                      + " vs " + juce::String (wantBeats, 3));

        check ("...and it selects that block without growing a twin sampler",
               pianoRoll.getSampleSelection().size() == 1
                 && pianoRoll.getSampleSelection().front() == clipId
                 && rack.getNumSlots() == slotsBefore,
               "slots " + juce::String (rack.getNumSlots()) + " vs "
                 + juce::String (slotsBefore));

        //-- 6b. the target is the block under the pointer, not the old selection
        // A block arrives selected when it is created (P33), so the check above
        // cannot tell "the drop selected its target" from "the target happened
        // to be selected already". Two blocks and a stale selection can.
        const int otherClip = pianoRoll.addSampleAt (420.0, 50, phase27B);

        pianoRoll.setSampleSelection ({ clipId });

        const bool replacedOther = pianoRoll.replaceSamplesWithSlot (otherClip, samplerA);

        check ("...and the block under the pointer is the one that changes",
               replacedOther && layer->findSample (otherClip) != nullptr
                 && layer->findSample (otherClip)->outputSlotId == samplerA
                 && pianoRoll.getSampleSelection().size() == 1
                 && pianoRoll.getSampleSelection().front() == otherClip,
               "slot " + juce::String (layer->findSample (otherClip) != nullptr
                                         ? layer->findSample (otherClip)->outputSlotId : -999)
                 + " / sel " + juce::String ((int) pianoRoll.getSampleSelection().size()));

        //-- 7. an instrument has no file to hand over -------------------------
        const bool refusedSwap = pianoRoll.replaceSamplesWithSlot (clipId, synthOne);

        check ("An instrument cannot hand a block a sample",
               ! refusedSwap && layer->findSample (clipId) != nullptr
                 && layer->findSample (clipId)->outputSlotId == samplerB,
               "slot " + juce::String (layer->findSample (clipId) != nullptr
                                         ? layer->findSample (clipId)->outputSlotId : -999));

        //-- 8. the no-op rule the menu already has, inherited by the gesture --
        if (auto* clip = layer->findSample (clipId))
            clip->lengthBeats = 1.5;

        const bool again = pianoRoll.replaceSamplesWithSlot (clipId, samplerB);

        check ("Dropping the same card again leaves a trimmed block alone",
               ! again && layer->findSample (clipId) != nullptr
                 && std::abs (layer->findSample (clipId)->lengthBeats - 1.5) < 1.0e-9,
               "len " + juce::String (layer->findSample (clipId) != nullptr
                                        ? layer->findSample (clipId)->lengthBeats : -1.0, 3));

        //-- 9. the gesture itself: a real drop event at a real point ----------
        // The checks above drive the two rules; these two drive the DISPATCH —
        // which payload means what — through itemDropped, at a point on a note
        // and a block that are genuinely on screen. A probe sheet is used so
        // the hit test cannot land on the demo's own content: hitTestSample
        // answers with the first block whose rectangle contains the point, and
        // "first" over a demo arrangement is not something this test controls.
        const double scrollBefore = pianoRoll.getScrollBeats();
        const int    activeBefore = stack.getActiveLayerIndex();

        const int probeIndex = stack.addLayer (utf8 ("P53 探针"), juce::Colour (0xff556677));

        pianoRoll.setScrollBeats (0.0);

        auto* probe = stack.getLayer (probeIndex);
        const auto grid = pianoRoll.getGridBounds();

        // The pitch is SEARCHED, not assumed: the roll is 128 lanes tall and
        // only some of them are on screen — a fixture the pointer cannot reach
        // would make this pass for the wrong reason.
        int probeNote = -1;

        for (const int pitch : { 60, 48, 72, 36, 84, 24, 96 })
        {
            NoteEvent candidate;
            candidate.pitch       = pitch;
            candidate.startBeat   = 2.0;
            candidate.lengthBeats = 1.0;

            const int id = probe->addNote (candidate);
            const auto bounds = pianoRoll.getNoteScreenBounds (probeIndex, id);

            if (! bounds.isEmpty() && bounds.getWidth() > 4.0f
                 && grid.contains (bounds.getCentre()))
            {
                probeNote = id;
                break;
            }

            probe->removeNote (id);
        }

        if (probeNote < 0)
        {
            check ("Phase 53 needs one visible note to drop on", false, "no pitch was on screen");
        }
        else
        {
            const auto noteRect = pianoRoll.getNoteScreenBounds (probeIndex, probeNote);

            pianoRoll.simulateCardDrop (instrumentDragDescription (synthOne),
                                        noteRect.getCentre());

            check ("A drop event on a note routes that note to the card's synth",
                   probe->findNote (probeNote) != nullptr
                     && probe->findNote (probeNote)->outputSlotOverride == synthOne,
                   "override " + juce::String (probe->findNote (probeNote) != nullptr
                                                 ? probe->findNote (probeNote)->outputSlotOverride
                                                 : -999));

            // The same event shape, the other meaning: a sampler card lands on
            // a block and swaps its file in.
            const int probePitch = probe->findNote (probeNote)->pitch;
            const int probeBlock = pianoRoll.addSampleAt (2.0, probePitch, phase27A);

            const auto blockRect = pianoRoll.getSampleScreenBounds (probeIndex, probeBlock);
            const juce::Point<float> blockPoint (blockRect.getX() + ui::pf (3.0),
                                                 blockRect.getBottom() - ui::pf (2.0));

            pianoRoll.simulateCardDrop (samplerDragDescription (samplerB), blockPoint);

            check ("A drop event on a block swaps in the card's file",
                   probe->findSample (probeBlock) != nullptr
                     && probe->findSample (probeBlock)->outputSlotId == samplerB,
                   "slot " + juce::String (probe->findSample (probeBlock) != nullptr
                                             ? probe->findSample (probeBlock)->outputSlotId
                                             : -999));
        }

        stack.removeLayer (probeIndex);
        stack.setActiveLayerIndex (juce::jlimit (0, stack.getNumLayers() - 1, activeBefore));
        pianoRoll.setScrollBeats (scrollBefore);

        //-- cleanup -----------------------------------------------------------
        pianoRoll.setSelection ({});
        layer->removeSample (clipId);
        layer->removeSample (otherClip);
        layer->removeNote (noteA);
        layer->removeNote (noteB);

        rack.removeSlot (samplerA);
        rack.removeSlot (samplerB);
        rack.removeSlot (synthOne);
        rack.removeSlot (synthTwo);

        stack.notifyNotesChanged();
        pruneOrphanSamplerSlots();

        pianoRoll.setSnapEnabled (oldSnap);
    } ();

    //-- Phase 54: the card a note selection plays on, and the tool menu ------
    [&]
    {
        auto* layer = stack.getActiveLayer();

        if (layer == nullptr)
        {
            check ("Phase 54 needs a layer", false, "no active layer");
            return;
        }

        const auto previousSelection = pianoRoll.getSelection();
        const int  previousFocus     = getFocusedSlotId();
        const auto previousTool      = pianoRoll.getTool();
        const bool previousSnap      = pianoRoll.isSnapEnabled();

        // A clean starting point: no selection, no spotlight. Both are states
        // the checks below are about, so neither may be inherited.
        focusSynth (-1);
        pianoRoll.setSelection ({});

        const int synthA = rack.addSynth();
        const int synthB = rack.addSynth();

        const auto makeNote = [layer] (int pitch, double beat, int override)
        {
            NoteEvent note;
            note.pitch              = pitch;
            note.startBeat          = beat;
            note.lengthBeats        = 1.0;
            note.outputSlotOverride = override;
            return layer->addNote (note);
        };

        const int noteToA = makeNote (84, 600.0, synthA);
        const int noteToB = makeNote (86, 601.0, synthB);

        const auto describe = [] (const std::vector<int>& slots)
        {
            juce::String out;

            for (int id : slots)
                out += juce::String (id) + " ";

            return out.isEmpty() ? juce::String ("none") : out;
        };

        //-- 1. one note marks the card that plays it -------------------------
        pianoRoll.setSelection ({ noteToA });
        updateRackRoutingFocus();

        const auto marksA = rackPanel.getSelectionRouteSlots();

        check ("Selecting a note marks the card it plays on",
               marksA.size() == 1 && marksA.front() == synthA,
               describe (marksA) + "vs " + juce::String (synthA));

        // The whole reason this mark exists: P30 took the spotlight off the
        // selection because it veiled the grid. A mark that also turned the
        // veil on would be the same bug with a new name.
        check ("...and it is not the spotlight: nothing gets dimmed",
               ! rackPanel.hasRoutingFocus() && getFocusedSlotId() < 0,
               "focus " + juce::String (getFocusedSlotId())
                 + " / active " + juce::String ((int) rackPanel.hasRoutingFocus()));

        //-- 2. one selection, two synths, two rings --------------------------
        pianoRoll.setSelection ({ noteToA, noteToB });
        updateRackRoutingFocus();

        const auto marksBoth = rackPanel.getSelectionRouteSlots();

        check ("A selection that straddles two synths rings both cards",
               marksBoth.size() == 2 && marksBoth[0] < marksBoth[1]
                 && marksBoth[0] == juce::jmin (synthA, synthB)
                 && marksBoth[1] == juce::jmax (synthA, synthB),
               describe (marksBoth));

        //-- 3. letting go takes the marks away -------------------------------
        pianoRoll.setSelection ({});
        updateRackRoutingFocus();

        check ("Clearing the selection clears the marks",
               rackPanel.getSelectionRouteSlots().empty(),
               describe (rackPanel.getSelectionRouteSlots()));

        //-- 4. the two states coexist without overwriting each other --------
        // A card lit by hand must survive a selection that points elsewhere —
        // that is the P32 rule the spotlight already had, and the mark lives
        // next to it rather than replacing it.
        focusSynth (synthA);
        pianoRoll.setSelection ({ noteToB });
        updateRackRoutingFocus();

        const auto marksElsewhere = rackPanel.getSelectionRouteSlots();

        check ("A hand-lit card keeps its spotlight while the selection marks the other one",
               getFocusedSlotId() == synthA && rackPanel.hasRoutingFocus()
                 && marksElsewhere.size() == 1 && marksElsewhere.front() == synthB,
               "focus " + juce::String (getFocusedSlotId()) + " / marks "
                 + describe (marksElsewhere));

        //-- 5. the mark is resolved, never remembered ------------------------
        // Deleting the synth a selected note names does not leave a ring around
        // nothing: the note falls back down the priority chain and the ring
        // follows it. A stored slot id would keep pointing at the hole.
        rack.removeSlot (synthB);

        updateRackRoutingFocus();

        const int fallback = routing::layerSlotId (*layer, rack);
        const auto marksFallback = rackPanel.getSelectionRouteSlots();

        check ("A note whose synth was deleted marks where it falls back to",
               marksFallback.size() == 1 && marksFallback.front() == fallback,
               describe (marksFallback) + "vs " + juce::String (fallback));

        //-- 6. the menu bar's third title ------------------------------------
        const auto fileItem = getMenuBarItemBounds (0);
        const auto editItem = getMenuBarItemBounds (1);
        const auto toolItem = getMenuBarItemBounds (2);

        check (utf8 ("The menu bar has three titles, and the third is 工具"),
               MainComponent::menuTitle (0) == utf8 ("文件")
                 && MainComponent::menuTitle (1) == utf8 ("编辑")
                 && MainComponent::menuTitle (2) == utf8 ("工具")
                 && getMenuBarItemBounds (MainComponent::numMenus).isEmpty()
                 && fileItem.getRight() <= editItem.getX()
                 && editItem.getRight() <= toolItem.getX()
                 && getMenuBarBounds().contains (toolItem),
               juce::String (toolItem.getX()) + ".." + juce::String (toolItem.getRight()));

        //-- 7. the menu IS the strip -----------------------------------------
        struct MenuItem { int id; juce::String text; bool enabled; bool ticked; };

        const auto readTools = [this]
        {
            juce::PopupMenu menu;
            fillToolsMenu (menu);

            std::vector<MenuItem> out;

            for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
            {
                const auto& item = it.getItem();

                // Id 0 is a section header — it has text and a place, but
                // nothing to invoke, so counting it as a button would make the
                // "nine buttons" check fail on a correct menu.
                if (item.isSeparator || item.itemID <= 0)
                    continue;

                out.push_back ({ item.itemID, item.text, item.isEnabled, item.isTicked });
            }

            return out;
        };

        const auto slotsFor = [] (const std::vector<MenuItem>& items, int id) -> const MenuItem*
        {
            for (const auto& item : items)
                if (item.id == id)
                    return &item;

            return nullptr;
        };

        pianoRoll.setTool (PianoRollComponent::Tool::knife);

        const auto tools = readTools();

        const auto* knifeItem = slotsFor (tools, 25);
        const auto* pencilItem = slotsFor (tools, 21);

        check (utf8 ("The 工具 menu carries all nine strip buttons"),
               tools.size() == 9 && tools.front().id == 21 && tools.back().id == 29,
               juce::String ((int) tools.size()) + " items");

        check ("...and ticks the mode the strip is actually in",
               knifeItem != nullptr && pencilItem != nullptr
                 && knifeItem->ticked && ! pencilItem->ticked
                 && knifeItem->text.contains (utf8 ("刻刀"))
                 && tools.front().text.contains (utf8 ("画笔")),
               knifeItem == nullptr ? juce::String ("no knife item")
                                    : "knife " + juce::String ((int) knifeItem->ticked)
                                        + " / pencil " + juce::String ((int) pencilItem->ticked));

        //-- 8. an item does what the button does -----------------------------
        applyToolsChoice (21);

        const bool wentToPencil = pianoRoll.getTool() == PianoRollComponent::Tool::pencil;

        const bool snapBefore = pianoRoll.isSnapEnabled();
        applyToolsChoice (29);
        const bool toggled = pianoRoll.isSnapEnabled() != snapBefore;
        applyToolsChoice (29);

        check ("Choosing a mode in the menu switches the strip's own tool",
               wentToPencil && toggled && pianoRoll.isSnapEnabled() == snapBefore,
               juce::String ((int) pianoRoll.getTool()) + " / snap "
                 + juce::String ((int) pianoRoll.isSnapEnabled()));

        //-- 9. the actions grey out exactly when the strip's do --------------
        // The paint state is a session flag that earlier phases load, and a
        // LOADED state keeps the copy item enabled on purpose — clearing it has
        // to be possible with nothing selected. So the bare read has to start
        // from "nothing loaded", or it tests last block's leftover.
        if (pianoRoll.hasPencilState())
            pianoRoll.clearPencilState();

        pianoRoll.setSelection ({});

        const auto bare = readTools();

        const auto* bareGroup    = slotsFor (bare, 27);
        const auto* bareUngroup  = slotsFor (bare, 28);
        const auto* bareCopy     = slotsFor (bare, 26);

        pianoRoll.setSelection ({ noteToA });

        const auto withSelection = readTools();

        const auto* armedGroup   = slotsFor (withSelection, 27);
        const auto* armedCopy    = slotsFor (withSelection, 26);

        check (utf8 ("编组/解组") + " and " + utf8 ("复制状态") + " grey out with nothing selected, like the strip",
               bareGroup != nullptr && bareUngroup != nullptr && bareCopy != nullptr
                 && armedGroup != nullptr && armedCopy != nullptr
                 && ! bareGroup->enabled && ! bareUngroup->enabled
                 && ! bareCopy->enabled && armedGroup->enabled && armedCopy->enabled,
               "bare " + juce::String ((int) (bareGroup != nullptr && bareGroup->enabled))
                 + " / armed " + juce::String ((int) (armedGroup != nullptr && armedGroup->enabled)));

        // ...and a LOADED pencil stays clearable with nothing selected: an exit
        // that needs something selected first is not an exit.
        pianoRoll.copyNoteStateFromSelection();

        const auto loaded = readTools();
        const auto* loadedCopy = slotsFor (loaded, 26);

        check (utf8 ("...but a loaded 画笔状态 can be cleared with nothing selected"),
               loadedCopy != nullptr && loadedCopy->enabled
                 && loadedCopy->text.contains (utf8 ("清除"))
                 && pianoRoll.hasPencilState(),
               loadedCopy == nullptr ? juce::String ("no copy item") : loadedCopy->text);

        pianoRoll.clearPencilState();

        //-- 10. the card a selection names is never scrolled off its own list --
        // Phase 54 also fixed this: the offset is a pixel count and the view is
        // not fixed, so a rack grown while it was short kept an offset that
        // left every card above the view — a blank rack with the 添加音源
        // button still drawn, which reads as "my instruments are gone". Local
        // models: the shared rack is a fixture for a dozen other checks.
        {
            RackModel localRack;
            RackPanel localPanel (localRack);

            localPanel.setSize (ui::px (300), ui::px (260));   // short: cards get pushed out

            for (int i = 0; i < 6; ++i)
                localRack.addSynth();

            const int shortScroll = localPanel.getScrollY();

            // Now tall enough for everything. The offset must come back to 0,
            // not stay where the short panel left it.
            localPanel.setSize (ui::px (300), ui::px (2000));

            const int excess = juce::jmax (0, localPanel.getContentHeight() - localPanel.getViewHeight());

            check ("The card list never scrolls past its own content after a resize",
                   shortScroll > 0 && localPanel.getScrollY() == 0 && excess == 0,
                   "short " + juce::String (shortScroll) + " / now "
                     + juce::String (localPanel.getScrollY()) + " / excess "
                     + juce::String (excess));
        }

        //-- Phase 56: a trim is a selection edit, a group is one object -------
        // Local models: this block adds blocks, groups and copies, and the shared
        // fixtures would have to be unwound item by item afterwards.
        {
            LayerStack t;
            RackModel  r;

            t.addLayer (utf8 ("P56"), palette::layer1);

            auto* sheet = t.getLayer (0);

            const auto p56Source = loadSampleFile (phase27B);
            const int  slot      = r.addSampler (p56Source.buffer, phase27B.getFullPathName(), "P56");

            const auto makeClip = [&] (double start, double length)
            {
                SampleEvent clip;
                clip.name         = "P56";
                clip.path         = phase27B.getFullPathName();
                clip.audio        = p56Source.buffer;
                clip.startBeat    = start;
                clip.lengthBeats  = length;
                clip.pitch        = 72;
                clip.laneSpan     = 2;
                clip.outputSlotId = slot;
                return sheet->addSample (clip);
            };

            PianoRollComponent roll (t, r);
            roll.setSize (1200, 520);

            const auto lengthOf = [sheet] (int id)
            {
                const auto* clip = sheet->findSample (id);
                return clip != nullptr ? clip->lengthBeats : -1.0;
            };

            const auto startOf = [sheet] (int id)
            {
                const auto* clip = sheet->findSample (id);
                return clip != nullptr ? clip->startBeat : -1.0;
            };

            const auto endOf = [sheet] (int id)
            {
                const auto* clip = sheet->findSample (id);
                return clip != nullptr ? clip->endBeat() : -1.0;
            };

            const int c1 = makeClip (0.0, 2.0);
            const int c2 = makeClip (6.0, 3.0);
            const int c3 = makeClip (12.0, 1.0);

            roll.setSampleSelection ({ c1, c2, c3 });

            //-- 1. grabbing one tail stretches the whole selection -----------
            // The delta, not the pointer: 2 + 3 + 1 becomes 4 + 5 + 3, each block
            // keeping its own length difference.
            const bool trimmed = roll.simulateSampleTrim (c1, 4.0, true);

            check ("Trimming one block in a multi-selection stretches all of them",
                   trimmed
                     && std::abs (lengthOf (c1) - 4.0) < 1.0e-6
                     && std::abs (lengthOf (c2) - 5.0) < 1.0e-6
                     && std::abs (lengthOf (c3) - 3.0) < 1.0e-6,
                   juce::String (lengthOf (c1), 2) + " / " + juce::String (lengthOf (c2), 2)
                     + " / " + juce::String (lengthOf (c3), 2));

            //-- 2. grabbing a head moves every start, ends stay put ----------
            const double endBefore = endOf (c2);

            const bool headTrimmed = roll.simulateSampleTrim (c2, 7.0, false);

            check ("...and a head trim moves every start by that delta, ends pinned",
                   headTrimmed
                     && std::abs (startOf (c1) - 1.0) < 1.0e-6
                     && std::abs (startOf (c2) - 7.0) < 1.0e-6
                     && std::abs (startOf (c3) - 13.0) < 1.0e-6
                     && std::abs (endOf (c1) - 4.0) < 1.0e-6
                     && std::abs (endOf (c2) - endBefore) < 1.0e-6
                     && std::abs (endOf (c3) - 15.0) < 1.0e-6,
                   juce::String (startOf (c1), 2) + " / " + juce::String (startOf (c2), 2)
                     + " / " + juce::String (startOf (c3), 2));

            //-- 3. a group with a block in it pastes as ONE group -------------
            NoteEvent note;
            note.pitch        = 60;
            note.startBeat    = 20.0;
            note.lengthBeats  = 1.0;
            const int n1 = sheet->addNote (note);

            note.pitch     = 64;
            note.startBeat = 21.0;
            const int n2 = sheet->addNote (note);

            const int gid = sheet->addGroup (utf8 ("P56 组"), groupPalette::group2);
            sheet->assignNotesToGroup ({ n1, n2 }, gid);
            sheet->assignSamplesToGroup ({ c1 }, gid);

            roll.selectGroup (gid);

            roll.copySelectionToClipboards();       // the real Ctrl+C path
            const bool asGroup = roll.isGroupClipboard();
            const int  groupsBefore = (int) sheet->getGroups().size();

            // Deliberately the OLD entry point: it used to be the one that
            // minted a second group, and Phase 57 made it hand a group
            // clipboard over to pasteGroupFromClipboard. A test that only ever
            // calls pasteEditClipboard would not notice that forwarding going
            // away — the blocks would quietly stay on the clipboard.
            const bool pastedGroup = roll.pasteNotesFromClipboard();
            const int  groupsAfter = (int) sheet->getGroups().size();

            // Phase 57: no second group — the members land back in `gid`, so
            // the group now exists in two places with one name, one trim and
            // one mount. This is the line the user asked for; counting groups
            // is how it is visible.
            check ("A group with a block in it pastes into that same group, block included",
                   asGroup && pastedGroup
                     && groupsAfter == groupsBefore
                     && sheet->findGroup (gid) != nullptr
                     && sheet->countNotesInGroup (gid) == 4
                     && sheet->countSamplesInGroup (gid) == 2,
                   juce::String (groupsBefore) + " -> " + juce::String (groupsAfter)
                     + utf8 ("，组内现 ")
                     + juce::String ((int) sheet->countNotesInGroup (gid)) + utf8 (" 音符 / ")
                     + juce::String ((int) sheet->countSamplesInGroup (gid)) + utf8 (" 采样"));

            check ("The pasted group is what ends up selected",
                   (int) roll.getSelection().size() == 2
                     && (int) roll.getSampleSelection().size() == 1,
                   juce::String ((int) roll.getSelection().size()) + " notes / "
                     + juce::String ((int) roll.getSampleSelection().size()) + " clips");

            //-- 4. a group made of blocks ALONE pastes as a group ------------
            const int c4   = makeClip (40.0, 2.0);
            const int gid2 = sheet->addGroup (utf8 ("P56 采样组"), groupPalette::group3);
            sheet->assignSamplesToGroup ({ c4 }, gid2);

            //-- 4a. Phase 57: the frame sits ON the block, not 3px off it -----
            // The gap the user can see between a group's border and a block's
            // border was the frame's 3px expansion. Frame and member box are now
            // one rectangle, so all four edges are compared — "contains" would
            // pass just as happily with the old gap in place.
            {
                // Both rectangles are culled against the visible grid, so the
                // block has to be on screen before they can be compared.
                roll.setScrollBeats (38.0);

                const auto frame = roll.groupFrameOf (0, gid2);
                const auto box   = roll.getSampleScreenBounds (0, c4);

                const bool sameEdges = ! frame.isEmpty() && ! box.isEmpty()
                                         && std::abs (frame.getX() - box.getX()) < 0.01f
                                         && std::abs (frame.getY() - box.getY()) < 0.01f
                                         && std::abs (frame.getRight() - box.getRight()) < 0.01f
                                         && std::abs (frame.getBottom() - box.getBottom()) < 0.01f;

                check ("A group's frame leaves no gap around its block",
                       sameEdges,
                       "frame " + frame.toString() + " block " + box.toString());
            }

            roll.selectGroup (gid2);

            roll.copySelectionToClipboards();
            const bool clipsOnly = roll.isGroupClipboard() && roll.isSampleOnlyGroupClipboard();
            const int  before2     = (int) sheet->getGroups().size();

            const bool pastedClips = roll.pasteEditClipboard();
            const int  after2      = (int) sheet->getGroups().size();

            check ("A group of blocks alone also pastes into itself",
                   clipsOnly && pastedClips
                     && after2 == before2
                     && sheet->countNotesInGroup (gid2) == 0
                     && sheet->countSamplesInGroup (gid2) == 2,
                   juce::String (before2) + " -> " + juce::String (after2)
                     + utf8 ("，组内 ")
                     + juce::String ((int) sheet->countSamplesInGroup (gid2)) + utf8 (" 个采样"));

            //-- 4b. cutting that group takes its block with it ----------------
            // The block went into the clipboard as part of the group, so a cut
            // that removed only the notes would paste back a duplicate of audio
            // that never left the timeline.
            {
                const int c5 = makeClip (60.0, 2.0);

                NoteEvent cutNote;
                cutNote.pitch       = 55;
                cutNote.startBeat   = 60.0;
                cutNote.lengthBeats = 1.0;
                const int cn = sheet->addNote (cutNote);

                const int gidCut = sheet->addGroup (utf8 ("P56 剪切组"), groupPalette::group1);
                sheet->assignNotesToGroup ({ cn }, gidCut);
                sheet->assignSamplesToGroup ({ c5 }, gidCut);

                roll.selectGroup (gidCut);
                const bool cutOk = roll.cutNotesToClipboard();

                const bool gone = sheet->findNote (cn) == nullptr
                                    && sheet->findSample (c5) == nullptr;

                const bool backOk = roll.pasteEditClipboard();

                const NoteGroup* restored = nullptr;

                for (const auto& g : sheet->getGroups())
                    if (g.getId() != gid && g.getId() != gid2 && g.getId() != gidCut
                         && sheet->countNotesInGroup (g.getId()) == 1
                         && sheet->countSamplesInGroup (g.getId()) == 1)
                        restored = &g;

                check ("Cutting a group takes its block off the timeline, and the paste brings both back",
                       cutOk && gone && backOk && restored != nullptr,
                       juce::String ((int) cutOk) + " / " + juce::String ((int) gone) + " / "
                         + juce::String ((int) backOk) + " / "
                         + (restored != nullptr ? utf8 ("回了组") : utf8 ("没有组")));
            }

            //-- 5. 复制到后一小节 lands on the next BAR LINE -----------------
            NoteEvent barNote;
            barNote.pitch       = 48;
            barNote.startBeat   = 0.0;
            barNote.lengthBeats = 1.0;
            const int b1 = sheet->addNote (barNote);

            const int gid3 = sheet->addGroup (utf8 ("P56 小节"), groupPalette::group4);
            sheet->assignNotesToGroup ({ b1 }, gid3);

            const bool moved = roll.duplicateGroupToNextBar (0, gid3);

            // Phase 58: the copy is another APPEARANCE of the same group, not a
            // second NoteGroup, so it is found by its occurrence number — "a
            // group I have not seen before" no longer exists to look for.
            const auto startOfOccurrence = [&] (int gid, int occurrence)
            {
                for (int id : sheet->getOccurrenceNoteIds (gid, occurrence))
                    if (const auto* n = sheet->findNote (id))
                        return n->startBeat;

                return -1.0;
            };

            const double copyAt = startOfOccurrence (gid3, 1);

            check (utf8 ("复制到后一小节") + " puts the copy on the bar line after a group that starts on one",
                   moved && std::abs (copyAt - 4.0) < 1.0e-6,
                   juce::String (copyAt, 2));

            //-- 6. a group that starts off the grid, and is longer than a bar -
            NoteEvent longNote;
            longNote.pitch       = 50;
            longNote.startBeat   = 2.0;
            longNote.lengthBeats = 6.0;
            const int b2 = sheet->addNote (longNote);

            const int gid4 = sheet->addGroup (utf8 ("P56 长组"), groupPalette::group5);
            sheet->assignNotesToGroup ({ b2 }, gid4);

            roll.duplicateGroupToNextBar (0, gid4);

            const double copyAt2 = startOfOccurrence (gid4, 1);

            // 2.0 + "next bar" would be 4.0, which is inside the source (2..8) —
            // the copy goes to 8.0 instead, so the two never overlap.
            check ("...and a group that runs past the next bar lands clear of itself",
                   std::abs (copyAt2 - 8.0) < 1.0e-6,
                   juce::String (copyAt2, 2));

            //-- 7. the menu actually offers it -------------------------------
            juce::PopupMenu menu;
            roll.fillGroupMenu (menu, 0, gid3);

            bool   offersNextBar = false;
            juce::String itemTexts;

            for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
            {
                const auto& item = it.getItem();

                if (item.isSeparator)
                    continue;

                itemTexts += item.text + " | ";

                if (item.text.contains (utf8 ("复制到后一小节")))
                    offersNextBar = true;
            }

            check ("The group's context menu offers " + utf8 ("复制到后一小节"), offersNextBar, itemTexts);
        }

        //-- cleanup -----------------------------------------------------------
        pianoRoll.setSelection (previousSelection);
        pianoRoll.setTool (previousTool);
        pianoRoll.setSnapEnabled (previousSnap);

        layer->removeNote (noteToA);
        layer->removeNote (noteToB);

        rack.removeSlot (synthA);

        updateRackRoutingFocus();
        focusSynth (previousFocus);
        stack.notifyNotesChanged();
        pruneOrphanSamplerSlots();
    } ();

    //-- Phase 58: a group is a definition, an appearance is a thing ----------
    // The user's report was two sentences with one cause. Pasting a group put
    // the copy INSIDE the group ("粘到了组的内部"), so dragging either copy
    // moved both, and "the group's own object" was never on the timeline at
    // all. A group now has occurrences; these checks pin down what that means
    // for a drag out of the group list, for moving one copy, and for the block
    // press that used to grab a neighbour.
    [&]
    {
        LayerStack stack;
        RackModel  rack;

        stack.addLayer (utf8 ("出现"), palette::layer1);

        auto* layer = stack.getLayer (0);

        PianoRollComponent roll (stack, rack);
        roll.setSize (1400, 620);
        roll.setScrollBeats (0.0);

        const auto makeNote = [layer] (int pitch, double beat, double length)
        {
            NoteEvent note;
            note.pitch       = pitch;
            note.startBeat   = beat;
            note.lengthBeats = length;
            return layer->addNote (note);
        };

        const int noteA = makeNote (60, 1.0, 1.0);
        const int noteB = makeNote (64, 2.0, 1.0);

        const int gid = layer->addGroup (utf8 ("P58 组"), groupPalette::group2);
        layer->assignNotesToGroup ({ noteA, noteB }, gid);

        //-- 1. the payload names the sheet AND the group ---------------------
        // Two numbers, because group ids are only unique inside a layer: a
        // payload carrying just the id would land the sheet-2 row on whichever
        // sheet happened to be active.
        int payLayer = -1;
        int payGroup = -1;
        const auto payload = groupDragDescription (2, 7);

        int junkLayer = -1;
        int junkGroup = -1;

        // Parsed into a bool BEFORE check(): MSVC evaluates a call's arguments
        // right to left, so a description that read payLayer inline would be
        // built while it still held -1 and print "-1, -1" for every failure —
        // a diagnostic that lies about the thing it is diagnosing.
        const bool payloadParsed = groupDragTarget (payload, payLayer, payGroup);

        check ("A group drag carries both the sheet and the group",
               payloadParsed
                 && payLayer == 2 && payGroup == 7
                 && ! groupDragTarget ("atomic-group:", junkLayer, junkGroup)
                 && ! groupDragTarget ("atomic-group::", junkLayer, junkGroup)
                 && ! groupDragTarget (samplerDragDescription (3), junkLayer, junkGroup)
                 && ! groupDragTarget ("C:\\loops\\kick.wav", junkLayer, junkGroup),
               payload + " -> " + juce::String (payLayer) + ", " + juce::String (payGroup));

        //-- 2. the drop is one MORE appearance, not a second group -----------
        // A fixed 100 px into the grid: far enough from the source to be a
        // different place, and a beat both ends can compute from the roll's own
        // mapping (beat = scroll + px / beatWidth).
        const auto   grid     = roll.getGridBounds();
        const double beatWide = (double) roll.getBeatWidth();

        // The offset is a DENSITY-SCALED pixel count, so it has to be the same
        // number on both sides of the division: ui::pf (100) is 130 at scale
        // 1.3, and dividing the scaled x by an unscaled 100 is off by 0.625
        // beats — which is exactly how far the first run of this check missed.
        const float  dropOffset = ui::pf (100);
        const float  dropX      = grid.getX() + dropOffset;
        const double dropBeat   = roll.getScrollBeats() + (double) dropOffset / beatWide;

        const int groupsBefore = (int) layer->getGroups().size();

        roll.simulateCardDrop (groupDragDescription (0, gid),
                               { dropX, grid.getCentreY() });

        const auto occurrences = layer->getGroupOccurrences (gid);

        const auto startOf = [&] (int occurrence)
        {
            double at = -1.0;

            for (int id : layer->getOccurrenceNoteIds (gid, occurrence))
                if (const auto* note = layer->findNote (id))
                    at = at < 0.0 ? note->startBeat : juce::jmin (at, note->startBeat);

            return at;
        };

        const int newOccurrence = occurrences.empty() ? -1 : occurrences.back();

        check ("Dragging a group row onto the grid adds an appearance, not a group",
               (int) layer->getGroups().size() == groupsBefore
                 && (int) occurrences.size() == 2
                 && std::abs (startOf (0) - 1.0) < 1.0e-9
                 && newOccurrence == 1
                 && std::abs (startOf (newOccurrence) - dropBeat) < 0.02,
               juce::String ((int) layer->getGroups().size()) + utf8 (" 组 / ")
                 + juce::String ((int) occurrences.size()) + utf8 (" 处，新起点 ")
                 + juce::String (startOf (newOccurrence), 3)
                 + utf8 ("（应 ") + juce::String (dropBeat, 3) + utf8 ("）"));

        //-- 3. each appearance moves on its own ------------------------------
        // This is the bug's headline: before occurrences, "drag one copy" was
        // "drag the group", and the other copy came along.
        const double secondBefore = startOf (newOccurrence);

        roll.simulateGroupMove (0, gid, 4.0, 0, newOccurrence);

        const double sourceAfterFirstMove = startOf (0);
        const double secondAfterFirstMove = startOf (newOccurrence);

        roll.simulateGroupMove (0, gid, 2.0, 0, 0);

        check ("The appearances of one group move independently",
               std::abs (sourceAfterFirstMove - 1.0) < 1.0e-9
                 && std::abs (secondAfterFirstMove - (secondBefore + 4.0)) < 1.0e-9
                 && std::abs (startOf (0) - 3.0) < 1.0e-9
                 && std::abs (startOf (newOccurrence) - (secondBefore + 4.0)) < 1.0e-9,
               utf8 ("源 ") + juce::String (startOf (0), 2)
                 + utf8 (" / 第二处 ") + juce::String (startOf (newOccurrence), 2));

        //-- 4. `groupocc` survives the file ---------------------------------
        const auto savedXml = project::toXmlText (stack, rack, 120.0);

        LayerStack savedStack;
        RackModel  savedRack;
        double     savedBpm   = 0.0;
        juce::String savedError;

        const bool reloaded = project::fromXmlText (savedXml, savedStack, savedRack,
                                                    savedBpm, savedError);

        int reloadedOccurrences = -1;

        if (auto* reloadedLayer = savedStack.getLayer (0))
            if (reloadedLayer->findGroup (gid) != nullptr)
                reloadedOccurrences = (int) reloadedLayer->getGroupOccurrences (gid).size();

        check ("Both appearances come back from a saved file",
               reloaded && reloadedOccurrences == 2,
               juce::String (reloadedOccurrences) + utf8 (" 处")
                 + (savedError.isEmpty() ? juce::String() : " · " + savedError));

        //-- 5. dissolving ONE appearance leaves the group alone --------------
        const bool dissolvedOne = roll.disbandGroup (0, gid, newOccurrence);

        const auto afterDisband = layer->getGroupOccurrences (gid);

        check ("Dissolving one appearance keeps the group and its other appearance",
               dissolvedOne
                 && (int) layer->getGroups().size() == groupsBefore
                 && afterDisband.size() == 1 && afterDisband.front() == 0,
               juce::String ((int) afterDisband.size()) + utf8 (" 处，组 ")
                 + juce::String ((int) layer->getGroups().size()));

        //-- 6. a press on a selected block's tail grabs THAT block ----------
        // Two identical blocks, the second drawn on top. The hit test can only
        // answer "the top one"; the press used to believe it, rewrote the
        // selection to it and trimmed the wrong block — which the user saw as
        // "selecting and dragging the length sometimes fails".
        SampleEvent lower;
        lower.name        = "lower";
        lower.startBeat   = 1.0;
        lower.lengthBeats = 4.0;
        lower.pitch       = 60;
        lower.laneSpan    = 2;

        const int lowerId = layer->addSample (lower);
        const int upperId = layer->addSample (lower);   // exact overlap, added last

        roll.setSampleSelection ({ lowerId });

        const auto lowerRect = roll.getSampleScreenBounds (0, lowerId);
        const auto upperRect = roll.getSampleScreenBounds (0, upperId);

        const juce::Point<float> onTail (lowerRect.getRight() - ui::pf (2.0f),
                                         lowerRect.getCentreY());

        const auto grab = roll.getBlockGrabAtForTest (onTail);

        check ("A press on a selected block's tail grabs that block, not the one on top",
               ! lowerRect.isEmpty() && upperRect.contains (onTail)
                 && grab.first == lowerId
                 && grab.second,
               utf8 ("抓到 ") + juce::String (grab.first)
                 + utf8 ("（下层 ") + juce::String (lowerId)
                 + utf8 ("，上层 ") + juce::String (upperId) + "）");

        layer->removeSample (lowerId);
        layer->removeSample (upperId);
    } ();

    //-- Phase 59: the trim band belongs to the block it is drawn on ---------
    // "选中多个采样拖动长度时只有最右边那个拖动才有效" was one line of arithmetic:
    // `x >= right - band` has no left neighbour, so it was true for every pixel
    // to the RIGHT of a block, and `x <= left + band` for every pixel to its
    // LEFT. With three selected blocks a press on the middle one's tail was
    // answered by the next block's head — whose head cannot move (its source
    // offset clamps it), so the drag did nothing at all. The press has to be
    // inside the block whose edge it claims.
    [&]
    {
        LayerStack stack;
        RackModel  rack;

        stack.addLayer (utf8 ("拉伸"), palette::layer1);

        auto* layer = stack.getLayer (0);

        PianoRollComponent roll (stack, rack);
        roll.setSize (1400, 620);
        roll.setScrollBeats (0.0);

        // Three adjacent blocks on one lane, so the middle one's tail IS the
        // right one's head: the exact geometry that made the wrong block win.
        const auto addBlock = [layer] (const char* name, double start, double length)
        {
            SampleEvent clip;
            clip.name        = name;
            clip.startBeat   = start;
            clip.lengthBeats = length;
            clip.pitch       = 60;
            clip.laneSpan    = 2;
            return layer->addSample (clip);
        };

        const int leftId  = addBlock ("left",  1.0, 2.0);
        const int midId   = addBlock ("mid",   3.0, 2.0);
        const int rightId = addBlock ("right", 5.0, 2.0);

        roll.setSampleSelection ({ leftId, midId, rightId });

        const auto leftRect  = roll.getSampleScreenBounds (0, leftId);
        const auto midRect   = roll.getSampleScreenBounds (0, midId);
        const auto rightRect = roll.getSampleScreenBounds (0, rightId);

        const auto   grid     = roll.getGridBounds();
        const double beatWide = (double) roll.getBeatWidth();

        // Two density-scaled pixels inside the middle block's tail. Two, not
        // zero: a pixel on the boundary belongs to the NEXT block, and a test
        // that pressed exactly there would be testing the tie-break instead of
        // the thing the user does.
        const auto midTail = juce::Point<float> (midRect.getRight() - ui::pf (2.0f),
                                                 midRect.getCentreY());
        const auto leftHead = juce::Point<float> (leftRect.getX() + ui::pf (2.0f),
                                                  leftRect.getCentreY());

        const auto midGrab  = roll.getBlockGrabAtForTest (midTail);
        const auto headGrab = roll.getBlockGrabAtForTest (leftHead);

        check ("A press on the middle block's tail grabs the middle block",
               ! midRect.isEmpty()
                 && midGrab.first == midId && midGrab.second,
               utf8 ("抓到 ") + juce::String (midGrab.first)
                 + utf8 ("（应为 ") + juce::String (midId) + utf8 ("），尾部 ")
                 + (midGrab.second ? "yes" : "no"));

        check ("A press on the selection's head grabs the head, not the neighbour next to it",
               headGrab.first == leftId && ! headGrab.second,
               utf8 ("抓到 ") + juce::String (headGrab.first)
                 + utf8 ("（应为 ") + juce::String (leftId) + utf8 ("）"));

        // Past the last block there is no edge at all. The old band test said
        // "the rightmost block's tail" for every pixel to its right, so this is
        // the check that keeps the containment honest.
        const auto pastEnd = roll.getBlockGrabAtForTest (
            { rightRect.getRight() + ui::pf (40.0f), rightRect.getCentreY() });

        check ("A press in the empty grid past a block grabs nothing",
               pastEnd.first < 0,
               utf8 ("抓到 ") + juce::String (pastEnd.first));

        //-- one delta for the whole selection --------------------------------
        // Pressing the middle block's tail must trim all three by the SAME
        // amount — that is what "选中多个采样拖动长度" means. The delta is
        // computed from the block the pointer owns and then applied to the rest,
        // so the assertion compares all three against one number.
        const double pressBeat = roll.getScrollBeats()
                                   + (double) (midTail.x - grid.getX()) / beatWide;
        const double wanted    = pressBeat - 1.0;          // drag a beat shorter
        const double delta     = wanted - 5.0;             // the middle ends at beat 5

        roll.simulateSampleTrim (midId, wanted, true);

        const auto lengthOf = [layer] (int id)
        {
            const auto* clip = layer->findSample (id);
            return clip != nullptr ? clip->lengthBeats : -1.0;
        };

        check ("Trimming from the middle block's tail trims the whole selection by one delta",
               std::abs (lengthOf (leftId)  - (2.0 + delta)) < 1.0e-6
                 && std::abs (lengthOf (midId)   - (2.0 + delta)) < 1.0e-6
                 && std::abs (lengthOf (rightId) - (2.0 + delta)) < 1.0e-6
                 && std::abs ((layer->findSample (rightId)->startBeat) - 5.0) < 1.0e-9,
               juce::String (lengthOf (leftId), 3) + " / "
                 + juce::String (lengthOf (midId), 3) + " / "
                 + juce::String (lengthOf (rightId), 3)
                 + utf8 ("（应 ") + juce::String (2.0 + delta, 3) + utf8 ("）"));

        //-- the group list mirrors the roll's selection ----------------------
        // The roll is where a group gets selected; the panel is where it is
        // listed. "选中组时编组栏那行要高亮" is that mirror, and the mirror is
        // derived from the selection rather than kept as a second flag — a flag
        // is how the two views would drift apart.
        NoteEvent note;
        note.pitch       = 60;
        note.startBeat   = 1.0;
        note.lengthBeats = 1.0;
        const int noteInGroup = layer->addNote (note);

        note.pitch        = 62;
        note.startBeat    = 2.0;
        const int noteAlso  = layer->addNote (note);   // a second member, so "half" exists

        note.pitch = 64;
        const int noteAlone   = layer->addNote (note);

        const int gid  = layer->addGroup (utf8 ("镜像组"), groupPalette::group3);
        const int gid2 = layer->addGroup (utf8 ("邻居组"), groupPalette::group4);

        layer->assignNotesToGroup ({ noteInGroup, noteAlso }, gid);
        layer->assignNotesToGroup ({ noteAlone },             gid2);

        roll.selectGroup (gid);

        const auto selected = roll.getSelectedGroups();

        GroupPanel panel (stack, rack);
        panel.setSize (300, 400);

        std::vector<GroupPanel::RowRef> panelRows;

        for (const auto& pair : selected)
            panelRows.push_back ({ pair.first, pair.second });

        panel.setSelectedGroups (panelRows);

        check ("Selecting a group in the roll lights its row in the group panel",
               selected.size() == 1
                 && selected.front().first == 0 && selected.front().second == gid
                 && panel.getSelectedRowCount() == 1
                 && panel.isRowSelected (0, gid)
                 && ! panel.isRowSelected (0, gid2),
               juce::String ((int) selected.size()) + utf8 (" 组，面板 ")
                 + juce::String (panel.getSelectedRowCount()) + utf8 (" 行"));

        // Half a group is not the group: one note out of a pair must not light
        // the row, or every click on a member would claim "the whole group".
        roll.setSelection ({ noteInGroup });

        const auto partial = roll.getSelectedGroups();

        panel.setSelectedGroups ({});

        check ("A partial selection does not light the group's row",
               partial.empty() && panel.getSelectedRowCount() == 0
                 && ! panel.isRowSelected (0, gid),
               juce::String ((int) partial.size()) + utf8 (" 组"));
    } ();

    //-- Phase 59: the wiring, on the real panel ---------------------------
    // The check above proves the pieces; this one proves they are plugged in.
    // A mirror that nobody pushes to looks exactly like no mirror at all, and
    // the self-test's local panel would never notice.
    [&]
    {
        auto* liveLayer = stack.getActiveLayer();

        if (liveLayer == nullptr)
            return;

        const auto previousSelection = pianoRoll.getSelection();
        const int  layerIndex        = stack.getActiveLayerIndex();

        NoteEvent note;
        note.pitch       = 72;
        note.startBeat   = 0.0;
        note.lengthBeats = 1.0;
        const int a = liveLayer->addNote (note);
        note.pitch  = 74;
        const int b = liveLayer->addNote (note);

        const int tempGroup = liveLayer->addGroup (utf8 ("镜像接线"), groupPalette::group5);
        liveLayer->assignNotesToGroup ({ a, b }, tempGroup);

        stack.notifyNotesChanged();

        pianoRoll.selectGroup (tempGroup);

        const bool lit = groupPanel.isRowSelected (layerIndex, tempGroup);

        pianoRoll.setSelection ({ a });

        const bool cleared = ! groupPanel.isRowSelected (layerIndex, tempGroup);

        pianoRoll.setSelection (previousSelection);

        liveLayer->assignNotesToGroup ({ a, b }, -1);
        liveLayer->removeNote (a);
        liveLayer->removeNote (b);
        liveLayer->removeGroup (tempGroup);

        stack.notifyNotesChanged();

        check ("The real group panel follows the roll's selection",
               lit && cleared,
               utf8 ("选中时亮=") + (lit ? "yes" : "no")
                 + utf8 ("，半选时灭=") + (cleared ? "yes" : "no"));
    } ();

    //-- Phase 60: 编组栏能点、可选中，且长得跟机架卡一样 --------------------
    {
        LayerStack t60;
        RackModel  r60;
        t60.addLayer (utf8 ("P60"), palette::layer1);

        if (auto* l60 = t60.getLayer (0))
        {
            NoteEvent n60;
            n60.pitch = 60; n60.startBeat = 0.0; n60.lengthBeats = 1.0;
            const int a60 = l60->addNote (n60);
            n60.pitch = 64;
            const int b60 = l60->addNote (n60);
            n60.pitch = 67;
            const int c60 = l60->addNote (n60);

            const int gA = l60->addGroup (utf8 ("甲组"), groupPalette::group1);
            const int gB = l60->addGroup (utf8 ("乙组"), groupPalette::group2);

            l60->assignNotesToGroup ({ a60, b60 }, gA);
            l60->assignNotesToGroup ({ c60 },       gB);

            GroupPanel panel (t60, r60);
            panel.setSize (ui::px (300), ui::px (420));

            int heardLayer = -1;
            int heardGroup = -1;
            int shouts     = 0;

            panel.onGroupSelected = [&] (int layerIndex, int groupId)
            {
                heardLayer = layerIndex;
                heardGroup = groupId;
                ++shouts;
            };

            const bool first  = panel.selectRow (0);
            const bool onA    = panel.isRowSelected (0, gA) && ! panel.isRowSelected (0, gB);

            // A second click on the other row: the first one is a selection only
            // if the selection can move. Both checks in one, because "it lit the
            // row" alone would also pass if the panel just lit everything.
            const bool second = panel.selectRow (1);
            const bool moved  = panel.isRowSelected (0, gB) && ! panel.isRowSelected (0, gA);

            check ("Clicking a group row selects that group, and only that one",
                   first && second && onA && moved
                     && heardLayer == 0 && heardGroup == gB && shouts == 2,
                   juce::String (shouts) + utf8 (" 次点击，最后回传 ") + juce::String (heardLayer)
                     + "/" + juce::String (heardGroup));

            // The mark itself. The user asked for the group frame to look like a
            // sampler / synth card, so the row's frame has to carry the shared
            // ring in theme::accent — NOT a tint of the group's own colour.
            //
            // A style is a picture, so this reads the picture back: "it calls the
            // shared helper" is not something a call can prove, and a screenshot
            // nobody diffs is not a test.
            panel.setSelectedGroups ({ GroupPanel::RowRef { 0, gA } });

            const auto shot   = panel.createComponentSnapshot (panel.getLocalBounds(), true);
            const int  plateX = ui::px (4);      // the row plate starts here
            const int  rowA   = panel.getMuteBounds (0).getCentreY();
            const int  rowB   = panel.getMuteBounds (1).getCentreY();

            const auto closestToAccent = [&] (int y)
            {
                float best = 1.0e9f;

                for (int x = plateX - 3; x <= plateX + 7; ++x)
                {
                    const auto p = shot.getPixelAt (juce::jlimit (0, shot.getWidth()  - 1, x),
                                                    juce::jlimit (0, shot.getHeight() - 1, y));

                    // Manhattan distance in RGB: accent is (0x4f, 0xd1, 0xc5), and
                    // the plate colour is nowhere near it, so no colour space
                    // argument is needed to tell them apart.
                    const float distance = (float) (std::abs ((int) p.getRed()   - (int) theme::accent.getRed())
                                                 + std::abs ((int) p.getGreen() - (int) theme::accent.getGreen())
                                                 + std::abs ((int) p.getBlue()  - (int) theme::accent.getBlue()));

                    best = juce::jmin (best, distance);
                }

                return best;
            };

            const float selectedRow = closestToAccent (rowA);
            const float plainRow    = closestToAccent (rowB);

            check ("The selected row wears the same ring as a rack card",
                   shot.isValid() && selectedRow < 60.0f && plainRow > 150.0f,
                   utf8 ("选中行最接近 accent 的像素差 ") + juce::String (selectedRow, 0)
                     + utf8 ("，未选中行 ") + juce::String (plainRow, 0));
        }
    }

    //-- Phase 60: the wiring — a click on the REAL panel selects in the roll --
    [&]
    {
        auto* liveLayer = stack.getActiveLayer();

        if (liveLayer == nullptr)
            return;

        const auto previousSelection = pianoRoll.getSelection();
        const int  layerIndex        = stack.getActiveLayerIndex();

        NoteEvent note;
        note.pitch       = 79;
        note.startBeat   = 0.0;
        note.lengthBeats = 1.0;
        const int a = liveLayer->addNote (note);
        note.pitch  = 81;
        const int b = liveLayer->addNote (note);

        const int tempGroup = liveLayer->addGroup (utf8 ("点击接线"), groupPalette::group5);
        liveLayer->assignNotesToGroup ({ a, b }, tempGroup);
        stack.notifyNotesChanged();

        pianoRoll.setSelection ({});

        int rowIndex = -1;
        const auto rows = groupPanel.getRows();

        for (int i = 0; i < (int) rows.size(); ++i)
            if (rows[(size_t) i].layerIndex == layerIndex && rows[(size_t) i].groupId == tempGroup)
                rowIndex = i;

        const bool clicked = rowIndex >= 0 && groupPanel.selectRow (rowIndex);
        const auto selectedNotes = pianoRoll.getSelection();

        const bool bothNotes = std::find (selectedNotes.begin(), selectedNotes.end(), a) != selectedNotes.end()
                            && std::find (selectedNotes.begin(), selectedNotes.end(), b) != selectedNotes.end();

        const bool lit = groupPanel.isRowSelected (layerIndex, tempGroup);

        pianoRoll.setSelection (previousSelection);

        liveLayer->assignNotesToGroup ({ a, b }, -1);
        liveLayer->removeNote (a);
        liveLayer->removeNote (b);
        liveLayer->removeGroup (tempGroup);

        stack.notifyNotesChanged();

        check ("Clicking a row on the real group panel selects that group in the roll",
               clicked && bothNotes && lit,
               juce::String ((int) selectedNotes.size()) + utf8 (" 个音符入选，行亮=")
                 + (lit ? "yes" : "no"));
    } ();

    //-- Phase 60: the empty tail comes back when the view leaves it -----------
    {
        LayerStack t60b;
        RackModel  r60b;
        PianoRollComponent roll (t60b, r60b);
        roll.setSize (1200, 520);

        t60b.addLayer (utf8 ("P60 滚动"), palette::layer1);

        NoteEvent n60b;
        n60b.pitch = 60; n60b.startBeat = 0.0; n60b.lengthBeats = 2.0;
        t60b.getLayer (0)->addNote (n60b);

        roll.resetTimeline();

        const double baseLength = roll.getTimelineBeats();
        const float  baseThumb  = roll.getScrollThumbBounds().getWidth();

        // Pushing the view into empty space still extends the roll (Phase 11)...
        roll.setScrollBeats (140.0);

        const double grownLength = roll.getTimelineBeats();
        const float  grownThumb  = roll.getScrollThumbBounds().getWidth();

        // ...and coming back has to give BOTH back: the space on the ruler and
        // the length of the scroll thumb. Phase 11 only ever grew, so the thumb
        // stayed a stub for the rest of the session.
        roll.setScrollBeats (0.0);

        const double backLength = roll.getTimelineBeats();
        const float  backThumb  = roll.getScrollThumbBounds().getWidth();

        check ("Scrolling into the empty tail lengthens the roll, and scrolling back gives it up",
               grownLength > baseLength
                 && grownThumb < baseThumb - 1.0f
                 && std::abs (backLength - baseLength) < 1.0e-9
                 && std::abs (backThumb - baseThumb) < 1.0f,
               juce::String (baseLength, 0) + utf8 (" -> ") + juce::String (grownLength, 0)
                 + utf8 (" -> ") + juce::String (backLength, 0) + utf8 (" beats，滑块宽 ")
                 + juce::String (baseThumb, 0) + utf8 (" -> ") + juce::String (grownThumb, 0)
                 + utf8 (" -> ")               + juce::String (backThumb, 0));
    }

    //-- Phase 61: zooming the viewport is another way the ruler gets asked ----
    {
        LayerStack t61; RackModel r61; PianoRollComponent roll (t61, r61);
        roll.setSize (1200, 520);

        t61.addLayer (utf8 ("P61 缩放"), palette::layer1);

        NoteEvent n61;
        n61.pitch = 60; n61.startBeat = 0.0; n61.lengthBeats = 2.0;
        t61.getLayer (0)->addNote (n61);

        roll.resetTimeline();

        const double baseLength = roll.getTimelineBeats();
        const float  baseThumb  = roll.getScrollThumbBounds().getWidth();

        // Zoom out notch by notch: every notch must leave a ruler at least as wide
        // as the window — the same notch, not the one after it. Zooming out pivots
        // on a beat, so the anchored scroll is negative at the moment of the ask;
        // the old test summed that negative scroll with the visible width, got a
        // number smaller than the ruler, and said "no growth needed". The right
        // edge therefore stayed dead until the next notch caught up.
        int    shortNotches = 0;
        double outLength    = baseLength;
        juce::String trace;

        for (int i = 0; i < 8; ++i)
        {
            roll.setBeatWidth (roll.getBeatWidth() / 1.8f);

            const double visible = roll.getVisibleBeats();
            const double length  = roll.getTimelineBeats();

            if (visible > length + 1.0e-9)
                ++shortNotches;

            outLength = length;
            trace += juce::String (visible, 0) + utf8 ("视/") + juce::String (length, 0)
                       + utf8 ("长 ");
        }

        check ("Zooming out lengthens the roll on the same notch, not the next one",
               shortNotches == 0 && outLength > baseLength,
               juce::String (baseLength, 0) + utf8 (" -> ") + juce::String (outLength, 0)
                 + utf8 (" beats；每档 ") + trace);

        // Coming back in has to hand the beats over again. The zoom pivots on the
        // middle of the view, so the scroll lands at 34.5 beats — still short of
        // the 64 the project already had, which is the point: the ruler that grew
        // for the wide view must not be kept once the wide view is gone.
        roll.setBeatWidth (PianoRollComponent::defaultBeatWidth);

        check ("Zooming back in gives the extra ruler back, thumb included",
               std::abs (roll.getTimelineBeats() - baseLength) < 1.0e-9
                 && std::abs (roll.getScrollThumbBounds().getWidth() - baseThumb) < 1.0f,
               juce::String (outLength, 0) + utf8 (" -> ") + juce::String (roll.getTimelineBeats(), 0)
                 + utf8 (" beats，滑块宽 ") + juce::String (baseThumb, 0)
                 + utf8 (" -> ") + juce::String (roll.getScrollThumbBounds().getWidth(), 0));
    }

    //-- Phase 62: one shape, several places ---------------------------------
    // A group is ONE shape in MANY places. What an appearance owns is where it
    // sits; everything else — members, pitches, lengths, curves — belongs to the
    // group, and editing one appearance edits the group.
    {
        LayerStack t62;
        RackModel  r62;
        PianoRollComponent roll (t62, r62);
        roll.setSize (1200, 520);

        t62.addLayer (utf8 ("P62 同源"), palette::layer1);

        auto* layer = t62.getLayer (0);
        const int gid = layer->addGroup (utf8 ("组 A"), groupPalette::group1);

        NoteEvent seed;
        seed.pitch       = 60;
        seed.lengthBeats = 1.0;

        seed.startBeat = 0.0;
        const int a0 = layer->addNote (seed);

        seed.startBeat = 1.0;
        seed.pitch     = 62;
        const int a1 = layer->addNote (seed);

        layer->assignNotesToGroup ({ a0, a1 }, gid, 0);
        layer->setOccurrenceStart (gid, 0, 0.0);

        seed.startBeat = 8.0;
        seed.pitch     = 60;
        const int b0 = layer->addNote (seed);

        seed.startBeat = 9.0;
        seed.pitch     = 62;
        const int b1 = layer->addNote (seed);

        layer->assignNotesToGroup ({ b0, b1 }, gid, 1);
        layer->setOccurrenceStart (gid, 1, 8.0);

        //-- 1. editing one member edits the group ---------------------------
        roll.setSelection ({ a0 });
        roll.simulateNoteResize (a0, 1.5, true);      // tail of a0 -> 1.5

        const auto* twinB0 = layer->findNote (b0);
        const auto* twinB1 = layer->findNote (b1);

        check (utf8 ("同源：改一处音符的长度，另一处的对应音符同时变长，位置不动"),
               twinB0 != nullptr && twinB1 != nullptr
                 && std::abs (twinB0->lengthBeats - 1.5) < 1.0e-9
                 && std::abs (twinB0->startBeat - 8.0) < 1.0e-9
                 && std::abs (twinB1->lengthBeats - 1.0) < 1.0e-9,
               utf8 ("第二处：") + juce::String (twinB0 != nullptr ? twinB0->lengthBeats : -1.0, 3)
                 + utf8 (" 拍 @") + juce::String (twinB0 != nullptr ? twinB0->startBeat : -1.0, 3));

        //-- 2. a member that leaves the shape leaves it everywhere ----------
        roll.setSelection ({ a1 });
        roll.deleteSelectedNotes();

        check (utf8 ("同源：删掉一处里的一个音符，另一处对应的那个也没了，别的还在"),
               layer->findNote (b1) == nullptr
                 && layer->countNotesInOccurrence (gid, 0) == 1
                 && layer->countNotesInOccurrence (gid, 1) == 1
                 && layer->findNote (b0) != nullptr,
               juce::String (layer->countNotesInOccurrence (gid, 0)) + utf8 (" / ")
                 + juce::String (layer->countNotesInOccurrence (gid, 1)) + utf8 (" 个成员"));

        //-- 3. moving a whole appearance is a change of POSITION -------------
        roll.simulateGroupMove (0, gid, 2.0, 0, 1);   // the second copy -> beat 10

        const auto* movedB0 = layer->findNote (b0);
        const auto* keptA0  = layer->findNote (a0);

        const bool movedOnlyOne = movedB0 != nullptr && keptA0 != nullptr
                                    && std::abs (movedB0->startBeat - 10.0) < 1.0e-9
                                    && std::abs (keptA0->startBeat - 0.0) < 1.0e-9
                                    && std::abs (layer->occurrenceStart (gid, 1) - 10.0) < 1.0e-9;

        // ...and the shape is still shared afterwards: resizing the moved copy
        // reaches the one that stayed, at ITS own place.
        roll.setSelection ({ b0 });
        roll.simulateNoteResize (b0, 12.25, true);

        keptA0 = layer->findNote (a0);

        check (utf8 ("整处拖动只动自己，形状仍然同源（各按自己的起点对齐）"),
               movedOnlyOne && keptA0 != nullptr
                 && std::abs (keptA0->lengthBeats - 2.25) < 1.0e-9
                 && std::abs (keptA0->startBeat - 0.0) < 1.0e-9,
               utf8 ("第二处起 ") + juce::String (movedB0 != nullptr ? movedB0->startBeat : -1.0, 2)
                 + utf8 ("，第一处长度 ") + juce::String (keptA0 != nullptr ? keptA0->lengthBeats : -1.0, 2));

        //-- 4. the blocks are members too -----------------------------------
        SampleEvent block;
        block.name        = "blk";
        block.lengthBeats = 2.0;
        block.pitch       = 40;
        block.laneSpan    = 1;

        block.startBeat = 0.0;
        const int clipA = layer->addSample (block);
        layer->assignSamplesToGroup ({ clipA }, gid, 0);

        block.startBeat = 10.0;
        const int clipB = layer->addSample (block);
        layer->assignSamplesToGroup ({ clipB }, gid, 1);

        roll.simulateSampleTrim (clipA, 1.5, true);       // tail of the first block -> 1.5

        const auto* twinClip = layer->findSample (clipB);

        check (utf8 ("同源：采样块的宽度一起变，块的位置各在各处"),
               twinClip != nullptr
                 && std::abs (twinClip->lengthBeats - 1.5) < 1.0e-9
                 && std::abs (twinClip->startBeat - 10.0) < 1.0e-9,
               utf8 ("第二处的块：") + juce::String (twinClip != nullptr ? twinClip->lengthBeats : -1.0, 2)
                 + utf8 (" 拍 @") + juce::String (twinClip != nullptr ? twinClip->startBeat : -1.0, 2));

        //-- 5. the origins survive the file ---------------------------------
        const auto savedXml = project::toXmlText (t62, r62, 120.0);

        LayerStack reloadedStack;
        RackModel  reloadedRack;
        double     reloadedBpm = 0.0;
        juce::String reloadedError;

        const bool reloaded = project::fromXmlText (savedXml, reloadedStack, reloadedRack,
                                                    reloadedBpm, reloadedError);

        double origin0 = -1.0, origin1 = -1.0;

        if (auto* reloadedLayer = reloadedStack.getLayer (0))
            if (reloadedLayer->findGroup (gid) != nullptr)
            {
                origin0 = reloadedLayer->occurrenceStart (gid, 0);
                origin1 = reloadedLayer->occurrenceStart (gid, 1);
            }

        check (utf8 ("每处出现的位置跟着文件走，读回来还是 0 和 10"),
               reloaded && std::abs (origin0 - 0.0) < 1.0e-9 && std::abs (origin1 - 10.0) < 1.0e-9,
               juce::String (origin0, 2) + utf8 (" / ") + juce::String (origin1, 2)
                 + (reloadedError.isEmpty() ? juce::String() : " · " + reloadedError));
    }

    //-- Phase 72: 采样块的高度是上下两条边的事 -------------------------------
    // 长度一直有两种抓法（头、尾两块带子），高度却只有右键菜单——"这个块该多厚"
    // 只能一行一行点。两条水平边就是同一件事，转九十度。
    {
        LayerStack t72;
        RackModel  r72;
        PianoRollComponent roll (t72, r72);
        roll.setSize (1200, 620);

        t72.addLayer (utf8 ("P72 高度"), palette::layer1);
        auto* layer = t72.getLayer (0);

        const auto makeBlock = [layer] (const char* name, double start, int pitch, int span)
        {
            SampleEvent clip;
            clip.name        = name;
            clip.startBeat   = start;
            clip.lengthBeats = 4.0;
            clip.pitch       = pitch;
            clip.laneSpan    = span;
            return layer->addSample (clip);
        };

        // 一行中心的屏幕 y。块占 [pitch, topPitch]，矩形的 y 是 topPitch 那行的
        // 顶边，行高 = 高度 / 行数。
        const auto rowCentreY = [] (juce::Rectangle<float> b, int topPitch, int span, int lane)
        {
            const float laneH = b.getHeight() / (float) juce::jmax (1, span);
            return b.getY() + (float) (topPitch - lane) * laneH + laneH * 0.5f;
        };

        const auto showSpan = [] (const SampleEvent* c)
        {
            return c == nullptr ? juce::String ("-")
                                : juce::String (c->pitch) + "+" + juce::String (c->laneSpan);
        };

        //-- 1. 上边缘：底部不动，向上长 ------------------------------------
        const int upId = makeBlock ("up", 1.0, 60, 2);
        roll.setSampleSelection ({ upId });

        const auto upRect = roll.getSampleScreenBounds (0, upId);
        roll.simulateSampleSpanDrag (upId, rowCentreY (upRect, 61, 2, 63), true);

        const auto* up = layer->findSample (upId);

        check (utf8 ("拖采样块的上边缘，块向上长高，底部那一行不动"),
               up != nullptr && up->pitch == 60 && up->laneSpan == 4 && up->topPitch() == 63,
               showSpan (up) + utf8 ("（应为 60+4，顶 63）"));

        //-- 2. 四条边都能认出来 --------------------------------------------
        // 顶边和底边各自回答"我在这里"，正中间回答"哪条都不是"——否则整块都是
        // 缩放带，就再也拖不动位置了。
        const auto tallRect  = roll.getSampleScreenBounds (0, upId);
        const auto edgeTop   = roll.getBlockEdgeAtForTest ({ tallRect.getCentreX(),
                                                            tallRect.getY() + ui::pf (2.0f) });
        const auto edgeBottom = roll.getBlockEdgeAtForTest ({ tallRect.getCentreX(),
                                                             tallRect.getBottom() - ui::pf (2.0f) });
        const auto edgeNone  = roll.getBlockEdgeAtForTest (tallRect.getCentre());

        check (utf8 ("采样块的顶边、底边各是一条缩放带，正中间不是"),
               edgeTop.first == upId && edgeTop.second == 3
                 && edgeBottom.first == upId && edgeBottom.second == 4
                 && edgeNone.second == 0,
               juce::String (edgeTop.second) + " / " + juce::String (edgeBottom.second)
                 + " / " + juce::String (edgeNone.second) + utf8 ("（应为 3 / 4 / 0）"));

        //-- 3. 下边缘：顶部不动，向下长 ------------------------------------
        const int downId = makeBlock ("down", 6.0, 60, 2);
        roll.setSampleSelection ({ downId });

        const auto downRect = roll.getSampleScreenBounds (0, downId);
        roll.simulateSampleSpanDrag (downId, rowCentreY (downRect, 61, 2, 58), false);

        const auto* down = layer->findSample (downId);

        check (utf8 ("拖采样块的下边缘，块向下长高，顶上那一行不动"),
               down != nullptr && down->pitch == 58 && down->topPitch() == 61 && down->laneSpan == 4,
               showSpan (down) + utf8 ("（应为 58+4，顶 61）"));

        //-- 4. 两个方向都翻不过去 ------------------------------------------
        // 顶边拖到底边以下 = 一行高，不是负数；底边拖到顶边以上 = 一行高，
        // 也不会"翻过来从另一侧长"。
        roll.setSampleSelection ({ upId });
        roll.simulateSampleSpanDrag (upId, rowCentreY (tallRect, 63, 4, 55), true);

        const auto* squashedTop = layer->findSample (upId);

        roll.setSampleSelection ({ downId });
        roll.simulateSampleSpanDrag (downId, rowCentreY (downRect, 61, 4, 70), false);

        const auto* squashedBottom = layer->findSample (downId);

        check (utf8 ("上边缘拖过底边、下边缘拖过顶边，块都停在一行高，不会翻面"),
               squashedTop != nullptr && squashedTop->laneSpan == 1 && squashedTop->pitch == 60
                 && squashedBottom != nullptr && squashedBottom->laneSpan == 1
                 && squashedBottom->pitch == 61,
               showSpan (squashedTop) + " / " + showSpan (squashedBottom)
                 + utf8 ("（应为 60+1 / 61+1）"));

        //-- 5. 天花板是网格顶行 --------------------------------------------
        // 没有这个上限，加高会把块顶到 128 行外面去：laneSpan 数到 129，
        // 而画出来的那一半在网格外，用户只看到"加高没反应"。
        const int highId = makeBlock ("high", 12.0, 120, 2);
        roll.setSampleSelection ({ highId });

        const auto highRect = roll.getSampleScreenBounds (0, highId);
        roll.simulateSampleSpanDrag (highId, rowCentreY (highRect, 121, 2, 127), true);

        const auto* high = layer->findSample (highId);
        const int afterDrag = high != nullptr ? high->laneSpan : -1;

        // 已经贴上顶行了，再"加高一行"也不该加：菜单那一步走的是同一个上限，
        // 而 128 行是网格的尽头。
        roll.growSelectedSamples (1);

        const int afterGrow = high != nullptr ? high->laneSpan : -1;

        check (utf8 ("采样块长到网格顶行为止，不会伸到 128 行外面"),
               high != nullptr && afterDrag == 8 && high->topPitch() == 127
                 && afterGrow == 8,
               showSpan (high) + utf8 ("，拖到 ") + juce::String (afterDrag)
                 + utf8 (" 行，再加高还是 ") + juce::String (afterGrow)
                 + utf8 (" 行（应为 8 / 8）"));

        //-- 6. 编组：一个块变高，别的副本跟着变高 --------------------------
        const int shaped = layer->addGroup (utf8 ("高度组"), groupPalette::group1);

        const int originId = makeBlock ("copy", 20.0, 40, 2);
        layer->assignSamplesToGroup ({ originId }, shaped, 0);
        layer->setOccurrenceStart (shaped, 0, 20.0);

        const int twinId = makeBlock ("copy", 40.0, 40, 2);
        layer->assignSamplesToGroup ({ twinId }, shaped, 1);
        layer->setOccurrenceStart (shaped, 1, 40.0);

        roll.setSampleSelection ({ originId });

        const auto originRect = roll.getSampleScreenBounds (0, originId);
        roll.simulateSampleSpanDrag (originId, rowCentreY (originRect, 41, 2, 45), true);

        const auto* originClip = layer->findSample (originId);
        const auto* twinClip   = layer->findSample (twinId);

        check (utf8 ("编组里拖高一个采样块，另一处副本同时变高，位置各在各处"),
               originClip != nullptr && twinClip != nullptr
                 && originClip->laneSpan == 6 && originClip->topPitch() == 45
                 && twinClip->laneSpan == 6 && twinClip->pitch == 40
                 && std::abs (twinClip->startBeat - 40.0) < 1.0e-9,
               showSpan (originClip) + " / " + showSpan (twinClip)
                 + utf8 ("，副本起 ") + juce::String (twinClip != nullptr ? twinClip->startBeat : -1.0, 1));

        //-- 7. 菜单的加高一行也是一步，不是一像素 --------------------------
        // 菜单作用于整个选择（和删除、恢复长度一样），并且每个块各自 +1 行；
        // 副本由 syncShapeOfClip 跟上，不靠调用方记得。
        const int menuA = makeBlock ("menuA", 60.0, 50, 2);
        const int menuB = makeBlock ("menuB", 66.0, 44, 3);

        roll.setSampleSelection ({ menuA, menuB });
        roll.growSelectedSamples (1);

        const auto* clipA = layer->findSample (menuA);
        const auto* clipB = layer->findSample (menuB);

        check (utf8 ("右键加高一行作用于整个选择，每块各自长一行"),
               clipA != nullptr && clipA->laneSpan == 3
                 && clipB != nullptr && clipB->laneSpan == 4,
               showSpan (clipA) + " / " + showSpan (clipB) + utf8 ("（应为 50+3 / 44+4）"));

        roll.setSampleSelection ({ originId });
        roll.growSelectedSamples (1);

        const auto* menuOrigin = layer->findSample (originId);
        const auto* menuTwin   = layer->findSample (twinId);

        check (utf8 ("菜单加高一行，编组里的副本也长一行"),
               menuOrigin != nullptr && menuTwin != nullptr
                 && menuOrigin->laneSpan == 7 && menuTwin->laneSpan == 7,
               showSpan (menuOrigin) + " / " + showSpan (menuTwin) + utf8 ("（应为 40+7 / 40+7）"));

        //-- 8. 改高度不会把副本的内容换掉 ----------------------------------
        // 配对靠"成员在排序里的序号"，而排序曾经把 pitch 算进去：拖底边让块
        // 跨过同组的另一块，排序一变，序号配错，于是两个副本互换名字和音频。
        const int ordered = layer->addGroup (utf8 ("排序组"), groupPalette::group2);

        const int a0 = makeBlock ("aa", 80.0, 60, 2);
        const int b0 = makeBlock ("bb", 80.0, 48, 2);
        layer->assignSamplesToGroup ({ a0, b0 }, ordered, 0);
        layer->setOccurrenceStart (ordered, 0, 80.0);

        const int a1 = makeBlock ("aa", 90.0, 60, 2);
        const int b1 = makeBlock ("bb", 90.0, 48, 2);
        layer->assignSamplesToGroup ({ a1, b1 }, ordered, 1);
        layer->setOccurrenceStart (ordered, 1, 90.0);

        roll.setSampleSelection ({ a0 });

        const auto aRect = roll.getSampleScreenBounds (0, a0);
        roll.simulateSampleSpanDrag (a0, rowCentreY (aRect, 61, 2, 40), false);

        const auto* sortedA0 = layer->findSample (a0);
        const auto* sortedB0 = layer->findSample (b0);
        const auto* sortedA1 = layer->findSample (a1);
        const auto* sortedB1 = layer->findSample (b1);

        check (utf8 ("拖块下边缘越过同组另一块，两块的名字和音频不会互换"),
               sortedA0 != nullptr && sortedB0 != nullptr
                 && sortedA1 != nullptr && sortedB1 != nullptr
                 && sortedA0->name == "aa" && sortedB0->name == "bb"
                 && sortedA1->name == "aa" && sortedB1->name == "bb"
                 && sortedA0->pitch == 40 && sortedA1->pitch == 40
                 && sortedB1->pitch == 48,
               (sortedA0 != nullptr ? sortedA0->name : juce::String ("-")) + " / "
                 + (sortedB0 != nullptr ? sortedB0->name : juce::String ("-")) + utf8 (" @ ")
                 + juce::String (sortedA1 != nullptr ? sortedA1->pitch : -1) + " / "
                 + juce::String (sortedB1 != nullptr ? sortedB1->pitch : -1));

        //-- 9. 同一组的两个副本同时在选择里，变高只落一次 ------------------
        // 组自己会把形状抄给别的副本（绝对值），而选择集那条路是"起点值 +
        // delta"。两者混起来就是"抄成 4 行、再加 2 行" = 两倍高，而且每多拖
        // 一个像素就再长一次。
        const int doubled = layer->addGroup (utf8 ("双份组"), groupPalette::group3);

        const int d0 = makeBlock ("dd", 110.0, 70, 2);
        const int e0 = makeBlock ("ee", 110.0, 66, 2);
        layer->assignSamplesToGroup ({ d0, e0 }, doubled, 0);
        layer->setOccurrenceStart (doubled, 0, 110.0);

        const int d1 = makeBlock ("dd", 120.0, 70, 2);
        const int e1 = makeBlock ("ee", 120.0, 66, 2);
        layer->assignSamplesToGroup ({ d1, e1 }, doubled, 1);
        layer->setOccurrenceStart (doubled, 1, 120.0);

        roll.setSampleSelection ({ d0, d1 });

        const auto dRect = roll.getSampleScreenBounds (0, d0);
        roll.simulateSampleSpanDrag (d0, rowCentreY (dRect, 71, 2, 73), true);

        const auto* dd0 = layer->findSample (d0);
        const auto* dd1 = layer->findSample (d1);

        check (utf8 ("选择里同时有同一组的两个副本，变高只落一次，不会长成两倍"),
               dd0 != nullptr && dd1 != nullptr
                 && dd0->laneSpan == 4 && dd1->laneSpan == 4,
               showSpan (dd0) + " / " + showSpan (dd1) + utf8 ("（应为 70+4 / 70+4）"));
    }

    //-- Phase 75: 组里多选几块，上下拖要一起动 ------------------------------
    // 单选时每个块只看自己，怎么算都"对"；多选时一次手势要落成同一件事，
    // 这才是会出问题的地方——一处 clamp 吃到一点 delta，其余块就各走各的。
    {
        LayerStack t75;
        RackModel  r75;
        PianoRollComponent roll (t75, r75);
        roll.setSize (1200, 620);

        t75.addLayer (utf8 ("P75 高度"), palette::layer1);
        auto* layer = t75.getLayer (0);

        const auto makeBlock = [layer] (const char* name, double start, int pitch, int span)
        {
            SampleEvent clip;
            clip.name        = name;
            clip.startBeat   = start;
            clip.lengthBeats = 4.0;
            clip.pitch       = pitch;
            clip.laneSpan    = span;
            return layer->addSample (clip);
        };

        const auto rowCentreY = [] (juce::Rectangle<float> b, int topPitch, int span, int lane)
        {
            const float laneH = b.getHeight() / (float) juce::jmax (1, span);
            return b.getY() + (float) (topPitch - lane) * laneH + laneH * 0.5f;
        };

        const auto showSpan = [] (const SampleEvent* c)
        {
            return c == nullptr ? juce::String ("-")
                                : juce::String (c->pitch) + "+" + juce::String (c->laneSpan);
        };

        //-- 1. 同一次出现里的三块，一起拖上边缘 ---------------------------
        const int a = makeBlock ("75a", 1.0, 60, 2);
        const int b = makeBlock ("75b", 6.0, 64, 2);
        const int c = makeBlock ("75c", 11.0, 68, 3);

        const int gid = layer->addGroup (utf8 ("P75 组"), groupPalette::group1);
        layer->assignSamplesToGroup ({ a, b, c }, gid);

        roll.setSampleSelection ({ a, b, c });

        const auto rectA = roll.getSampleScreenBounds (0, a);
        roll.simulateSampleSpanDrag (a, rowCentreY (rectA, 61, 2, 64), true);

        const auto* ra = layer->findSample (a);
        const auto* rb = layer->findSample (b);
        const auto* rc = layer->findSample (c);

        check (utf8 ("组里选中三块一起拖上边缘，三块长同样多"),
               ra != nullptr && rb != nullptr && rc != nullptr
                 && ra->laneSpan == 5 && rb->laneSpan == 5 && rc->laneSpan == 6,
               showSpan (ra) + " / " + showSpan (rb) + " / " + showSpan (rc)
                 + utf8 ("（应为 60+5 / 64+5 / 68+6）"));

        //-- 3. 走完整手势：先按上边缘，再拖 -------------------------------
        // 上面的钩子拿"此刻"当起点；鼠标路径拿"按下时"的快照当起点。多选时
        // 这两者会分家——一次应用里前面的块已经被改过，后面的块还在看旧值。
        // 需求说的就是"组编辑内部"：没双击进去之前，按下会被回答成"整个组"。
        roll.enterGroupEdit (gid);
        roll.setSampleSelection ({ a, b, c });

        const auto rectA2 = roll.getSampleScreenBounds (0, a);
        // 靠左、贴顶：离上边 2 px、离左边 4 px，最近的那条边是上边；
        // 放正中间会撞上块的曲线把手（它就在顶部中央）。
        const juce::Point<float> topEdgeA { rectA2.getX() + ui::pf (4.0f),
                                            rectA2.getY() + ui::pf (2.0f) };

        const auto probe = roll.getBlockEdgeAtForTest (topEdgeA);
        const bool armed = roll.simulateSpanDragFromPress (topEdgeA,
                                                           rowCentreY (rectA2, 64, 5, 66));

        const auto* pa = layer->findSample (a);
        const auto* pb = layer->findSample (b);
        const auto* pc = layer->findSample (c);

        check (utf8 ("按下上边缘再拖（真实手势），三块仍然长一样多"),
               armed && pa != nullptr && pb != nullptr && pc != nullptr
                 && pa->laneSpan == 7 && pb->laneSpan == 7 && pc->laneSpan == 8,
               (armed ? juce::String ("armed ") : juce::String ("not armed "))
                 + juce::String (probe.first) + ":" + juce::String (probe.second) + " "
                 + showSpan (pa) + " / " + showSpan (pb) + " / " + showSpan (pc)
                 + utf8 ("（应为 60+7 / 64+7 / 68+8）"));

        //-- 2. 同一次出现里的三块，一起拖下边缘 ---------------------------
        // 上下两条边走的是两套算式（上边固定底行、下边固定顶行），一套对不代表
        // 另一套对。
        {
            LayerStack t2;
            RackModel  r2;
            PianoRollComponent roll2 (t2, r2);
            roll2.setSize (1200, 620);

            t2.addLayer (utf8 ("P75 下边"), palette::layer1);
            auto* l2 = t2.getLayer (0);

            const auto block2 = [l2] (const char* name, double start, int pitch, int span)
            {
                SampleEvent clip;
                clip.name        = name;
                clip.startBeat   = start;
                clip.lengthBeats = 4.0;
                clip.pitch       = pitch;
                clip.laneSpan    = span;
                return l2->addSample (clip);
            };

            const int m0 = block2 ("m0", 1.0, 60, 2);
            const int m1 = block2 ("m1", 6.0, 64, 2);
            const int m2 = block2 ("m2", 11.0, 68, 3);

            const int gm = l2->addGroup (utf8 ("P75 下边组"), groupPalette::group3);
            l2->assignSamplesToGroup ({ m0, m1, m2 }, gm);

            roll2.enterGroupEdit (gm);
            roll2.setSampleSelection ({ m0, m1, m2 });

            const auto rectM = roll2.getSampleScreenBounds (0, m0);
            const juce::Point<float> bottomEdgeM { rectM.getX() + ui::pf (4.0f),
                                                   rectM.getBottom() - ui::pf (2.0f) };

            roll2.simulateSpanDragFromPress (bottomEdgeM, rowCentreY (rectM, 61, 2, 58));

            const auto* rm0 = l2->findSample (m0);
            const auto* rm1 = l2->findSample (m1);
            const auto* rm2 = l2->findSample (m2);

            check (utf8 ("组里选中三块一起拖下边缘，三块向下长同样多"),
                   rm0 != nullptr && rm1 != nullptr && rm2 != nullptr
                     && rm0->pitch == 58 && rm0->laneSpan == 4
                     && rm1->pitch == 62 && rm1->laneSpan == 4
                     && rm2->pitch == 66 && rm2->laneSpan == 5,
                   showSpan (rm0) + " / " + showSpan (rm1) + " / " + showSpan (rm2)
                     + utf8 ("（应为 58+4 / 62+4 / 66+5）"));
        }

        //-- 3. 按在已选中块的中间：这一按是它的，不是压在它上面那块的 ------
        // 两块叠在同一拍同一行（复制粘贴叠到自己身上就是最常见的来历）时，
        // 命中测试答的是"画在最上面的"，而那不是用户选中的那一块。选择被
        // 改成它，多选就只剩一个块跟着动——这就是"有时候不同步"。
        {
            LayerStack t2b;
            RackModel  r2b;
            PianoRollComponent roll2b (t2b, r2b);
            roll2b.setSize (1200, 620);

            t2b.addLayer (utf8 ("P75 叠块"), palette::layer1);
            auto* l2b = t2b.getLayer (0);

            const auto block2b = [l2b] (const char* name, double start, int pitch, int span)
            {
                SampleEvent clip;
                clip.name        = name;
                clip.startBeat   = start;
                clip.lengthBeats = 4.0;
                clip.pitch       = pitch;
                clip.laneSpan    = span;
                return l2b->addSample (clip);
            };

            // under 先加（画在下面），over 后加（画在上面），位置完全一样。
            const int under = block2b ("under", 1.0, 60, 2);
            const int over  = block2b ("over",  1.0, 60, 2);
            const int mate  = block2b ("mate",  8.0, 60, 2);

            roll2b.setSampleSelection ({ under, mate });

            const auto underRect = roll2b.getSampleScreenBounds (0, under);
            roll2b.simulatePressForTest (underRect.getCentre());

            check (utf8 ("按在已选中块的中间，选择不被压在它上面的那块顶掉，起拖的是整个选择"),
                   over > 0
                     && roll2b.isSampleSelected (under) && roll2b.isSampleSelected (mate)
                     && ! roll2b.isSampleSelected (over)
                     && roll2b.getClipDragCountForTest() == 2,
                   juce::String (roll2b.isSampleSelected (under) ? "U" : "-")
                     + juce::String (roll2b.isSampleSelected (mate) ? "M" : "-")
                     + juce::String (roll2b.isSampleSelected (over) ? "O" : "-")
                     + utf8 (" / 起拖 ") + juce::String (roll2b.getClipDragCountForTest())
                     + utf8 (" 个（应为 UM- / 2）"));
        }

        //-- 4. 同一个组的两处出现，副本同时被选中 -------------------------
        // 两处出现是同一个"形状"：谁被拖谁就是模板。两处都被选中时，delta 只能
        // 加一次，而且加到各自按下时的形状上——否则主块被同步抄一遍、再被 delta
        // 推一遍，副本就追不上了。
        {
            LayerStack t2;
            RackModel  r2;
            PianoRollComponent roll2 (t2, r2);
            roll2.setSize (1200, 620);

            t2.addLayer (utf8 ("P75 两处"), palette::layer1);
            auto* l2 = t2.getLayer (0);

            const auto block2 = [l2] (const char* name, double start, int pitch, int span)
            {
                SampleEvent clip;
                clip.name        = name;
                clip.startBeat   = start;
                clip.lengthBeats = 4.0;
                clip.pitch       = pitch;
                clip.laneSpan    = span;
                return l2->addSample (clip);
            };

            const int p0 = block2 ("p0", 1.0, 60, 2);
            const int q0 = block2 ("q0", 60.0, 64, 2);
            const int p1 = block2 ("p0", 41.0, 60, 2);
            const int q1 = block2 ("q0", 100.0, 64, 2);

            const int g2 = l2->addGroup (utf8 ("P75 两处组"), groupPalette::group2);
            l2->assignSamplesToGroup ({ p0, q0 }, g2, 0);
            l2->assignSamplesToGroup ({ p1, q1 }, g2, 1);
            l2->setOccurrenceStart (g2, 1, 40.0);

            // 两处各自的对应成员同时被选中。
            roll2.setSampleSelection ({ p0, p1 });

            const auto rectP = roll2.getSampleScreenBounds (0, p0);
            roll2.simulateSampleSpanDrag (p0, rowCentreY (rectP, 61, 2, 64), true);

            const auto* rp0 = l2->findSample (p0);
            const auto* rp1 = l2->findSample (p1);
            const auto* rq0 = l2->findSample (q0);
            const auto* rq1 = l2->findSample (q1);

            check (utf8 ("同一个组的两处副本同时选中，拖上边缘后两处高度仍然一样"),
                   rp0 != nullptr && rp1 != nullptr && rq0 != nullptr && rq1 != nullptr
                     && rp0->laneSpan == 5 && rp1->laneSpan == 5
                     && rq0->laneSpan == 2 && rq1->laneSpan == 2,
                   showSpan (rp0) + " / " + showSpan (rp1) + "  ·  "
                     + showSpan (rq0) + " / " + showSpan (rq1)
                     + utf8 ("（应为 60+5 / 60+5 · 64+2 / 64+2）"));
        }

        //-- 5. 两处出现的形状本来就不一致时，拖一次要收敛到"拖到哪儿" ------
        // 老工程、撤销回来的快照都能造出这个状态。每写一块就同步一次的话，
        // 第一块的副本先把第二块的当前值改掉，第二块再拿自己的快照加 delta 写
        // 回去，两处轮流当模板——最后停在 7，而指针停在 5。
        {
            LayerStack t4;
            RackModel  r4;
            PianoRollComponent roll4 (t4, r4);
            roll4.setSize (1200, 620);

            t4.addLayer (utf8 ("P75 不同步"), palette::layer1);
            auto* l4 = t4.getLayer (0);

            const auto block4 = [l4] (const char* name, double start, int pitch, int span)
            {
                SampleEvent clip;
                clip.name        = name;
                clip.startBeat   = start;
                clip.lengthBeats = 4.0;
                clip.pitch       = pitch;
                clip.laneSpan    = span;
                return l4->addSample (clip);
            };

            const int u0 = block4 ("u0", 1.0, 60, 2);    // occ 0: 2 行
            const int v0 = block4 ("v0", 6.0, 64, 3);
            const int u1 = block4 ("u0", 41.0, 60, 4);   // occ 1: 4 行（不一致）
            const int v1 = block4 ("v0", 46.0, 64, 5);

            const int g4 = l4->addGroup (utf8 ("P75 不同步组"), groupPalette::group5);
            l4->assignSamplesToGroup ({ u0, v0 }, g4, 0);
            l4->assignSamplesToGroup ({ u1, v1 }, g4, 1);
            l4->setOccurrenceStart (g4, 1, 40.0);

            roll4.setSampleSelection ({ u0, u1 });

            const auto rectU = roll4.getSampleScreenBounds (0, u0);
            roll4.simulateSampleSpanDrag (u0, rowCentreY (rectU, 61, 2, 64), true);

            const auto* ru0 = l4->findSample (u0);
            const auto* ru1 = l4->findSample (u1);

            check (utf8 ("两处形状本来不一致时，拖一次停在指针指着的那个高度，不是停在更远的那个"),
                   ru0 != nullptr && ru1 != nullptr
                     && ru0->laneSpan == 5 && ru1->laneSpan == 5,
                   showSpan (ru0) + " / " + showSpan (ru1)
                     + utf8 ("（应为 60+5 / 60+5）"));
        }

        //-- 6. 两处出现的成员数不一样时，多选拖拽仍然落到同一个高度 ------
        // 这不是想象出来的状态：整处删除只影响那一处（P62），所以一个组的两
        // 处出现本来就可能一个有 2 个成员、一个只有 1 个。组同步在这种情况
        // 下会"多退少补"，如果它插在循环中间，后面的块就在拿旧值跟新模板比。
        {
            LayerStack t3;
            RackModel  r3;
            PianoRollComponent roll3 (t3, r3);
            roll3.setSize (1200, 620);

            t3.addLayer (utf8 ("P75 不等"), palette::layer1);
            auto* l3 = t3.getLayer (0);

            const auto block3 = [l3] (const char* name, double start, int pitch, int span)
            {
                SampleEvent clip;
                clip.name        = name;
                clip.startBeat   = start;
                clip.lengthBeats = 4.0;
                clip.pitch       = pitch;
                clip.laneSpan    = span;
                return l3->addSample (clip);
            };

            const int s0 = block3 ("s0", 1.0, 60, 2);
            const int t0 = block3 ("t0", 6.0, 64, 2);
            const int s1 = block3 ("s0", 41.0, 60, 2);   // 第二处只有 s0 这一个

            const int g3 = l3->addGroup (utf8 ("P75 不等组"), groupPalette::group4);
            l3->assignSamplesToGroup ({ s0, t0 }, g3, 0);
            l3->assignSamplesToGroup ({ s1 }, g3, 1);
            l3->setOccurrenceStart (g3, 1, 40.0);

            roll3.setSampleSelection ({ s0, s1 });

            const auto rectS = roll3.getSampleScreenBounds (0, s0);
            roll3.simulateSampleSpanDrag (s0, rowCentreY (rectS, 61, 2, 64), true);

            const auto* rs0 = l3->findSample (s0);
            const auto* rs1 = l3->findSample (s1);

            check (utf8 ("两处出现成员数不一样时，同时选中两处的块拖高度，两处高度仍然一样"),
                   rs0 != nullptr && rs1 != nullptr
                     && rs0->laneSpan == 5 && rs1->laneSpan == 5,
                   showSpan (rs0) + " / " + showSpan (rs1)
                     + utf8 ("（应为 60+5 / 60+5）"));
        }
    }

    //-- Phase 76: 双击进组不许收窄已有的选区 ---------------------------------
    // 用户报的"组内调多个采样高度，有时要拖两次其他才跟"，根子不在高度算术
    // 上——216 种组合的模糊检查都过——而在"进组"这一步偷走了选区：
    //
    //   点组 → 整次出现被选中（P73 的规矩）；
    //   双击其中一个块进组 → P73 把选区改写成"被双击的那一个"；
    //   拖动它的上边缘 → 只有它一个跟着长。
    //
    // 第二次之所以好了，是因为用户重新选了一遍。同一个东西已经在选区里，一个
    // 打开手势就不该把它换掉——和 P65 给组名 pill 定的那条一样。
    {
        LayerStack t76;
        RackModel  r76;
        PianoRollComponent roll (t76, r76);
        roll.setSize (1200, 620);

        t76.addLayer (utf8 ("P76 高度"), palette::layer1);
        auto* layer = t76.getLayer (0);

        const int gid = layer->addGroup (utf8 ("P76 组"), groupPalette::group1);

        const auto makeBlock = [layer, gid] (const char* name, double start, int pitch, int occ)
        {
            SampleEvent clip;
            clip.name            = name;
            clip.startBeat       = start;
            clip.lengthBeats     = 4.0;
            clip.pitch           = pitch;
            clip.laneSpan        = 2;
            clip.groupId         = gid;
            clip.groupOccurrence = occ;
            return layer->addSample (clip);
        };

        const int a0 = makeBlock ("a", 1.0, 60, 0);
        const int b0 = makeBlock ("b", 6.0, 64, 0);
        const int c0 = makeBlock ("c", 11.0, 68, 0);

        makeBlock ("a", 41.0, 60, 1);
        makeBlock ("b", 46.0, 64, 1);
        makeBlock ("c", 51.0, 68, 1);

        layer->setOccurrenceStart (gid, 0, 0.0);
        layer->setOccurrenceStart (gid, 1, 40.0);

        const auto spansOf = [&] ()
        {
            juce::String out;

            for (const auto& s : layer->getSamples())
                out << s.name << "." << s.groupOccurrence << "=" << s.pitch << "+"
                    << s.laneSpan << "  ";

            return out;
        };

        //-- 1. 点一个块：闭组状态回答"整次出现" ---------------------------
        roll.simulatePressForTest (roll.getSampleScreenBounds (0, a0).getCentre());

        const int whole = (int) roll.getSampleSelection().size();

        //-- 2. 双击同一个块进组：进组，但不是"改成只选它" ------------------
        roll.simulateDoubleClickForTest (roll.getSampleScreenBounds (0, a0).getCentre());

        check (utf8 ("双击进组不会把已经选中的整次出现收窄成一个块"),
               roll.getEditingGroupId() == gid && whole == 3
                 && roll.isSampleSelected (a0) && roll.isSampleSelected (b0)
                 && roll.isSampleSelected (c0),
               juce::String (whole) + utf8 (" 块被选中 → 进组后 ")
                 + juce::String ((int) roll.getSampleSelection().size()) + utf8 (" 块（应为 3 → 3）"));

        //-- 3. 接着拖 a0 的上边缘两格：同一次出现的三块一起长 --------------
        const auto rect = roll.getSampleScreenBounds (0, a0);
        const float laneH = rect.getHeight() / 2.0f;
        const float x = rect.getX() + rect.getWidth() * 0.35f;
        const float y = rect.getY() + ui::pf (2.0f);

        const bool armed = roll.simulateSpanDragFromPress ({ x, y }, y - laneH * 2.0f);

        const auto* ra = layer->findSample (a0);
        const auto* rb = layer->findSample (b0);
        const auto* rc = layer->findSample (c0);

        check (utf8 ("进组后拖一块的上边缘，同一处出现的三块一起长高"),
               armed && ra != nullptr && rb != nullptr && rc != nullptr
                 && ra->laneSpan == 4 && rb->laneSpan == 4 && rc->laneSpan == 4,
               (armed ? juce::String ("armed ") : juce::String ("not armed ")) + spansOf()
                 + utf8 ("（应为 a=60+4 b=64+4 c=68+4，两处各一份）"));
    }

    //-- Phase 76: 高度拖拽的模糊检查 -----------------------------------------
    // 抽头的音高、层高、拖动格数、上/下边缘、选中组合、副本个数全扫一遍：同一次
    // 出现里被选中的块必须走同样多。算术要是哪天被改坏，这一条会先挂。
    {
        int checked = 0;
        juce::String firstBad;

        for (int occ = 1; occ <= 3 && firstBad.isEmpty(); ++occ)
        for (int members = 2; members <= 3 && firstBad.isEmpty(); ++members)
        for (int basePitch = 40; basePitch <= 100 && firstBad.isEmpty(); basePitch += 20)
        for (int span0 = 1; span0 <= 4 && firstBad.isEmpty(); span0 += 3)
        for (int selCount = 2; selCount <= members && firstBad.isEmpty(); ++selCount)
        for (int dir = -1; dir <= 1 && firstBad.isEmpty(); dir += 2)
        for (int lanes = 1; lanes <= 3 && firstBad.isEmpty(); ++lanes)
        {
            LayerStack tF;
            RackModel  rF;
            PianoRollComponent roll (tF, rF);
            roll.setSize (1200, 620);

            tF.addLayer (utf8 ("FUZZ"), palette::layer1);
            auto* layer = tF.getLayer (0);

            const int gid = layer->addGroup (utf8 ("F"), groupPalette::group1);

            std::vector<int> ids;

            for (int o = 0; o < occ; ++o)
                for (int m = 0; m < members; ++m)
                {
                    SampleEvent clip;
                    clip.name            = juce::String ("m") + juce::String (m);
                    clip.startBeat       = 1.0 + m * 5.0 + o * 40.0;
                    clip.lengthBeats     = 4.0;
                    clip.pitch           = basePitch + m * 3;
                    clip.laneSpan        = span0;
                    clip.groupId         = gid;
                    clip.groupOccurrence = o;
                    ids.push_back (layer->addSample (clip));
                }

            for (int o = 0; o < occ; ++o)
                layer->setOccurrenceStart (gid, o, o * 40.0);

            roll.enterGroupEdit (gid);

            std::vector<int> sel;

            for (int i = 0; i < selCount; ++i)
                sel.push_back (ids[(size_t) i]);

            roll.setSampleSelection (sel);

            const int primary = sel.front();
            const auto r0 = roll.getSampleScreenBounds (0, primary);

            if (! roll.getGridBounds().contains (r0.getCentre()))
                continue;                       // 看的见的块才按得到

            const float x = r0.getX() + r0.getWidth() * 0.35f;
            const bool fromTop = dir < 0;
            const float pressY = fromTop ? r0.getY() + ui::pf (2.0f)
                                         : r0.getBottom() - ui::pf (2.0f);

            const float laneH = r0.getHeight() / (float) span0;
            const float targetY = pressY + (fromTop ? -1.0f : 1.0f) * laneH * (float) lanes;

            const bool armed = roll.simulateSpanDragFromPress ({ x, pressY }, targetY);
            const auto* p = layer->findSample (primary);

            if (! armed || p == nullptr)
            {
                firstBad = "not armed (occ=" + juce::String (occ) + " m=" + juce::String (members)
                             + " base=" + juce::String (basePitch) + " span0=" + juce::String (span0)
                             + " sel=" + juce::String (selCount) + ")";
                break;
            }

            const int primaryDelta = p->laneSpan - span0;

            for (size_t i = 0; i < sel.size(); ++i)
            {
                const auto* c = layer->findSample (sel[i]);

                if (c == nullptr)
                {
                    firstBad = "selected block vanished";
                    break;
                }

                // 只有真正被网格顶端夹住的块才允许偏离。
                const int ceil = juce::jlimit (1, SampleEvent::numPitchLanes,
                                               128 - (basePitch + (int) i * 3));

                if (c->laneSpan != juce::jlimit (1, ceil, span0 + primaryDelta))
                {
                    firstBad = "sel#" + juce::String ((int) i) + " span " + juce::String (span0)
                                 + " -> " + juce::String (c->laneSpan) + "，抽头却 "
                                 + juce::String (span0) + " -> " + juce::String (p->laneSpan)
                                 + "（occ=" + juce::String (occ) + " m=" + juce::String (members)
                                 + " base=" + juce::String (basePitch) + " sel=" + juce::String (selCount)
                                 + " lanes=" + juce::String (lanes) + " top=" + juce::String ((int) fromTop) + "）";
                    break;
                }
            }

            ++checked;
        }

        check (utf8 ("模糊：同一次出现里多块一起拖高度，从没漏过一块"),
               firstBad.isEmpty(),
               firstBad.isEmpty() ? juce::String (checked) + utf8 (" 组合全过") : firstBad);
    }

    //-- Phase 76: 拖到键盘最顶上，夹住的是每个块自己的天花板 ------------------
    // 上面那条模糊检查只看得见中间三十来格，怎么拖都够不到 128，所以它算的
    // 那条 jlimit(1, ceil, …) 一直是防守性的——把兄弟块的夹取整段删掉，它照样
    // 全过（咬测 4 就是这么过的）。真到顶的那一下必须单独测：主块被顶到 127
    // 之后，剩下的空间要由每个块自己算，站得高的先封顶，站得低的继续走。
    {
        LayerStack t76t;
        RackModel  r76t;
        PianoRollComponent roll (t76t, r76t);
        roll.setSize (1200, 620);

        t76t.addLayer (utf8 ("P76 顶"), palette::layer1);
        auto* layer = t76t.getLayer (0);

        const int gidT = layer->addGroup (utf8 ("T"), groupPalette::group1);

        const auto addBlock = [&] (const char* name, int pitch, int span)
        {
            SampleEvent clip;
            clip.name        = name;
            clip.startBeat   = 1.0 + (pitch - 64) * 0.5;
            clip.lengthBeats = 4.0;
            clip.pitch       = pitch;
            clip.laneSpan    = span;
            clip.groupId     = gidT;
            return layer->addSample (clip);
        };

        const int low  = addBlock ("low",  64, 2);   // 头顶还有 64 格
        const int high = addBlock ("high", 88, 2);   // 头顶只剩 40 格

        roll.enterGroupEdit (gidT);
        roll.setSampleSelection ({ low, high });

        const auto rect = roll.getSampleScreenBounds (0, low);
        const float x = rect.getX() + rect.getWidth() * 0.35f;
        const float y = rect.getY() + ui::pf (2.0f);

        // 往上拽两百格：远多于任何一条天花板，两根都得撞顶。
        const bool armedT = roll.simulateSpanDragFromPress ({ x, y }, y - ui::pf (1400.0f));

        const auto* pl = layer->findSample (low);
        const auto* ph = layer->findSample (high);

        check (utf8 ("拖到键盘最顶上，主块停在 127，高处的兄弟只长到自己那一格天花板"),
               armedT && pl != nullptr && ph != nullptr
                 && pl->topPitch() == PianoRollComponent::numPitches - 1
                 && pl->laneSpan == PianoRollComponent::numPitches - 64
                 && ph->laneSpan == PianoRollComponent::numPitches - 88,
               (armedT ? juce::String ("armed ") : juce::String ("not armed "))
                 + (pl != nullptr ? juce::String (pl->pitch) + "+" + juce::String (pl->laneSpan)
                                  : juce::String ("?")) + " / "
                 + (ph != nullptr ? juce::String (ph->pitch) + "+" + juce::String (ph->laneSpan)
                                  : juce::String ("?"))
                 + utf8 ("（应为 64+64 / 88+40：低的顶到 127，高的先封顶）"));
    }

    //-- Phase 77: 双击 ghost 音符 = 去那一层编辑 ------------------------------
    // 单击仍然点穿（P1/P20：看不见的图层不是能抓住的东西），但"让我过去"是
    // 另一个动词，而且在那一像素上活动层什么都没有，双击没有别的意思可言。
    // 判据要落在两件事上：活动图层换了，**且**换过去之后那东西能改——组里的
    // 成员要连组一起打开，不然接下去那一下还是整组一起拖（P73）。
    {
        LayerStack t77;
        RackModel  r77;
        PianoRollComponent roll (t77, r77);
        roll.setSize (1200, 620);

        t77.addLayer (utf8 ("底"), palette::layer1);        // 0，待会儿的 ghost
        t77.addLayer (utf8 ("表"), palette::layer2);        // 1，addLayer 把新层设为活动

        auto* under = t77.getLayer (0);
        auto* over  = t77.getLayer (1);

        const int plain   = under->addNote ({ 0, 72, 0.0, 1.0, 0.9f });
        const int grouped = under->addNote ({ 0, 76, 4.0, 1.0, 0.9f });
        const int partner = under->addNote ({ 0, 80, 6.0, 1.0, 0.9f });

        const int gid = under->addGroup (utf8 ("G"), groupPalette::group1);
        under->assignNotesToGroup ({ grouped, partner }, gid);

        const int onTop = over->addNote ({ 0, 64, 0.0, 1.0, 0.9f });

        t77.setLayerVisible (0, true);      // 眼睛开着才有 ghost

        const auto centreOfUnder = [&] (int id)
        {
            return roll.getNoteScreenBounds (0, id).getCentre();
        };

        //-- 1. 单击还是点穿 ---------------------------------------------
        roll.setSelection ({ onTop });
        roll.simulatePressForTest (centreOfUnder (plain));

        check (utf8 ("单击 ghost 音符仍然点穿，选不到它"),
               roll.getEditableNoteIdAt (centreOfUnder (plain)) == -1
                 && t77.getActiveLayerIndex() == 1,
               juce::String (roll.getEditableNoteIdAt (centreOfUnder (plain)))
                 + utf8 (" / 层 ") + juce::String (t77.getActiveLayerIndex()));

        //-- 2. 双击不分组的那一个：换层 + 选中它 + 打开它的包络 --------------
        // 最后那半条是为了"同一手势同一结果"：在那一层上双击同一个音符会打开
        // 它的包络检查器，跨层双击只做换层就成了两套语义。
        int openedLayer = -2, openedNote = -2;
        roll.onNoteDoubleClicked = [&] (int layerIndex, int noteId)
        {
            openedLayer = layerIndex;
            openedNote  = noteId;
        };

        roll.setSelection ({ onTop });
        roll.simulateDoubleClickForTest (centreOfUnder (plain));

        check (utf8 ("双击 ghost 音符会换成它所在的图层，选中它，并打开它的包络"),
               t77.getActiveLayerIndex() == 0 && roll.isNoteSelectedForTest (plain)
                 && roll.getSelection().size() == 1 && roll.getEditingGroupId() < 0
                 && openedLayer == 0 && openedNote == plain,
               utf8 ("层 ") + juce::String (t77.getActiveLayerIndex())
                 + utf8 (" / 选中 ") + juce::String ((int) roll.getSelection().size())
                 + utf8 (" 个 / 组 ") + juce::String (roll.getEditingGroupId())
                 + utf8 (" / 包络 ") + juce::String (openedLayer) + ":"
                 + juce::String (openedNote) + utf8 ("（应为 0:") + juce::String (plain) + "）");

        //-- 3. 双击组里的那一个：换层 + 进组 + 整次出现 -------------------
        // 只换层不进组的话，用户到了那一层还是要再双击一次，而这一步正是这一
        // 阶段要省掉的。整次出现而不是单个成员，是 P74 的答案。
        // 上一轮已经把活动层挪到 0 了，先退回去，让 grouped 重新变回 ghost。
        t77.setActiveLayerIndex (1);
        openedLayer = openedNote = -2;      // 组里的成员双击是"打开组"，不是打开包络
        roll.simulateDoubleClickForTest (centreOfUnder (grouped));

        check (utf8 ("双击组里的 ghost 音符会连组一起打开，并选中整次出现"),
               t77.getActiveLayerIndex() == 0 && roll.getEditingGroupId() == gid
                 && roll.isNoteSelectedForTest (grouped) && roll.isNoteSelectedForTest (partner)
                 && openedNote == -2,
               juce::String (t77.getActiveLayerIndex()) + utf8 (" / 组 ")
                 + juce::String (roll.getEditingGroupId()) + utf8 (" / 选中 ")
                 + juce::String ((int) roll.getSelection().size()) + utf8 (" 个")
                 + (openedNote == -2 ? juce::String() : utf8 (" / 却也打开了包络")));

        //-- 4. 同一像素上活动层自己有音符：先问活动层 ---------------------
        // 判据是"这一像素在这一层回答谁"，不是"选中了谁"：普通音符的双击本来
        // 就不改选区（P76 打开不是选择），它只回答"我点了你"。
        const int shadow = over->addNote ({ 0, 72, 0.0, 1.0, 0.9f });
        t77.setActiveLayerIndex (1);
        roll.setSelection ({});

        roll.simulateDoubleClickForTest (roll.getNoteScreenBounds (1, shadow).getCentre());

        check (utf8 ("ghost 上面压着活动层自己的音符时，双击改的是它，不会跳到 ghost 那一层"),
               t77.getActiveLayerIndex() == 1
                 && roll.getEditableNoteIdAt (roll.getNoteScreenBounds (1, shadow).getCentre()) == shadow,
               utf8 ("层 ") + juce::String (t77.getActiveLayerIndex())
                 + utf8 (" / 命中 ") + juce::String (roll.getEditableNoteIdAt (
                       roll.getNoteScreenBounds (1, shadow).getCentre()))
                 + utf8 ("（应为 1 / ") + juce::String (shadow) + "）");

        //-- 4b. 钩子自己的契约：绝不回答"你正在编辑的这一层" ----------------
        // 上一条那个像素上其实**有** ghost（另一层同一位置也有音符），所以在那里
        // 答 0 是对的。要问"会不会把自己当成 ghost"，得挑一个只有活动层有东西
        // 的位置——那种地方没有任何东西可跳。
        const int ownOnly = over->addNote ({ 0, 68, 10.0, 1.0, 0.9f });

        check (utf8 ("只有活动层自己在那儿有音符时，ghost 命中不会回答自己这一层"),
               roll.getGhostLayerAtForTest (roll.getNoteScreenBounds (1, ownOnly).getCentre()) == -1,
               juce::String (roll.getGhostLayerAtForTest (roll.getNoteScreenBounds (1, ownOnly).getCentre()))
                 + utf8 ("（应为 -1，活动层是 1）"));

        //-- 5. 眼睛关掉的图层不参与 -------------------------------------
        // 必须挑一个**只**在 ghost 层有东西的像素：用 plain 的话会撞上第 4 条
        // 刚压在同一个位置的 shadow，活动层先回答，这条就变成空断言了。
        t77.setLayerVisible (0, false);
        t77.setActiveLayerIndex (1);

        roll.simulateDoubleClickForTest (centreOfUnder (grouped));

        check (utf8 ("眼睛关掉的图层，双击它原来的位置不会跳过去"),
               t77.getActiveLayerIndex() == 1
                 && roll.getEditableNoteIdAt (centreOfUnder (grouped)) == -1,
               utf8 ("层 ") + juce::String (t77.getActiveLayerIndex())
                 + utf8 (" / 命中 ") + juce::String (roll.getEditableNoteIdAt (centreOfUnder (grouped)))
                 + utf8 ("（应为 1 / -1）"));
    }

    //-- Phase 78: 双击采样块 = 打开它的包络窗 -------------------------------
    // 采样块的曲线一直只有右上角那个 12 px 的 chevron 能开（P49 的内联框）。
    // 双击要的是另一种东西：和音符那边同款的面板窗口——说清楚"这是哪个块、
    // 哪条曲线、去哪"，并且有地方画形状。
    //
    // 这一段只测手势那一半：双击有没有把「哪个块的窗」问对人，以及三条边界
    // （音符优先、闭组先"进去"、ghost 换层）。窗口本身下一段单独测。
    {
        LayerStack t78;
        RackModel  r78;
        PianoRollComponent roll (t78, r78);
        roll.setSize (1200, 620);

        t78.addLayer (utf8 ("底"), palette::layer1);       // 0，待会儿的 ghost
        t78.addLayer (utf8 ("表"), palette::layer2);       // 1，活动层

        auto* under = t78.getLayer (0);
        auto* over  = t78.getLayer (1);

        SampleEvent shape;
        shape.name        = "P78";
        shape.path        = "P78.wav";
        shape.startBeat   = 0.0;
        shape.lengthBeats = 4.0;
        shape.pitch       = 60;
        shape.laneSpan    = 4;

        const int block = over->addSample (shape);

        SampleEvent inGroup = shape;
        inGroup.pitch       = 72;
        inGroup.laneSpan    = 2;
        inGroup.startBeat   = 6.0;
        const int g1 = over->addSample (inGroup);

        SampleEvent inGroup2 = inGroup;
        inGroup2.pitch       = 76;
        inGroup2.startBeat   = 8.0;
        const int g2 = over->addSample (inGroup2);

        const int gid = over->addGroup (utf8 ("P78 组"), groupPalette::group1);
        over->assignSamplesToGroup ({ g1, g2 }, gid);

        const auto centreOfClip = [&] (int layerIndex, int id)
        {
            return roll.getSampleScreenBounds (layerIndex, id).getCentre();
        };

        int openedLayer  = -2;
        int openedSample = -2;

        roll.onSampleDoubleClicked = [&] (int layerIndex, int sampleId)
        {
            openedLayer  = layerIndex;
            openedSample = sampleId;
        };

        //-- 1. 不分组、没人压着：请宿主开窗 -----------------------------
        roll.simulateDoubleClickForTest (centreOfClip (1, block));

        check (utf8 ("双击采样块会把「哪个块的窗」问对人，并且不顺手开内联框"),
               openedLayer == 1 && openedSample == block && ! roll.isSampleAutomationOpen(),
               juce::String (openedLayer) + ":" + juce::String (openedSample)
                 + utf8 (" / 内联框 ") + (roll.isSampleAutomationOpen() ? utf8 ("开着") : utf8 ("关着"))
                 + utf8 ("（应为 1:") + juce::String (block) + utf8 (" / 关着）"));

        //-- 2. 内联框开着时：窗口接管，内联框收掉 -----------------------
        // 同一根曲线不能有两个编辑器同时说"我在编辑它"。chevron 的内联框
        // 和这个窗口是同一件事的两档，开一档就得关另一档。
        roll.openSampleAutomation (1, block);
        openedLayer = openedSample = -2;
        roll.simulateDoubleClickForTest (centreOfClip (1, block));

        check (utf8 ("内联框开着时双击，窗口接手并先把内联框收掉"),
               openedSample == block && ! roll.isSampleAutomationOpen(),
               juce::String (openedSample) + utf8 (" / 内联框 ")
                 + (roll.isSampleAutomationOpen() ? utf8 ("还开着") : utf8 ("关着"))
                 + utf8 ("（应为 ") + juce::String (block) + utf8 (" / 关着）"));

        //-- 3. 块上压着音符：先回答音符 ---------------------------------
        const int overNote = over->addNote ({ 0, 61, 1.0, 1.0, 0.9f });
        int openedNote = -2;
        roll.onNoteDoubleClicked = [&] (int, int noteId) { openedNote = noteId; };

        openedLayer = openedSample = -2;
        roll.simulateDoubleClickForTest (roll.getNoteScreenBounds (1, overNote).getCentre());

        check (utf8 ("采样块上压着音符时，双击改的是音符，不会打开块的窗"),
               openedNote == overNote && openedSample == -2,
               juce::String (openedNote) + ":" + juce::String (openedSample)
                 + utf8 ("（应为 ") + juce::String (overNote) + utf8 (": -2）"));

        over->removeNote (overNote);      // 别让它的像素挡在下一条路上

        //-- 4. 闭组里的块：双击是"让我进去" -----------------------------
        openedLayer = openedSample = -2;
        roll.simulateDoubleClickForTest (centreOfClip (1, g1));

        check (utf8 ("双击闭组里的采样块只进组，不会顺手把窗推到你面前"),
               roll.getEditingGroupId() == gid && openedSample == -2,
               juce::String (roll.getEditingGroupId()) + utf8 (" / 窗 ")
                 + juce::String (openedSample) + utf8 ("（应为 ") + juce::String (gid) + " / -2）");

        //-- 5. 已经进去了：同一个双击现在才是开窗 -----------------------
        roll.simulateDoubleClickForTest (centreOfClip (1, g1));

        check (utf8 ("进了组之后再双击那个块，打开的是它的窗"),
               roll.getEditingGroupId() == gid && openedSample == g1,
               juce::String (openedSample) + utf8 ("（应为 ") + juce::String (g1)
                 + utf8 ("，组 ") + juce::String (roll.getEditingGroupId()) + "）");

        //-- 6. ghost 块：换层 + 开窗 -----------------------------------
        // 和音符那条同构（P77）：跳过去之后，那个手势在那一层上会做的事也要
        // 一起做，否则用户到了那儿还得再双击一次。
        SampleEvent shadowClip = shape;
        shadowClip.pitch       = 78;
        shadowClip.laneSpan    = 2;
        const int ghostBlock = under->addSample (shadowClip);

        t78.setLayerVisible (0, true);      // 眼睛开着才有 ghost
        t78.setActiveLayerIndex (1);        // 第 4 条已在组里，这里显式设回来

        openedLayer = openedSample = -2;
        roll.simulateDoubleClickForTest (centreOfClip (0, ghostBlock));

        check (utf8 ("双击 ghost 采样块会换成它所在的图层，并打开它的窗"),
               t78.getActiveLayerIndex() == 0 && openedLayer == 0 && openedSample == ghostBlock,
               utf8 ("层 ") + juce::String (t78.getActiveLayerIndex())
                 + utf8 (" / 窗 ") + juce::String (openedLayer) + ":"
                 + juce::String (openedSample)
                 + utf8 ("（应为 0 / 0:") + juce::String (ghostBlock) + utf8 ("）"));
    }

    //-- Phase 78: 采样包络窗本身 ---------------------------------------------
    // 窗口的判据不能是"画出来了"，只能是三件事：指着哪个块、编辑的是不是模型里
    // 那条曲线（而不是副本）、模型变了之后还知不知道自己该关。
    {
        LayerStack t78b;
        RackModel  r78b;
        SampleEnvelopePanel panel (t78b, r78b);
        panel.setSize (SampleEnvelopePanel::panelWidth, SampleEnvelopePanel::panelHeight);

        t78b.addLayer (utf8 ("窗"), palette::layer1);
        auto* layer = t78b.getLayer (0);

        SampleEvent clip;
        clip.name        = "P78b";
        clip.path        = "P78b.wav";
        clip.startBeat   = 2.0;
        clip.lengthBeats = 4.0;
        const int id = layer->addSample (clip);

        auto* model = layer->findSample (id);

        panel.openFor (0, id);

        check (utf8 ("窗口打开后指着那个块，编辑器拿的是模型里那条音量曲线"),
               panel.isOpen() && panel.getEditedSampleId() == id && panel.getEditedLayerIndex() == 0
                 && panel.getEditor().getEnvelope() == &model->volumeCurve,
               juce::String (panel.getEditedSampleId()) + utf8 ("（应为 ") + juce::String (id) + "）");

        //-- 切到声相：写回模型，编辑器跟着换 ----------------------------
        panel.setTargetOrdinal (1);

        check (utf8 ("窗口切到声相：写回模型，编辑器改的也换成那条曲线"),
               panel.getTargetOrdinal() == 1
                 && model->automationTarget == SampleAutomationTarget::pan
                 && panel.getEditor().getEnvelope() == &model->panCurve,
               juce::String (panel.getTargetOrdinal()) + utf8 (" / 模型 ")
                 + juce::String ((int) model->automationTarget) + utf8 ("（应为 1 / 1）"));

        //-- 声相上的「直线」是中置，不是硬右 ---------------------------
        // 两条曲线的中性点不一样（音量 1.0、声相 0.5）。一个"直线"按钮如果
        // 只会写 1.0，在声相上就是硬右——按钮名字和结果对不上。
        panel.applyPreset (2);

        check (utf8 ("声相曲线上的「直线」落在 0.5 中置，不是 1.0 硬右"),
               juce::approximatelyEqual (model->panCurve.valueAt (0.5f), 0.5f)
                 && model->panCurve.isEnabled(),
               juce::String (model->panCurve.valueAt (0.5f), 3)
                 + (model->panCurve.isEnabled() ? utf8 (" / 启用") : utf8 (" / 停用"))
                 + utf8 ("（应为 0.500 / 启用）"));

        //-- 音量上的「渐弱」从 1 落到 0 ---------------------------------
        panel.setTargetOrdinal (0);
        panel.applyPreset (1);

        check (utf8 ("音量曲线上的「渐弱」从 1 落到 0"),
               model->volumeCurve.valueAt (0.0f) > 0.9f
                 && model->volumeCurve.valueAt (1.0f) < 0.1f
                 && model->volumeCurve.isEnabled(),
               juce::String (model->volumeCurve.valueAt (0.0f), 2) + " -> "
                 + juce::String (model->volumeCurve.valueAt (1.0f), 2));

        //-- 换活动层：关 -----------------------------------------------
        t78b.addLayer (utf8 ("别的层"), palette::layer2);   // 新层即活动层

        check (utf8 ("换到别的图层时窗口自己关掉，不会留在看不见的块上"),
               ! panel.isOpen(),
               panel.isOpen() ? utf8 ("还开着") : utf8 ("已关"));

        //-- 删块：关 ---------------------------------------------------
        t78b.setActiveLayerIndex (0);
        panel.openFor (0, id);
        layer->removeSample (id);
        t78b.notifyNotesChanged();

        check (utf8 ("块被删掉后窗口自己关掉"),
               ! panel.isOpen(),
               panel.isOpen() ? utf8 ("还开着") : utf8 ("已关"));
    }

    //-- Phase 73: 编组要先双击进去，才能改里面的东西 -------------------------
    // 组是一个对象：点它就整块拖，像视频剪辑里的片段。把七个音符里的一个单独
    // 挪走是另一种意图，需要另一种手势——双击。在此之前，任何一次按下都得到
    // "整个出现"这个答案，所以一次误拖不可能悄悄把成员带出队形。
    {
        LayerStack t73;
        RackModel  r73;
        PianoRollComponent roll (t73, r73);
        roll.setSize (1200, 620);

        t73.addLayer (utf8 ("P73 组"), palette::layer1);
        auto* layer = t73.getLayer (0);

        const int na      = layer->addNote ({ 0, 60, 0.0, 2.0, 0.9f });
        const int nb      = layer->addNote ({ 0, 64, 6.0, 2.0, 0.9f });
        // 组框之外，但仍在可见区里：pitch 56 在组成员（60..71）下方，
        // beat 4 在组的时间范围内。放太靠右会落到可视区外——那样这一按
        // 落在空白上，断言测的就不是"点到组外成员"了。
        const int outside = layer->addNote ({ 0, 56, 4.0, 2.0, 0.9f });

        const int gid = layer->addGroup (utf8 ("P73 组"), groupPalette::group1);
        layer->assignNotesToGroup ({ na, nb }, gid);

        SampleEvent clip;
        clip.name        = "p73";
        clip.startBeat   = 12.0;
        clip.lengthBeats = 2.0;
        clip.pitch       = 70;
        clip.laneSpan    = 2;

        const int clipId = layer->addSample (clip);
        layer->assignSamplesToGroup ({ clipId }, gid);

        const auto centreOf = [&] (int id)
        {
            return roll.getNoteScreenBounds (0, id).getCentre();
        };

        const auto showSel = [&] ()
        {
            return juce::String (roll.isNoteSelectedForTest (na) ? "A" : "-")
                     + juce::String (roll.isNoteSelectedForTest (nb) ? "B" : "-")
                     + juce::String (roll.isNoteSelectedForTest (outside) ? "O" : "-")
                     + utf8 (" / 组 ") + juce::String (roll.getEditingGroupId());
        };

        //-- 1. 没进去之前，按下只回答"整个组" -----------------------------
        roll.setSelection ({});
        roll.simulatePressForTest (centreOf (na));

        check (utf8 ("未进入组内编辑时，点组里一个音符会选中整组，而不是那一个"),
               roll.isNoteSelectedForTest (na) && roll.isNoteSelectedForTest (nb) && ! roll.isNoteSelectedForTest (outside)
                 && roll.getEditingGroupId() < 0,
               showSel() + utf8 ("（应为 AB-，组 -1）"));

        //-- 2. 双击才是"进去" --------------------------------------------
        // Phase 76: 进组不再收窄选区。上一击刚把整次出现选中，双击是"打开"，
        // 不是"改成只选这个"——收窄的后果见 P76 的高度拖动用例。
        roll.simulateDoubleClickForTest (centreOf (na));

        check (utf8 ("双击组里的音符会进入这个组的编辑，刚选中的整组不会被收窄"),
               roll.getEditingGroupId() == gid
                 && roll.isNoteSelectedForTest (na) && roll.isNoteSelectedForTest (nb),
               showSel() + utf8 ("（应为 AB-，组 ") + juce::String (gid) + "）");

        //-- 3. 进去之后，点谁选谁 ----------------------------------------
        // Phase 76: 「收窄成这一个」只发生在按的这个还不在选区里的时候。
        // 按在已经选中的成员上不能替换选区——多选之后还要能按住其中一个把
        // 整组一起拖走（P65 给组名 pill 定的是同一条）。两条都要能过，
        // 只测后面那半会让"多选后按一下就散了"变成静默的回归。
        roll.simulatePressForTest (centreOf (nb));

        const bool keptTheSelection = roll.getEditingGroupId() == gid
                                        && roll.isNoteSelectedForTest (na)
                                        && roll.isNoteSelectedForTest (nb);

        check (utf8 ("组内编辑时按在已选中的音符上，不会把选区收窄成它一个"),
               keptTheSelection,
               showSel() + utf8 ("（应为 AB-，组 ") + juce::String (gid) + "）");

        roll.setSelection ({});
        roll.simulatePressForTest (centreOf (nb));

        check (utf8 ("进入组内编辑后，点没被选中的音符就只选它一个"),
               roll.isNoteSelectedForTest (nb) && ! roll.isNoteSelectedForTest (na) && roll.getEditingGroupId() == gid,
               showSel() + utf8 ("（应为 -B-，组未退出）"));

        //-- 4. 双击空白退出 ----------------------------------------------
        const auto grid = roll.getGridBounds();

        roll.simulateDoubleClickForTest ({ grid.getRight() - ui::pf (24.0f),
                                           grid.getBottom() - ui::pf (24.0f) });

        check (utf8 ("组内编辑时双击空白区域会退出编辑"),
               roll.getEditingGroupId() < 0,
               juce::String (roll.getEditingGroupId()) + utf8 ("（应为 -1）"));

        //-- 5. 采样块也一样：没进去就只是"这一组" ------------------------
        roll.setSelection ({});
        roll.simulatePressForTest (roll.getSampleScreenBounds (0, clipId).getCentre());

        check (utf8 ("未进入组内编辑时，点组里的采样块会抓住整个组"),
               roll.isNoteSelectedForTest (na) && roll.isNoteSelectedForTest (nb) && roll.isSampleSelected (clipId)
                 && roll.getEditingGroupId() < 0,
               showSel() + utf8 (" / 块 ") + (roll.isSampleSelected (clipId) ? "1" : "0"));

        //-- 6. Phase 75: 组内点空白 = 取消选择，不是又抓起整组 ----------
        // 双击进去之后，组框里除了成员就是空白。以前这一击被组框接住，
        // 变成"选中整组并起拖"——用户在组里想取消选择，结果反而全选了一遍。
        roll.simulateDoubleClickForTest (centreOf (na));
        roll.simulatePressForTest (centreOf (nb));

        const bool hadSelection = roll.isNoteSelectedForTest (nb);

        // 组框里、成员之外的一小块：na 在 beat 0..2，nb 在 6..8，
        // 取 beat 4（组框内、两个成员之间）来点。
        const float gapX = roll.getNoteScreenBounds (0, na).getRight()
                             + (roll.getNoteScreenBounds (0, nb).getX()
                                  - roll.getNoteScreenBounds (0, na).getRight()) * 0.5f;

        roll.simulatePressForTest ({ gapX, centreOf (na).y });

        check (utf8 ("进入组内编辑后，点组里的空白区域会取消选择，而不是重新抓住整组"),
               hadSelection && roll.getEditingGroupId() == gid
                 && roll.getSelection().empty() && roll.getSampleSelection().empty(),
               juce::String ((int) roll.getSelection().size()) + utf8 (" 个音符 / 块 ")
                 + juce::String ((int) roll.getSampleSelection().size()) + utf8 (" / 组 ")
                 + juce::String (roll.getEditingGroupId())
                 + utf8 ("（应为 0 个 / 0 / ") + juce::String (gid) + utf8 ("）"));

        //-- 7. 组外一击就是离开 ------------------------------------------
        roll.simulateDoubleClickForTest (centreOf (na));
        const bool wasInside = roll.getEditingGroupId() == gid;

        roll.simulatePressForTest (centreOf (outside));

        check (utf8 ("组内编辑时点到组外的音符，会离开这个组"),
               wasInside && roll.getEditingGroupId() < 0
                 && roll.isNoteSelectedForTest (outside) && ! roll.isNoteSelectedForTest (na),
               showSel() + (wasInside ? utf8 ("（进去过）") : utf8 ("（没进去）")));

        //-- 8. 连"抓住边缘改长度"也是进去之后才行 ------------------------
        // 选中了整组不等于抓住了整组：选中和起拖是两次写入，一个只选对了、
        // 却把单音符的缩放臂起来的实现，能骗过所有只看选择的断言。
        const auto tailOf = [&] (int id)
        {
            const auto b = roll.getNoteScreenBounds (0, id);
            return juce::Point<float> { b.getRight() - ui::pf (3.0f),
                                        b.getBottom() - ui::pf (3.0f) };
        };

        roll.setSelection ({});
        roll.simulatePressForTest (tailOf (na));

        const bool lockedGrab = roll.getDragOriginalCountForTest() == 2
                                  && roll.getResizeNoteIdForTest() == -1
                                  && ! roll.pressArmedNoteResizeForTest();

        roll.simulateDoubleClickForTest (centreOf (na));
        roll.simulatePressForTest (tailOf (na));

        // Phase 76: 进组不再收窄选区，所以这里手上还是两块——判据不能是
        // 「原样有几个」（那是"会动的东西"，两块当然两个），而是「谁会被
        // 缩放」。P25 把它锁在 dragResizeNoteId 上：拖长度只拉指针下那一块，
        // 不管选区里有几个。测数量会在"多选"和"只缩放一个"同时成立时误报。
        const bool openGrab = roll.getResizeNoteIdForTest() == na
                                && roll.pressArmedNoteResizeForTest()
                                && roll.isNoteSelectedForTest (na);

        check (utf8 ("组的长度只有进去之后才能单独改：没进去时按边缘仍是整组，进去后才是缩放那一个"),
               lockedGrab && openGrab,
               juce::String (roll.getDragOriginalCountForTest())
                 + (roll.pressArmedNoteResizeForTest() ? utf8 (" 个 / 缩放") : utf8 (" 个 / 不缩放"))
                 + utf8 (" / 缩放目标 ") + juce::String (roll.getResizeNoteIdForTest())
                 + utf8 ("（关门时应 2 个 / 不缩放 / -1，开门后缩放目标应是 ")
                 + juce::String (na) + "；"
                 + (lockedGrab ? utf8 ("关门对") : utf8 ("关门错"))
                 + " / " + (openGrab ? utf8 ("开门对") : utf8 ("开门错")) + "）");
    }

    //-- Phase 74: 选中一个成员，就是选中它所在的编组 --------------------------
    // 选择是"就是这些东西"这句话。组是一个东西，所以框选切到一个成员，回来
    // 的必须是整次出现——否则下一次 Delete 就从队形里撕掉一个成员，而这正是
    // 关闭状态的存在意义。打开编辑的组例外：在它里面，成员就是那个对象。
    {
        LayerStack t74;
        RackModel  r74;
        PianoRollComponent roll (t74, r74);
        roll.setSize (1200, 620);

        t74.addLayer (utf8 ("P74 选择"), palette::layer1);
        auto* layer = t74.getLayer (0);

        const int na = layer->addNote ({ 0, 60, 0.0, 2.0, 0.9f });
        const int nb = layer->addNote ({ 0, 64, 6.0, 2.0, 0.9f });

        const int gid = layer->addGroup (utf8 ("P74 组"), groupPalette::group1);
        layer->assignNotesToGroup ({ na, nb }, gid);

        SampleEvent clip;
        clip.name        = "p74";
        clip.startBeat   = 12.0;
        clip.lengthBeats = 2.0;
        clip.pitch       = 70;
        clip.laneSpan    = 2;

        const int clipId = layer->addSample (clip);
        layer->assignSamplesToGroup ({ clipId }, gid);

        // 第二个组：行错开、拍重叠也行，行的不同就是不相交。
        const int nc = layer->addNote ({ 0, 66, 1.0, 1.0, 0.9f });
        const int nd = layer->addNote ({ 0, 67, 9.0, 1.0, 0.9f });

        layer->assignNotesToGroup ({ nc, nd },
                                   layer->addGroup (utf8 ("P74 二组"), groupPalette::group2));

        // 组外的松散音符：扩散只在组里发生，它必须原样听命中。
        const int loose = layer->addNote ({ 0, 52, 16.0, 2.0, 0.9f });

        const auto showSel = [&] ()
        {
            const auto mark = [&] (int id) { return roll.isNoteSelectedForTest (id) ? "1" : "0"; };

            return juce::String (mark (na)) + mark (nb) + mark (nc) + mark (nd)
                     + mark (loose) + utf8 (" / 块 ") + (roll.isSampleSelected (clipId) ? "1" : "0")
                     + utf8 (" / 组 ") + juce::String (roll.getEditingGroupId());
        };

        const auto bandOver = [&] (int id, float inset)
        {
            return roll.getNoteScreenBounds (0, id).reduced (inset);
        };

        //-- 1. 框选只切到一个音符，回来的是整个出现 ----------------------
        roll.setSelection ({});
        roll.simulateMarquee (bandOver (na, ui::pf (3.0f)));

        check (utf8 ("框选切到组里一个音符，选中的是整个出现（含采样块）"),
               roll.isNoteSelectedForTest (na) && roll.isNoteSelectedForTest (nb)
                 && roll.isSampleSelected (clipId) && ! roll.isNoteSelectedForTest (loose),
               showSel() + utf8 ("（应为 11000 / 块 1）"));

        //-- 2. 框选只切到采样块的一角，回来同样是整组 --------------------
        roll.setSelection ({});
        roll.simulateMarquee (roll.getSampleScreenBounds (0, clipId)
                                  .reduced (ui::pf (3.0f)).withTrimmedLeft (ui::pf (6.0f)));

        check (utf8 ("框选切到组里采样块的一角，选中的同样是整个出现"),
               roll.isSampleSelected (clipId) && roll.isNoteSelectedForTest (na)
                 && roll.isNoteSelectedForTest (nb),
               showSel() + utf8 ("（应为 11000 / 块 1）"));

        //-- 3. 打开编辑的组不扩散：那里成员就是对象 ----------------------
        roll.simulateDoubleClickForTest (roll.getNoteScreenBounds (0, na).getCentre());
        const bool groupOpen = roll.getEditingGroupId() == gid;

        roll.setSelection ({});
        roll.simulateMarquee (bandOver (na, ui::pf (3.0f)));

        const juce::String detail73 = showSel()
                                        + utf8 ("（应为 10000 / 块 0 / 组 ")
                                        + juce::String (gid);

        check (utf8 ("进入组内编辑后，框选只选到框里的那个成员"),
               groupOpen && roll.isNoteSelectedForTest (na)
                 && ! roll.isNoteSelectedForTest (nb) && ! roll.isSampleSelected (clipId),
               detail73);

        roll.exitGroupEdit();

        //-- 4. Shift 点另一组的成员：是加进选择，不是把选择换掉 ----------
        roll.setSelection ({});
        roll.simulatePressForTest (roll.getNoteScreenBounds (0, na).getCentre());

        const bool firstGroup = roll.isNoteSelectedForTest (na) && roll.isNoteSelectedForTest (nb);

        roll.simulatePressForTest (roll.getNoteScreenBounds (0, nc).getCentre(),
                                   juce::ModifierKeys (juce::ModifierKeys::shiftModifier));

        check (utf8 ("Shift 点另一个组的成员，前一组的成员留在选择里，新的一组整组加入"),
               firstGroup && roll.isNoteSelectedForTest (nc) && roll.isNoteSelectedForTest (nd)
                 && roll.isNoteSelectedForTest (na) && roll.isNoteSelectedForTest (nb)
                 && roll.isSampleSelected (clipId),
               showSel() + utf8 ("（应为 11110 / 块 1）"));

        //-- 5. Shift 框选同样是"加" --------------------------------------
        roll.setSelection ({});
        roll.simulatePressForTest (roll.getNoteScreenBounds (0, na).getCentre());

        roll.setMarqueeAdditiveForTest (true);
        roll.simulateMarquee (bandOver (nc, ui::pf (3.0f)));
        roll.setMarqueeAdditiveForTest (false);

        const bool keptBoth = roll.isNoteSelectedForTest (na) && roll.isNoteSelectedForTest (nb)
                                && roll.isNoteSelectedForTest (nc) && roll.isNoteSelectedForTest (nd)
                                && roll.isSampleSelected (clipId);

        // ...and a sweep that catches nothing must not throw that away either.
        roll.setMarqueeAdditiveForTest (true);
        roll.simulateMarquee ({ -4000.0f, -4000.0f, ui::pf (10.0f), ui::pf (10.0f) });
        roll.setMarqueeAdditiveForTest (false);

        check (utf8 ("Shift 框选把新的一组加进选择，空扫也不会把选择清空"),
               keptBoth && roll.isNoteSelectedForTest (na) && roll.isNoteSelectedForTest (nb)
                 && roll.isNoteSelectedForTest (nc) && roll.isNoteSelectedForTest (nd)
                 && roll.isSampleSelected (clipId),
               showSel() + utf8 ("（应为 11110 / 块 1）"));
    }

    //-- Phase 64 / 66: the playhead follower must not oscillate ---------------
    {
        LayerStack t64;
        RackModel  r64;
        PianoRollComponent roll (t64, r64);
        roll.setSize (1200, 520);
        t64.addLayer (utf8 ("P64 跟随"), palette::layer1);
        roll.setPlayheadBeat (0.0);
        roll.setTransportActive (true);

        const double visible = roll.getVisibleBeats();

        double lastScroll = roll.getScrollBeats();
        int    jumps      = 0;
        int    backwards  = 0;
        double biggest    = 0.0;
        double overshoot  = 0.0;
        double worstGap   = 0.0;     // distance from the playhead to the right edge
        bool   following  = false;

        // 120 BPM at 60 fps is about 0.033 beat per frame — the same cadence the
        // transport timer drives this with.
        for (double b = 0.033; b <= 220.0; b += 0.033)
        {
            roll.setPlayheadBeat (b);

            const double now = roll.getScrollBeats();

            // Phase 66: the playhead may never leave the window on the right.
            // "Stays on the last beat of the ruler" is exactly this number.
            overshoot = juce::jmax (overshoot, b - (now + visible));

            // ...and once the follow has started, it has to KEEP the playhead at
            // the edge. A page-turn lands it in the middle of the window and
            // waits: the gap is then most of a screen, which is the thing that
            // is not wanted here.
            if (following)
                worstGap = juce::jmax (worstGap, (now + visible) - b);

            if (! juce::approximatelyEqual (now, lastScroll))
            {
                ++jumps;
                following = true;

                if (now < lastScroll - 1.0e-9)
                    ++backwards;

                biggest = juce::jmax (biggest, std::abs (now - lastScroll));
                lastScroll = now;
            }
        }

        // Phase 64's bug was the push undoing itself every frame; Phase 66 moved
        // the trigger from 75% of the window to the edge itself, which makes the
        // follow a continuous slide rather than a page-turn. Both are pinned
        // here: no backwards step, no playhead outside on the right, and the
        // playhead never parked in the middle of the window either.
        check (utf8 ("播放头前进时始终贴在窗口右边界，视口只向右滑"),
               backwards == 0 && overshoot < 0.01 && worstGap < 0.1,
               juce::String (jumps) + utf8 (" 次跟随，回退 ") + juce::String (backwards)
                 + utf8 (" 次，最大步进 ") + juce::String (biggest, 2)
                 + utf8 (" 拍，越过右边界 ") + juce::String (overshoot, 3)
                 + utf8 (" 拍，离右边界最远 ") + juce::String (worstGap, 2)
                 + utf8 (" 拍（窗口 ") + juce::String (visible, 1) + utf8 (" 拍）"));

        // ...and the hand-back still does its own job: a loop jump or a rewind
        // that puts the playhead back where the user was looking returns the
        // window. Suppressing the oscillation must not suppress this.
        const double pushed = roll.getScrollBeats();

        roll.setPlayheadBeat (0.0);

        check (utf8 ("播放头跳回原窗口时，视图仍然归还"),
               std::abs (roll.getScrollBeats()) < 1.0e-6,
               juce::String (pushed, 1) + utf8 (" -> ") + juce::String (roll.getScrollBeats(), 1));
    }

    //-- Phase 66: the way BACK is pinned to the left edge --------------------
    {
        LayerStack t66;
        RackModel  r66;
        PianoRollComponent roll (t66, r66);
        roll.setSize (1200, 520);
        t66.addLayer (utf8 ("P66 回退"), palette::layer1);
        roll.setPlayheadBeat (0.0);
        roll.setTransportActive (true);

        const double visible = roll.getVisibleBeats();

        // Walk out past the right edge, the way playing does.
        for (double b = 0.2; b <= 140.0; b += 0.2)
            roll.setPlayheadBeat (b);

        const double farScroll = roll.getScrollBeats();

        // ...then come back, one frame at a time. Scrubbing and a loop jump both
        // look like this to the roll.
        int    increases = 0;
        double outside   = 0.0;
        double last      = roll.getScrollBeats();

        for (double b = 139.8; b >= 20.0; b -= 0.2)
        {
            roll.setPlayheadBeat (b);

            const double now = roll.getScrollBeats();

            // "Stays on the first beat of the ruler" means never left of the
            // window's own left edge.
            outside = juce::jmax (outside, now - b);

            if (now > last + 1.0e-9)
                ++increases;

            last = now;
        }

        check (utf8 ("播放头回退时始终贴在窗口左边界，视口只向左滑"),
               increases == 0 && outside < 0.01 && roll.getScrollBeats() < farScroll,
               juce::String (farScroll, 1) + utf8 (" -> ") + juce::String (roll.getScrollBeats(), 1)
                 + utf8 (" · 反向移动 ") + juce::String (increases)
                 + utf8 (" 次，播放头越出左边 ") + juce::String (outside, 3)
                 + utf8 (" 拍（窗口 ") + juce::String (visible, 1) + utf8 (" 拍）"));
    }

    //-- Phase 66: a rewind to beat 0 is a return, not a scroll ---------------
    {
        LayerStack t66z;
        RackModel  r66z;
        PianoRollComponent roll (t66z, r66z);
        roll.setSize (1200, 520);
        t66z.addLayer (utf8 ("P66 归零"), palette::layer1);
        roll.setPlayheadBeat (0.0);

        // The window the user is looking at is NOT at the start, and the
        // playhead starts inside it.
        roll.setScrollBeats (100.0);
        roll.setPlayheadBeat (101.0);
        roll.setTransportActive (true);

        const double parked = roll.getScrollBeats();

        // A rewind to the very beginning: the left edge must not chase it.
        roll.setPlayheadBeat (0.0);

        check (utf8 ("播放头归零时不把视口拽回开头（排除 0 点）"),
               std::abs (roll.getScrollBeats() - parked) < 1.0e-6,
               juce::String (parked, 1) + utf8 (" -> ") + juce::String (roll.getScrollBeats(), 1));
    }

    //-- Phase 66: the name tag is never wider than the group -----------------
    {
        LayerStack t66t;
        RackModel  r66t;

        t66t.addLayer (utf8 ("P66 标签"), palette::layer1);

        auto* layer = t66t.getLayer (0);

        NoteEvent seed;
        seed.pitch    = 72;      // middle C: a fresh roll centres its scroll there
        seed.velocity = 0.9f;

        // 0.1 beats is 5 px on screen (getNoteBounds' floor) — far narrower than
        // the 34 px minimum pill used to be.
        seed.startBeat   = 8.0;
        seed.lengthBeats = 0.1;
        const int thin = layer->addNote (seed);

        // ...and one with room to spare, so "clamped" is not "always squashed".
        seed.startBeat   = 20.0;
        seed.lengthBeats = 4.0;
        const int wide = layer->addNote (seed);

        const int gThin = layer->addGroup (utf8 ("窄组"), groupPalette::group1);
        const int gWide = layer->addGroup (utf8 ("宽组"), groupPalette::group2);

        layer->assignNotesToGroup ({ thin }, gThin, 0);
        layer->assignNotesToGroup ({ wide }, gWide, 0);

        PianoRollComponent roll (t66t, r66t);
        roll.setSize (1400, 520);

        const auto thinPill = roll.getGroupLabelBounds (0, gThin, 0);
        const auto thinBox  = roll.getGroupFrameBoundsForTest (0, gThin, 0);
        const auto widePill = roll.getGroupLabelBounds (0, gWide, 0);
        const auto wideBox  = roll.getGroupFrameBoundsForTest (0, gWide, 0);

        check (utf8 ("组比名字标签还窄时，标签收成组的宽度"),
               ! thinPill.isEmpty() && ! thinBox.isEmpty()
                 && thinPill.getWidth() <= thinBox.getWidth() + 0.5f
                 && thinPill.getWidth() >= thinBox.getWidth() - 0.5f,
               juce::String (thinPill.getWidth(), 1) + utf8 (" px 标签 vs ")
                 + juce::String (thinBox.getWidth(), 1) + utf8 (" px 组"));

        check (utf8 ("足够宽的组，标签仍是自己该有的宽度（不是一律压扁）"),
               widePill.getWidth() > ui::pf (20)
                 && widePill.getWidth() <= wideBox.getWidth() + 0.5f,
               juce::String (widePill.getWidth(), 1) + utf8 (" px 标签 vs ")
                 + juce::String (wideBox.getWidth(), 1) + utf8 (" px 组"));

        check (utf8 ("标签窄到放不下曲线把手时，把手不出现在标签外面"),
               roll.getGroupAutomationHandleBounds (0, gThin, 0).isEmpty()
                 && ! roll.getGroupAutomationHandleBounds (0, gWide, 0).isEmpty(),
               juce::String (thinPill.getWidth(), 1) + utf8 (" px / ")
                 + juce::String (widePill.getWidth(), 1) + utf8 (" px"));
    }

    //-- Phase 67: scrubbing past the edge drags the window with it -----------
    {
        LayerStack t67;
        RackModel  r67;
        PianoRollComponent roll (t67, r67);
        roll.setSize (1200, 520);
        t67.addLayer (utf8 ("P67 拖拽"), palette::layer1);
        roll.setPlayheadBeat (0.0);

        // The host's wiring minus the engine: a locate comes back as a playhead
        // move. That is the path the gesture has to survive — the roll is not
        // the only thing that writes playheadBeat.
        roll.onLocateRequested = [&roll] (double beat) { roll.setPlayheadBeat (beat); };

        const auto   grid    = roll.getGridBounds();
        const double visible = roll.getVisibleBeats();
        const double parked  = roll.getScrollBeats();
        const double startTimeline = roll.getTimelineBeats();

        //-- 1. inside the window a scrub is still just a scrub ------------
        // The new code must not turn every drag into a scroll: between the edges
        // the pointer IS the playhead, and the picture stays put.
        roll.simulateRulerScrubForTest (grid.getX() + grid.getWidth() * 0.5f);

        const double insideScroll = roll.getScrollBeats();
        const double insideHead   = roll.getPlayheadBeatForTest();

        check (utf8 ("标尺拖拽落在窗口内：视口不动，播放头跟着指针走"),
               std::abs (insideScroll - parked) < 1.0e-9
                 && std::abs (insideHead - visible * 0.5) < 0.05,
               utf8 ("视口 ") + juce::String (insideScroll, 2)
                 + utf8 ("，播放头 ") + juce::String (insideHead, 2)
                 + utf8 (" 拍（窗口 ") + juce::String (visible, 1) + utf8 (" 拍）"));

        //-- 2. past the RIGHT edge the window is dragged along ------------
        // Two and a half windows out, so this also has to GROW the ruler: clamped
        // to the old length the head would stop at beat 64 while the pointer kept
        // going, which is the bug this is here to prevent.
        roll.simulateRulerScrubForTest (grid.getRight() + (float) (grid.getWidth() * 2.5));

        const double rightScroll = roll.getScrollBeats();
        const double rightHead   = roll.getPlayheadBeatForTest();

        check (utf8 ("指针拖到右边界外：视口跟着右移，播放头钉在右边界"),
               rightScroll > insideScroll + 1.0
                 && rightHead > startTimeline + 1.0
                 && std::abs (rightHead - (rightScroll + visible)) < 0.05
                 && roll.getTimelineBeats() >= rightHead - 1.0e-6,
               juce::String (insideScroll, 2) + utf8 (" -> ") + juce::String (rightScroll, 2)
                 + utf8 ("，播放头 ") + juce::String (rightHead, 2)
                 + utf8 ("，标尺 ") + juce::String (roll.getTimelineBeats(), 1) + utf8 (" 拍"));

        //-- 3. ...and back out the LEFT edge, the same way ----------------
        roll.simulateRulerScrubForTest (grid.getX() - 200.0f);

        const double leftScroll = roll.getScrollBeats();
        const double leftHead   = roll.getPlayheadBeatForTest();

        check (utf8 ("指针拖到左边界外：视口跟着左移，播放头钉在左边界"),
               leftScroll < rightScroll - 1.0
                 && std::abs (leftHead - leftScroll) < 0.05,
               juce::String (rightScroll, 2) + utf8 (" -> ") + juce::String (leftScroll, 2)
                 + utf8 ("，播放头 ") + juce::String (leftHead, 2));

        //-- 4. beat 0 is a wall, not a place to keep scrolling past -------
        // The pointer can be dragged off the left side of the screen; the roll
        // starts at 0 and so does the head. "排除 0 点" in Phase 66 was about the
        // follower, and it holds here for the same reason: there is nothing to
        // the left of the first beat.
        roll.simulateRulerScrubForTest (grid.getX() - 5000.0f);

        check (utf8 ("一直拖到 0 点左边：视口停在 0，播放头停在 0"),
               std::abs (roll.getScrollBeats()) < 1.0e-9
                 && std::abs (roll.getPlayheadBeatForTest()) < 1.0e-9,
               juce::String (roll.getScrollBeats(), 2) + utf8 (" / ")
                 + juce::String (roll.getPlayheadBeatForTest(), 2));
    }

    //-- Phase 67: while the button is down, the drag owns the window ----------
    {
        LayerStack t67b;
        RackModel  r67b;
        PianoRollComponent roll (t67b, r67b);
        roll.setSize (1200, 520);
        t67b.addLayer (utf8 ("P67 所有权"), palette::layer1);
        roll.setPlayheadBeat (0.0);

        // The transport keeps running during a scrub — a locate seeks, it does
        // not stop. So the follower is still being asked to move the window
        // while the mouse is asking for something else. This fixture is that
        // collision: a locate at a beat far to the right, delivered mid-drag.
        roll.onLocateRequested = [&roll] (double) { roll.setPlayheadBeat (200.0); };
        roll.setTransportActive (true);

        const auto   grid   = roll.getGridBounds();
        const double parked = roll.getScrollBeats();

        roll.simulateRulerScrubForTest (grid.getX() + grid.getWidth() * 0.5f);

        check (utf8 ("拖拽播放头期间，播放跟随不抢视口"),
               std::abs (roll.getScrollBeats() - parked) < 1.0e-9,
               juce::String (parked, 2) + utf8 (" -> ")
                 + juce::String (roll.getScrollBeats(), 2));
    }

    //-- Phase 68: the grid thins out instead of turning into texture ---------
    {
        LayerStack t68;
        RackModel  r68;
        PianoRollComponent roll (t68, r68);
        roll.setSize (1200, 520);
        t68.addLayer (utf8 ("P68 网格"), palette::layer1);

        // At the default 48 px/beat a 1/16 sits 12 px from its neighbour: two
        // hairlines you can still tell apart, and a line you can aim at.
        const double wideStep = roll.visibleGridStep();

        std::vector<double> wideLines;
        roll.collectGridLines (0.0, 8.0, wideLines);       // 33 lines: 0, .25, … 8

        // Zoomed to the floor (12 px/beat) the same 1/16 is 3 px apart — that is
        // shading, not a grid, and snapping to it is aiming at nothing.
        roll.setBeatWidth (PianoRollComponent::minBeatWidth);

        const double tightStep = roll.visibleGridStep();

        std::vector<double> tightLines;
        roll.collectGridLines (0.0, 8.0, tightLines);      // 9 lines: 0, 1, … 8

        check (utf8 ("缩到最小时网格步长升到 1 拍，1/16 的线不再画"),
               std::abs (wideStep - PianoRollComponent::gridDivision) < 1.0e-9
                 && std::abs (tightStep - 1.0) < 1.0e-9
                 && wideLines.size() == 33
                 && tightLines.size() == 9,
               juce::String (wideStep, 2) + utf8 (" / ") + juce::String (tightStep, 2)
                 + utf8 (" 拍，" ) + juce::String ((int) wideLines.size()) + utf8 (" / ")
                 + juce::String ((int) tightLines.size()) + utf8 (" 根线"));

        //-- the snap follows the same step ---------------------------------
        // 2.3 sits between two lines either way; WHICH line is the question. On
        // the 1/16 grid it lands on 2.25 (a line that is no longer drawn at this
        // zoom), on the beat grid on 2.0.
        const double tightSnap  = roll.snapBeatForTest (2.3);
        const double tightSnap2 = roll.snapBeatForTest (5.9);

        check (utf8 ("缩到最小时吸附落在拍线上，不落在看不见的 1/16 上"),
               std::abs (tightSnap - 2.0) < 1.0e-9
                 && std::abs (tightSnap2 - 6.0) < 1.0e-9,
               juce::String (tightSnap, 2) + utf8 (" / ") + juce::String (tightSnap2, 2));

        //-- every snap answer has to BE one of the drawn lines --------------
        // The two functions are written against the same step, but "written
        // against" is not "cannot drift", so the fixture checks the property
        // instead of the implementation: whatever a drag asks for, the answer is
        // a line that exists in the list the painter walks.
        int offGrid = 0;

        for (double wanted = 0.1; wanted < 7.9; wanted += 0.37)
        {
            const double snapped = roll.snapBeatForTest (wanted);

            bool onALine = false;

            for (double line : tightLines)
                if (std::abs (line - snapped) < 1.0e-9)
                    onALine = true;

            if (! onALine)
                ++offGrid;
        }

        check (utf8 ("吸附结果永远是画出来的那根线"),
               offGrid == 0, juce::String (offGrid) + utf8 (" 次落在空处"));

        //-- bars stay bars after the step climbs ---------------------------
        // The old painter asked `step % 16 == 0` for a bar line. That is true
        // only while the step is 1/16: at one beat per step, index 4 is beat 4,
        // which is a bar — and `4 % 16` says "plain beat". The whole grid would
        // then come out flat, with no heavy lines to navigate by.
        const float alphaBar  = PianoRollComponent::gridLineAlphaForTest (4.0);
        const float alphaBeat = PianoRollComponent::gridLineAlphaForTest (3.0);
        const float alphaSub  = PianoRollComponent::gridLineAlphaForTest (3.25);

        check (utf8 ("步长升到一拍后，第 4 拍仍画成小节线"),
               std::abs (alphaBar - 0.16f) < 1.0e-6f
                 && std::abs (alphaBeat - 0.075f) < 1.0e-6f
                 && std::abs (alphaSub - 0.030f) < 1.0e-6f,
               juce::String (alphaBar, 3) + utf8 (" / ") + juce::String (alphaBeat, 3)
                 + utf8 (" / ") + juce::String (alphaSub, 3));

        //-- and zooming back in restores the fine grid ---------------------
        roll.setBeatWidth (PianoRollComponent::defaultBeatWidth);

        std::vector<double> backLines;
        roll.collectGridLines (0.0, 8.0, backLines);

        check (utf8 ("放大回来后 1/16 的线回来了，吸附也回到 1/16"),
               std::abs (roll.visibleGridStep() - PianoRollComponent::gridDivision) < 1.0e-9
                 && backLines.size() == 33
                 && std::abs (roll.snapBeatForTest (2.3) - 2.25) < 1.0e-9,
               juce::String (roll.visibleGridStep(), 2) + utf8 (" 拍，")
                 + juce::String ((int) backLines.size()) + utf8 (" 根线，吸附到 ")
                 + juce::String (roll.snapBeatForTest (2.3), 2));
    }

    //-- Phase 69: the grid subdivides on the way in, down to 1/64 beat --------
    {
        LayerStack t69;
        RackModel  r69;
        PianoRollComponent roll (t69, r69);
        roll.setSize (1200, 520);

        t69.addLayer (utf8 ("P69 细网格"), palette::layer1);

        // The ceiling is where a 1/64 beat is 12 px wide — the same 12 px a whole
        // beat gets at the floor. Anything lower and the finest rung could never
        // appear, which is the point of raising it from 180.
        roll.setBeatWidth (5000.0f);                 // asks for far more than allowed

        const float  ceiling  = roll.getBeatWidth();
        const double fineStep = roll.visibleGridStep();

        std::vector<double> fineLines;
        roll.collectGridLines (0.0, 1.0, fineLines);        // 65 lines: 0, 1/64 … 1

        check (utf8 ("放大到上限时网格细分到 1/64 拍"),
               std::abs (ceiling - PianoRollComponent::maxBeatWidth) < 0.01f
                 && std::abs (fineStep - PianoRollComponent::finestGridStep) < 1.0e-9,
               juce::String (ceiling, 1) + utf8 (" px/拍，步长 ")
                 + juce::String (fineStep, 5) + utf8 (" 拍"));

        check (utf8 ("1/64 拍档下 0…1 拍之间画 65 根线"),
               (int) fineLines.size() == 65,
               juce::String ((int) fineLines.size()) + utf8 (" 根线"));

        // Snap follows the same step. 0.30 is 19.2 sixty-fourths → line 19
        // (0.296875); on the 1/16-beat grid the same question answers 0.3125, so
        // this one number tells the two rungs apart. (0.31 would not: both grids
        // round it to 0.3125, which is how the first version of this case failed
        // to bite.) 0.5 is line 32 exactly either way.
        check (utf8 ("放大到 1/64 时吸附落在 1/64 的线上"),
               std::abs (roll.snapBeatForTest (0.30) - 0.296875) < 1.0e-9
                 && std::abs (roll.snapBeatForTest (0.5) - 0.5) < 1.0e-9,
               juce::String (roll.snapBeatForTest (0.30), 5) + utf8 (" / ")
                 + juce::String (roll.snapBeatForTest (0.5), 5));

        // A note can now BE that short. Through `applyNoteResize`, i.e. the path
        // a tail drag takes — asserting the constant alone would only test the
        // floor twice.
        auto* layer69 = t69.getLayer (0);

        NoteEvent seed;
        seed.pitch       = 60;
        seed.startBeat   = 0.0;
        seed.lengthBeats = 1.0;

        const int note69 = layer69->addNote (seed);

        roll.shrinkNoteTailForTest (note69, 0.015625);

        const auto* shrunk = layer69->findNote (note69);

        check (utf8 ("放大到 1/64 时音符尾巴能缩到 1/64 拍长"),
               shrunk != nullptr && std::abs (shrunk->lengthBeats - 0.015625) < 1.0e-9,
               juce::String (shrunk != nullptr ? shrunk->lengthBeats : -1.0, 5)
                 + utf8 (" 拍"));

        // ...and the rung below the ceiling is still the 1/16 beat, not a jump
        // straight from 1/64 to the 1/16 note.
        roll.setBeatWidth (150.0f);

        std::vector<double> midLines;
        roll.collectGridLines (0.0, 1.0, midLines);         // 17 lines: 0, 1/16 … 1

        check (utf8 ("150 px/拍时网格停在 1/16 拍档"),
               std::abs (roll.visibleGridStep() - 0.0625) < 1.0e-9
                 && (int) midLines.size() == 17,
               juce::String (roll.visibleGridStep(), 5) + utf8 (" 拍，")
                 + juce::String ((int) midLines.size()) + utf8 (" 根线"));

        // Four levels on screen at once must still read as four: the 1/64 line
        // gets its own dimmer alpha instead of borrowing the 1/16's.
        const float aBar  = PianoRollComponent::gridLineAlphaForTest (4.0);
        const float aBeat = PianoRollComponent::gridLineAlphaForTest (1.0);
        const float a16   = PianoRollComponent::gridLineAlphaForTest (0.25);
        const float a64   = PianoRollComponent::gridLineAlphaForTest (0.015625);

        check (utf8 ("1/64 的线比 1/16 的线更暗，四级层次都还在"),
               std::abs (aBar  - 0.16f)  < 1.0e-6f
                 && std::abs (aBeat - 0.075f) < 1.0e-6f
                 && std::abs (a16   - 0.030f) < 1.0e-6f
                 && std::abs (a64   - 0.018f) < 1.0e-6f,
               juce::String (aBar, 3) + utf8 (" / ") + juce::String (aBeat, 3)
                 + utf8 (" / ") + juce::String (a16, 3) + utf8 (" / ")
                 + juce::String (a64, 3));
    }

    //-- Phase 70: a round trip keeps what the user actually set --------------
    [&]
    {
        // P0-1: which lane the layer's header points at. Written as
        // `autotarget`, because the ORDER of the <Automation> elements is not
        // a rule — reading it that way made pan win every single time.
        LayerStack s1;
        RackModel  r1;
        MixerModel m1;

        s1.addLayer (utf8 ("往返"), palette::layer1);

        auto* layer = s1.getLayer (0);
        layer->setAutomationTarget (LayerAutomationTarget::volume);
        layer->setAutomationExpanded (true);
        layer->automationFor (LayerAutomationTarget::volume).addPoint (1.0, 0.5f, 4.0);
        layer->automationFor (LayerAutomationTarget::pan).addPoint (1.0, -0.5f, 4.0);

        // P0-2: a clip at the smallest length P69 can draw. 0.015625 is 1/64
        // beat; the model used to round it up to 0.25 on the way in.
        SampleEvent clip;
        clip.startBeat   = 0.0;
        clip.lengthBeats = 0.015625;
        clip.pitch       = 60;
        layer->addSample (clip);

        const auto xml = project::toXmlText (s1, r1, 120.0, &m1, nullptr);

        LayerStack s2;
        RackModel  r2;
        MixerModel m2;
        double       bpm2 = 0.0;
        juce::String error;

        const bool loaded = project::fromXmlText (xml, s2, r2, bpm2, error, &m2, nullptr, nullptr);

        const auto* round = s2.getLayer (0);
        const auto& clips = round != nullptr ? round->getSamples()
                                             : s1.getLayer (0)->getSamples();

        check (utf8 ("图层的当前自动化轨往返后仍是 volume，不是文件里最后那条"),
               loaded && round != nullptr
                 && round->getAutomationTarget() == LayerAutomationTarget::volume
                 && round->isAutomationExpanded(),
               round != nullptr
                 ? juce::String (round->getAutomationTarget() == LayerAutomationTarget::pan ? "pan" : "volume")
                 : error);

        check (utf8 ("1/64 拍的采样块往返后仍是 1/64 拍，不被撑成 1/16 音符"),
               loaded && clips.size() == 1
                 && std::abs (clips[0].lengthBeats - 0.015625) < 1.0e-9,
               juce::String (clips.size() == 1 ? clips[0].lengthBeats : -1.0, 5)
                 + utf8 (" 拍"));

        // The same rule has to hold for a file written BEFORE Phase 70, which
        // has no `autotarget` at all: the lane that was open (expanded="1") is
        // the best guess available, and "the last element" is not a guess, it
        // is a coin toss that always lands on pan.
        const auto legacy = project::toXmlText (s1, r1, 120.0, &m1, nullptr)
                                .replace (" autotarget=\"volume\"", "")
                                .replace (" autotarget=\"pan\"", "");

        LayerStack s3;
        RackModel  r3;
        double       bpm3 = 0.0;
        juce::String error3;

        const bool legacyLoaded = project::fromXmlText (legacy, s3, r3, bpm3, error3, nullptr, nullptr, nullptr);

        check (utf8 ("旧工程没有 autotarget 时，按「展开着的那条」认当前轨"),
               legacyLoaded && s3.getLayer (0) != nullptr
                 && s3.getLayer (0)->getAutomationTarget() == LayerAutomationTarget::volume,
               s3.getLayer (0) != nullptr
                 ? juce::String (s3.getLayer (0)->getAutomationTarget() == LayerAutomationTarget::pan ? "pan" : "volume")
                 : error3);

        // P0-3: loading a project REPLACES the routing. A console that was
        // wired 0 → 1 must not keep that route when the file says the master,
        // and a correct route must not be refused because of a leftover.
        LayerStack s4;
        RackModel  r4;
        MixerModel clean;

        s4.addLayer (utf8 ("路由"), palette::layer1);
        const auto routingXml = project::toXmlText (s4, r4, 120.0, &clean, nullptr);

        MixerModel dirty;
        dirty.setChannelCount (16);
        const bool allowed = dirty.setChannelOutput (0, 1);

        LayerStack s5;
        RackModel  r5;
        double       bpm5 = 0.0;
        juce::String error5;

        const bool routingLoaded = project::fromXmlText (routingXml, s5, r5, bpm5, error5, &dirty, nullptr, nullptr);

        check (utf8 ("加载工程后通道 0 回到主输出，上一个会话的路由不残留"),
               allowed && routingLoaded
                 && dirty.getChannelOutput (0) == MixerModel::masterChannel
                 && dirty.getChannelCount() == MixerModel::defaultChannels,
               juce::String (utf8 (allowed ? "设得进" : "设不进")) + utf8 ("，读回 ")
                 + juce::String (dirty.getChannelOutput (0)));
    } ();

    //-- Phase 70: the interaction fixes that needed a geometry or an index ----
    [&]
    {
        // P2-5: deleting a layer ABOVE the active one has to move the active
        // index with it. Clamping alone left the user editing a different
        // sheet.
        //
        // The active layer has to be one that is NOT last, or clamping and
        // shifting give the same answer and the fixture proves nothing: with
        // four layers and A selected, removing layer 0 must land on 0, while a
        // clamp-only implementation stays on 1 because 1 is still in range.
        LayerStack s;
        s.addLayer (utf8 ("A"), palette::layer1);
        s.addLayer (utf8 ("B"), palette::layer1);
        s.addLayer (utf8 ("C"), palette::layer1);
        s.addLayer (utf8 ("D"), palette::layer1);
        s.setActiveLayerIndex (1);                      // B is active, D is last

        const int before = s.getActiveLayerIndex();
        s.removeLayer (0);

        check (utf8 ("删掉活动层上方的一层，活动层跟着上移一格"),
               before == 1 && s.getActiveLayerIndex() == 0 && s.getNumLayers() == 3,
               juce::String (before) + utf8 (" -> ") + juce::String (s.getActiveLayerIndex())
                 + utf8 ("，剩 ") + juce::String (s.getNumLayers()) + utf8 (" 层"));
    } ();

    [&]
    {
        // P2-4: one layer has no ghosts, so the header eye must not be drawn at
        // all. It used to answer "all visible" no matter what, which turned the
        // toggle into a button with two identical positions.
        LayerStack s;
        RackModel  r;
        LayerPanel panel (s, r);
        panel.setSize (LayerPanel::panelWidth, 400);

        s.addLayer (utf8 ("独层"), palette::layer1);
        const auto single = panel.getVisibilityToggleBounds();

        s.addLayer (utf8 ("第二层"), palette::layer2);
        const auto pair = panel.getVisibilityToggleBounds();

        check (utf8 ("只有一层时标题栏不画全部可见的眼睛，两层时才画"),
               single.isEmpty() && ! pair.isEmpty(),
               juce::String (single.getWidth()) + utf8 (" px / ")
                 + juce::String (pair.getWidth()) + utf8 (" px"));
    } ();

    [&]
    {
        // P2-6: the fader's hit area used to reach 8 px sideways into a 4 px
        // gap, so its last 4 px were the meter's left bar.
        LayerStack s;
        RackModel  r;
        MixerModel m;
        MixerPanel panel (s, r, m);
        panel.setSize (1400, 520);

        bool clear = true;

        for (int ch = 0; ch < m.getChannelCount(); ++ch)
            clear = clear && panel.faderHitClearsMeter (ch);

        check (utf8 ("推子的点击区停在电平表左边，不再吃掉电平表一根柱子"),
               clear && panel.faderHitClearsMeter (MixerModel::masterChannel),
               juce::String (clear ? "clear" : "overlaps"));
    } ();

    [&]
    {
        // P2-8: a selection must not survive a console that shrank under it.
        LayerStack s;
        RackModel  r;
        MixerModel m;
        MixerPanel panel (s, r, m);
        panel.setSize (1400, 520);

        m.setChannelCount (24);
        panel.setSelectedChannel (20);
        const int wide = panel.getSelectedChannel();

        m.setChannelCount (8);                        // mixerChanged() fires
        const int narrow = panel.getSelectedChannel();

        // ...and asking for a strip that does not exist is clamped here, not
        // left for whoever reads the number next.
        panel.setSelectedChannel (100);
        const int bogus = panel.getSelectedChannel();

        check (utf8 ("通道数缩到 8 时选中跟着夹回，不指向不存在的通道"),
               wide == 20 && narrow == 7 && bogus == 7,
               juce::String (wide) + utf8 (" -> ") + juce::String (narrow)
                 + utf8 ("，直接选 100 得 ") + juce::String (bogus));
    } ();

    [&]
    {
        // P2-7: pasting on top of the source pushes the copy clear by ONE GRID
        // STEP — which at 1/64 zoom is a 1/64 beat, not the 1/16 the advance
        // used to be hard-coded to.
        LayerStack s;
        RackModel  r;
        PianoRollComponent roll (s, r);
        roll.setSize (1200, 520);

        s.addLayer (utf8 ("P70 粘贴推进"), palette::layer1);

        auto* layer = s.getLayer (0);

        NoteEvent n;
        n.pitch       = 60;
        n.startBeat   = 0.0;
        n.lengthBeats = 0.015625;

        const int id = layer->addNote (n);

        roll.setBeatWidth (PianoRollComponent::maxBeatWidth);    // 1/64 网格
        roll.setSelection ({ id });
        roll.setPlayheadBeat (0.0);                              // 锚点 = 源位置
        roll.copySelectionToClipboards();

        const bool pasted = roll.pasteNotesFromClipboard();

        double pushed = -1.0;

        for (const auto& note : layer->getNotes())
            if (note.id != id)
                pushed = pushed < 0.0 ? note.startBeat : juce::jmin (pushed, note.startBeat);

        check (utf8 ("1/64 网格上原地粘贴只推进一格 1/64，不跳到 1/16"),
               pasted && std::abs (pushed - 0.015625) < 1.0e-9,
               juce::String (pushed, 5) + utf8 (" 拍"));
    } ();

    //-- Phase 65: two selected groups drag together --------------------------
    {
        LayerStack t65;
        RackModel  r65;
        PianoRollComponent roll (t65, r65);
        roll.setSize (1200, 520);

        t65.addLayer (utf8 ("P65 多组拖动"), palette::layer1);

        auto* layer = t65.getLayer (0);
        const int gA = layer->addGroup (utf8 ("组 A"), groupPalette::group1);
        const int gB = layer->addGroup (utf8 ("组 B"), groupPalette::group2);

        NoteEvent seed;
        seed.pitch       = 60;
        seed.lengthBeats = 1.0;

        seed.startBeat = 0.0;  const int a0 = layer->addNote (seed);
        seed.startBeat = 1.0;  const int a1 = layer->addNote (seed);
        seed.startBeat = 4.0;  const int b0 = layer->addNote (seed);
        seed.startBeat = 5.0;  const int b1 = layer->addNote (seed);

        layer->assignNotesToGroup ({ a0, a1 }, gA, 0);
        layer->setOccurrenceStart (gA, 0, 0.0);
        layer->assignNotesToGroup ({ b0, b1 }, gB, 0);
        layer->setOccurrenceStart (gB, 0, 4.0);

        roll.setSelection ({ a0, a1, b0, b1 });

        // Grab group A's label and drag it two beats. Both groups are selected
        // whole, so this is "carry the selection" — the gesture that used to
        // narrow the selection to A and leave B standing.
        roll.simulateGroupMove (0, gA, 2.0, 0, 0);

        const double originA = layer->occurrenceStart (gA, 0);
        const double originB = layer->occurrenceStart (gB, 0);
        const auto*  movedA  = layer->findNote (a0);
        const auto*  movedB  = layer->findNote (b0);

        check (utf8 ("两个组一起选中时，拖其中一个两个组一起走"),
               movedA != nullptr && movedB != nullptr
                 && std::abs (movedA->startBeat - 2.0) < 1.0e-6
                 && std::abs (movedB->startBeat - 6.0) < 1.0e-6
                 && std::abs (originA - 2.0) < 1.0e-6
                 && std::abs (originB - 6.0) < 1.0e-6,
               juce::String (movedA != nullptr ? movedA->startBeat : -1.0, 2)
                 + utf8 (" / ") + juce::String (movedB != nullptr ? movedB->startBeat : -1.0, 2)
                 + utf8 (" · 起点 ") + juce::String (originA, 2)
                 + utf8 (" / ") + juce::String (originB, 2));

        // ...and a group that is NOT in the selection still narrows to itself,
        // which is what a plain click on a label has to keep doing.
        roll.setSelection ({ a0, a1 });
        roll.simulateGroupMove (0, gB, 1.0, 0, 0);

        const auto* untouchedA = layer->findNote (a0);
        const auto* movedB2    = layer->findNote (b0);

        check (utf8 ("只选中一个组时，拖另一个组不会顺带拖走它"),
               untouchedA != nullptr && movedB2 != nullptr
                 && std::abs (untouchedA->startBeat - 2.0) < 1.0e-6
                 && std::abs (movedB2->startBeat - 7.0) < 1.0e-6,
               juce::String (untouchedA != nullptr ? untouchedA->startBeat : -1.0, 2)
                 + utf8 (" / ") + juce::String (movedB2 != nullptr ? movedB2->startBeat : -1.0, 2));
    }

    //-- Phase 65: the title-bar eye flips every ghost at once ----------------
    {
        LayerStack t65e;
        RackModel  r65e;
        LayerPanel panel (t65e, r65e);
        panel.setSize (LayerPanel::panelWidth, 720);

        t65e.addLayer (utf8 ("L0"), palette::layer1);
        t65e.addLayer (utf8 ("L1"), palette::layer2);
        t65e.addLayer (utf8 ("L2"), palette::layer3);
        t65e.setActiveLayerIndex (2);

        t65e.setLayerVisible (0, false);
        t65e.setLayerVisible (1, false);

        const bool beforeAll = panel.areAllLayersVisible();

        panel.toggleAllLayerVisibility();       // -> 全部可见

        const bool nowAll = panel.areAllLayersVisible()
                              && t65e.getLayer (0) != nullptr && t65e.getLayer (0)->isVisible()
                              && t65e.getLayer (1) != nullptr && t65e.getLayer (1)->isVisible();

        panel.toggleAllLayerVisibility();       // -> 仅当前可见

        const bool nowOnlyActive = ! panel.areAllLayersVisible()
                                     && t65e.getLayer (0) != nullptr && ! t65e.getLayer (0)->isVisible()
                                     && t65e.getLayer (1) != nullptr && ! t65e.getLayer (1)->isVisible()
                                     && t65e.getLayer (2) != nullptr && t65e.getLayer (2)->isVisible();

        check (utf8 ("图层栏眼睛：一次切换全部可见 / 仅当前可见"),
               ! beforeAll && nowAll && nowOnlyActive,
               juce::String (beforeAll ? 1 : 0) + " / " + juce::String (nowAll ? 1 : 0)
                 + " / " + juce::String (nowOnlyActive ? 1 : 0));

        // The button lives in the title bar and must not sit on the title text.
        const auto eyeBox = panel.getVisibilityToggleBounds();

        check (utf8 ("眼睛按钮落在标题栏右上角，不压标题文字"),
               eyeBox.getRight() <= panel.getWidth() && eyeBox.getWidth() > 0
                 && eyeBox.getBottom() <= LayerPanel::headerHeight,
               juce::String (eyeBox.getX()) + "," + juce::String (eyeBox.getY())
                 + " " + juce::String (eyeBox.getWidth()) + "x" + juce::String (eyeBox.getHeight()));
    }

    //-- Phase 63: N objects in, N objects out ---------------------------------
    {
        LayerStack t63;
        RackModel  r63;
        PianoRollComponent roll (t63, r63);
        roll.setSize (1200, 520);

        t63.addLayer (utf8 ("P63 多组"), palette::layer1);

        auto* layer = t63.getLayer (0);
        const int gA = layer->addGroup (utf8 ("组 A"), groupPalette::group1);
        const int gB = layer->addGroup (utf8 ("组 B"), groupPalette::group2);

        NoteEvent seed;
        seed.pitch       = 60;
        seed.lengthBeats = 1.0;

        seed.startBeat = 0.0;  const int a0 = layer->addNote (seed);
        seed.startBeat = 1.0;  const int a1 = layer->addNote (seed);
        seed.startBeat = 4.0;  const int b0 = layer->addNote (seed);
        seed.startBeat = 5.0;  const int b1 = layer->addNote (seed);

        layer->assignNotesToGroup ({ a0, a1 }, gA, 0);
        layer->setOccurrenceStart (gA, 0, 0.0);
        layer->assignNotesToGroup ({ b0, b1 }, gB, 0);
        layer->setOccurrenceStart (gB, 0, 4.0);

        //-- 1. two groups in, two groups out ------------------------------
        // The copy is asked what it collected before the paste can hide the
        // answer: a clipboard that only ever holds one object cannot come back
        // with two, no matter how the paste is written.
        roll.setPlayheadBeat (20.0);
        roll.setSelection ({ a0, a1, b0, b1 });
        roll.copySelectionToClipboards();

        const int objects = roll.getGroupClipboardCount();

        roll.pasteEditClipboard();

        const auto occA = layer->getGroupOccurrences (gA);
        const auto occB = layer->getGroupOccurrences (gB);

        check (utf8 ("两个组一起复制粘贴 = 各自多一处，不是一个组装着两家的内容"),
               objects == 2
                 && layer->getGroups().size() == 2
                 && occA.size() == 2 && occB.size() == 2
                 && layer->countNotesInOccurrence (gA, occA.back()) == 2
                 && layer->countNotesInOccurrence (gB, occB.back()) == 2,
               juce::String (objects) + utf8 (" 个对象；组数 ")
                 + juce::String ((int) layer->getGroups().size())
                 + utf8 ("；A ") + juce::String ((int) occA.size()) + utf8 (" 处，B ")
                 + juce::String ((int) occB.size()) + utf8 (" 处"));

        //-- 1b. both rows light up in the group panel ----------------------
        // The panel mirrors this list. With the whole-selection test it was
        // empty for EVERY group the moment two were selected, so "select two
        // groups" looked like "select nothing" in the panel.
        const int litRows = (int) roll.getSelectedGroups().size();

        check (utf8 ("两组同时选中时，编组栏认得这两个组"),
               litRows == 2, juce::String (litRows) + utf8 (" 个组"));

        //-- 2. one anchor keeps the gap ------------------------------------
        // Two groups four beats apart have to paste four beats apart. Anchoring
        // each object on its own start would stack them on one beat — which is
        // what "a single group holding both" looked like.
        const double startA = occA.size() == 2 ? layer->occurrenceStart (gA, occA.back()) : -1.0;
        const double startB = occB.size() == 2 ? layer->occurrenceStart (gB, occB.back()) : -1.0;

        check (utf8 ("粘贴后两组仍相距 4 拍（20 与 24）"),
               std::abs (startA - 20.0) < 1.0e-9 && std::abs (startB - 24.0) < 1.0e-9,
               juce::String (startA, 2) + utf8 (" / ") + juce::String (startB, 2));

        //-- 3. the SAME group selected in two places -----------------------
        // The other half of the user's report, and the reason the old symptom
        // read as "one group holding two": on screen two brackets of one reused
        // group are "两个组" too, and selecting both used to paste ONE
        // appearance carrying all four members.
        std::vector<int> bothA;

        for (const auto& note : layer->getNotes())
            if (note.groupId == gA)
                bothA.push_back (note.id);

        roll.setPlayheadBeat (40.0);
        roll.setSelection (bothA);
        roll.copySelectionToClipboards();

        const int objectsSame = roll.getGroupClipboardCount();

        roll.pasteEditClipboard();

        const auto occA2 = layer->getGroupOccurrences (gA);

        bool anyCrowded = false;

        for (int occurrence : occA2)
            if (layer->countNotesInOccurrence (gA, occurrence) > 2)
                anyCrowded = true;

        check (utf8 ("同一组的两处一起复制 = 再来两处，没有一处被塞进四个成员"),
               objectsSame == 2 && occA2.size() == 4 && ! anyCrowded,
               juce::String (objectsSame) + utf8 (" 个对象；")
                 + juce::String ((int) occA2.size()) + utf8 (" 处")
                 + (anyCrowded ? utf8 ("，有处被塞满") : juce::String()));

        //-- 4. a loose note copied beside a group stays loose --------------
        // The mixed selection is the one that used to lose the split entirely:
        // either the note joined the group or the group fell apart, depending
        // on which pass ran last.
        seed.startBeat = 7.0;
        const int stray = layer->addNote (seed);

        roll.setPlayheadBeat (80.0);
        roll.setSelection ({ a0, a1, stray });
        roll.copySelectionToClipboards();
        roll.pasteEditClipboard();

        int looseAt87 = 0;

        for (const auto& note : layer->getNotes())
            if (note.groupId < 0 && std::abs (note.startBeat - 87.0) < 1.0e-9)
                ++looseAt87;

        check (utf8 ("组 + 散音符一起复制：组还是组，散音符还是散的"),
               looseAt87 == 1 && layer->getGroupOccurrences (gA).size() == 5,
               juce::String (looseAt87) + utf8 (" 个散音符；A 现在 ")
                 + juce::String ((int) layer->getGroupOccurrences (gA).size()) + utf8 (" 处"));
    }

    //-- Phase 62: copy/paste must never ungroup -------------------------------
    {
        LayerStack t62p;
        RackModel  r62p;
        PianoRollComponent roll (t62p, r62p);
        roll.setSize (1200, 520);

        t62p.addLayer (utf8 ("P62 粘贴"), palette::layer1);

        auto* layer = t62p.getLayer (0);
        const int gid = layer->addGroup (utf8 ("组 B"), groupPalette::group1);

        NoteEvent seed;
        seed.pitch       = 60;
        seed.lengthBeats = 1.0;

        seed.startBeat = 0.0;
        const int c0 = layer->addNote (seed);
        seed.startBeat = 1.0;
        const int c1 = layer->addNote (seed);

        layer->assignNotesToGroup ({ c0, c1 }, gid, 0);
        layer->setOccurrenceStart (gid, 0, 0.0);

        seed.startBeat = 8.0;
        const int d0 = layer->addNote (seed);
        seed.startBeat = 9.0;
        const int d1 = layer->addNote (seed);

        layer->assignNotesToGroup ({ d0, d1 }, gid, 1);
        layer->setOccurrenceStart (gid, 1, 8.0);

        //-- 1. copying ONE appearance of a reused group copies THE GROUP -----
        // The old whole-group test asked for "every member of the group", which
        // a two-place group never satisfies from one place — so the copy fell
        // through to the loose-note path. That is the "有时候" in "粘贴有时候会
        // 取消编组": it depended on how many places the group already had.
        roll.setPlayheadBeat (20.0);
        roll.setSelection ({ c0, c1 });
        roll.copySelectionToClipboards();
        roll.pasteEditClipboard();

        const auto occurrences = layer->getGroupOccurrences (gid);

        check (utf8 ("复制「一处出现」再粘贴 = 这一组的第三处，原处不被塞进新音符"),
               occurrences.size() == 3
                 && layer->countNotesInOccurrence (gid, 0) == 2
                 && layer->countNotesInOccurrence (gid, occurrences.back()) == 2,
               juce::String ((int) occurrences.size()) + utf8 (" 处；第一处 ")
                 + juce::String (layer->countNotesInOccurrence (gid, 0)) + utf8 (" 个成员"));

        //-- 2. a loose copy of a member stays loose -------------------------
        roll.setSelection ({ c0 });
        roll.copySelectionToClipboards();
        roll.setPlayheadBeat (32.0);
        roll.pasteEditClipboard();

        int looseAtPaste = 0;

        for (const auto& note : layer->getNotes())
            if (note.groupId < 0 && std::abs (note.startBeat - 32.0) < 1.0e-9)
                ++looseAtPaste;

        check (utf8 ("复制组里的单个音符再粘贴，落地的是散音符，不会悄悄回到原组"),
               looseAtPaste == 1 && layer->countNotesInOccurrence (gid, 0) == 2,
               juce::String (looseAtPaste) + utf8 (" 个散音符，第一处仍有 ")
                 + juce::String (layer->countNotesInOccurrence (gid, 0)) + utf8 (" 个"));

        //-- 3. deleting a whole appearance deletes only that appearance -----
        roll.setSelection ({ d0, d1 });
        roll.deleteSelectedNotes();

        const auto afterDelete = layer->getGroupOccurrences (gid);

        check (utf8 ("整处删除只删这一处，别处原样留着"),
               std::find (afterDelete.begin(), afterDelete.end(), 1) == afterDelete.end()
                 && layer->countNotesInOccurrence (gid, 0) == 2,
               juce::String ((int) afterDelete.size()) + utf8 (" 处；第一处 ")
                 + juce::String (layer->countNotesInOccurrence (gid, 0)) + utf8 (" 个成员"));
    }

    //-- P79: 改名 -----------------------------------------------------------
    // 三个名字、三处入口、一份数据。要证的不是"能改"，而是改完之后别处说的是
    // 同一个词：采样块头上那行字、钢琴窗组框的标题，以前都是各自抄一份的副本。
    {
        //-- A. 图层名：双击名字就地改 ------------------------------------
        LayerStack t79;
        RackModel  r79;
        LayerPanel sheets (t79, r79);
        sheets.setSize (LayerPanel::panelWidth, 720);

        t79.addLayer (utf8 ("图层 1"), palette::layer1);
        t79.addLayer (utf8 ("图层 2"), palette::layer2);

        sheets.simulateDoubleClickForTest (sheets.getNameBounds (0).getCentre());

        // isVisible() 也算进来：数据和"编辑器在编辑"都可以是对的，而框根本
        // 没被显示出来（父组件 addChildComponent 默认隐藏）——用户看到的是
        // 一个什么都发生了、屏幕上什么都没变的程序。
        check (utf8 ("双击图层名会就地打开改名框，指的是那一行"),
               sheets.getRenamingLayerForTest() == 0
                 && sheets.getNameEditorForTest().isEditing()
                 && sheets.getNameEditorForTest().isVisible(),
               juce::String (sheets.getRenamingLayerForTest()) + utf8 ("（应为 0，框开着）")
                 + utf8 ("，可见 ") + (sheets.getNameEditorForTest().isVisible() ? "1" : "0"));

        sheets.getNameEditorForTest().getEditor().setText (utf8 ("弦乐"), juce::dontSendNotification);
        sheets.getNameEditorForTest().close (true);

        check (utf8 ("回车之后图层名就改了，改名框也收了"),
               t79.getLayer (0) != nullptr
                 && t79.getLayer (0)->getName() == utf8 ("弦乐")
                 && ! sheets.getNameEditorForTest().isEditing()
                 && ! sheets.getNameEditorForTest().isVisible()
                 && sheets.getRenamingLayerForTest() == -1,
               (t79.getLayer (0) != nullptr ? t79.getLayer (0)->getName() : juce::String())
                 + utf8 (" / 框 ") + (sheets.getNameEditorForTest().isEditing() ? utf8 ("还开着")
                                                                              : utf8 ("收着")));

        // 双击的是行体，不是名字：那是"选中这一层"，不是"给它改名"
        const auto belowName = sheets.getNameBounds (0).getBottomLeft() + juce::Point<int> (6, 12);
        sheets.simulateDoubleClickForTest (belowName);

        check (utf8 ("双击名字以外的行体不会开改名框"),
               sheets.getRenamingLayerForTest() == -1,
               juce::String (sheets.getRenamingLayerForTest()) + utf8 ("（应为 -1）"));

        //-- B. 组名：编组栏里改，钢琴窗的组框跟着说 ----------------------
        auto* layerA = t79.getLayer (0);

        const int g79 = layerA->addGroup (utf8 ("组 A"), groupPalette::group1);
        layerA->addNote ({ 101, 60, 0.0, 1.0, 0.9f });
        layerA->assignNotesToGroup ({ 101 }, g79);

        GroupPanel groups (t79, r79);
        groups.setSize (280, 560);

        groups.simulateDoubleClickForTest (groups.getNameBounds (0).getCentre());

        check (utf8 ("双击编组框的名字会就地打开改名框"),
               groups.getRenamingRowForTest() == 0
                 && groups.getNameEditorForTest().isEditing()
                 && groups.getNameEditorForTest().isVisible(),
               juce::String (groups.getRenamingRowForTest()) + utf8 ("（应为 0）"));

        groups.getNameEditorForTest().getEditor().setText (utf8 ("主歌和弦"), juce::dontSendNotification);
        groups.getNameEditorForTest().close (true);

        // 一份数据：编组栏和钢琴窗读的是同一个 NoteGroup::name，不是两份副本
        check (utf8 ("编组栏改完，模型里那个名字就是新的（钢琴窗画的就是它）"),
               layerA->findGroup (g79) != nullptr
                 && layerA->findGroup (g79)->getName() == utf8 ("主歌和弦"),
               layerA->findGroup (g79) != nullptr ? layerA->findGroup (g79)->getName()
                                                  : juce::String (utf8 ("组没了")));

        //-- C. 音源名：面板只管报，宿主负责落地 --------------------------
        RackPanel cards (r79);
        cards.setSize (RackPanel::panelWidth, 720);

        int          reportedSlot = -1;
        juce::String reportedWord;

        cards.onSlotRenamed = [&] (int id, juce::String word)
        {
            reportedSlot = id;
            reportedWord = word;
        };

        const int firstSlot = r79.getSlotIdAt (0);
        cards.simulateDoubleClickForTest (cards.getNameBounds (0).getCentre());

        check (utf8 ("双击音源卡片的名字会就地打开改名框"),
               cards.getRenamingSlotForTest() == 0
                 && cards.getNameEditorForTest().isEditing()
                 && cards.getNameEditorForTest().isVisible(),
               juce::String (cards.getRenamingSlotForTest()) + utf8 ("（应为 0）"));

        cards.getNameEditorForTest().getEditor().setText (utf8 ("垫底"), juce::dontSendNotification);
        cards.getNameEditorForTest().close (true);

        check (utf8 ("音源改名报给宿主的是（槽 id，新名），不是卡片序号"),
               reportedSlot == firstSlot && reportedWord == utf8 ("垫底"),
               juce::String (reportedSlot) + utf8 (":") + reportedWord
                 + utf8 ("（应为 ") + juce::String (firstSlot) + utf8 (":垫底）"));

        // 没有宿主时面板自己落地（测试夹具就是这种情况），别让改名凭空消失
        RackPanel bare (r79);
        bare.setSize (RackPanel::panelWidth, 720);
        bare.simulateDoubleClickForTest (bare.getNameBounds (0).getCentre());
        bare.getNameEditorForTest().getEditor().setText (utf8 ("自己改"), juce::dontSendNotification);
        bare.getNameEditorForTest().close (true);

        check (utf8 ("没接宿主时，面板自己把名字写进机架"),
               r79.getSlotName (firstSlot) == utf8 ("自己改"),
               r79.getSlotName (firstSlot));

        //-- D. 空名一律拒收 ----------------------------------------------
        r79.setSlotName (firstSlot, "   ");
        t79.setLayerName (0, "  ");
        t79.setGroupName (0, g79, "");

        check (utf8 ("空名字被三个入口一起拒收（改不出一行找不到的东西）"),
               r79.getSlotName (firstSlot) == utf8 ("自己改")
                 && t79.getLayer (0)->getName() == utf8 ("弦乐")
                 && layerA->findGroup (g79)->getName() == utf8 ("主歌和弦"),
               r79.getSlotName (firstSlot) + " / " + t79.getLayer (0)->getName()
                 + " / " + layerA->findGroup (g79)->getName());

        //-- E. 插件槽：用户起的名字压过插件的名字 ------------------------
        // P38 的规矩是"插件槽显示插件名"，那是对没被改过名的槽说的。用第二个
        // 槽，别让上一步那个已经改过名的槽把这条测成恒真。
        const int pluginSlotId = r79.getSlotIdAt (1);
        auto* hosted = r79.findSlot (pluginSlotId);
        hosted->pluginId   = 4242;
        hosted->pluginName = "Serum";

        const juce::String beforeName = hosted->getDisplayName();
        r79.setSlotName (pluginSlotId, utf8 ("温暖铺底"));

        check (utf8 ("插件槽被用户改名后显示用户的名字，而不是插件名"),
               beforeName == "Serum" && hosted->getDisplayName() == utf8 ("温暖铺底"),
               beforeName + utf8 (" -> ") + hosted->getDisplayName());

        //-- F. 采样块头上那行字 ------------------------------------------
        PianoRollComponent roll79 (t79, r79);
        roll79.setSize (1200, 620);

        SampleEvent onSampler;
        onSampler.name         = utf8 ("拍子");
        onSampler.path         = "kick.wav";
        onSampler.outputSlotId = firstSlot;
        onSampler.startBeat    = 0.0;
        onSampler.lengthBeats  = 4.0;
        onSampler.pitch        = 60;
        onSampler.laneSpan     = 4;

        // 这一段要的是"块名 != 槽名"，槽名自己定，不借上一步的
        r79.setSlotName (firstSlot, utf8 ("温暖铺底"));

        const int onSamplerId = layerA->addSample (onSampler);

        // 块名和槽名不同才画箭头（P27）：这是"这块放的是别的音源"的提示，
        // 改名之后两边又一致了，箭头就该消失。
        check (utf8 ("块名和音源名不同时，块头写成「块名 → 音源名」"),
               roll79.getSampleLabelForTest (0, onSamplerId)
                 == utf8 ("拍子") + "  " + utf8 ("\xe2\x86\x92") + " " + utf8 ("温暖铺底"),
               roll79.getSampleLabelForTest (0, onSamplerId));

        auto* clipRef = layerA->findSample (onSamplerId);
        clipRef->name = utf8 ("温暖铺底");

        check (utf8 ("两者同名时块头只写一遍（不自己重复自己）"),
               roll79.getSampleLabelForTest (0, onSamplerId) == utf8 ("温暖铺底"),
               roll79.getSampleLabelForTest (0, onSamplerId));

        //-- G. 改名字不许把人从打开的组里踢出去 --------------------------
        // P73 的规矩是 layersChanged() 无条件关掉编辑中的组（撤销会把同一个 id
        // 带回来）。改名不是那种事：用户正站在这个组里给它改名。
        LayerStack t79b;
        RackModel  r79b;
        PianoRollComponent rollB (t79b, r79b);
        rollB.setSize (1200, 620);

        t79b.addLayer (utf8 ("层"), palette::layer1);

        auto* layerB = t79b.getLayer (0);
        const int gB = layerB->addGroup (utf8 ("组"), groupPalette::group1);
        layerB->addNote ({ 201, 60, 0.0, 1.0, 0.9f });
        layerB->assignNotesToGroup ({ 201 }, gB);

        rollB.enterGroupEdit (gB);

        t79b.setGroupName (0, gB, utf8 ("改过的组名"));
        t79b.setLayerName (0, utf8 ("改过的层名"));

        check (utf8 ("给正在编辑的组改名，不会把编辑态关掉"),
               rollB.getEditingGroupId() == gB
                 && layerB->findGroup (gB) != nullptr
                 && layerB->findGroup (gB)->getName() == utf8 ("改过的组名")
                 && layerB->getName() == utf8 ("改过的层名"),
               juce::String (rollB.getEditingGroupId()) + utf8 ("（应为 ")
                 + juce::String (gB) + utf8 ("）"));

        //-- H. 宿主：采样音源改名，块跟着走 ------------------------------
        MainComponent host;
        auto& hostRack  = host.getRack();
        auto& hostStack = host.getLayerStack();

        const int samplerId  = hostRack.addSynth();
        const auto samplerName = hostRack.getSlotName (samplerId);

        SampleEvent follows     = onSampler;
        follows.name            = samplerName;      // 抄来的名字：该跟着走
        follows.outputSlotId    = samplerId;
        follows.pitch           = 72;
        const int followsId     = hostStack.getLayer (0)->addSample (follows);

        SampleEvent namedByHand = follows;
        namedByHand.name        = utf8 ("我的拍子");   // 自己起的名：不该动
        namedByHand.pitch       = 76;
        const int namedId       = hostStack.getLayer (0)->addSample (namedByHand);

        host.renameSynthSlot (samplerId, utf8 ("鼓组"));

        const auto* followed  = hostStack.getLayer (0)->findSample (followsId);
        const auto* untouched = hostStack.getLayer (0)->findSample (namedId);

        check (utf8 ("采样音源改名：抄着它名字的块跟着改，自己起过名的块不动"),
               hostRack.getSlotName (samplerId) == utf8 ("鼓组")
                 && followed != nullptr && followed->name == utf8 ("鼓组")
                 && untouched != nullptr && untouched->name == utf8 ("我的拍子"),
               (followed != nullptr ? followed->name : juce::String())
                 + " / " + (untouched != nullptr ? untouched->name : juce::String()));

        check (utf8 ("块头现在写的就是槽的名字（采样器上那行字跟着变）"),
               host.pianoRollForTest().getSampleLabelForTest (0, followsId) == utf8 ("鼓组"),
               host.pianoRollForTest().getSampleLabelForTest (0, followsId));

        //-- I. 改名进撤销历史 --------------------------------------------
        // 两个历史点：建槽建块之后一个、改名之后一个。只 commit 一次的话，
        // undo 回到的是构造时的空工程，测的就成了"撤销能删掉一切"。
        host.commitHistory();
        host.renameSynthSlot (samplerId, utf8 ("鼓组二"));
        host.commitHistory();
        host.undo();

        check (utf8 ("撤销能把音源改名和块头一起还回去"),
               hostRack.getSlotName (samplerId) == utf8 ("鼓组")
                 && hostStack.getLayer (0)->findSample (followsId) != nullptr
                 && hostStack.getLayer (0)->findSample (followsId)->name == utf8 ("鼓组"),
               hostRack.getSlotName (samplerId) + " / "
                 + (hostStack.getLayer (0)->findSample (followsId) != nullptr
                      ? hostStack.getLayer (0)->findSample (followsId)->name
                      : juce::String (utf8 ("块没了"))));
    }

    //-- P80: 工程速度自动化 -----------------------------------------------
    // 速度一旦会变，"一拍几秒"就不再是一个数：它是 1/tempo 在拍上的积分。
    // 这一段要证三件事：积分对（用独立的中点法验算，不是把实现里的公式再抄
    // 一遍）、播放真按它走、曲线能存回工程文件。
    {
        //-- A. 图本身：平坦与渐变 ----------------------------------------
        const auto flatMap = TempoMap::flat (120.0);

        check (utf8 ("平坦 120 BPM：8 拍正好 4 秒，4 秒正好 8 拍"),
               std::abs (flatMap.secondsAtBeat (8.0) - 4.0) < 1.0e-12
                 && std::abs (flatMap.beatAtSeconds (4.0) - 8.0) < 1.0e-12,
               juce::String (flatMap.secondsAtBeat (8.0), 6) + utf8 (" s / ")
                 + juce::String (flatMap.beatAtSeconds (4.0), 6) + utf8 (" 拍"));

        TempoMap ramp;
        ramp.setPoints ({ { 0.0, 120.0 }, { 16.0, 60.0 } });

        // 中点数够多之后，中点法的误差远超 1e-6 看不出来；用它当"独立证人"
        auto numericSeconds = [] (const TempoMap& map, double toBeat, int steps)
        {
            double sum = 0.0;
            const double h = toBeat / (double) steps;

            for (int i = 0; i < steps; ++i)
                sum += 60.0 / map.bpmAtBeat (((double) i + 0.5) * h) * h;

            return sum;
        };

        const double rampEnd = ramp.secondsAtBeat (16.0);

        check (utf8 ("渐慢图的积分与数值积算一致（16 拍 120->60 约 11.09 秒）"),
               std::abs (rampEnd - numericSeconds (ramp, 16.0, 200000)) < 1.0e-6
                 && std::abs (rampEnd - 11.0903548889) < 1.0e-6,
               juce::String (rampEnd, 6) + utf8 (" s（数值 ")
                 + juce::String (numericSeconds (ramp, 16.0, 200000), 6) + utf8 ("）"));

        // 中点 8 拍处 tempo 是 90：慢下来之后每一拍都比 0.5 秒长
        check (utf8 ("渐慢到中途，8 拍已经比 4 秒长（4.603 秒）"),
               std::abs (ramp.secondsAtBeat (8.0) - 4.6029131592) < 1.0e-6
                 && ramp.secondsAtBeat (8.0) > 4.0,
               juce::String (ramp.secondsAtBeat (8.0), 6) + utf8 (" s"));

        // 往返：秒->拍->秒必须回到原处，否则音符起点会和播放头错开
        double worstRoundTrip = 0.0;

        for (double b : { 0.0, 3.7, 8.0, 12.5, 16.0, 24.0, 40.0 })
            worstRoundTrip = juce::jmax (worstRoundTrip,
                                         std::abs (ramp.beatAtSeconds (ramp.secondsAtBeat (b)) - b));

        check (utf8 ("拍/秒两个方向互为反函数（含曲线两端之外的外推）"),
               worstRoundTrip < 1.0e-9,
               juce::String (worstRoundTrip, 12));

        //-- B. 模型：0..1 的曲线怎么变成 BPM ------------------------------
        TempoModel tm;

        check (utf8 ("刚建好的速度模型：曲线平在 120，自动化是关的"),
               ! tm.isAutomated()
                 && tm.buildMap (120.0, 64.0).isFlat()
                 && std::abs (tm.getAutomation().valueAtBeat (32.0) - 0.5f) < 1.0e-6f,
               juce::String (tm.getAutomation().valueAtBeat (32.0), 3));

        tm.setAutomated (true);

        check (utf8 ("打开自动化但没画：速度还是 120（曲线上那个 0.5 就是 120）"),
               tm.buildMap (120.0, 64.0).isFlat()
                 && std::abs (tm.buildMap (120.0, 64.0).bpmAtBeat (0.0) - 120.0) < 1.0e-9,
               juce::String (tm.buildMap (120.0, 64.0).bpmAtBeat (0.0), 3));

        auto& lane80 = tm.getAutomation();
        lane80.resetFlat (0.0f, 64.0);                 // {0, 0} .. {64, 0}
        lane80.addPoint (8.0, 0.5f, 64.0);             // 插进中间，索引 1
        lane80.movePoint (2, 64.0, 1.0f, 64.0);        // 尾端那个 -> 240（索引 2）

        const auto rampMap = tm.buildMap (120.0, 64.0);

        check (utf8 ("曲线 0/0.5/1 对应 60/120/240 BPM（v=0.5 就是工程默认）"),
               std::abs (rampMap.bpmAtBeat (0.0) - 60.0) < 1.0e-9
                 && std::abs (rampMap.bpmAtBeat (8.0) - 120.0) < 1.0e-9
                 && std::abs (rampMap.bpmAtBeat (64.0) - 240.0) < 1.0e-9,
               juce::String (rampMap.bpmAtBeat (0.0), 1) + " / "
                 + juce::String (rampMap.bpmAtBeat (8.0), 1) + " / "
                 + juce::String (rampMap.bpmAtBeat (64.0), 1));

        //-- C. 发布：音符起点、走带位置、一秒走几拍 -----------------------
        LayerStack t80;
        RackModel  r80;
        r80.addSynth();

        t80.addLayer (utf8 ("层"), palette::layer1);
        t80.getLayer (0)->addNote ({ 801, 60, 8.0, 1.0, 0.9f });

        PlaybackEngine engine80;
        engine80.setSampleRate (48000.0);
        engine80.publishSchedule (t80, r80, 120.0, 0.0, nullptr, &tm);

        {
            auto schedule = engine80.getSchedule();
            const double expected = rampMap.secondsAtBeat (8.0);

            check (utf8 ("音符起奏秒数走速度图，不再乘一个常数"),
                   schedule != nullptr && ! schedule->notes.empty()
                     && std::abs (schedule->notes.front().startSeconds - expected) < 1.0e-9
                     && std::abs (expected - 8.0 * 0.5) > 1.0,
                   juce::String (schedule != nullptr && ! schedule->notes.empty()
                                   ? schedule->notes.front().startSeconds : -1.0, 4)
                     + utf8 (" s（平坦 120 会是 4.0）"));
        }

        engine80.setPositionBeats (8.0);

        check (utf8 ("走带位置按图换算成秒（8 拍 = 图上的秒数）"),
               std::abs (engine80.getPosition() - rampMap.secondsAtBeat (8.0)) < 1.0e-9,
               juce::String (engine80.getPosition(), 4) + utf8 (" s"));

        // 一秒音频走几拍：这条才是"音频真的变快了"，上面两条只是账算得对
        auto beatsInOneSecond = [] (float curveValue)
        {
            TempoModel speed;
            speed.getAutomation().resetFlat (curveValue, 64.0);
            speed.setAutomated (true);

            RackModel  rack81;
            LayerStack stack81;
            rack81.addSynth();
            stack81.addLayer (utf8 ("层"), palette::layer1);

            PlaybackEngine e;
            e.setSampleRate (48000.0);
            e.publishSchedule (stack81, rack81, 120.0, 0.0, nullptr, &speed);
            e.setPositionBeats (0.0);

            juce::AudioBuffer<float> oneSecond (2, 48000);
            e.renderNextBlock (oneSecond, 48000);
            return e.getPositionBeats();
        };

        // 渐快时"一秒走几拍"没有闭式解，只有"秒数对应的那一拍"：这一步量的
        // 是走带推进（按段的积分）与图的积分是不是同一条曲线。
        {
            TempoModel ramp;
            ramp.getAutomation().resetFlat (tempo::valueForBpm (60.0), 64.0);
            ramp.getAutomation().addPoint (32.0, tempo::valueForBpm (240.0), 64.0);
            ramp.getAutomation().movePoint (2, 64.0, tempo::valueForBpm (240.0), 64.0);
            ramp.setAutomated (true);

            RackModel  rackR;
            LayerStack stackR;
            rackR.addSynth();
            stackR.addLayer (utf8 ("层"), palette::layer1);

            PlaybackEngine e;
            e.setSampleRate (48000.0);
            e.publishSchedule (stackR, rackR, 120.0, 0.0, nullptr, &ramp);
            e.setPositionBeats (0.0);

            // 真按 512 的块推：整秒一块会把"段首速度"的误差放大 100 倍，
            // 量的就成了欧拉步长而不是这条规则。
            juce::AudioBuffer<float> block (2, 512);

            for (int done = 0; done < 48000; done += 512)
                e.renderNextBlock (block, juce::jmin (512, 48000 - done));

            const double travelled = e.getPositionBeats();
            const auto   map       = ramp.buildMap (120.0, 64.0);

            // 一秒之后走了多少拍，没有简单公式；但"图上这一拍正是 1.0 秒"是
            // 判据：走带推进出来的位置反算回去必须落回一秒。允许一点点误差，
            // 因为推进是按段用段首速度线性推的（一拍内的二次项）。
            check (utf8 ("渐快时走带推进与图的积分是同一条曲线（一秒 = 图上 1.0 秒）"),
                   juce::isPositiveAndBelow ((int) travelled, 65)
                     && std::abs (map.secondsAtBeat (travelled) - 1.0) < 0.004,
                   juce::String (travelled, 4) + utf8 (" 拍 -> ")
                     + juce::String (map.secondsAtBeat (travelled), 5) + utf8 (" s"));
        }

        check (utf8 ("一秒音频：240 BPM 走 4 拍，60 BPM 走 1 拍（曲线真的驱动了走带）"),
               std::abs (beatsInOneSecond (1.0f) - 4.0) < 1.0e-6
                 && std::abs (beatsInOneSecond (0.0f) - 1.0) < 1.0e-6,
               juce::String (beatsInOneSecond (1.0f), 4) + " / "
                 + juce::String (beatsInOneSecond (0.0f), 4));

        //-- D. 入口：速度框右键加一行，加两次还是一条 ----------------------
        MainComponent host;

        host.showTempoAutomation();
        host.showTempoAutomation();

        check (utf8 ("速度自动化行加两次也只有一条（同一条曲线不重复开行）"),
               host.hasTempoAutomationTrack()
                 && std::count_if (host.getAutomationTracks().begin(),
                                   host.getAutomationTracks().end(),
                                   [] (const AutomationTrack& t)
                                   { return t.owner == AutomationOwner::tempo; }) == 1,
               juce::String (host.getAutomationTracks().size()) + utf8 (" 条轨道"));

        // 窗口要真的能认出这一行：只有轨道列表里有它、而 resolve() 不认这个
        // 所有者时，行会画成一条空线——截图上和"没有这一行"完全一样。
        check (utf8 ("速度行在窗口里解析成“工程速度 / 速度”，不是一条空线"),
               host.getAutomationWindowForTest().getRowNameForTest (0)
                 == utf8 ("工程速度") + " / " + utf8 ("速度"),
               host.getAutomationWindowForTest().getRowNameForTest (0));

        //-- E. 工程文件：曲线存得住，老文件读回默认 -----------------------
        {
            LayerStack t82;
            RackModel  r82;
            t82.addLayer (utf8 ("层"), palette::layer1);

            TempoModel saved;
            saved.getAutomation().resetFlat (0.0f, 64.0);
            saved.getAutomation().addPoint (16.0, 0.75f, 64.0);
            saved.setAutomated (true);

            const juce::String xml = project::toXmlText (t82, r82, 97.5, nullptr, nullptr, &saved);

            LayerStack t83;
            RackModel  r83;
            TempoModel reloaded;
            double bpm83 = 0.0;
            juce::String error83;

            const bool ok83 = project::fromXmlText (xml, t83, r83, bpm83, error83,
                                                    nullptr, nullptr, nullptr, &reloaded);

            check (utf8 ("速度曲线随工程存盘：开关和形状都回来了"),
                   ok83
                     && reloaded.isAutomated()
                     && std::abs (reloaded.getAutomation().valueAtBeat (16.0) - 0.75f) < 1.0e-6f
                     && std::abs (reloaded.getAutomation().valueAtBeat (0.0)) < 1.0e-6f
                     && std::abs (bpm83 - 97.5) < 1.0e-6,
                   error83.isNotEmpty() ? error83
                                        : juce::String (reloaded.getAutomation().valueAtBeat (16.0), 3)
                                            + utf8 (" / 手动 ") + juce::String (bpm83, 1));

            // 老文件（v25 的写法：根本没有 <TempoAutomation>）读进一个已经开了
            // 自动化的会话：必须是"回到默认"，而不是"上一个工程的速度图接着用"。
            const juce::String oldXml = project::toXmlText (t82, r82, 120.0);

            LayerStack t84;
            RackModel  r84;
            double bpm84 = 0.0;
            juce::String error84;

            const bool ok84 = project::fromXmlText (oldXml, t84, r84, bpm84, error84,
                                                    nullptr, nullptr, nullptr, &reloaded);

            check (utf8 ("老工程里没有速度自动化：读回来是默认的平坦 120，不是上一个工程的图"),
                   ok84
                     && ! reloaded.isAutomated()
                     && reloaded.buildMap (120.0, 64.0).isFlat()
                     && std::abs (reloaded.buildMap (120.0, 64.0).bpmAtBeat (32.0) - 120.0) < 1.0e-9,
                   error84.isNotEmpty() ? error84
                                        : juce::String (reloaded.buildMap (120.0, 64.0).bpmAtBeat (32.0), 2));
        }

        //-- F. 撤销：速度图是一个人改的一句话 ------------------------------
        host.showTempoAutomation();
        host.commitHistory();

        const int track80 = host.getAutomationModel().findTrackId (AutomationOwner::tempo, 0, 0);
        juce::ignoreUnused (track80);

        host.getTempoModel().getAutomation().resetFlat (0.0f, 64.0);
        host.getTempoModel().getAutomation().addPoint (32.0, 1.0f, 64.0);
        host.getTempoModel().setAutomated (true);
        host.commitHistory();
        host.undo();

        check (utf8 ("撤销把速度曲线和它的开关一起还回去"),
               ! host.getTempoModel().isAutomated()
                 && std::abs (host.getTempoModel().getAutomation().valueAtBeat (32.0) - 0.5f) < 1.0e-6f,
               juce::String (host.getTempoModel().getAutomation().valueAtBeat (32.0), 3)
                 + (host.getTempoModel().isAutomated() ? utf8 (" 还开着") : utf8 (" 关着")));
    }

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
