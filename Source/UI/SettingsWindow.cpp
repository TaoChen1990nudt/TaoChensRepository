#include "SettingsWindow.h"

#include "Util/Metrics.h"
#include "UI/TypingPiano.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>
#include <vector>

#include "Util/Text.h"
#include "Util/Theme.h"

namespace atomic
{

namespace
{
    const juce::Colour& kBackground = theme::surface0;
    const juce::Colour kPanel      { 0xff15161b };
    const juce::Colour kRow        { 0xff191b21 };
    const juce::Colour& kOutline   = theme::surface4;
    const juce::Colour& kText      = theme::inkMuted;
    const juce::Colour& kAccent    = theme::accent;

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
    /** What plays the synth, and what is arriving on it.

        Phase 40: two independent switches, not one three-way choice. Phase 39
        asked which single source to use, which meant turning the controller off
        to play the computer keyboard — and those two are exactly what people
        have in front of them at the same time.

        The page owns no device. A MIDI port can only be opened once, so the hub
        that opens it and the synth that hears it have to be the same object —
        which is why the device list here is a VIEW of MidiInputHub rather than a
        list of its own.
    */
    class MidiSettingsPage final : public juce::Component,
                                   private juce::Timer
    {
    public:
        explicit MidiSettingsPage (MidiInputHub& hubToUse)
            : hub (hubToUse)
        {
            rescanButton.setButtonText (utf8 ("重新扫描设备"));
            rescanButton.onClick = [this]
            {
                hub.refreshDevices();
                refreshRows();
            };

            addAndMakeVisible (rescanButton);

            refreshRows();
            startTimerHz (10);
        }

        ~MidiSettingsPage() override { stopTimer(); }

        /** The host applies the switches: it is the one that has to release the
            notes a source was holding when that source is turned off. */
        std::function<void (MidiSources)> onSourcesChanged;

        /** Asked on every paint, so the caption cannot go stale. */
        std::function<juce::String()> getTargetName;

        //-- Layout, in design units -----------------------------------------
        static constexpr int switchRowTop    = 44;
        static constexpr int switchRowStep   = 28;
        static constexpr int switchRowHeight = 26;
        static constexpr int numSwitches     = 2;

        static constexpr int blockGap      = 10;
        static constexpr int blockTitle    = 16;
        static constexpr int mapRows       = 4;
        static constexpr int mapRowHeight  = 20;
        static constexpr int mapRowStep    = 23;
        static constexpr int deviceRowStep = 28;
        static constexpr int monitorHeight = 48;

        static juce::Rectangle<int> switchRowBounds (int index, int width)
        {
            return { atomic::ui::px (12), atomic::ui::px (switchRowTop + index * switchRowStep),
                     width - atomic::ui::px (24), atomic::ui::px (switchRowHeight) };
        }

        /** Where the two blocks go, worked out from the switches.

            With both sources on they share one page, so the space has to be
            divided rather than assumed — and a short window must clip the device
            list instead of drawing it over the monitor. Paint AND hit-testing
            read this one plan: a device row you can see but not click (or the
            reverse) is what computing the layout twice produces. */
        struct Plan
        {
            juce::Rectangle<int> keyboard;    // empty when the keyboard is off
            juce::Rectangle<int> devices;     // empty when the controller is off
            juce::Rectangle<int> monitor;     // inside `devices`, stuck to its bottom
            int deviceRowsTop = 0;            // design units
            int maxDeviceRows = 0;            // how many rows fit above the monitor
        };

        Plan plan() const
        {
            Plan p;

            const auto sources = hub.getSources();

            int y = switchRowTop + numSwitches * switchRowStep + 8;

            const int keyboardHeight = blockTitle + mapRows * mapRowStep + 6 + 16;

            if (sources.typingKeyboard)
            {
                p.keyboard = { atomic::ui::px (12), atomic::ui::px (y),
                               getWidth() - atomic::ui::px (24), atomic::ui::px (keyboardHeight) };

                y += keyboardHeight + blockGap;
            }

            if (sources.externalController)
            {
                const int top    = atomic::ui::px (y);
                const int bottom = getHeight() - atomic::ui::px (6);

                p.devices = { atomic::ui::px (12), top,
                              getWidth() - atomic::ui::px (24), juce::jmax (0, bottom - top) };

                auto monitorArea = p.devices.reduced (atomic::ui::px (2), 0);
                p.monitor = monitorArea.removeFromBottom (atomic::ui::px (monitorHeight));

                p.deviceRowsTop = y + blockTitle + 4;

                const int rowsBottom = p.monitor.getY() - atomic::ui::px (6);
                const int rowsTopPx  = atomic::ui::px (p.deviceRowsTop);

                p.maxDeviceRows = juce::jmax (0, (rowsBottom - rowsTopPx)
                                                   / atomic::ui::px (deviceRowStep));
            }

            return p;
        }

        static juce::Rectangle<int> deviceRowBounds (const Plan& p, int index, int width)
        {
            return { atomic::ui::px (14),
                     atomic::ui::px (p.deviceRowsTop + index * deviceRowStep),
                     width - atomic::ui::px (30), atomic::ui::px (24) };
        }

        //-- Painting ---------------------------------------------------------
        void paint (juce::Graphics& g) override
        {
            g.fillAll (kPanel);

            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.setFont (boldFont (12.0f));
            g.drawText (utf8 ("按键发声的来源"),
                        atomic::ui::px (14), atomic::ui::px (10),
                        atomic::ui::px (300), atomic::ui::px (18),
                        juce::Justification::centredLeft, false);

            g.setColour (kText.withAlpha (0.7f));
            g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));
            g.drawText (utf8 ("两个开关可以同时打开：控制器和电脑键盘互不排斥。"),
                        atomic::ui::px (14), atomic::ui::px (27),
                        getWidth() - atomic::ui::px (140), atomic::ui::px (14),
                        juce::Justification::centredLeft, true);

            drawSwitches (g);

            const auto p = plan();

            if (! p.keyboard.isEmpty())
                drawKeyboardBlock (g, p.keyboard);

            if (! p.devices.isEmpty())
                drawDeviceBlock (g, p);

            drawTargetLine (g);
        }

        void resized() override
        {
            rescanButton.setBounds (getWidth() - atomic::ui::px (122), atomic::ui::px (8),
                                    atomic::ui::px (108), atomic::ui::px (22));
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            const auto position = e.getPosition();

            for (int i = 0; i < numSwitches; ++i)
                if (switchRowBounds (i, getWidth()).contains (position))
                {
                    flipSwitch (i);
                    return;
                }

            // The device rows are drawn from a plan, so they are clicked from the
            // same plan. The loop bound matters twice over: rows past
            // maxDeviceRows are not on screen, and clicking where a row is not
            // drawn would open a port the user cannot see.
            const auto p = plan();

            if (p.devices.isEmpty())
                return;

            for (int i = 0; i < (int) rows.size() && i < p.maxDeviceRows; ++i)
                if (deviceRowBounds (p, i, getWidth()).contains (position))
                {
                    // setDeviceOpen reports what actually happened: a port
                    // another app is holding does not open, and the tick must
                    // not claim it did.
                    hub.setDeviceOpen (rows[(size_t) i].identifier,
                                       ! rows[(size_t) i].open);
                    refreshRows();
                    return;
                }
        }

    private:
        void flipSwitch (int index)
        {
            auto next = hub.getSources();

            if (index == 0)
                next.typingKeyboard = ! next.typingKeyboard;
            else
                next.externalController = ! next.externalController;

            // The host applies it. Only it knows what has to be released on the
            // way out, and it also has to tell the panels.
            if (onSourcesChanged != nullptr)
                onSourcesChanged (next);

            // The host applied it; this page only reflects the result. Ports may
            // have been closed on the way out, so the list has to be re-read.
            refreshRows();
        }

        void refreshRows()
        {
            // Turning the controller on is the moment its list starts to matter,
            // and a device plugged in since the window opened has to appear then.
            if (hub.usesExternalController())
                hub.refreshDevices();

            rows = hub.getDevices();

            // Scanning for controllers is only a thing to do while the controller
            // switch is on; next to a keyboard-only page the button is a red
            // herring.
            rescanButton.setVisible (hub.usesExternalController());

            repaint();
        }

        void timerCallback() override
        {
            // The monitor is the only thing here that changes on its own.
            const auto monitor = hub.getMonitor();

            if (monitor.count != lastMessageCount)
            {
                lastMessageCount = monitor.count;
                repaint();
            }
        }

        void drawSwitches (juce::Graphics& g)
        {
            const auto sources = hub.getSources();

            for (int i = 0; i < numSwitches; ++i)
            {
                const auto bounds = switchRowBounds (i, getWidth());
                const bool on = (i == 0) ? sources.typingKeyboard : sources.externalController;

                g.setColour (on ? kAccent.withAlpha (0.14f) : kRow);
                g.fillRoundedRectangle (bounds.toFloat(), 4.0f);

                // A tick box, not a radio dot: two of these can be on at once.
                const auto box = juce::Rectangle<float> (13.0f, 13.0f)
                                    .withCentre ({ (float) bounds.getX() + atomic::ui::pf (18),
                                                   (float) bounds.getCentreY() });

                g.setColour (on ? kAccent : juce::Colours::white.withAlpha (0.22f));
                g.drawRoundedRectangle (box, 3.0f, 1.2f);

                if (on)
                    g.fillRoundedRectangle (box.reduced (atomic::ui::pf (3.5f)), 1.5f);

                g.setColour (juce::Colours::white.withAlpha (on ? 0.92f : 0.6f));
                g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (11.0f))));
                g.drawText (switchCaption (i),
                            bounds.withTrimmedLeft (atomic::ui::px (38)),
                            juce::Justification::centredLeft, true);

                // How many ports are actually open, on the row that owns them.
                if (i == 1 && on)
                {
                    g.setColour (kAccent.withAlpha (0.75f));
                    g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
                    g.drawText (utf8 ("已打开 ") + juce::String (hub.getNumOpenDevices())
                                  + utf8 (" 个端口"),
                                bounds.withTrimmedRight (atomic::ui::px (14)),
                                juce::Justification::centredRight, true);
                }
            }
        }

        static juce::String switchCaption (int index)
        {
            return index == 0 ? utf8 ("电脑键盘（FL Studio 键位）")
                              : utf8 ("外部 MIDI 控制器");
        }

        void drawKeyboardBlock (juce::Graphics& g, juce::Rectangle<int> area)
        {
            const int left = area.getX() + atomic::ui::px (8);

            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.setFont (boldFont (11.0f));
            g.drawText (utf8 ("键位映射 · FL Studio"),
                        left, area.getY(), atomic::ui::px (300), atomic::ui::px (blockTitle),
                        juce::Justification::centredLeft, false);

            struct Line { const char* caption; typing::Row row; };
            const Line lines[] {
                { "白键 · 低八度", typing::Row::lowerWhite },
                { "黑键 · 低八度", typing::Row::lowerBlack },
                { "白键 · 高八度", typing::Row::upperWhite },
                { "黑键 · 高八度", typing::Row::upperBlack }
            };

            int y = area.getY() + atomic::ui::px (blockTitle + 4);

            for (const auto& line : lines)
            {
                const int rowHeight = atomic::ui::px (mapRowHeight);

                g.setColour (kRow);
                g.fillRoundedRectangle (juce::Rectangle<int> (area.getX(), y,
                                                              area.getWidth(), rowHeight).toFloat(),
                                        3.0f);

                g.setColour (kText.withAlpha (0.8f));
                g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));
                g.drawText (utf8 (line.caption),
                            area.getX() + atomic::ui::px (10), y,
                            atomic::ui::px (92), rowHeight,
                            juce::Justification::centredLeft, false);

                g.setColour (juce::Colours::white.withAlpha (0.9f));
                g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.5f))));
                g.drawText (typing::rowKeys (line.row),
                            area.getX() + atomic::ui::px (104), y,
                            atomic::ui::px (150), rowHeight,
                            juce::Justification::centredLeft, false);

                g.setColour (kAccent.withAlpha (0.85f));
                g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));
                g.drawText (typing::rowNotes (line.row),
                            area.getX() + atomic::ui::px (258), y,
                            area.getWidth() - atomic::ui::px (270), rowHeight,
                            juce::Justification::centredLeft, false);

                y += atomic::ui::px (mapRowStep);
            }

            g.setColour (kText.withAlpha (0.6f));
            g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
            g.drawText (utf8 ("小键盘 + / − 换八度。字母键此时归音源（G 编组、Ctrl+S 保存仍可用）。"),
                        left, area.getBottom() - atomic::ui::px (18),
                        area.getWidth() - atomic::ui::px (16), atomic::ui::px (16),
                        juce::Justification::centredLeft, true);
        }

        void drawDeviceBlock (juce::Graphics& g, const Plan& p)
        {
            const int left = p.devices.getX() + atomic::ui::px (8);

            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.setFont (boldFont (11.0f));
            g.drawText (utf8 ("MIDI 输入设备"),
                        left, p.devices.getY(), atomic::ui::px (300), atomic::ui::px (blockTitle),
                        juce::Justification::centredLeft, false);

            // Sharing the page with the keyboard block clips the list; saying so
            // beats drawing four devices and pretending the fifth does not exist.
            if ((int) rows.size() > p.maxDeviceRows)
            {
                g.setColour (kText.withAlpha (0.5f));
                g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
                g.drawText (utf8 ("显示 ") + juce::String (p.maxDeviceRows) + utf8 (" / ")
                              + juce::String ((int) rows.size()) + utf8 (" 个"),
                            p.devices.withTrimmedRight (atomic::ui::px (8))
                                     .withHeight (atomic::ui::px (blockTitle)),
                            juce::Justification::centredRight, true);
            }

            //-- Rows --------------------------------------------------------
            if (rows.empty())
            {
                g.setColour (kText.withAlpha (0.6f));
                g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.5f))));
                g.drawFittedText (utf8 ("没有检测到 MIDI 输入设备。接上后点“重新扫描设备”，或只用电脑键盘。"),
                                  juce::Rectangle<int> (left, atomic::ui::px (p.deviceRowsTop),
                                                        p.devices.getWidth() - atomic::ui::px (16),
                                                        atomic::ui::px (44)),
                                  juce::Justification::topLeft, 2);
            }
            else
            {
                for (int i = 0; i < (int) rows.size() && i < p.maxDeviceRows; ++i)
                {
                    const auto bounds = deviceRowBounds (p, i, getWidth());
                    const auto& row   = rows[(size_t) i];

                    g.setColour (i % 2 == 0 ? kRow : juce::Colour (0xff15171c));
                    g.fillRoundedRectangle (bounds.toFloat(), 4.0f);

                    const auto box = juce::Rectangle<float> (12.0f, 12.0f)
                                        .withCentre ({ (float) bounds.getX() + atomic::ui::pf (18),
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
            }

            //-- Monitor -----------------------------------------------------
            g.setColour (juce::Colour (0xff0d0e12));
            g.fillRoundedRectangle (p.monitor.toFloat(), 4.0f);

            g.setColour (kText.withAlpha (0.85f));
            g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.0f))));
            g.drawText (monitorText (hub.getMonitor()),
                        p.monitor.reduced (atomic::ui::px (8), 0).withHeight (atomic::ui::px (18)),
                        juce::Justification::centredLeft, true);

            g.setColour (kText.withAlpha (0.45f));
            g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
            g.drawText (utf8 ("端口可以一起用；按键即发声，录音（写进钢琴窗）还没接。"),
                        p.monitor.reduced (atomic::ui::px (8), 0)
                                  .removeFromBottom (atomic::ui::px (18)),
                        juce::Justification::centredLeft, true);
        }

        /** "目标音源：Synth A" — the other half of "why is nothing sounding".
            A key that goes to no synth is silent for a reason the user can fix,
            and this line is the reason. It sits on the title row, right-aligned,
            because the bottom of the page belongs to the monitor. */
        void drawTargetLine (juce::Graphics& g)
        {
            const auto sources = hub.getSources();

            if (! sources.any() || getTargetName == nullptr)
                return;

            const int reserved = hub.usesExternalController() ? atomic::ui::px (130)
                                                              : atomic::ui::px (16);

            g.setColour (kAccent.withAlpha (0.8f));
            g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));
            g.drawText (utf8 ("目标音源：") + getTargetName(),
                        juce::Rectangle<int> (atomic::ui::px (14), atomic::ui::px (10),
                                              getWidth() - reserved - atomic::ui::px (28),
                                              atomic::ui::px (18)),
                        juce::Justification::centredRight, true);
        }

        static juce::String monitorText (const MidiInputHub::Monitor& monitor)
        {
            if (monitor.count <= 0)
                return utf8 ("等待 MIDI 消息…");

            juce::String what;

            switch (monitor.lastType)
            {
                case 1:  what = "Note On  " + atomic::noteName (monitor.lastNote)
                                  + "  vel " + juce::String (monitor.lastVelocity); break;
                case 2:  what = "Note Off " + atomic::noteName (monitor.lastNote); break;
                case 3:  what = "CC " + juce::String (monitor.lastNote)
                                  + "  =  " + juce::String (monitor.lastVelocity); break;
                default: what = utf8 ("其它消息"); break;
            }

            return what + utf8 ("   ·   通道 ") + juce::String (monitor.lastChannel)
                     + utf8 ("   ·   共 ") + juce::String (monitor.count) + utf8 (" 条");
        }

        MidiInputHub& hub;

        /** A copy of the hub's device list, refreshed when it can change — not
            queried inside paint(), which runs far more often than hardware
            changes and would allocate on every repaint. */
        std::vector<MidiInputHub::DeviceInfo> rows;

        juce::TextButton rescanButton;

        int lastMessageCount = 0;

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
    Panel (PluginHost& host, juce::AudioDeviceManager& devices, MidiInputHub& midiHub)
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

        auto* midi = new MidiSettingsPage (midiHub);
        midi->onSourcesChanged = [this] (MidiSources sources)
        {
            if (onMidiSourcesChanged != nullptr)
                onMidiSourcesChanged (sources);
        };
        midi->getTargetName = [this]
        {
            return getLiveTargetName != nullptr ? getLiveTargetName() : juce::String();
        };

        midiPage = midi;
        tabs.addTab (utf8 ("MIDI 设置"), kPanel, midi, true);

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
    std::function<void (MidiSources)> onMidiSourcesChanged;
    std::function<juce::String()>     getLiveTargetName;

    friend class SettingsWindow;

private:
    /** Declared FIRST so it is destroyed LAST: every child that inherits it is
        already gone by then. */
    juce::LookAndFeel_V4 look;

    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };

    /** Owned by the TabbedComponent; this is only here to call rebuild(). */
    PluginManagerPage* plugins = nullptr;

    /** Same, for the MIDI page's refresh after the plugin list moved. */
    MidiSettingsPage* midiPage = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Panel)
};

//==============================================================================
SettingsWindow::SettingsWindow (PluginHost& pluginHost,
                                juce::AudioDeviceManager& deviceManager,
                                MidiInputHub& midiHub)
    : juce::DocumentWindow (utf8 ("设置"), kBackground, juce::DocumentWindow::closeButton)
{
    setUsingNativeTitleBar (true);

    auto* content = new Panel (pluginHost, deviceManager, midiHub);

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

    content->onMidiSourcesChanged = [this] (MidiSources sources)
    {
        if (onMidiSourcesChanged != nullptr)
            onMidiSourcesChanged (sources);
    };

    content->getLiveTargetName = [this]
    {
        return getLiveTargetName != nullptr ? getLiveTargetName() : juce::String();
    };

    panel = content;

    setContentOwned (content, false);
    setResizable (true, false);

    // Phase 40: taller than Phase 39's 480. With both sources on, the keyboard
    // map and the device list share one page, and 480 left the list three rows
    // tall — enough to hide the device you were looking for.
    setResizeLimits (700, 400, 1400, 900);
    centreWithSize (atomic::ui::px (800), atomic::ui::px (520));
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
