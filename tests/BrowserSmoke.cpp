#include "ui/Messages.h"
#include <Application.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <Message.h>
#include <Messenger.h>
#include <OS.h>
#include <Path.h>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <climits>
#include <cstring>
#include <filesystem>
#include <fstream>
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
int main(int argc, char** argv)
{
    team_id team = -1;
    const char* expectedBackend = nullptr;
    bool navigationOnly = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--navigation-only")) {
            navigationOnly = true;
        } else if (!std::strcmp(argv[i], "--team") && i + 1 < argc) {
            char* end = nullptr;
            errno = 0;
            long parsed = std::strtol(argv[++i], &end, 10);
            if (errno || !*argv[i] || *end || parsed <= 0 || parsed > INT_MAX) {
                std::fputs("Invalid native browser team ID\n", stderr);
                return 2;
            }
            team = static_cast<team_id>(parsed);
        } else if (!std::strcmp(argv[i], "--backend") && i + 1 < argc
            && (!std::strcmp(argv[i + 1], "modern") || !std::strcmp(argv[i + 1], "legacy"))) {
            expectedBackend = argv[++i];
        } else {
            std::fputs("Usage: BrowserSmoke [--team ID] [--backend modern|legacy] [--navigation-only]\n", stderr);
            return 2;
        }
    }
    if (expectedBackend && team < 0) {
        std::fputs("Backend-specific smoke tests require an explicit --team ID\n", stderr);
        return 2;
    }
    status_t status = B_NO_INIT;
    BApplication application("application/x-vnd.Kunanyi-Summit-smoke", &status);
    if (status != B_OK) {
        std::fprintf(stderr, "FAIL initialize native smoke application: %s\n", std::strerror(status));
        return 1;
    }
    BMessenger app("application/x-vnd.Kunanyi-Summit", team);
    Check(app.IsValid(), "native browser is running");
    if (!app.IsValid()) return 1;
    BMessage request(B_GET_PROPERTY), reply;
    request.AddSpecifier("Window", int32(0));
    BMessenger window;
    app.SendMessage(&request, &reply, 1000000, 1000000);
    Check(reply.FindMessenger("result", &window) == B_OK && window.IsValid(), "window responds through native scripting");
    if (!window.IsValid()) return 1;
    auto initial = State(window);
    if (expectedBackend) {
        const char* backend = nullptr;
        bool matches = app.Team() == team && initial.FindString("backend", &backend) == B_OK
            && !std::strcmp(backend, expectedBackend);
        Check(matches, "exact browser team reports the required engine backend");
        if (!matches) return 1;
    }
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
    char localDirectory[] = "/tmp/summit-browser-XXXXXX";
    if (!mkdtemp(localDirectory)) { Check(false, "create local document fixture"); return 1; }
    const auto localPath = std::filesystem::path(localDirectory) / "page #?% 雪.html";
    { std::ofstream file(localPath); file << "<!doctype html><title>Summit local document</title><p>Local file opened.</p>"; }
    entry_ref localRef;
    const bool hasLocalFile = get_ref_for_path(localPath.c_str(), &localRef) == B_OK;
    Check(hasLocalFile, "create local document with URL delimiters and Unicode in its filename");
    if (hasLocalFile) {
        Send(window, summit::kNavigate, localPath.c_str());
        Check(Wait(window, [](const BMessage& s) { return Title(s) == "Summit local document"; }), "native path navigation opens the exact local filename");
        BMessage refs(B_REFS_RECEIVED);
        refs.AddRef("refs", &localRef);
        window.SendMessage(&refs);
        Check(Wait(window, [&](const BMessage& s) { return Count(s) == count + 1 && Title(s) == "Summit local document"; }), "file panel references open the exact local filename in a new tab");
        Send(window, summit::kCloseTab);
        Check(Wait(window, [&](const BMessage& s) { return Count(s) == count; }), "closing a local document keeps the window responsive");
    }
    Send(window, summit::kNavigate, "http://10.0.2.2:8765/basic");
    Wait(window, [](const BMessage& s) { return Title(s) == "Summit fixture PASS"; });
    std::filesystem::remove_all(localDirectory);
    if (!navigationOnly) {
        BPath userDirectory;
        const bool hasUserDirectory = find_directory(B_USER_DIRECTORY, &userDirectory) == B_OK;
        Check(hasUserDirectory, "locate the native Downloads folder");
        if (hasUserDirectory) {
            const std::string token = std::to_string(find_thread(nullptr)) + "-" + std::to_string(system_time());
            const auto downloadPath = std::filesystem::path(userDirectory.Path()) / "Downloads" / ("summit-test-" + token + ".txt");
            const bool unusedFilename = !std::filesystem::exists(downloadPath);
            Check(unusedFilename, "download fixture has a unique destination");
            if (unusedFilename) {
                const std::string url = "http://10.0.2.2:8765/download?token=" + token;
                Send(window, summit::kNavigate, url.c_str());
                Check(Wait(window, [&](const BMessage&) {
                    std::ifstream file(downloadPath, std::ios::binary);
                    return std::string(std::istreambuf_iterator<char>(file), {}) == "Summit download fixture\n";
                }), "HTTP attachment saves its exact contents in Downloads");
                Check(Wait(window, [](const BMessage& s) {
                    const char* engine = "";
                    const char* status = "";
                    if (s.FindString("haiku_webkit", &engine) != B_OK || s.FindString("status", &status) != B_OK) return false;
                    const bool reportsCompletion = std::string(engine).find("+summit.") != std::string::npos;
                    return std::string(status) == (reportsCompletion
                        ? "Download complete — open Downloads to view the file"
                        : "Download ended — open Downloads to view the file");
                }), "download notification reflects the engine's available completion information");
                std::filesystem::remove(downloadPath);
            }
        }
        Send(window, summit::kNavigate, "http://10.0.2.2:8765/basic");
        Check(Wait(window, [](const BMessage& s) { return Title(s) == "Summit fixture PASS"; }), "browsing remains usable after a download");
    } else {
        std::puts("SKIP downloads: explicit navigation-only test scope");
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
