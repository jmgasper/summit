#include "ui/Messages.h"
#include <Application.h>
#include <Message.h>
#include <Messenger.h>
#include <OS.h>
#include <cstdio>
#include <string>

static BMessage State(const BMessenger& target)
{
    BMessage request(summit::kBrowserState), reply;
    target.SendMessage(&request, &reply, 1000000, 1000000);
    return reply;
}

static std::string Title(const BMessage& state)
{
    int64 selected = -1;
    state.FindInt64("selected", &selected);
    BMessage tab;
    for (int32 i = 0; state.FindMessage("tab", i, &tab) == B_OK; ++i) {
        int64 id = -1;
        const char* title = "";
        tab.FindInt64("id", &id);
        tab.FindString("title", &title);
        if (id == selected) return title;
    }
    return {};
}

int main(int argc, char** argv)
{
    BApplication application("application/x-vnd.Kunanyi-Summit-Probe");
    BMessenger app("application/x-vnd.Kunanyi-Summit");
    if (!app.IsValid()) { std::fprintf(stderr, "Summit is not running.\n"); return 1; }
    BMessage request(B_GET_PROPERTY), reply;
    request.AddSpecifier("Window", int32(0));
    BMessenger window;
    if (app.SendMessage(&request, &reply, 1000000, 1000000) != B_OK
        || reply.FindMessenger("result", &window) != B_OK || !window.IsValid()) return 1;
    if (argc > 1) {
        BMessage navigate(summit::kNavigate);
        navigate.AddString("url", argv[1]);
        if (window.SendMessage(&navigate) != B_OK) return 1;
    }
    if (argc > 2) {
        const bigtime_t deadline = system_time() + 25000000;
        while (system_time() < deadline) {
            auto state = State(window);
            if (Title(state) == argv[2]) {
                state.PrintToStream();
                std::puts("PASS expected browser page title");
                return 0;
            }
            snooze(100000);
        }
        State(window).PrintToStream();
        std::fprintf(stderr, "FAIL expected page title: %s\n", argv[2]);
        return 1;
    }
    State(window).PrintToStream();
    return 0;
}
