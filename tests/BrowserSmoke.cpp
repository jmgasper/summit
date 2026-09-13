#include "ui/Messages.h"
#include <Application.h>
#include <Message.h>
#include <Messenger.h>
#include <OS.h>
#include <cstdio>
#include <functional>
#include <string>

static int checks = 0, failures = 0;
static void Check(bool ok, const char* label)
{
    ++checks;
    if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    std::fflush(stdout);
}
static BMessage State(const BMessenger& window)
{
    BMessage request(summit::kBrowserState), reply;
    if (window.SendMessage(&request, &reply, 1000000, 1000000) != B_OK) return {};
    return reply;
}
static int32 Count(const BMessage& state)
{
    int32 count = -1; state.FindInt32("count", &count); return count;
}
static int64 Selected(const BMessage& state)
{
    int64 id = -1; state.FindInt64("selected", &id); return id;
}
static std::string Title(const BMessage& state)
{
    BMessage tab;
    for (int32 i = 0; state.FindMessage("tab", i, &tab) == B_OK; ++i) {
        int64 id = -1; const char* title = "";
        tab.FindInt64("id", &id); tab.FindString("title", &title);
        if (id == Selected(state)) return title;
    }
    return {};
}
static bool Loading(const BMessage& state)
{
    BMessage tab;
    for (int32 i = 0; state.FindMessage("tab", i, &tab) == B_OK; ++i) {
        int64 id = -1;
        bool loading = true;
        tab.FindInt64("id", &id); tab.FindBool("loading", &loading);
        if (id == Selected(state)) return loading;
    }
    return true;
}
static bool Wait(const BMessenger& window, const std::function<bool(const BMessage&)>& predicate)
{
    const bigtime_t deadline = system_time() + 20000000;
    while (system_time() < deadline) {
        if (predicate(State(window))) return true;
        snooze(100000);
    }
    return false;
}
static void Send(const BMessenger& window, uint32 what, const char* url = nullptr, int64 id = -1)
{
    BMessage message(what);
    if (url) message.AddString("url", url);
    if (id >= 0) message.AddInt64("id", id);
    window.SendMessage(&message);
}
int main()
{
    BApplication application("application/x-vnd.Kunanyi-Summit-smoke");
    BMessenger app("application/x-vnd.Kunanyi-Summit");
    Check(app.IsValid(), "native browser is running");
    if (!app.IsValid()) return 1;
    BMessage request(B_GET_PROPERTY), reply;
    request.AddSpecifier("Window", int32(0));
    BMessenger window;
    app.SendMessage(&request, &reply, 1000000, 1000000);
    Check(reply.FindMessenger("result", &window) == B_OK && window.IsValid(), "window responds through native scripting");
    if (!window.IsValid()) return 1;
    auto initial = State(window);
    const auto count = Count(initial);
    const auto original = Selected(initial);
    Check(count >= 1, "session has a selected tab");
    Send(window, summit::kNavigate, "http://10.0.2.2:8765/basic");
    Check(Wait(window, [](const BMessage& s) { return Title(s) == "Summit fixture PASS"; }), "real HTTP, JavaScript, DOM, CSS, storage, fetch and cookie fixture");
    Check(Wait(window, [](const BMessage& s) { return Title(s) == "Summit fixture PASS" && !Loading(s); }), "completed page clears its loading indicator");
    Send(window, summit::kNavigate, "http://10.0.2.2:8765/slow");
    Check(Wait(window, [](const BMessage& s) { return Title(s) == "Summit pending image" && Loading(s); }), "DOM readiness keeps the loading indicator while an image is pending");
    Check(Wait(window, [](const BMessage& s) { return Title(s) == "Summit completed image" && !Loading(s); }), "resource completion clears the loading indicator");
    Send(window, summit::kNavigate, "http://10.0.2.2:8765/basic");
    Check(Wait(window, [](const BMessage& s) { return Title(s) == "Summit fixture PASS"; }), "navigation remains usable after delayed resources complete");
    Send(window, summit::kNewTab, "http://10.0.2.2:8765/second");
    Check(Wait(window, [&](const BMessage& s) { return Count(s) == count + 1 && Title(s) == "Summit second page"; }), "new tab creates a live WebKit page");
    auto selected = Selected(State(window));
    Check(selected != original, "tab identifiers are stable and distinct");
    Send(window, summit::kNavigate, "http://10.0.2.2:8765/basic");
    Check(Wait(window, [](const BMessage& s) { return Title(s) == "Summit fixture PASS"; }), "second tab navigates independently");
    Send(window, summit::kBack);
    Check(Wait(window, [](const BMessage& s) { return Title(s) == "Summit second page"; }), "back restores the previous document");
    Send(window, summit::kForward);
    Check(Wait(window, [](const BMessage& s) { return Title(s) == "Summit fixture PASS"; }), "forward restores the next document");
    Send(window, summit::kCloseTab, nullptr, original);
    Check(Wait(window, [&](const BMessage& s) { return Count(s) == count && Selected(s) == selected; }), "closing a background tab preserves selection");
    Send(window, summit::kReopenTab);
    Check(Wait(window, [&](const BMessage& s) { return Count(s) == count + 1 && Selected(s) != original && Selected(s) != selected && Title(s) == "Summit fixture PASS"; }), "reopen restores the closed page with a fresh identifier");
    auto reopened = Selected(State(window));
    Send(window, summit::kSelectTab, nullptr, selected);
    Check(Wait(window, [&](const BMessage& s) { return Selected(s) == selected; }), "selecting a background tab");
    snooze(200000); // Allow the engine's resent progress notifications to arrive.
    Check(Wait(window, [&](const BMessage& s) { return Selected(s) == selected && !Loading(s); }), "selecting a completed tab keeps its loading indicator cleared");
    Send(window, summit::kCloseTab, nullptr, selected);
    Check(Wait(window, [&](const BMessage& s) { return Count(s) == count && Selected(s) == reopened; }), "closing the foreground tab selects a live neighbour");
    Send(window, summit::kBookmark);
    Send(window, summit::kSaveSession);
    for (int i = 0; i < 4; ++i) {
        Send(window, summit::kNewTab, "http://10.0.2.2:8765/second");
        bool opened = Wait(window, [&](const BMessage& s) { return Count(s) == count + 1 && Title(s) == "Summit second page"; });
        Check(opened, "tab creation remains responsive across repeated lifecycle transitions");
        Send(window, summit::kCloseTab);
        Check(Wait(window, [&](const BMessage& s) { return Count(s) == count; }), "tab shutdown releases the page without freezing the window");
    }
    Send(window, summit::kNewTab, "summit:home");
    Check(Wait(window, [&](const BMessage& s) {
        const char* address = nullptr;
        return Count(s) == count + 1 && Title(s) == "Start Page"
            && s.FindString("address", &address) == B_OK && std::string(address).empty();
    }), "new start tab presents an empty address field for typing");
    Send(window, summit::kCloseTab);
    Check(Wait(window, [&](const BMessage& s) { return Count(s) == count; }), "closing the start tab returns to the document");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
