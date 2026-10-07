// Verify real MSE playback, decoded canvas frames, seeking, abort and changeType.
#define SUMMIT_EXTENSION_MANAGER_HELPERS_ONLY
#include "ModernExtensionManagerTests.cpp"

int main(int argc, char** argv)
{
    if (argc < 4 || argc > 6) {
        std::fprintf(stderr, "Usage: %s TEAM BROWSER_EXECUTABLE FIXTURE_BASE_URL [FIRST_CASE] [CASE_COUNT]\n", argv[0]);
        return 2;
    }
    BApplication test("application/x-vnd.Summit-StreamingTests");
    team_id team = std::atoi(argv[1]);
    BMessenger app;
    bool owned = false;
    try {
        app_info info;
        Require(Wait([&] {
            app = BMessenger(nullptr, team);
            return app.IsValid() && be_roster->GetRunningAppInfo(team, &info) == B_OK;
        }), "Streaming browser registers");
        BPath executable(&info.ref);
        Require(std::filesystem::canonical(executable.Path()) == std::filesystem::canonical(argv[2]),
            "Streaming test owns the browser executable");
        owned = true;
        BMessenger browser;
        Require(Wait([&] {
            browser = Window(app, 0);
            return browser.IsValid() && Count(State(browser)) > 0;
        }), "Streaming browser window and initial tab are ready");
        auto title = [&] {
            auto state = State(browser);
            BMessage tab;
            state.FindMessage("tab", Index(state, Selected(state)), &tab);
            return String(tab, "title");
        };
        const int firstCase = argc >= 5 ? std::atoi(argv[4]) : 0;
        const int caseCount = argc == 6 ? std::atoi(argv[5]) : 31;
        Require(firstCase >= 0 && firstCase < caseCount && caseCount <= 100, "valid streaming test range");
        bool passed = true;
        for (int i = firstCase; i < caseCount; ++i) {
            auto url = std::string(argv[3]) + "/test.html?case=" + std::to_string(i);
            Send(browser, summit::kNavigate, -1, url);
            Require(Wait([&] { return title() == "Streaming ready " + std::to_string(i); }), "Streaming fixture ready");
            FocusWindow(browser);
            Click(Page(browser, Selected(State(browser))), BPoint(80, 40));
            Require(Wait([&] { return title().starts_with("Streaming_PASS ") || title().starts_with("Streaming_FAIL "); }, 60000000),
                "Streaming playback test finishes");
            std::printf("Streaming_CASE %s\n", title().c_str());
            if (!title().starts_with("Streaming_PASS "))
                passed = false;
            std::fflush(stdout);
        }
        Send(app, B_QUIT_REQUESTED);
        Require(Wait([&] { team_info info; return get_team_info(team, &info) != B_OK; }), "Streaming browser quits normally");
        std::puts(passed ? "Streaming_RESULT PASS" : "Streaming_RESULT FAIL");
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Streaming_FAILURE %s\n", error.what());
        if (owned) Cleanup(app, team);
        return 1;
    }
}
