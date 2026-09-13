#include "NativeSelectPopupHaiku.h"
#include <Application.h>
#include <Window.h>
#include <array>
#include <cstdio>
#include <cstring>

using WebKit::NativeSelectPopupHaiku;

namespace {
constexpr uint32 completed = 'psdn';
constexpr std::array names { "selection-skips-disabled-and-separator", "escape", "cancel", "destroy-owner", "cancel-before-start", "empty", "tall-rtl", "disabled-selected" };
}

class PopupTests final : public BApplication {
public:
    PopupTests() : BApplication("application/x-vnd.Kunanyi-Summit-PopupTests") { }

    void ReadyToRun() override
    {
        // BApplication rounds to 100,000 microseconds; a smaller rate disables Pulse.
        SetPulseRate(100000);
        bigtime_t clickSpeed = 0;
        get_click_speed(&clickSpeed);
        m_readyDelay = std::max<bigtime_t>(500000, clickSpeed + 150000);
        Start();
    }

    void MessageReceived(BMessage* message) override
    {
        if (message->what != completed) {
            BApplication::MessageReceived(message);
            return;
        }
        int32 test = -1, index = -2;
        message->FindInt32("test", &test);
        message->FindInt32("index", &index);
        Check(test == static_cast<int32>(m_test), "completion belongs to current popup");
        if (test != static_cast<int32>(m_test))
            return;
        ++m_completions[m_test];
        Check(m_completions[m_test] == 1, "one completion per popup");
        Check(index == (m_test == 0 ? 5 : -1), "selection or cancellation result");
        if (m_test != 4 && m_test != 5)
            Check(m_pulses >= 3, "application loop remains responsive while menu tracks");
        m_popup.reset();
        m_completedAt = system_time();
    }

    void Pulse() override
    {
        if (m_finished)
            return;
        ++m_pulses;
        if (m_completedAt) {
            if (system_time() - m_completedAt < 250000 || NativeSelectPopupHaiku::activeCount())
                return;
            Check(m_completions[m_test] == 1, "no delayed duplicate completion");
            std::printf("POPUP %s complete\n", names[m_test]);
            if (++m_test == names.size() || m_timedOut) {
                Check(!NativeSelectPopupHaiku::activeCount(), "native tracking workers drained before application exit");
                std::printf("POPUP_RESULT %s checks=%u\n", m_failed ? "FAIL" : "PASS", m_checks);
                m_finished = true;
                PostMessage(B_QUIT_REQUESTED);
            } else
                Start();
            return;
        }
        if (system_time() - m_started > 10000000) {
            if (!m_timedOut) {
                Check(false, "popup exceeded native behavior deadline");
                if (m_popup)
                    m_popup->cancel();
                m_timedOut = true;
            }
            if (!NativeSelectPopupHaiku::activeCount()) {
                m_finished = true;
                PostMessage(B_QUIT_REQUESTED);
            }
            return;
        }
        if (!m_popup || m_acted || m_test == 4 || m_test == 5)
            return;
        auto target = m_popup->target();
        if (!target.IsValid())
            return;
        if (!m_readyAt)
            m_readyAt = system_time();
        if (system_time() - m_readyAt < m_readyDelay)
            return;
        if (target.LockTargetWithTimeout(100000) != B_OK)
            return;
        BLooper* looper = nullptr;
        auto* menu = dynamic_cast<BMenu*>(target.Target(&looper));
        Check(menu && looper, "native menu target exists");
        if (menu) {
            Check(menu->CountItems() == (m_test == 6 ? 100 : 6), "native list indices preserved");
            Check(menu->IsRadioMode(), "single selection native menu");
            Check(menu->FindMarkedIndex() == (m_test == 6 ? 99 : m_test == 7 ? 4 : 2), "current selected option marked");
            if (m_test != 6) {
                Check(!menu->ItemAt(0)->IsEnabled() && !std::strcmp(menu->ItemAt(0)->Label(), "Group"), "group label present and cannot be chosen");
                Check(!menu->ItemAt(4)->IsEnabled(), "disabled option cannot be chosen");
                Check(dynamic_cast<BSeparatorItem*>(menu->ItemAt(3)), "separator preserved");
            }
            auto frame = menu->Window()->Frame();
            auto screen = BScreen(menu->Window()).Frame();
            Check(frame.left >= screen.left - 1 && frame.top >= screen.top - 1
                && frame.right <= screen.right + 1 && frame.bottom <= screen.bottom + 1, "popup window stays on screen");
            if (m_test == 6)
                Check(menu->Bounds().Height() > menu->Window()->Bounds().Height(), "large list uses native scrolling");
        }
        if (looper)
            looper->Unlock();
        m_acted = true;
        if (m_test == 0) {
            Key(target, B_DOWN_ARROW);
            Key(target, B_ENTER);
        } else if (m_test == 1)
            Key(target, B_ESCAPE);
        else if (m_test == 3)
            m_popup.reset();
        else
            m_popup->cancel();
    }

    bool QuitRequested() override
    {
        if (m_finished)
            return true;
        Check(false, "unexpected application quit");
        if (m_popup)
            m_popup->cancel();
        return false;
    }

    int Result() const { return m_finished && !m_failed ? 0 : 1; }

private:
    void Check(bool result, const char* description)
    {
        ++m_checks;
        if (!result) {
            m_failed = true;
            std::fprintf(stderr, "FAIL %s: %s\n", m_test < names.size() ? names[m_test] : "cleanup", description);
        }
    }

    void Key(BMessenger target, char key)
    {
        BMessage message(B_KEY_DOWN);
        const char bytes[] = { key, 0 };
        message.AddString("bytes", bytes);
        Check(target.SendMessage(&message, static_cast<BHandler*>(nullptr), 0) == B_OK, "native keyboard message queued");
    }

    void Start()
    {
        std::printf("BEGIN POPUP %s\n", names[m_test]);
        m_started = system_time();
        m_readyAt = m_completedAt = 0;
        m_pulses = 0;
        m_acted = false;
        std::vector<NativeSelectPopupHaiku::Item> items {
            { "Group", true, true, false }, { "Alpha" }, { "Beta" },
            { "", false, false, true }, { "Disabled", false }, { "Omega" }
        };
        if (m_test == 5)
            items.clear();
        if (m_test == 6) {
            items.clear();
            for (size_t index = 0; index < 100; ++index)
                items.push_back({ "Long list option " + std::to_string(index) });
        }
        BRect screen = BScreen().Frame();
        BRect anchor = m_test == 6 ? BRect(screen.right - 40, screen.bottom - 20, screen.right + 100, screen.bottom + 10) : BRect(50, 60, 240, 90);
        m_popup = std::make_unique<NativeSelectPopupHaiku>(std::move(items), anchor, m_test == 6,
            m_test == 6 ? 99 : m_test == 7 ? 4 : 2,
            [target = BMessenger(this), test = m_test](int32 index) {
                BMessage message(completed);
                message.AddInt32("test", test);
                message.AddInt32("index", index);
                target.SendMessage(&message);
            });
        if (m_test == 4)
            m_popup->cancel();
        Check(m_popup->start() == B_OK, "native worker started");
        Check(m_popup->start() == B_NOT_ALLOWED, "one tracking operation per owner");
    }

    std::unique_ptr<NativeSelectPopupHaiku> m_popup;
    std::array<unsigned, names.size()> m_completions { };
    size_t m_test { 0 };
    bigtime_t m_started { 0 }, m_readyAt { 0 }, m_completedAt { 0 }, m_readyDelay { 0 };
    unsigned m_pulses { 0 }, m_checks { 0 };
    bool m_acted { false }, m_finished { false }, m_failed { false }, m_timedOut { false };
};

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    PopupTests tests;
    if (tests.InitCheck() != B_OK)
        return 1;
    tests.Run();
    return tests.Result();
}
