#include "ProjectIO.h"

#include "Media/SampleLibrary.h"

namespace atomic
{

// The rack clamps its own send index without including the mixer; this is the
// one place both numbers are visible, so this is where they are kept honest.
static_assert (SynthSlot::maxMixerChannels == MixerModel::numChannels,
               "SynthSlot::maxMixerChannels must match MixerModel::numChannels");

namespace project
{

namespace
{
    /** JUCE writes a PluginDescription with the tag "PLUGIN"; a reader that
        only looks for "Plugin" finds nothing and every VST slot silently comes
        back empty — which looks exactly like "my plugins were uninstalled".
        Accept both spellings, since the exact case is not ours to pick. */
    juce::XmlElement* findPluginElement (juce::XmlElement& parent)
    {
        for (auto* child : parent.getChildIterator())
            if (child->hasTagName ("PLUGIN") || child->hasTagName ("Plugin"))
                return child;

        return nullptr;
    }

    //-- Small conversions -----------------------------------------------------
    juce::String colourToHex (juce::Colour colour)
    {
        return juce::String::toHexString ((juce::int64) (juce::uint32) colour.getARGB());
    }

    juce::Colour colourFromHex (const juce::String& text, juce::Colour fallback)
    {
        if (text.isEmpty())
            return fallback;

        const auto argb = (juce::uint32) text.getHexValue64();

        // A zero alpha would make the layer invisible in the roll, which looks
        // exactly like "my notes disappeared". Treat it as a bad value.
        if ((argb >> 24) == 0)
            return fallback;

        return juce::Colour (argb);
    }

    juce::String waveformToText (Waveform waveform)
    {
        switch (waveform)
        {
            case Waveform::sine:     return "sine";
            case Waveform::saw:      return "saw";
            case Waveform::square:   return "square";
            case Waveform::triangle: return "triangle";
        }

        return "saw";
    }

    Waveform waveformFromText (const juce::String& text)
    {
        if (text == "sine")     return Waveform::sine;
        if (text == "square")   return Waveform::square;
        if (text == "triangle") return Waveform::triangle;

        return Waveform::saw;
    }

    juce::String layerTargetToText (LayerAutomationTarget target)
    {
        return target == LayerAutomationTarget::pan ? "pan" : "volume";
    }

    LayerAutomationTarget layerTargetFromText (const juce::String& text)
    {
        return text == "pan" ? LayerAutomationTarget::pan : LayerAutomationTarget::volume;
    }

    juce::String slotTargetToText (SlotAutomationTarget target)
    {
        return target == SlotAutomationTarget::level  ? "level"
             : target == SlotAutomationTarget::plugin ? "plugin"
             : target == SlotAutomationTarget::pan    ? "pan"
             : target == SlotAutomationTarget::mute   ? "mute"
             : target == SlotAutomationTarget::solo   ? "solo"
                                                      : "filter";
    }

    SlotAutomationTarget slotTargetFromText (const juce::String& text)
    {
        return text == "level"  ? SlotAutomationTarget::level
             : text == "plugin" ? SlotAutomationTarget::plugin
             : text == "pan"    ? SlotAutomationTarget::pan
             : text == "mute"   ? SlotAutomationTarget::mute
             : text == "solo"   ? SlotAutomationTarget::solo
                                : SlotAutomationTarget::filter;
    }

    /** Floats are written with enough digits to survive a round trip through
        the text file, and with a dot regardless of the machine's locale. */
    juce::String number (double value, int decimals = 6)
    {
        return juce::String (value, decimals);
    }

    //-- Writing ---------------------------------------------------------------
    void writeLane (juce::XmlElement& parent, const AutomationLane& lane,
                    const juce::String& targetText, bool expanded)
    {
        auto* xml = parent.createNewChildElement ("Automation");
        xml->setAttribute ("enabled", (int) lane.isEnabled());
        xml->setAttribute ("target", targetText);
        xml->setAttribute ("expanded", (int) expanded);

        for (const auto& point : lane.getPoints())
        {
            auto* p = xml->createNewChildElement ("Point");
            p->setAttribute ("beat", number (point.beat, 8));
            p->setAttribute ("value", number (point.value));
        }
    }

    void writeEnvelope (juce::XmlElement& parent, const NoteEnvelope& envelope,
                        const char* tag = "Envelope")
    {
        auto* xml = parent.createNewChildElement (tag);
        xml->setAttribute ("enabled", (int) envelope.isEnabled());

        for (const auto& point : envelope.getPoints())
        {
            auto* p = xml->createNewChildElement ("Point");
            p->setAttribute ("time", number (point.time));
            p->setAttribute ("value", number (point.value));
        }
    }

    /** Empty slots are not written at all: a file with 64 empty <Insert> tags
        is noise, and "missing" already means "empty". */
    void writeInserts (juce::XmlElement& parent, const std::array<fx::Settings, fx::ChainState::maxSlots>& inserts)
    {
        for (int slot = 0; slot < (int) inserts.size(); ++slot)
        {
            const auto& insert = inserts[(size_t) slot];

            if (insert.type == fx::Type::none)
                continue;

            auto* xml = parent.createNewChildElement ("Insert");
            xml->setAttribute ("slot", slot);
            xml->setAttribute ("type", (int) insert.type);
            xml->setAttribute ("p1", number (insert.params[(size_t) 0]));
            xml->setAttribute ("p2", number (insert.params[(size_t) 1]));
            xml->setAttribute ("enabled", (int) insert.enabled);

            // Phase 47 (v20): all knobs, comma-separated. p1/p2 stay written
            // for one-version tolerance; the full string is authoritative.
            {
                juce::String params;

                for (int i = 0; i < fx::maxParams; ++i)
                    params << (i == 0 ? "" : ",") << number (insert.params[(size_t) i]);

                xml->setAttribute ("params", params);
            }

            if (insert.plugin != nullptr)
                if (auto descriptionXml = insert.plugin->createXml())
                    xml->addChildElement (descriptionXml.release());
        }
    }

    void readInserts (const juce::XmlElement& parent,
                      std::array<fx::Settings, fx::ChainState::maxSlots>& inserts,
                      PluginHost* plugins)
    {
        for (auto* child = parent.getFirstChildElement(); child != nullptr; child = child->getNextElement())
        {
            if (! child->hasTagName ("Insert"))
                continue;

            const int slot = juce::jlimit (0, (int) inserts.size() - 1,
                                           child->getIntAttribute ("slot", 0));

            auto& insert = inserts[(size_t) slot];
            insert.type    = fx::typeFromInt (child->getIntAttribute ("type", 0));
            insert.enabled = child->getBoolAttribute ("enabled", true);

            // v20 writes every knob as a comma string; v19 and older carried
            // only the first two, which keep their historic meanings.
            insert.params.fill (0.5f);
            insert.params[(size_t) 1] = 0.45f;   // delay feedback's historic default

            if (child->hasAttribute ("params"))
            {
                const auto tokens = juce::StringArray::fromTokens (
                                        child->getStringAttribute ("params"), ",", "");

                for (int i = 0; i < juce::jmin ((int) tokens.size(), fx::maxParams); ++i)
                    insert.params[(size_t) i] = juce::jlimit (0.0f, 1.0f,
                                                              (float) tokens[(size_t) i].getDoubleValue());
            }
            else
            {
                insert.params[(size_t) 0] = juce::jlimit (0.0f, 1.0f,
                                                          (float) child->getDoubleAttribute ("p1", 0.5));
                insert.params[(size_t) 1] = juce::jlimit (0.0f, 1.0f,
                                                          (float) child->getDoubleAttribute ("p2", 0.45));
            }

            if (auto* pluginXml = findPluginElement (*child))
            {
                juce::PluginDescription description;

                if (description.loadFromXml (*pluginXml))
                {
                    insert.type       = fx::Type::vst;
                    insert.pluginName = description.name;
                    insert.plugin     = std::make_shared<const juce::PluginDescription> (description);

                    // A missing plugin leaves pluginId at 0: the insert keeps
                    // its name, prints "未载入", and passes audio untouched.
                    if (plugins != nullptr)
                        insert.pluginId = plugins->load (description);
                }
            }
        }
    }

    //-- Reading ---------------------------------------------------------------
    void readLane (const juce::XmlElement& xml, AutomationLane& lane,
                   bool& enabled, juce::String& targetText, bool& expanded)
    {
        std::vector<AutomationPoint> points;

        for (auto* child = xml.getFirstChildElement(); child != nullptr; child = child->getNextElement())
        {
            if (! child->hasTagName ("Point"))
                continue;

            points.push_back ({ child->getDoubleAttribute ("beat", 0.0),
                                (float) child->getDoubleAttribute ("value", 1.0) });
        }

        lane.loadPoints (std::move (points));
        enabled  = xml.getBoolAttribute ("enabled", false);
        targetText = xml.getStringAttribute ("target");
        expanded = xml.getBoolAttribute ("expanded", false);
    }

    void readEnvelope (const juce::XmlElement& xml, NoteEnvelope& envelope)
    {
        std::vector<EnvelopePoint> points;

        for (auto* child = xml.getFirstChildElement(); child != nullptr; child = child->getNextElement())
        {
            if (! child->hasTagName ("Point"))
                continue;

            points.push_back ({ (float) child->getDoubleAttribute ("time", 0.0),
                                (float) child->getDoubleAttribute ("value", 1.0) });
        }

        envelope.loadPoints (std::move (points));
        envelope.setEnabled (xml.getBoolAttribute ("enabled", true));
    }

    /** Phase 49: the v14-v20 fade pair, rebuilt as the curve that replaced it.

        The old audio multiplied two linear ramps measured in beats inwards
        from each end; on a normalised curve that is exactly the shape below.
        Migration, not preservation: the model no longer has the fields, and
        the user's next save writes the curve. No fades at all leaves the idle
        curve alone — disabled, flat, unity — which is what "no fade" meant. */
    void migrateFadesToCurve (const juce::XmlElement& sampleXml, NoteEnvelope& curve)
    {
        const double fadeIn  = juce::jmax (0.0, sampleXml.getDoubleAttribute ("fadein", 0.0));
        const double fadeOut = juce::jmax (0.0, sampleXml.getDoubleAttribute ("fadeout", 0.0));

        if (fadeIn <= 0.0 && fadeOut <= 0.0)
            return;

        std::vector<EnvelopePoint> points;

        if (fadeIn > 0.0)
        {
            points.push_back ({ 0.0f, 0.0f });
            points.push_back ({ 1.0f, 1.0f });   // time clamped below to in/len
        }

        if (fadeOut > 0.0)
        {
            points.push_back ({ 0.0f, 1.0f });   // time clamped below to 1-out/len
            points.push_back ({ 1.0f, 0.0f });
        }

        // The ramp lengths are normalised against the block as saved; putting
        // them through loadPoints means the pinning and sorting rules are the
        // editor's own, so a migrated curve is indistinguishable from a drawn
        // one.
        const double lengthBeats = juce::jmax (1.0e-6, sampleXml.getDoubleAttribute ("length", 4.0));

        if (fadeIn > 0.0)
        {
            points[(size_t) 1].time = (float) juce::jmin (1.0, fadeIn / lengthBeats);

            if (fadeOut > 0.0)
                points[(size_t) 2].time = (float) juce::jmax (0.0, 1.0 - fadeOut / lengthBeats);
        }
        else
        {
            points[(size_t) 0].time = (float) juce::jmax (0.0, 1.0 - fadeOut / lengthBeats);
        }

        curve.loadPoints (std::move (points));
        curve.setEnabled (true);
    }
}

//==============================================================================
juce::String toXmlText (const LayerStack& stack, const RackModel& rack, double bpm,
                        const MixerModel* mixer, const AutomationModel* automation)
{
    juce::XmlElement root ("AtomicDAW");
    root.setAttribute ("version", formatVersion);
    root.setAttribute ("tempo", number (bpm, 4));
    root.setAttribute ("timelineBeats", number (timeline::defaultLengthBeats, 2));

    //-- Rack ----------------------------------------------------------------
    {
        auto* rackXml = root.createNewChildElement ("Rack");

        for (int i = 0; i < rack.getNumSlots(); ++i)
        {
            const auto* slot = rack.getSlot (i);

            if (slot == nullptr)
                continue;

            auto* xml = rackXml->createNewChildElement ("Synth");
            xml->setAttribute ("id", slot->id);
            xml->setAttribute ("name", slot->name);
            xml->setAttribute ("colour", colourToHex (slot->colour));
            xml->setAttribute ("waveform", waveformToText (slot->waveform));
            xml->setAttribute ("levelDb", number (slot->levelDb));
            xml->setAttribute ("pan", number (slot->pan));
            xml->setAttribute ("muted", (int) slot->muted);
            xml->setAttribute ("soloed", (int) slot->soloed);

            // Phase 17: only the folded state is written when it is set, so a
            // v6 file (which has no such attribute) reads back unfolded.
            if (slot->collapsed)
                xml->setAttribute ("collapsed", 1);

            // P79: "the user typed this name" — written only when true, so
            // every file made before this existed reads back with the flag off
            // and behaves exactly as it did (built-ins answer with `name`,
            // hosted slots with the plugin's). Deliberately NOT a version
            // bump: an optional attribute an older build ignores costs the old
            // build nothing but a rename it would not have shown anyway.
            if (slot->userName)
                xml->setAttribute ("userName", 1);

            // Phase 11. Only written when the synth actually feeds a console
            // channel; a missing attribute reads back as -1, which is "direct to
            // master" — so a v4 file keeps sounding exactly as it did.
            if (slot->sendsToMixer())
                xml->setAttribute ("mixer", slot->mixerChannel);

            // Phase 27: a sampler slot writes its kind and the file it holds.
            // Everything else about the card (fader, pan, send, curves) is
            // written the same way, because a sampler IS a rack slot — it just
            // has nothing to oscillate.
            if (slot->isSampler())
            {
                xml->setAttribute ("kind", "sampler");

                if (slot->samplePath.isNotEmpty())
                    xml->setAttribute ("sample", slot->samplePath);
            }

            if (slot->plugin != nullptr)
                if (auto descriptionXml = slot->plugin->createXml())
                    xml->addChildElement (descriptionXml.release());

            // Phase 42: the third lane is written for every slot, like the
            // other two — an empty curve costs five numbers and means "no
            // plugin parameter chosen", which reads back as exactly that.
            if (slot->drivesPluginParam())
                xml->setAttribute ("pluginparam", slot->getPluginParam());

            // Which lane the card's header is pointed at, said out loud. It used
            // to be inferred from the ORDER of the <Automation> elements (the
            // last one won), which is not a rule, it is an accident — and a
            // third lane would have turned that accident into "always plugin".
            xml->setAttribute ("autotarget", slotTargetToText (slot->automationTarget));

            for (auto target : { SlotAutomationTarget::filter, SlotAutomationTarget::level,
                                 SlotAutomationTarget::plugin, SlotAutomationTarget::pan,
                                 SlotAutomationTarget::mute, SlotAutomationTarget::solo })
                writeLane (*xml, slot->automationFor (target),
                           slotTargetToText (target),
                           target == slot->automationTarget && slot->automationExpanded);
        }
    }

    //-- Mixer ---------------------------------------------------------------
    if (mixer != nullptr)
    {
        auto* mixXml = root.createNewChildElement ("Mixer");
        mixXml->setAttribute ("masterLevelDb", number (mixer->getMasterLevelDb()));
        mixXml->setAttribute ("masterPan", number (mixer->getMasterPan()));
        mixXml->setAttribute ("masterMuted", (int) mixer->isMasterMuted());

        // v13 (Phase 26): the master strip listens too, and "how it is being
        // listened to" is part of the session — reopening a project should not
        // silently un-fold a mono check the user was working under.
        if (mixer->isMasterMono())
            mixXml->setAttribute ("masterMono", 1);
        mixXml->setAttribute ("channels", mixer->getChannelCount());

        // Phase 25: the ceiling is 127 strips, so writing every addressable
        // channel would put 127 empty <Channel> elements in every file. Only the
        // live ones are written — plus any hidden strip that still carries an
        // insert or a route, because shrinking the console and growing it back
        // used to be lossless and that promise is worth keeping.
        const auto carriesState = [mixer] (int channel)
        {
            if (mixer->getChannelOutput (channel) != MixerModel::masterChannel)
                return true;

            for (const auto& insert : mixer->getInserts (channel))
                if (insert.type != fx::Type::none)
                    return true;

            return false;
        };

        const int liveCount = mixer->getChannelCount();
        int highest = liveCount - 1;

        for (int channel = liveCount; channel < MixerModel::numChannels; ++channel)
            if (carriesState (channel))
                highest = channel;

        for (int channel = 0; channel <= highest; ++channel)
        {
            auto* channelXml = mixXml->createNewChildElement ("Channel");
            channelXml->setAttribute ("index", channel);

            // v10 (Phase 23): where this strip's output goes. The sentinel is
            // `masterChannel` (the ceiling, 127) and is also the default, so an
            // older file that has no `out` attribute reads back as a flat
            // console — which is what it was.
            channelXml->setAttribute ("out", mixer->getChannelOutput (channel));

            if (mixer->isChannelMono (channel))
                channelXml->setAttribute ("mono", 1);

            writeInserts (*channelXml, mixer->getInserts (channel));
        }

        auto* masterXml = mixXml->createNewChildElement ("MasterInserts");
        writeInserts (*masterXml, mixer->getInserts (MixerModel::masterChannel));

        // Phase 18: the master bus's curves. They live in the mixer model,
        // so they are written here rather than with the layer / slot lanes.
        // Phase 43: the mute gate joined them; a v18 reader ignores the unknown
        // target and opens the project with the other two intact.
        auto* masterAutoXml = mixXml->createNewChildElement ("MasterAutomation");
        writeLane (*masterAutoXml, mixer->automationFor (0), "level", false);
        writeLane (*masterAutoXml, mixer->automationFor (1), "pan", false);
        writeLane (*masterAutoXml, mixer->automationFor (2), "mute", false);
    }

    //-- Automation window ---------------------------------------------------
    if (automation != nullptr)
    {
        auto* autoXml = root.createNewChildElement ("AutomationWindow");

        for (const auto& track : automation->getTracks())
        {
            auto* trackXml = autoXml->createNewChildElement ("Track");
            trackXml->setAttribute ("id", track.id);
            trackXml->setAttribute ("owner", track.owner == AutomationOwner::layer ? "layer"
                                             : track.owner == AutomationOwner::group ? "group"
                                             : track.owner == AutomationOwner::master ? "master"
                                                                                      : "slot");
            trackXml->setAttribute ("ownerId", track.ownerId);
            trackXml->setAttribute ("target", track.target);
        }
    }

    //-- Layers --------------------------------------------------------------
    {
        auto* layersXml = root.createNewChildElement ("Layers");
        layersXml->setAttribute ("active", stack.getActiveLayerIndex());

        for (int i = 0; i < stack.getNumLayers(); ++i)
        {
            const auto* layer = stack.getLayer (i);

            if (layer == nullptr)
                continue;

            auto* xml = layersXml->createNewChildElement ("Layer");
            xml->setAttribute ("id", layer->getId());
            xml->setAttribute ("name", layer->getName());
            xml->setAttribute ("colour", colourToHex (layer->getColour()));
            xml->setAttribute ("visible", (int) layer->isVisible());
            xml->setAttribute ("output", layer->getOutputSlotId());
            xml->setAttribute ("levelDb", number (layer->getLevelDb()));
            xml->setAttribute ("pan", number (layer->getPan()));
            xml->setAttribute ("muted", (int) layer->isMuted());
            xml->setAttribute ("soloed", (int) layer->isSoloed());

            if (layer->isCollapsed())
                xml->setAttribute ("collapsed", 1);

            // Which lane the layer's header and the automation window point at,
            // said out loud — slot cards have done this since Phase 42 for the
            // same reason. Leaving it to be inferred from the ORDER of the
            // <Automation> elements means the last one read always wins, which
            // is "pan, always", regardless of what the user was editing.
            xml->setAttribute ("autotarget", layerTargetToText (layer->getAutomationTarget()));

            for (auto target : { LayerAutomationTarget::volume, LayerAutomationTarget::pan })
                writeLane (*xml, layer->automationFor (target),
                           layerTargetToText (target),
                           target == layer->getAutomationTarget() && layer->isAutomationExpanded());

            //-- Phase 16: groups, written BEFORE the notes ---------------
            // A group owns its mount and its curve; the notes only carry the
            // id. Reading a note whose group is missing repairs to "ungrouped",
            // so the order in the file is a convenience, not a requirement.
            if (! layer->getGroups().empty())
            {
                auto* groupsXml = xml->createNewChildElement ("Groups");

                for (const auto& group : layer->getGroups())
                {
                    auto* groupXml = groupsXml->createNewChildElement ("Group");
                    groupXml->setAttribute ("id", group.getId());
                    groupXml->setAttribute ("name", group.getName());
                    groupXml->setAttribute ("colour", colourToHex (group.getColour()));

                    // Absent means "follow the layer" — that is what an old file
                    // (which has no groups at all) and a plain group both mean.
                    if (! group.followsLayerOutput())
                        groupXml->setAttribute ("output", group.getOutputSlotId());

                    // Phase 17: the group's own trim. Absent = unity / centre,
                    // which is exactly what a v6 group meant.
                    if (group.getLevelDb() != 0.0f)
                        groupXml->setAttribute ("levelDb", number (group.getLevelDb()));

                    if (group.getPan() != 0.0f)
                        groupXml->setAttribute ("pan", number (group.getPan()));

                    // Phase 35: only when set, so most groups stay byte-identical
                    // and a v16 reader sees a group it understands.
                    if (group.isMuted())
                        groupXml->setAttribute ("mute", 1);

                    if (group.isCollapsed())
                        groupXml->setAttribute ("collapsed", 1);

                    // Phase 24: which plugin parameter the third lane drives.
                    // Written before the lanes so a reader knows what the
                    // `<Automation target="plugin">` below is pointed at.
                    if (group.drivesPluginParam())
                        groupXml->setAttribute ("pluginparam", group.getPluginParam());

                    writeLane (*groupXml, group.getLevelAutomation(), "level", false);

                    // Phase 20: the pan lane, only when it carries something.
                    // A disabled-but-drawn curve is still worth saving (it is
                    // what "automation off, curve kept" means), so enabled is
                    // not the test — having points with a non-flat shape is.
                    if (group.getPanAutomation().isEnabled()
                          || group.getPanAutomation().getPoints().size() > 2)
                        writeLane (*groupXml, group.getPanAutomation(), "pan", false);

                    // Phase 24: the plugin lane. Same rule: save it when it is
                    // live or when it has a shape worth keeping.
                    if (group.getPluginAutomation().isEnabled()
                          || group.getPluginAutomation().getPoints().size() > 2)
                        writeLane (*groupXml, group.getPluginAutomation(), "plugin", false);

                    // Phase 62 (v25): where each appearance sits. Written as
                    // children rather than attributes because the set is open —
                    // a song may repeat a group any number of times, and
                    // "occurrence1start" style attributes would put a ceiling on
                    // it for no reason.
                    //
                    // Only the ORIGIN is stored. The members are already on the
                    // timeline with absolute beats; this number exists so that
                    // deleting the earliest of them cannot move the appearance.
                    for (const auto& entry : group.getOccurrenceStarts())
                    {
                        auto* occXml = groupXml->createNewChildElement ("Occurrence");
                        occXml->setAttribute ("index", entry.first);
                        occXml->setAttribute ("start", number (entry.second, 8));
                    }
                }
            }

            //-- Phase 17: audio clips ------------------------------------
            // Written before the notes so a reader meets the blocks that own
            // the low lanes first; neither depends on the other's order.
            if (! layer->getSamples().empty())
            {
                auto* samplesXml = xml->createNewChildElement ("Samples");

                for (const auto& sample : layer->getSamples())
                {
                    auto* sampleXml = samplesXml->createNewChildElement ("Sample");
                    sampleXml->setAttribute ("id", sample.id);
                    sampleXml->setAttribute ("name", sample.name);
                    sampleXml->setAttribute ("path", sample.path);
                    sampleXml->setAttribute ("start", number (sample.startBeat, 8));
                    sampleXml->setAttribute ("length", number (sample.lengthBeats, 8));
                    sampleXml->setAttribute ("pitch", sample.pitch);
                    sampleXml->setAttribute ("span", sample.laneSpan);

                    if (sample.gainDb != 0.0f)
                        sampleXml->setAttribute ("gainDb", number (sample.gainDb));

                    // Phase 27: only written when set, so a v13 project reads
                    // back with "follow the layer" and "no group" — which is
                    // exactly what it meant.
                    if (sample.outputSlotId >= 0)
                        sampleXml->setAttribute ("slot", sample.outputSlotId);

                    if (sample.groupId >= 0)
                        sampleXml->setAttribute ("group", sample.groupId);

                    // Phase 58 (v24): which appearance of that group. Absent =
                    // 0, the only appearance a pre-58 project could have.
                    if (sample.groupId >= 0 && sample.groupOccurrence != 0)
                        sampleXml->setAttribute ("groupocc", sample.groupOccurrence);

                    // Phase 29: the window. Written only when set, so a v14
                    // project comes back with offset 0 — which is exactly
                    // what it meant.
                    if (sample.sourceOffsetSeconds > 0.0)
                        sampleXml->setAttribute ("offset", number (sample.sourceOffsetSeconds, 8));

                    // Phase 49 (v21): the volume curve, same element a note
                    // carries. Always written when the block has one, so
                    // toggling the curve off keeps its shape on disk.
                    writeEnvelope (*sampleXml, sample.volumeCurve);

                    // Phase 50 (v22): the pan curve gets its own tag. It cannot
                    // share <Envelope> — a reader that took the first child
                    // would read whichever happened to be written first, which
                    // is exactly the "element order is the state" mistake P42
                    // made with automation lanes.
                    writeEnvelope (*sampleXml, sample.panCurve, "PanEnvelope");

                    // Always written: which curve the box was showing is state,
                    // not something to infer from which child exists.
                    sampleXml->setAttribute ("autotarget",
                                             sampleAutomationTargetKey (sample.automationTarget));

                    if (sample.reversed)
                        sampleXml->setAttribute ("reversed", 1);

                    // Phase 31: only written when it is something other than the
                    // window, so most clips stay unchanged on disk and a v15
                    // reader sees exactly what it expects.
                    if (sample.stretchMode != StretchMode::none)
                        sampleXml->setAttribute ("stretch", (int) sample.stretchMode);

                    // Phase 52: the 只变调 range. Absent means the default, so
                    // the attribute only appears once someone moved the control
                    // — and a v22 reader ignores it, which is the point of an
                    // additive attribute.
                    if (sample.pitchRangeSemitones != SampleEvent::defaultPitchRangeSemitones)
                        sampleXml->setAttribute ("pitchrange", sample.pitchRangeSemitones);
                }
            }

            auto* notesXml = xml->createNewChildElement ("Notes");

            for (const auto& note : layer->getNotes())
            {
                auto* noteXml = notesXml->createNewChildElement ("Note");
                noteXml->setAttribute ("id", note.id);
                noteXml->setAttribute ("pitch", note.pitch);
                noteXml->setAttribute ("start", number (note.startBeat, 8));
                noteXml->setAttribute ("length", number (note.lengthBeats, 8));
                noteXml->setAttribute ("velocity", number (note.velocity));

                // Only written when it is actually set: a missing attribute
                // reads back as -1 ("follow the layer"), so old files and new
                // ones agree on what "no override" looks like.
                if (note.outputSlotOverride >= 0)
                    noteXml->setAttribute ("output", note.outputSlotOverride);

                // Phase 8. Only written when it is not the default, so a v3
                // file whose notes are all plain level curves stays readable as
                // if it were v2.
                if (note.modTarget != NoteModTarget::level)
                    noteXml->setAttribute ("mod", noteModTargetKey (note.modTarget));

                // Phase 24: the note's curve drives a plugin parameter instead
                // of `mod`. Absent = the built-in target owns the curve.
                if (note.drivesPluginParam())
                    noteXml->setAttribute ("pluginparam", note.modPluginParam);

                if (note.groupId >= 0)
                    noteXml->setAttribute ("group", note.groupId);

                // Phase 58 (v24): which appearance of that group — see the
                // block above. Absent = 0.
                if (note.groupId >= 0 && note.groupOccurrence != 0)
                    noteXml->setAttribute ("groupocc", note.groupOccurrence);

                writeEnvelope (*noteXml, note.envelope);
            }
        }
    }

    juce::XmlElement::TextFormat format;
    format.lineWrapLength = 200;

    return root.toString (format);
}

//==============================================================================
bool fromXmlText (const juce::String& xmlText,
                  LayerStack& stack, RackModel& rack,
                  double& bpm, juce::String& error,
                  MixerModel* mixer, AutomationModel* automation, PluginHost* plugins)
{
    error.clear();

    auto root = juce::parseXML (xmlText);

    if (root == nullptr || ! root->hasTagName ("AtomicDAW"))
    {
        error = juce::String::fromUTF8 (u8"不是 AtomicDAW 工程文件");
        return false;
    }

    const int version = root->getIntAttribute ("version", 0);

    if (version > formatVersion)
    {
        error = juce::String::fromUTF8 (u8"工程由更新版本的 AtomicDAW 创建（格式 ")
                  + juce::String (version) + juce::String::fromUTF8 (u8"），无法读取");
        return false;
    }

    //-- Parse everything into temporaries ------------------------------------
    std::vector<SynthSlot>        parsedSlots;
    std::vector<std::unique_ptr<Layer>> parsedLayers;
    int    activeIndex = 0;
    double parsedBpm   = 120.0;

    // Mixer and automation list are collected here and applied only after the
    // whole document has parsed, same rule as the layers.
    std::array<std::array<fx::Settings, MixerModel::numInserts>, MixerModel::numChannels + 1> parsedInserts {};
    float  parsedMasterDb    = 0.0f;
    float  parsedMasterPan   = 0.0f;
    bool   parsedMasterMuted = false;
    bool   parsedMasterMono  = false;
    int    parsedChannelCount = MixerModel::defaultChannels;

    // Phase 23: strip-to-strip routing, staged like everything else. Starts at
    // "everyone goes to the master", which is also what a v9 file means.
    std::array<int, MixerModel::numChannels> parsedOutputs {};
    parsedOutputs.fill (MixerModel::masterChannel);

    // Phase 25: mono monitoring, staged with the rest of the console.
    std::array<bool, MixerModel::numChannels> parsedMono {};

    /** v11 and older used 8 as the master sentinel because the ceiling was eight
        strips; from v12 the sentinel is the ceiling itself. An old `out="8"`
        therefore means "master" and must not be read as channel 9. */
    const bool legacyMasterEight = version <= 11;
    std::array<std::vector<AutomationPoint>, MixerModel::numMasterParameters> parsedMasterCurves;
    std::array<bool, MixerModel::numMasterParameters> parsedMasterCurveOn { false, false };
    // readLane() writes into a lane; for the master curves we only want the
    // points, which are staged and applied at commit time like everything else.
    AutomationLane scratchLane;
    std::vector<AutomationTrack> parsedTracks;

    if (auto* rackXml = root->getChildByName ("Rack"))
    {
        for (auto* child = rackXml->getFirstChildElement(); child != nullptr; child = child->getNextElement())
        {
            if (! child->hasTagName ("Synth"))
                continue;

            SynthSlot slot;
            slot.id       = child->getIntAttribute ("id", (int) parsedSlots.size() + 1);
            slot.name     = child->getStringAttribute ("name", "Synth");
            slot.colour   = colourFromHex (child->getStringAttribute ("colour"),
                                           juce::Colour (0xff7c96f5));
            slot.waveform = waveformFromText (child->getStringAttribute ("waveform"));
            slot.levelDb  = juce::jlimit (-48.0f, 6.0f, (float) child->getDoubleAttribute ("levelDb", -6.0));
            slot.pan      = juce::jlimit (-1.0f, 1.0f, (float) child->getDoubleAttribute ("pan", 0.0));
            slot.muted    = child->getBoolAttribute ("muted", false);
            slot.soloed   = child->getBoolAttribute ("soloed", false);
            slot.collapsed = child->getBoolAttribute ("collapsed", false);
            slot.userName  = child->getBoolAttribute ("userName", false);
            slot.mixerChannel = juce::isPositiveAndBelow (child->getIntAttribute ("mixer", -1),
                                                          SynthSlot::maxMixerChannels)
                                  ? child->getIntAttribute ("mixer", -1)
                                  : -1;

            if (auto* pluginXml = findPluginElement (*child))
            {
                juce::PluginDescription description;

                if (description.loadFromXml (*pluginXml))
                {
                    slot.pluginName = description.name;
                    slot.plugin     = std::make_shared<const juce::PluginDescription> (description);

                    if (plugins != nullptr)
                        slot.pluginId = plugins->load (description);
                }
            }

            // Phase 27: a sampler re-decodes its file here, on the message
            // thread, exactly like a clip does. A file that has moved leaves
            // `sampleBuffer` null: the slot still exists, still carries its
            // name and send, and the card says "采样缺失" — an honest hole
            // instead of a project that refuses to open.
            if (child->getStringAttribute ("kind") == "sampler")
            {
                slot.sampler    = true;
                slot.samplePath = child->getStringAttribute ("sample");

                if (slot.samplePath.isNotEmpty())
                    if (auto loaded = loadSampleFile (juce::File (slot.samplePath)); loaded.ok())
                        slot.sampleBuffer = loaded.buffer;
            }

            // Phase 42: absent in a v17 file, which is "no plugin parameter".
            slot.setPluginParam (child->getIntAttribute ("pluginparam", -1));

            // v18 writes the selected lane explicitly. A v17 file has no such
            // attribute, so fall back to the old rule — "the lane that says
            // expanded is the one the header is on" — which is what those files
            // were written under.
            slot.automationTarget = slotTargetFromText (child->getStringAttribute ("autotarget", "filter"));

            bool         enabled = false, expanded = false;
            juce::String targetText;
            bool         sawTarget = child->hasAttribute ("autotarget");

            for (auto* autoXml = child->getFirstChildElement(); autoXml != nullptr; autoXml = autoXml->getNextElement())
            {
                if (! autoXml->hasTagName ("Automation"))
                    continue;

                const auto target = slotTargetFromText (autoXml->getStringAttribute ("target", "filter"));

                readLane (*autoXml, slot.automationFor (target), enabled, targetText, expanded);
                slot.automationFor (target).setEnabled (enabled);
                slot.automationExpanded = slot.automationExpanded || expanded;

                if (! sawTarget && expanded)
                    slot.automationTarget = target;
            }

            parsedSlots.push_back (std::move (slot));
        }
    }

    if (auto* layersXml = root->getChildByName ("Layers"))
    {
        activeIndex = layersXml->getIntAttribute ("active", 0);

        for (auto* child = layersXml->getFirstChildElement(); child != nullptr; child = child->getNextElement())
        {
            if (! child->hasTagName ("Layer"))
                continue;

            auto layer = std::make_unique<Layer> (
                            child->getStringAttribute ("name", "Layer"),
                            colourFromHex (child->getStringAttribute ("colour"), palette::layer1),
                            child->getIntAttribute ("output", 0));

            if (const int restoredId = child->getIntAttribute ("id", 0); restoredId > 0)
                layer->setId (restoredId);

            layer->setVisible (child->getBoolAttribute ("visible", true));
            layer->setLevelDb ((float) child->getDoubleAttribute ("levelDb", -3.0));
            layer->setPan ((float) child->getDoubleAttribute ("pan", 0.0));
            layer->setMuted (child->getBoolAttribute ("muted", false));
            layer->setSoloed (child->getBoolAttribute ("soloed", false));
            layer->setCollapsed (child->getBoolAttribute ("collapsed", false));

            bool         laneEnabled = false, laneExpanded = false;
            juce::String targetText;

            // Files written before Phase 70 carry no `autotarget`, so the only
            // trace of which lane was open is the one lane with `expanded="1"`.
            // Collect it here and decide once, after the loop — setting the
            // target per element is what made "the last one wins" happen.
            LayerAutomationTarget legacyTarget  = LayerAutomationTarget::volume;
            bool                  legacyKnown   = false;

            for (auto* autoXml = child->getFirstChildElement(); autoXml != nullptr; autoXml = autoXml->getNextElement())
            {
                if (! autoXml->hasTagName ("Automation"))
                    continue;

                const auto target = layerTargetFromText (autoXml->getStringAttribute ("target", "volume"));

                readLane (*autoXml, layer->automationFor (target), laneEnabled, targetText, laneExpanded);
                layer->automationFor (target).setEnabled (laneEnabled);
                layer->setAutomationExpanded (layer->isAutomationExpanded() || laneExpanded);

                if (laneExpanded)
                {
                    legacyTarget = target;
                    legacyKnown  = true;
                }
            }

            if (child->hasAttribute ("autotarget"))
                layer->setAutomationTarget (layerTargetFromText (child->getStringAttribute ("autotarget")));
            else if (legacyKnown)
                layer->setAutomationTarget (legacyTarget);

            std::vector<NoteEvent> notes;
            std::vector<NoteGroup> groups;

            if (auto* groupsXml = child->getChildByName ("Groups"))
            {
                for (auto* g = groupsXml->getFirstChildElement(); g != nullptr; g = g->getNextElement())
                {
                    if (! g->hasTagName ("Group"))
                        continue;

                    const int groupId = g->getIntAttribute ("id", 0);

                    if (groupId < 0)
                        continue;

                    NoteGroup group (groupId,
                                     g->getStringAttribute ("name",
                                                            groupPalette::defaultName ((int) groups.size())),
                                     colourFromHex (g->getStringAttribute ("colour"),
                                                    groupPalette::nextGroupColour ((int) groups.size())));

                    group.setOutputSlotId (g->getIntAttribute ("output", -1));
                    group.setLevelDb ((float) g->getDoubleAttribute ("levelDb", 0.0));
                    group.setPan ((float) g->getDoubleAttribute ("pan", 0.0));
                    group.setCollapsed (g->getBoolAttribute ("collapsed", false));
                    group.setPluginParam (g->getIntAttribute ("pluginparam", -1));
                    group.setMuted (g->getBoolAttribute ("mute", false));   // Phase 35

                    bool enabled = false, expanded = false;
                    juce::String groupTargetText;

                    for (auto* autoXml = g->getFirstChildElement(); autoXml != nullptr;
                         autoXml = autoXml->getNextElement())
                    {
                        // Phase 62 (v25): where an appearance sits. A project
                        // written before this has none, and Layer::occurrenceStart
                        // infers each one from its earliest member exactly once.
                        if (autoXml->hasTagName ("Occurrence"))
                        {
                            group.setOccurrenceStart (juce::jmax (0, autoXml->getIntAttribute ("index", 0)),
                                                      autoXml->getDoubleAttribute ("start", 0.0));
                            continue;
                        }

                        if (! autoXml->hasTagName ("Automation"))
                            continue;

                        // Phase 20: a group can carry two lanes now, so the
                        // target attribute decides which one this element is.
                        // v6 files have exactly one, and it never said its
                        // name — "level" is the only thing v6 could have meant.
                        // Phase 24 adds a third, and anything unrecognised
                        // still lands on level rather than being dropped.
                        const auto laneTargetText = autoXml->getStringAttribute ("target");
                        const auto laneTarget = laneTargetText == "pan"    ? GroupAutomationTarget::pan
                                              : laneTargetText == "plugin" ? GroupAutomationTarget::plugin
                                                                           : GroupAutomationTarget::level;

                        auto& lane = group.automationFor (laneTarget);

                        readLane (*autoXml, lane, enabled, groupTargetText, expanded);
                        lane.setEnabled (enabled);
                    }

                    groups.push_back (std::move (group));
                }
            }

            //-- Phase 17: audio clips ------------------------------------
            // Decoded here, on the message thread, once. A clip whose file has
            // moved keeps its rectangle and loads with `audio == nullptr`, which
            // the roll draws as "missing" — the alternative (refusing the whole
            // project) would punish the user for a renamed folder.
            std::vector<SampleEvent> samples;

            if (auto* samplesXml = child->getChildByName ("Samples"))
            {
                for (auto* sm = samplesXml->getFirstChildElement(); sm != nullptr; sm = sm->getNextElement())
                {
                    if (! sm->hasTagName ("Sample"))
                        continue;

                    SampleEvent sample;
                    sample.id          = sm->getIntAttribute ("id", (int) samples.size() + 1);
                    sample.name        = sm->getStringAttribute ("name", "Sample");
                    sample.path        = sm->getStringAttribute ("path");
                    sample.startBeat   = sm->getDoubleAttribute ("start", 0.0);
                    sample.lengthBeats = sm->getDoubleAttribute ("length", 4.0);
                    sample.pitch       = sm->getIntAttribute ("pitch", 60);
                    sample.laneSpan    = sm->getIntAttribute ("span", 4);
                    sample.gainDb      = (float) sm->getDoubleAttribute ("gainDb", 0.0);
                    sample.outputSlotId = sm->getIntAttribute ("slot", -1);
                    sample.groupId      = sm->getIntAttribute ("group", -1);
                    sample.groupOccurrence = juce::jmax (0, sm->getIntAttribute ("groupocc", 0));
                    sample.sourceOffsetSeconds = juce::jmax (0.0, sm->getDoubleAttribute ("offset", 0.0));

                    // Phase 49 (v21): the volume curve. v21 files carry it as
                    // an <Envelope> child; older files carried the two fade
                    // attributes, which migrate onto the same curve here so
                    // the ramps they saved still sound when played.
                    if (auto* envXml = sm->getChildByName ("Envelope"))
                        readEnvelope (*envXml, sample.volumeCurve);
                    else
                        migrateFadesToCurve (*sm, sample.volumeCurve);

                    // Phase 50 (v22). A v21 file has neither child nor
                    // attribute, and both defaults are the honest reading of
                    // "this block was never asked": a flat 0.5 pan curve that is
                    // switched off, and a box that shows the volume curve —
                    // which is the only curve a v21 block had.
                    if (auto* panXml = sm->getChildByName ("PanEnvelope"))
                        readEnvelope (*panXml, sample.panCurve);

                    sample.automationTarget = sampleAutomationTargetFromKey (
                        sm->getStringAttribute ("autotarget", "volume"));
                    sample.reversed = sm->getBoolAttribute ("reversed", false);

                    // Phase 31: clamped, not trusted. A stray 9 in the file must
                    // read back as "the window", not as an enum value nothing
                    // handles — the audio thread switches on this.
                    {
                        const int mode = sm->getIntAttribute ("stretch", 0);
                        sample.stretchMode = (mode >= 0 && mode <= 3) ? (StretchMode) mode
                                                                      : StretchMode::none;
                    }

                    // Phase 52: same rule for the range — a hand-edited 0 or
                    // 900 must land inside what the clamp in Playback::make
                    // promises, and it is read here rather than trusted there
                    // because the UI prints this number.
                    sample.pitchRangeSemitones = juce::jlimit (
                        SampleEvent::minPitchRangeSemitones,
                        SampleEvent::maxPitchRangeSemitones,
                        sm->getIntAttribute ("pitchrange", SampleEvent::defaultPitchRangeSemitones));

                    if (sample.path.isNotEmpty())
                    {
                        const juce::File audioFile (sample.path);

                        if (auto loaded = loadSampleFile (audioFile); loaded.ok())
                        {
                            sample.audio = loaded.buffer;

                            if (sample.name.isEmpty())
                                sample.name = audioFile.getFileName();
                        }
                    }

                    samples.push_back (std::move (sample));
                }
            }

            if (auto* notesXml = child->getChildByName ("Notes"))
            {
                for (auto* n = notesXml->getFirstChildElement(); n != nullptr; n = n->getNextElement())
                {
                    if (! n->hasTagName ("Note"))
                        continue;

                    NoteEvent note;
                    note.id          = n->getIntAttribute ("id", (int) notes.size() + 1);
                    note.pitch       = n->getIntAttribute ("pitch", 60);
                    note.startBeat   = n->getDoubleAttribute ("start", 0.0);
                    note.lengthBeats = n->getDoubleAttribute ("length", 1.0);
                    note.velocity    = (float) n->getDoubleAttribute ("velocity", 0.8);
                    note.outputSlotOverride = n->getIntAttribute ("output", -1);
                    note.groupId  = n->getIntAttribute ("group", -1);
                    note.groupOccurrence = juce::jmax (0, n->getIntAttribute ("groupocc", 0));
                    note.modTarget = noteModTargetFromKey (n->getStringAttribute ("mod", "level"));
                    note.modPluginParam = n->getIntAttribute ("pluginparam", -1);

                    if (auto* envXml = n->getChildByName ("Envelope"))
                        readEnvelope (*envXml, note.envelope);

                    notes.push_back (std::move (note));
                }
            }

            layer->restoreNotes (std::move (notes));
            layer->restoreSamples (std::move (samples));
            layer->restoreGroups (std::move (groups));   // also repairs stale ids

            // Phase 62: every appearance gets a stored origin now, while its
            // members are all still there. A loaded song must not move the first
            // time a group member is deleted.
            layer->materialiseOccurrenceStarts();

            parsedLayers.push_back (std::move (layer));
        }
    }

    if (parsedLayers.empty())
    {
        error = juce::String::fromUTF8 (u8"工程里没有任何图层");
        return false;
    }

    if (auto* mixXml = root->getChildByName ("Mixer"))
    {
        parsedMasterDb    = juce::jlimit (-48.0f, 6.0f, (float) mixXml->getDoubleAttribute ("masterLevelDb", 0.0));
        parsedMasterPan   = juce::jlimit (-1.0f, 1.0f, (float) mixXml->getDoubleAttribute ("masterPan", 0.0));
        parsedMasterMuted = mixXml->getBoolAttribute ("masterMuted", false);
        parsedMasterMono  = mixXml->getBoolAttribute ("masterMono", false);

        for (auto* child = mixXml->getFirstChildElement(); child != nullptr; child = child->getNextElement())
        {
            if (child->hasTagName ("Channel"))
            {
                const int channel = juce::jlimit (0, MixerModel::numChannels - 1,
                                                  child->getIntAttribute ("index", 0));
                readInserts (*child, parsedInserts[(size_t) channel], plugins);

                // Missing `out` = master. Anything out of range is read as the
                // master too: a stale destination must not survive a load just
                // because it was written by a future/edited file.
                const int out = child->getIntAttribute ("out", MixerModel::masterChannel);
                const bool wasMaster = out == MixerModel::masterChannel
                                        || (legacyMasterEight && out == 8);

                parsedOutputs[(size_t) channel] =
                    (wasMaster
                       || (juce::isPositiveAndBelow (out, MixerModel::numChannels) && out != channel))
                      ? (wasMaster ? MixerModel::masterChannel : out)
                      : MixerModel::masterChannel;

                parsedMono[(size_t) channel] = child->getIntAttribute ("mono", 0) != 0;
            }
            else if (child->hasTagName ("MasterInserts"))
            {
                readInserts (*child, parsedInserts[(size_t) MixerModel::masterChannel], plugins);
            }
        }

        parsedChannelCount = juce::jlimit (MixerModel::minChannels, MixerModel::maxChannels,
                                           mixXml->getIntAttribute ("channels",
                                                                    MixerModel::defaultChannels));

        if (auto* masterAutoXml = mixXml->getChildByName ("MasterAutomation"))
        {
            bool enabled = false;
            juce::String targetText;
            bool expanded = false;

            for (auto* child = masterAutoXml->getFirstChildElement(); child != nullptr;
                 child = child->getNextElement())
            {
                if (! child->hasTagName ("Automation"))
                    continue;

                const auto targetName = child->getStringAttribute ("target");
                const int index = targetName == "pan"  ? 1
                                : targetName == "mute" ? 2
                                                       : 0;

                readLane (*child, scratchLane, enabled, targetText, expanded);
                parsedMasterCurves[(size_t) index]   = scratchLane.getPoints();
                parsedMasterCurveOn[(size_t) index]  = enabled;
            }
        }
    }

    if (auto* autoXml = root->getChildByName ("AutomationWindow"))
    {
        for (auto* child = autoXml->getFirstChildElement(); child != nullptr; child = child->getNextElement())
        {
            if (! child->hasTagName ("Track"))
                continue;

            AutomationTrack track;
            track.id      = child->getIntAttribute ("id", 0);

            const juce::String ownerText = child->getStringAttribute ("owner", "layer");
            track.owner   = ownerText == "slot"  ? AutomationOwner::slot
                          : ownerText == "group" ? AutomationOwner::group
                          : ownerText == "master" ? AutomationOwner::master
                                                 : AutomationOwner::layer;

            track.ownerId = child->getIntAttribute ("ownerId", 0);
            track.target  = child->getIntAttribute ("target", 0);

            // Every owner has a positive id except the master bus, which is
            // singular and therefore always id 0.
            if (track.owner == AutomationOwner::master || track.ownerId > 0)
                parsedTracks.push_back (track);
        }
    }

    parsedBpm = juce::jlimit (30.0, 300.0, root->getDoubleAttribute ("tempo", 120.0));

    //-- Commit --------------------------------------------------------------
    rack.restoreSlots (std::move (parsedSlots));
    stack.restoreLayers (std::move (parsedLayers), activeIndex);
    bpm = parsedBpm;

    if (mixer != nullptr)
    {
        for (int channel = 0; channel <= MixerModel::masterChannel; ++channel)
            mixer->getInserts (channel) = parsedInserts[(size_t) channel];

        mixer->setMasterLevelDb (parsedMasterDb);
        mixer->setMasterPan (parsedMasterPan);
        mixer->setMasterMuted (parsedMasterMuted);
        mixer->setMasterMono (parsedMasterMono);

        for (int i = 0; i < MixerModel::numMasterParameters; ++i)
        {
            auto& lane = mixer->automationFor (i);

            lane.loadPoints (std::move (parsedMasterCurves[(size_t) i]));
            lane.setEnabled (parsedMasterCurveOn[(size_t) i]);
        }

        mixer->setChannelCount (parsedChannelCount);

        // After the count, not before: the destination has to be a live strip,
        // so the console has to be its final size first.
        //
        // Phase 70: all of them, unconditionally, and as one replacement. The
        // old loop skipped the master destination — which is the same thing as
        // saying "a strip that fed the master last time keeps feeding whatever
        // it fed last time", and a project that never touched routing inherited
        // a route from a different project. restoreRouting() also clears the
        // map before applying, so `wouldLoop` judges each route against a clean
        // console instead of against the leftovers.
        mixer->restoreRouting (parsedOutputs);

        // Mono is set both ways, not just when true: a load replaces the
        // console, and a strip that was listening in mono before must not keep
        // doing it because the new project says nothing about it.
        for (int channel = 0; channel < parsedChannelCount; ++channel)
            mixer->setChannelMono (channel, parsedMono[(size_t) channel]);

        mixer->notify();
    }

    if (automation != nullptr)
        automation->restoreTracks (std::move (parsedTracks));

    return true;
}

//==============================================================================
bool save (const juce::File& file,
           const LayerStack& stack, const RackModel& rack, double bpm,
           juce::String& error, const MixerModel* mixer, const AutomationModel* automation)
{
    error.clear();

    file.getParentDirectory().createDirectory();

    // replaceWithText with asUnicode = false writes UTF-8 without a BOM, which
    // is exactly what the XML header in the document declares.
    if (! file.replaceWithText (toXmlText (stack, rack, bpm, mixer, automation)))
    {
        error = juce::String::fromUTF8 (u8"无法写入 ") + file.getFullPathName();
        return false;
    }

    return true;
}

bool load (const juce::File& file,
           LayerStack& stack, RackModel& rack,
           double& bpm, juce::String& error,
           MixerModel* mixer, AutomationModel* automation, PluginHost* plugins)
{
    error.clear();

    if (! file.existsAsFile())
    {
        error = juce::String::fromUTF8 (u8"文件不存在：") + file.getFullPathName();
        return false;
    }

    return fromXmlText (file.loadFileAsString(), stack, rack, bpm, error,
                        mixer, automation, plugins);
}

} // namespace project
} // namespace atomic
