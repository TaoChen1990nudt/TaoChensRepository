#include "SettingsWindow.h"

#include "Util/Metrics.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>
#include <vector>

#include "Util/Text.h"

namespace atomic
{

namespace
{
    const juce::Colour kBackground { 0xff101114 };
    const juce::Colour kPanel      { 0xff15161b };
    const juce::Colour kRow        { 0xff191b21 };
    const juce::Colour kOutline    { 0xff2c2e37 };
    const juce::Colour kText       { 0xff9aa0ad };
    const juce::Colour kAccent     { 0xff4fd1c5 };

    juce::Font boldFont (float height)
    {
        return juce::Font (juce::FontOptions{}.withHeight (atomic::ui::sp (height)).withStyle ("Bold"));
    }

    //==========================================================================
    /** The audio tab is JUCE's device selector, unchanged. Hand-rolling one
        would only reproduce a worse version of the same thing. */
    class AudioSettingsPage final : public juce::Component
    {
    public:
        explicit AudioSettingsPage (juce::AudioDeviceManager& devices)
        {
            configureLook();

            selector = std::make_unique<juce::AudioDeviceSelectorComponent> (
                           devices,
                           0, 0,       // no audio inputs: this build only plays
                           1, 2,       // mono or stereo out
                           false,      // MIDI is its own tab
                           false,      // no MIDI output selector
                           true,       // stereo pairs
                           false);     // keep the advanced options visible

            addAndMakeVisible (*selector);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (kPanel);

            g.setColour (kText.withAlpha (0.75f));
            g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.0f))));
            g.drawText (utf8 ("改动立即生效：走带会按新的缓冲大小重新准备，插件也会重新 prepare。"),
                        getLocalBounds().removeFromBottom (atomic::ui::px (26))
                                          .withTrimmedLeft (atomic::ui::px (12)),
                        juce::Justification::centredLeft, true);
        }

        ~AudioSettingsPage() override
        {
            // The look-and-feel is a member, so it dies with this page; clearing
            // the pointer first keeps JUCE from touching a dead object.
            setLookAndFeel (nullptr);
        }

        void resized() override
        {
            selector->setBounds (getLocalBounds().withTrimmedBottom (atomic::ui::px (26))
                                                  .reduced (atomic::ui::px (8)));
        }

    private:
        void configureLook()
        {
            // Owned here, not global: the rest of the app paints itself by hand
            // and does not want JUCE's default widget palette changed.
            juce::LookAndFeel& lf = look;

            setLookAndFeel (&look);

            lf.setColour (juce::ResizableWindow::backgroundColourId, kPanel);
            lf.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff121419));
            lf.setColour (juce::ListBox::textColourId, juce::Colours::white.withAlpha (0.8f));
            lf.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff1c1f27));
            lf.setColour (juce::ComboBox::textColourId, juce::Colours::white.withAlpha (0.85f));
            lf.setColour (juce::ComboBox::outlineColourId, kOutline);
            lf.setColour (juce::Label::textColourId, kText);
            lf.setColour (juce::ToggleButton::textColourId, kText);
            lf.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff22252e));
            lf.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
            lf.setColour (juce::TextButton::textColourOffId, juce::Colours::white.withAlpha (0.8f));
        }

        juce::LookAndFeel_V4 look;
        std::unique_ptr<juce::AudioDeviceSelectorComponent> selector;
    };

    //==========================================================================
    /** MIDI inputs, and an honest monitor.

        The monitor exists because "MIDI 设置" that only lists device names is
        not a setting, it is a claim. Ticking a device opens it, and the panel
        shows the last message so a keyboard that is plugged in but silent can
        be told apart from one that is not there at all. */
    class MidiSettingsPage final : public juce::Component,
                                   private juce::MidiInputCallback,
                                   private juce::Timer
    {
    public:
        MidiSettingsPage()
        {
            rescanButton.setButtonText (utf8 ("重新扫描"));
            rescanButton.onClick = [this] { refreshDevices(); };
            addAndMakeVisible (rescanButton);

            refreshDevices();
            startTimerHz (10);
        }

        ~MidiSettingsPage() override
        {
            stopTimer();

            for (auto& row : rows)
                if (row.input != nullptr)
                {
                    row.input->stop();
                    row.input.reset();
                }
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (kPanel);

            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.setFont (boldFont (12.0f));
            g.drawText (utf8 ("MIDI 输入设备"), atomic::ui::px (14), atomic::ui::px (10),
                        atomic::ui::px (200), atomic::ui::px (18),
                        juce::Justification::centredLeft, false);

            g.setColour (kText.withAlpha (0.7f));
            g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));
            g.drawText (utf8 ("勾选即打开设备"), atomic::ui::px (14), atomic::ui::px (28),
                        atomic::ui::px (200), atomic::ui::px (14),
                        juce::Justification::centredLeft, false);

            if (rows.empty())
            {
                g.setColour (kText.withAlpha (0.6f));
                g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (11.0f))));
                g.drawText (utf8 ("没有检测到 MIDI 输入设备。接上键盘后点“重新扫描”。"),
                            getLocalBounds().withTrimmedTop (atomic::ui::px (56))
                                            .reduced (atomic::ui::px (14), 0)
                                            .removeFromTop (atomic::ui::px (40)),
                            juce::Justification::topLeft, true);
            }

            for (int i = 0; i < (int) rows.size(); ++i)
            {
                const auto bounds = getRowBounds (i);
                const auto& row   = rows[(size_t) i];

                g.setColour (i % 2 == 0 ? kRow : juce::Colour (0xff15171c));
                g.fillRoundedRectangle (bounds.toFloat(), 4.0f);

                // Checkbox.
                const auto box = juce::Rectangle<float> (12.0f, 12.0f)
                                    .withCentre ({ (float) bounds.getX() + 18.0f,
                                                   (float) bounds.getCentreY() });

                g.setColour (row.open ? kAccent : juce::Colours::white.withAlpha (0.18f));
                g.drawRoundedRectangle (box, 3.0f, 1.2f);

                if (row.open)
                    g.fillRoundedRectangle (box.reduced (atomic::ui::pf (3)), 1.5f);

                g.setColour (juce::Colours::white.withAlpha (row.open ? 0.9f : 0.55f));
                g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (11.0f))));
                g.drawText (row.name, bounds.withTrimmedLeft (atomic::ui::px (38)),
                            juce::Justification::centredLeft, true);
            }

            //-- Monitor -----------------------------------------------------
            const auto monitor = getLocalBounds().removeFromBottom (atomic::ui::px (54))
                                                     .reduced (atomic::ui::px (14), atomic::ui::px (8));

            g.setColour (juce::Colour (0xff0d0e12));
            g.fillRoundedRectangle (monitor.toFloat(), 4.0f);

            g.setColour (kText.withAlpha (0.85f));
            g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.0f))));
            g.drawText (getMonitorText(),
                        monitor.reduced (atomic::ui::px (8), 0).withHeight (atomic::ui::px (18)),
                        juce::Justification::centredLeft, true);

            g.setColour (kText.withAlpha (0.45f));
            g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
            g.drawText (utf8 ("MIDI 仅做监视，尚未录入音符：录音要到走带录制阶段才接。"),
                        monitor.reduced (atomic::ui::px (8), 0).removeFromBottom (atomic::ui::px (18)),
                        juce::Justification::centredLeft, true);
        }

        void resized() override
        {
            rescanButton.setBounds (getWidth() - atomic::ui::px (110), atomic::ui::px (10),
                                    atomic::ui::px (96), atomic::ui::px (22));
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            for (int i = 0; i < (int) rows.size(); ++i)
                if (getRowBounds (i).contains (e.getPosition()))
                {
                    setRowOpen (i, ! rows[(size_t) i].open);                    return;
                }
        }

    private:
        struct Row
        {
            juce::String                        name;
            juce::String                        identifier;
            std::unique_ptr<juce::MidiInput>    input;
            bool                                open = false;
        };

        juce::Rectangle<int> getRowBounds (int index) const
        {
            return { 12, 46 + index * 28, getWidth() - 24, 24 };
        }

        void refreshDevices()
        {
            for (auto& row : rows)
                if (row.input != nullptr)
                {
                    row.input->stop();
                    row.input.reset();
                }

            rows.clear();

            for (const auto& device : juce::MidiInput::getAvailableDevices())
            {
                Row row;
                row.name       = device.name;
                row.identifier = device.identifier;
                rows.push_back (std::move (row));
            }

            repaint();
        }

        void setRowOpen (int index, bool shouldOpen)
        {
            if (! juce::isPositiveAndBelow (index, (int) rows.size()))
                return;

            auto& row = rows[(size_t) index];

            if (shouldOpen)
            {
                if (row.input == nullptr)
                {
                    row.input = juce::MidiInput::openDevice (row.identifier, this);

                    if (row.input != nullptr)
                        row.input->start();
                }

                row.open = (row.input != nullptr);
            }
            else
            {
                if (row.input != nullptr)
                {
                    row.input->stop();
                    row.input.reset();
                }

                row.open = false;
            }

            repaint();
        }

        void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& message) override
        {
            ++messageCount;

            if (message.isNoteOn())
            {
                lastType.store (1);
                lastNote.store (message.getNoteNumber());
                lastVelocity.store (juce::roundToInt (message.getFloatVelocity() * 127.0f));
            }
            else if (message.isNoteOff())
            {
                lastType.store (2);
                lastNote.store (message.getNoteNumber());
            }
            else if (message.isController())
            {
                lastType.store (3);
                lastNote.store (message.getControllerNumber());
                lastVelocity.store (message.getControllerValue());
            }
            else
            {
                lastType.store (4);
            }

            lastChannel.store (message.getChannel());
            dirty = true;
        }

        void timerCallback() override
        {
            if (dirty)
            {
                dirty = false;
                repaint();
            }
        }

        juce::String getMonitorText() const
        {
            const int total = messageCount.load();

            if (total <= 0)
                return utf8 ("等待 MIDI 消息…");

            juce::String what;

            switch (lastType.load())
            {
                case 1:  what = "Note On  " + juce::MidiMessage::getMidiNoteName (lastNote.load(), true, true, 3)
                                  + "  vel " + juce::String (lastVelocity.load()); break;
                case 2:  what = "Note Off " + juce::MidiMessage::getMidiNoteName (lastNote.load(), true, true, 3); break;
                case 3:  what = "CC " + juce::String (lastNote.load()) + "  =  " + juce::String (lastVelocity.load()); break;
                default: what = utf8 ("其它消息"); break;
            }

            return what + utf8 ("   ·   通道 ") + juce::String (lastChannel.load())
                     + utf8 ("   ·   共 ") + juce::String (total) + utf8 (" 条");
        }

        std::vector<Row> rows;

        /** The empty state tells the user to press this, so it has to exist. */
        juce::TextButton rescanButton;

        std::atomic<int>   messageCount { 0 };
        std::atomic<int>   lastType     { 0 };
        std::atomic<int>   lastNote     { 0 };
        std::atomic<int>   lastVelocity { 0 };
        std::atomic<int>   lastChannel  { 1 };
        std::atomic<bool>  dirty        { false };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiSettingsPage)
    };

    //==========================================================================
    /** Two lists, because a synth in an insert slot is as useful as a reverb in
        the rack. Double-click puts the plugin in the right place. */
    class PluginManagerPage final : public juce::Component
    {
    public:
        explicit PluginManagerPage (PluginHost& pluginHostToUse)
            : host (pluginHostToUse),
              instruments (*this, true),
              effects (*this, false)
        {
            instrList.setModel (&instruments);
            instrList.setRowHeight (atomic::ui::px (26));
            instrList.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff121419));
            addAndMakeVisible (instrList);

            fxList.setModel (&effects);
            fxList.setRowHeight (atomic::ui::px (26));
            fxList.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff121419));
            addAndMakeVisible (fxList);

            scanButton.setButtonText (utf8 ("扫描 VST3"));
            scanButton.onClick = [this] { scanAsync(); };
            addAndMakeVisible (scanButton);

            rebuild();
        }

        std::function<void (const juce::PluginDescription&)> onAddInstrument;
        std::function<void (const juce::PluginDescription&)> onAddEffect;

        void paint (juce::Graphics& g) override
        {
            g.fillAll (kPanel);

            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.setFont (boldFont (11.5f));
            g.drawText (utf8 ("VST3 乐器") + "   " + juce::String ((int) instruments.entries.size()),
                        14, 8, 300, 18, juce::Justification::centredLeft, false);

            g.drawText (utf8 ("VST3 效果器") + "   " + juce::String ((int) effects.entries.size()),
                        14, getHeight() / 2 + 4, 300, 18,
                        juce::Justification::centredLeft, false);

            g.setColour (kText.withAlpha (0.6f));
            g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
            g.drawText (utf8 ("双击：乐器加到机架，效果器加到混音台当前通道的第一个空槽"),
                        getLocalBounds().removeFromBottom (atomic::ui::px (20))
                                         .withTrimmedLeft (atomic::ui::px (14)),
                        juce::Justification::centredLeft, true);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (atomic::ui::px (14), 0);
            scanButton.setBounds (area.getRight() - atomic::ui::px (110), atomic::ui::px (6),
                                  atomic::ui::px (110), atomic::ui::px (22));

            area.removeFromTop (atomic::ui::px (28));
            area.removeFromBottom (atomic::ui::px (22));

            const int half  = area.getHeight() / 2;
            const int label = atomic::ui::px (22);

            instrList.setBounds (area.removeFromTop (half).withTrimmedTop (label)
                                       .withTrimmedBottom (atomic::ui::px (8)));
            area.removeFromTop (label);
            fxList.setBounds (area);
        }

        /** Fills both lists from the host's library. */
        void rebuild()
        {
            instruments.entries.clear();
            effects.entries.clear();

            for (const auto& entry : host.getLibrary())
            {
                if (entry.isInstrument)
                    instruments.entries.push_back (entry.description);
                else
                    effects.entries.push_back (entry.description);
            }

            instrList.updateContent();
            fxList.updateContent();
            instrList.repaint();
            fxList.repaint();

            scanButton.setButtonText (host.hasScanned() ? utf8 ("重新扫描") : utf8 ("扫描 VST3"));

            repaint();
        }

        void scanAsync()
        {
            scanButton.setEnabled (false);
            scanButton.setButtonText (utf8 ("扫描中…"));

            juce::Component::SafePointer<PluginManagerPage> safe (this);

            // One message-loop turn first, so "扫描中…" is actually on screen
            // before a scan that may take seconds starts blocking the thread.
            juce::MessageManager::callAsync ([safe]
            {
                if (safe == nullptr)
                    return;

                safe->host.scan (true);        // the button means "look again"
                safe->rebuild();
                safe->scanButton.setEnabled (true);
                safe->scanButton.setButtonText (safe->host.getScanSummary());
            });
        }

    private:
        class EntryModel final : public juce::ListBoxModel
        {
        public:
            EntryModel (PluginManagerPage& pageToUse, bool onlyInstruments)
                : page (pageToUse), instruments (onlyInstruments) {}

            int getNumRows() override { return (int) entries.size(); }

            void paintListBoxItem (int row, juce::Graphics& g, int width, int height,
                                   bool selected) override
            {
                if (! juce::isPositiveAndBelow (row, (int) entries.size()))
                    return;

                const auto& description = entries[(size_t) row];

                if (selected)
                {
                    g.setColour (kAccent.withAlpha (0.18f));
                    g.fillRect (0, 0, width, height);
                }

                g.setColour (juce::Colours::white.withAlpha (0.86f));
                g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (11.0f))));
                g.drawText (description.name, atomic::ui::px (6), 0,
                            width - atomic::ui::px (220), height,
                            juce::Justification::centredLeft, true);

                g.setColour (kText.withAlpha (0.55f));
                g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));
                g.drawText (description.manufacturerName, width - atomic::ui::px (220), 0,
                            atomic::ui::px (150), height,
                            juce::Justification::centredRight, true);

                const int loaded = page.host.countInstancesOf (description);

                if (loaded > 0)
                {
                    g.setColour (kAccent.withAlpha (0.85f));
                    g.drawText (utf8 ("已载入 ") + juce::String (loaded),
                                width - 66, 0, 60, height,
                                juce::Justification::centredRight, true);
                }
            }

            void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
            {
                if (! juce::isPositiveAndBelow (row, (int) entries.size()))
                    return;

                const auto description = entries[(size_t) row];

                if (instruments)
                {
                    if (page.onAddInstrument != nullptr)
                        page.onAddInstrument (description);
                }
                else if (page.onAddEffect != nullptr)
                {
                    page.onAddEffect (description);
                }
            }

            std::vector<juce::PluginDescription> entries;

        private:
            PluginManagerPage& page;
            bool               instruments = false;
        };

        PluginHost& host;

        juce::ListBox  instrList { "instruments", nullptr };
        juce::ListBox  fxList    { "effects", nullptr };
        juce::TextButton scanButton;

        EntryModel instruments;
        EntryModel effects;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginManagerPage)
    };
}

//==============================================================================
class SettingsWindow::Panel final : public juce::Component
{
public:
    Panel (PluginHost& host, juce::AudioDeviceManager& devices)
    {
        // The app paints itself by hand and never installs a look-and-feel, so
        // JUCE's default (light grey) is what a TabbedComponent would draw in —
        // a light strip across a dark window. Owned here, not global.
        setLookAndFeel (&look);

        tabs.setTabBarDepth (atomic::ui::px (30));
        tabs.setOutline (0);
        tabs.setColour (juce::TabbedComponent::backgroundColourId, kBackground);
        tabs.setColour (juce::TabbedComponent::outlineColourId, kOutline);
        tabs.setColour (juce::TabbedButtonBar::tabOutlineColourId, kOutline);
        tabs.setColour (juce::TabbedButtonBar::frontOutlineColourId, kAccent);
        tabs.setColour (juce::TabbedButtonBar::tabTextColourId, kText);
        tabs.setColour (juce::TabbedButtonBar::frontTextColourId, juce::Colours::white);

        tabs.addTab (utf8 ("音频设置"), kPanel, new AudioSettingsPage (devices), true);
        tabs.addTab (utf8 ("MIDI 设置"), kPanel, new MidiSettingsPage(), true);

        auto* pluginPage = new PluginManagerPage (host);
        pluginPage->onAddInstrument = [this] (const juce::PluginDescription& d)
        {
            if (onAddInstrument != nullptr)
                onAddInstrument (d);
        };
        pluginPage->onAddEffect = [this] (const juce::PluginDescription& d)
        {
            if (onAddEffect != nullptr)
                onAddEffect (d);
        };

        plugins = pluginPage;
        tabs.addTab (utf8 ("插件管理器"), kPanel, pluginPage, true);

        addAndMakeVisible (tabs);
    }

    ~Panel() override
    {
        // The look-and-feel is a member: clear the pointer before it dies.
        setLookAndFeel (nullptr);
    }

    void resized() override
    {
        tabs.setBounds (getLocalBounds());
    }

    void selectTab (int index)
    {
        tabs.setCurrentTabIndex (juce::jlimit (0, tabs.getNumTabs() - 1, index));
    }

    int getNumTabs() const noexcept { return tabs.getNumTabs(); }

    void refreshPlugins()
    {
        if (plugins != nullptr)
            plugins->rebuild();
    }

    std::function<void (const juce::PluginDescription&)> onAddInstrument;
    std::function<void (const juce::PluginDescription&)> onAddEffect;

    friend class SettingsWindow;

private:
    /** Declared FIRST so it is destroyed LAST: every child that inherits it is
        already gone by then. */
    juce::LookAndFeel_V4 look;

    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };

    /** Owned by the TabbedComponent; this is only here to call rebuild(). */
    PluginManagerPage* plugins = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Panel)
};

//==============================================================================
SettingsWindow::SettingsWindow (PluginHost& pluginHost, juce::AudioDeviceManager& deviceManager)
    : juce::DocumentWindow (utf8 ("设置"), kBackground, juce::DocumentWindow::closeButton)
{
    setUsingNativeTitleBar (true);

    auto* content = new Panel (pluginHost, deviceManager);

    content->onAddInstrument = [this] (const juce::PluginDescription& description)
    {
        if (onAddInstrument != nullptr)
            onAddInstrument (description);
    };

    content->onAddEffect = [this] (const juce::PluginDescription& description)
    {
        if (onAddEffect != nullptr)
            onAddEffect (description);
    };

    panel = content;

    setContentOwned (content, false);
    setResizable (true, false);
    setResizeLimits (700, 400, 1400, 900);
    centreWithSize (atomic::ui::px (800), atomic::ui::px (480));
}

SettingsWindow::~SettingsWindow()
{
    // The panel is owned by the content component, which the window owns.
    panel = nullptr;
}

void SettingsWindow::showPage (int tabIndex)
{
    if (panel != nullptr)
        panel->selectTab (tabIndex);

    setVisible (true);
    toFront (true);
}

void SettingsWindow::refresh()
{
    if (panel != nullptr)
        panel->refreshPlugins();
}

int SettingsWindow::getNumPages() const
{
    return panel != nullptr ? panel->getNumTabs() : 0;
}

juce::Image SettingsWindow::createPageSnapshot (int tabIndex, int width, int height)
{
    if (panel == nullptr)
        return {};

    // Deliberately no setVisible / toFront: --snapshot must not put a window on
    // the desktop, and the panel renders identically whether it is on screen or
    // not (that is what createComponentSnapshot promises for a laid-out tree).
    panel->selectTab (tabIndex);
    panel->setSize (juce::jmax (atomic::ui::px (400), width),
                    juce::jmax (atomic::ui::px (240), height));

    return panel->createComponentSnapshot (panel->getLocalBounds(), true);
}

} // namespace atomic
