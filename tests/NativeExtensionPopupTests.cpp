/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "NativeExtensionPopupHaiku.h"
#include <Application.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <limits>

using WebKit::NativeExtensionPopupHaiku;
namespace {
constexpr uint32 nativeEvent = 'epnt';
constexpr std::array names { "hidden-show-resize-cancel", "escape", "focus-loss", "owner-destroyed", "modal-child-focus", "cancel-before-start", "factory-failure", "invalid-owner", "focused-owner-show", "unfocused-owner-show", "focus-loss-before-show" };
std::atomic<unsigned> destroyedViews { 0 };
std::atomic<bool> modalChild { false };
class TestView final : public BView {
public:
    explicit TestView(BRect frame) : BView(frame, "popup-test-content", B_FOLLOW_ALL, B_WILL_DRAW | B_NAVIGABLE) { SetViewColor(250, 250, 250); }
    ~TestView() override { ++destroyedViews; }
    void Draw(BRect) override
    {
        SetHighColor(30, 30, 30);
        DrawString("Summit extension popup host test", BPoint(12, 25));
        DrawString("Native window, focus and lifetime checks", BPoint(12, 48));
    }
};
}
class PopupTests final : public BApplication {
public:
    PopupTests() : BApplication("application/x-vnd.Kunanyi-Summit-ExtensionPopupTests") { }
    int result() const { return m_failed || !m_done ? 1 : 0; }
    void ReadyToRun() override
    {
        SetPulseRate(100000);
        auto screen = BRect(0, 0, 1279, 799);
        auto frame = NativeExtensionPopupHaiku::frameForSize(BRect(1200, 740, 1240, 765), BSize(1000, 900), screen);
        check(screen.Contains(frame), "oversized popup stays on screen");
        check(frame.Width() + 1 == 800 && frame.Height() + 1 == 600, "popup dimensions respect the extension maximum");
        frame = NativeExtensionPopupHaiku::frameForSize(BRect(10, 10, 20, 20), BSize(1, 2), screen);
        check(frame.Width() + 1 == 25 && frame.Height() + 1 == 25, "small popup dimensions retain a usable minimum");
        frame = NativeExtensionPopupHaiku::frameForSize(BRect(-900, -500, -890, -490), BSize(200, 150), screen);
        check(screen.Contains(frame), "offscreen owner anchor cannot hide the popup");
        NativeExtensionPopupHaiku invalid(BMessenger(), BRect(0, 0, std::numeric_limits<float>::infinity(), 40), "invalid", [](BRect r) { return new TestView(r); }, [](auto, auto) { });
        check(invalid.start() == B_BAD_VALUE && !NativeExtensionPopupHaiku::activeCount(), "invalid geometry starts no native worker");
        start();
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what != nativeEvent) { BApplication::MessageReceived(message); return; }
        auto test = message->GetInt32("test", -1);
        auto event = static_cast<NativeExtensionPopupHaiku::Event>(message->GetInt32("event", -1));
        check(test == static_cast<int32>(m_test), "native event belongs to the current popup");
        if (test != static_cast<int32>(m_test))
            return;
        if (event == NativeExtensionPopupHaiku::Event::Created) {
            check(++m_created == 1, "one native window is created");
            m_createdAt = system_time();
        } else if (event == NativeExtensionPopupHaiku::Event::Shown) {
            check(++m_shown == 1 && m_created == 1, "shown notification follows creation exactly once");
            m_shownAt = system_time();
        } else {
            check(++m_closed == 1, "native closure completes exactly once");
            check(destroyedViews == (m_test < 5 || m_test == 6 || m_test >= 8 ? 1u : 0u), "closure is reported after all created child views are destroyed");
            if (m_test >= 9) {
                check(m_shown == 0, "an unfocused programmatic request never reports native presentation");
                check(message->GetInt32("status", B_ERROR) == B_CANCELED, "lost owner focus cancels the pending native request");
                check(active(m_competitor), "rejected popup leaves the competing native window focused");
            }
            m_finishedAt = system_time();
            m_popup.reset();
        }
    }
    void Pulse() override
    {
        if (m_done)
            return;
        ++m_pulses;
        if (m_finishedAt) {
            if (NativeExtensionPopupHaiku::activeCount() || system_time() - m_finishedAt < 250000)
                return;
            if (m_competitor.IsValid()) {
                BMessage quit(B_QUIT_REQUESTED); m_competitor.SendMessage(&quit);
                return;
            }
            if (m_owner.IsValid()) {
                BMessage quit(B_QUIT_REQUESTED); m_owner.SendMessage(&quit);
                return;
            }
            check(m_closed == 1, "no delayed duplicate completion");
            std::printf("EXTENSION_POPUP %s complete\n", names[m_test]); std::fflush(stdout);
            if (m_owner.IsValid()) { BMessage quit(B_QUIT_REQUESTED); m_owner.SendMessage(&quit); }
            if (++m_test == names.size() || m_timeout) { finish(); return; }
            start();
            return;
        }
        if (system_time() - m_started > 10000000) {
            if (!m_timeout) {
                check(false, "native popup test completed before its deadline");
                m_timeout = true;
                releaseBlockedPopup();
                if (m_popup) m_popup->cancel();
            }
            if (!NativeExtensionPopupHaiku::activeCount()) {
                if (m_owner.IsValid()) { BMessage quit(B_QUIT_REQUESTED); m_owner.SendMessage(&quit); }
                else finish();
            }
            return;
        }
        if (!m_popup || (m_test >= 5 && m_test <= 7))
            return;
        if (m_blockedPopup) {
            if (!active(m_owner) && active(m_competitor)) {
                check(m_shown == 0, "focus changes after request while native presentation is still queued");
                releaseBlockedPopup();
            }
            return;
        }
        auto target = m_popup->target();
        if (!target.IsValid())
            return;
        if (!m_requestedShow && m_createdAt && system_time() - m_createdAt > 300000) {
            if ((m_test == 8 || m_test == 10) && !active(m_owner))
                return;
            if (m_test == 9 && (active(m_owner) || !active(m_competitor)))
                return;
            if (target.LockTargetWithTimeout(0) != B_OK)
                return;
            BLooper* looper = nullptr;
            auto* window = dynamic_cast<BWindow*>(target.Target(&looper));
            check(window && window->IsHidden(), "popup remains hidden until the engine requests presentation");
            m_requestedShow = true;
            if (m_test >= 8) {
                check(m_test == 9 ? !active(m_owner) : active(m_owner), "programmatic request has the intended initial owner focus");
                m_popup->show(true);
            } else
                m_popup->show();
            if (m_test == 10) {
                // Hold the native popup's looper across pulses so its queued
                // presentation runs only after an actual focus transition.
                m_blockedPopup = looper;
                createCompetitor();
            } else
                looper->Unlock();
            return;
        }
        if (!m_shownAt || system_time() - m_shownAt < 500000)
            return;
        if (!m_acted) {
            check(m_pulses >= 4, "application stays responsive while popup is open");
            m_acted = true;
            m_actionAt = system_time();
            if (m_test >= 8) {
                check(m_test == 8 && active(target), "focused programmatic popup receives native focus");
                m_popup->cancel();
                return;
            }
            if (m_test == 0) { m_popup->resize(BSize(420, 310)); return; }
            if (m_test == 1) {
                BMessage key(B_KEY_DOWN); const char escape[] = { B_ESCAPE, 0 };
                key.AddString("bytes", escape); key.AddInt32("raw_char", B_ESCAPE); key.AddInt32("modifiers", 0); key.AddInt32("key", 1); key.AddInt64("when", system_time());
                if (target.LockTargetWithTimeout(0) != B_OK) { m_acted = false; return; }
                BLooper* looper = nullptr; target.Target(&looper);
                // Haiku evaluates Escape/shortcuts only for preferred-target
                // input, as produced by its input server, not window-addressed messages.
                BMessenger input(nullptr, looper); looper->Unlock();
                input.SendMessage(&key);
            } else if (m_test == 2 || m_test == 4) {
                modalChild = m_test == 4;
                if (target.LockTargetWithTimeout(0) != B_OK) { m_acted = false; return; }
                BLooper* looper = nullptr; auto* window = dynamic_cast<BWindow*>(target.Target(&looper));
                check(window && window->IsActive(), "popup receives native keyboard focus before dismissal");
                looper->Unlock();
                // Activate(false) asks app_server to send the window behind;
                // floating subsets cannot be reordered like normal windows.
                // Selecting the owner causes an actual native focus transition.
                if (m_owner.LockTargetWithTimeout(0) != B_OK) { m_acted = false; return; }
                auto* owner = dynamic_cast<BWindow*>(m_owner.Target(&looper));
                owner->Activate(true); looper->Unlock();
            } else if (m_test == 3) { BMessage quit(B_QUIT_REQUESTED); m_owner.SendMessage(&quit); }
            return;
        }
        if (system_time() - m_actionAt < 500000)
            return;
        if (m_test == 0) {
            if (target.LockTargetWithTimeout(0) != B_OK) return;
            BLooper* looper = nullptr; auto* window = dynamic_cast<BWindow*>(target.Target(&looper));
            check(window && window->Bounds().Width() + 1 == 420 && window->Bounds().Height() + 1 == 310, "content resize reaches the native window");
            looper->Unlock();
            m_popup->cancel();
        } else if (m_test == 4 && modalChild.exchange(false))
            check(!m_closed, "a modal child temporarily prevents focus-loss dismissal");
    }
private:
    static bool active(const BMessenger& target)
    {
        if (target.LockTargetWithTimeout(0) != B_OK)
            return false;
        BLooper* looper = nullptr;
        auto* window = dynamic_cast<BWindow*>(target.Target(&looper));
        bool result = window && window->IsActive();
        if (looper) looper->Unlock();
        return result;
    }
    void createCompetitor()
    {
        auto* window = new BWindow(BRect(180, 220, 740, 550), "Summit competing focus window", B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS);
        m_competitor = BMessenger(window);
        window->Show();
        window->Activate(true);
    }
    void releaseBlockedPopup()
    {
        if (auto* looper = std::exchange(m_blockedPopup, nullptr))
            looper->Unlock();
    }
    void check(bool passed, const char* text)
    {
        ++m_checks; m_failed |= !passed;
        std::printf("%s %s\n", passed ? "PASS" : "FAIL", text); std::fflush(stdout);
    }
    void start()
    {
        m_created = m_shown = m_closed = m_pulses = 0;
        m_createdAt = m_shownAt = m_finishedAt = m_actionAt = 0;
        m_requestedShow = m_acted = false; destroyedViews = 0; modalChild = false;
        auto* owner = new BWindow(BRect(100, 130, 850, 660), "Summit popup test owner", B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS);
        m_owner = BMessenger(owner); owner->Show();
        if (m_test == 9) createCompetitor();
        const auto test = m_test;
        m_popup = std::make_unique<NativeExtensionPopupHaiku>(m_test == 7 ? BMessenger() : m_owner, BRect(780, 180, 820, 195), "Summit extension popup test",
            [test](BRect frame) -> BView* { if (test == 6) { auto view = std::make_unique<TestView>(frame); return nullptr; } return new TestView(frame); },
            [target = BMessenger(this), test](auto event, status_t status) {
                BMessage message(nativeEvent); message.AddInt32("test", test); message.AddInt32("event", static_cast<int32>(event)); message.AddInt32("status", status); target.SendMessage(&message);
            }, [] { return modalChild.load(); });
        if (m_test == 5) m_popup->cancel();
        check(m_popup->start() == B_OK, "native creation starts asynchronously");
        check(m_popup->start() == B_NOT_ALLOWED, "the same popup cannot start twice");
        m_started = system_time();
    }
    void finish()
    {
        if (m_done) return;
        releaseBlockedPopup();
        if (m_competitor.IsValid()) { BMessage quit(B_QUIT_REQUESTED); m_competitor.SendMessage(&quit); }
        m_done = true;
        check(!NativeExtensionPopupHaiku::activeCount(), "all native popup workers and windows have drained");
        std::printf("EXTENSION_POPUP_RESULT %s checks=%u\n", m_failed ? "FAIL" : "PASS", m_checks); std::fflush(stdout);
        PostMessage(B_QUIT_REQUESTED);
    }
    BMessenger m_owner;
    BMessenger m_competitor;
    BLooper* m_blockedPopup { nullptr };
    std::unique_ptr<NativeExtensionPopupHaiku> m_popup;
    size_t m_test { 0 };
    unsigned m_checks { 0 }, m_created { 0 }, m_shown { 0 }, m_closed { 0 }, m_pulses { 0 };
    bigtime_t m_started { 0 }, m_createdAt { 0 }, m_shownAt { 0 }, m_finishedAt { 0 }, m_actionAt { 0 };
    bool m_requestedShow { false }, m_acted { false }, m_done { false }, m_timeout { false }, m_failed { false };
};
int main() { PopupTests application; application.Run(); return application.result(); }
