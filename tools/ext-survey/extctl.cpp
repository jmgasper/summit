// Team-targeted extension install helper for the extension survey.
// Drives Summit's own Extensions window (the same messages its file panel and
// Install button send), never addresses Summit by signature alone.
//
//   extctl --team ID windows               list window titles and frames
//   extctl --team ID show                  open the Extensions window
//   extctl --team ID install PATH          as if PATH was chosen in "Add extension..."
//   extctl --team ID approve [files]       press Install on the review screen
//   extctl --team ID wframe TITLE L T R B  move the window whose title starts with TITLE
#include "ui/Messages.h"
#include <Application.h>
#include <Entry.h>
#include <Message.h>
#include <Messenger.h>
#include <Rect.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static const bigtime_t kTimeout = 5000000;

struct Win { BMessenger messenger; std::string title; BRect frame; };

static std::vector<Win> Windows(BMessenger& app)
{
    std::vector<Win> out;
    for (int32 i = 0; i < 64; ++i) {
        BMessage request(B_GET_PROPERTY), reply;
        request.AddSpecifier("Messenger");
        request.AddSpecifier("Window", i);
        if (app.SendMessage(&request, &reply, kTimeout, kTimeout) != B_OK) break;
        BMessenger window;
        if (reply.FindMessenger("result", &window) != B_OK || !window.IsValid()) break;
        Win w; w.messenger = window;
        BMessage t(B_GET_PROPERTY), tr; t.AddSpecifier("Title");
        const char* title = "";
        if (window.SendMessage(&t, &tr, kTimeout, kTimeout) == B_OK) tr.FindString("result", &title);
        w.title = title ? title : "";
        BMessage f(B_GET_PROPERTY), fr; f.AddSpecifier("Frame");
        if (window.SendMessage(&f, &fr, kTimeout, kTimeout) == B_OK) fr.FindRect("result", &w.frame);
        out.push_back(w);
    }
    return out;
}

static BMessenger FindWindow(BMessenger& app, const char* prefix)
{
    for (auto& w : Windows(app))
        if (!std::strncmp(w.title.c_str(), prefix, std::strlen(prefix))) return w.messenger;
    return {};
}

int main(int argc, char** argv)
{
    if (argc < 4 || std::strcmp(argv[1], "--team")) {
        std::fputs("usage: extctl --team ID windows|show|install PATH|approve [files]|wframe TITLE L T R B\n", stderr);
        return 2;
    }
    team_id team = static_cast<team_id>(std::strtol(argv[2], nullptr, 10));
    std::string command = argv[3];
    status_t status = B_NO_INIT;
    BApplication application("application/x-vnd.Kunanyi-Summit-extsurvey", &status);
    if (status != B_OK) return 5;
    BMessenger app("application/x-vnd.Kunanyi-Summit", team, &status);
    if (status != B_OK || !app.IsValid() || app.Team() != team) {
        std::fprintf(stderr, "team %ld is not a running Summit\n", long(team));
        return 3;
    }
    if (command == "windows") {
        for (auto& w : Windows(app))
            std::printf("%s\t%.0f %.0f %.0f %.0f\n", w.title.c_str(), w.frame.left, w.frame.top, w.frame.right, w.frame.bottom);
        return 0;
    }
    if (command == "show") {
        BMessage show(summit::kShowExtensions);
        return app.SendMessage(&show, static_cast<BHandler*>(nullptr), kTimeout) == B_OK ? 0 : 5;
    }
    BMessenger manager = FindWindow(app, "Extensions");
    if (command == "wframe") {
        if (argc < 9) return 2;
        BMessenger target = FindWindow(app, argv[4]);
        if (!target.IsValid()) { std::fputs("no such window\n", stderr); return 6; }
        BMessage set(B_SET_PROPERTY), done;
        set.AddSpecifier("Frame");
        set.AddRect("data", BRect(std::atof(argv[5]), std::atof(argv[6]), std::atof(argv[7]), std::atof(argv[8])));
        return target.SendMessage(&set, &done, kTimeout, kTimeout) == B_OK ? 0 : 5;
    }
    if (!manager.IsValid()) { std::fputs("Extensions window is not open (run show)\n", stderr); return 6; }
    if (command == "install") {
        if (argc < 5) return 2;
        entry_ref ref;
        if (get_ref_for_path(argv[4], &ref) != B_OK) { std::fputs("bad path\n", stderr); return 2; }
        BMessage selected(summit::kExtensionSelected);
        selected.AddRef("refs", &ref);
        return manager.SendMessage(&selected, static_cast<BHandler*>(nullptr), kTimeout) == B_OK ? 0 : 5;
    }
    if (command == "ready") {
        BMessage get(B_GET_PROPERTY), reply;
        get.AddSpecifier("Enabled");
        get.AddSpecifier("View", "extension-install");
        bool enabled = false;
        if (manager.SendMessage(&get, &reply, kTimeout, kTimeout) != B_OK || reply.FindBool("result", &enabled) != B_OK) {
            std::puts("unknown"); return 7;
        }
        std::puts(enabled ? "ready" : "busy");
        return enabled ? 0 : 1;
    }
    if (command == "approve") {
        if (argc > 4 && !std::strcmp(argv[4], "files")) {
            BMessage set(B_SET_PROPERTY), done;
            set.AddInt32("data", 1);
            set.AddSpecifier("Value");
            set.AddSpecifier("View", "extension-files");
            status_t s = manager.SendMessage(&set, &done, kTimeout, kTimeout);
            int32 error = B_OK; done.FindInt32("error", &error);
            std::printf("set files checkbox: %s / %s\n", std::strerror(s), std::strerror(error));
        }
        BMessage approve(summit::kExtensionApprove);
        return manager.SendMessage(&approve, static_cast<BHandler*>(nullptr), kTimeout) == B_OK ? 0 : 5;
    }
    return 2;
}
