// Verify real AAC source selection, native audio output, seeking and completion.
#define SUMMIT_EXTENSION_MANAGER_HELPERS_ONLY
#include "ModernExtensionManagerTests.cpp"

int main(int argc, char** argv)
{
    if (argc != 4 && argc != 5) {
        std::fprintf(stderr, "Usage: %s TEAM BROWSER_EXECUTABLE FIXTURE_BASE_URL [FIRST_CASE]\n", argv[0]);
        return 2;
    }
    BApplication test("application/x-vnd.Summit-AACTests");
    team_id team = std::atoi(argv[1]);
    BMessenger app;
    bool owned = false;
    try {
        app_info info;
        Require(Wait([&] {
            app = BMessenger(nullptr, team);
            return app.IsValid() && be_roster->GetRunningAppInfo(team, &info) == B_OK;
        }), "AAC browser registers");
        BPath executable(&info.ref);
        Require(std::filesystem::canonical(executable.Path()) == std::filesystem::canonical(argv[2]),
            "AAC test owns the browser executable");
        owned = true;
        auto browser = Window(app, 0);
        auto title = [&] {
            auto state = State(browser);
            BMessage tab;
            state.FindMessage("tab", Index(state, Selected(state)), &tab);
            return String(tab, "title");
        };
        const int firstCase = argc == 5 ? std::atoi(argv[4]) : 0;
        Require(firstCase >= 0 && firstCase < 12, "valid first audio test case");
        for (int i = firstCase; i < 12; ++i) {
            auto url = std::string(argv[3]) + "/test.html?case=" + std::to_string(i);
            Send(browser, summit::kNavigate, -1, url);
            Require(Wait([&] { return title() == "AAC ready " + std::to_string(i); }), "AAC fixture ready");
            FocusWindow(browser);
            Click(Page(browser, Selected(State(browser))), BPoint(80, 40));
            Require(Wait([&] { return title().starts_with("AAC_PASS ") || title().starts_with("AAC_FAIL "); }, 60000000),
                "AAC playback test finishes");
            std::printf("AAC_CASE %s\n", title().c_str());
            Require(title().starts_with("AAC_PASS "), "AAC source plays, pauses, seeks and ends without errors");
        }
        Send(app, B_QUIT_REQUESTED);
        Require(Wait([&] { team_info info; return get_team_info(team, &info) != B_OK; }), "AAC browser quits normally");
        std::puts("AAC_RESULT PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "AAC_FAILURE %s\n", error.what());
        if (owned) Cleanup(app, team);
        return 1;
    }
}
