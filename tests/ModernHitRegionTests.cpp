// Drive hit-region checks with real native mouse input into the owned browser.
#define SUMMIT_EXTENSION_MANAGER_HELPERS_ONLY
#include "ModernExtensionManagerTests.cpp"

int main(int argc, char** argv)
{
    if (argc != 4) {
        std::fprintf(stderr, "Usage: %s TEAM BROWSER_EXECUTABLE FIXTURE_URL\n", argv[0]);
        return 2;
    }
    BApplication test("application/x-vnd.Summit-HitRegionTests");
    team_id team = std::atoi(argv[1]);
    BMessenger app;
    bool owned = false;
    try {
        app_info info;
        Require(Wait([&] {
            app = BMessenger(nullptr, team);
            return app.IsValid() && be_roster->GetRunningAppInfo(team, &info) == B_OK;
        }), "Hit-region browser registers");
        BPath executable(&info.ref);
        Require(std::filesystem::canonical(executable.Path()) == std::filesystem::canonical(argv[2]),
            "Hit-region test owns the browser executable");
        owned = true;
        BMessenger browser;
        Require(Wait([&] {
            browser = Window(app, 0);
            return browser.IsValid() && Count(State(browser)) > 0;
        }), "Hit-region browser window and initial tab are ready");
        auto title = [&] {
            auto state = State(browser);
            BMessage tab;
            state.FindMessage("tab", Index(state, Selected(state)), &tab);
            return String(tab, "title");
        };
        Send(browser, summit::kNavigate, -1, argv[3]);
        auto readyTitle = [](const std::string& value) {
            return value.starts_with("HitRegion_READY ") || value.starts_with("HitRegion_PASS ") || value.starts_with("HitRegion_FAIL ");
        };
        std::string current;
        Require(Wait([&] { current = title(); return readyTitle(current); }), "Hit-region fixture ready");
        BMessenger page;
        Require(Wait([&] {
            auto selected = Selected(State(browser));
            if (selected < 0) return false;
            page = Page(browser, selected);
            return page.IsValid();
        }), "native canvas page messenger is ready");
        FocusWindow(browser);
        unsigned steps = 0;
        for (;;) {
            std::printf("HitRegion_STEP %s\n", current.c_str()); std::fflush(stdout);
            if (current.starts_with("HitRegion_PASS") || current.starts_with("HitRegion_FAIL"))
                break;
            int sequence, x, y;
            Require(std::sscanf(current.c_str(), "HitRegion_READY %d %d %d", &sequence, &x, &y) == 3,
                "fixture requests a native click");
            Require(sequence == static_cast<int>(++steps) && steps <= 150 && x >= 0 && y >= 0 && x < 2000 && y < 1000,
                "native click request is bounded and ordered");
            Click(page, BPoint(x, y));
            Require(Wait([&] {
                auto updated = title();
                // A native state query can time out while the window paints.
                // An empty response is not the fixture's next instruction.
                if (!readyTitle(updated) || updated == current) return false;
                current = std::move(updated);
                return true;
            }, 20000000), "native hit-region event delivered");
        }
        bool passed = current.starts_with("HitRegion_PASS");
        std::printf("HitRegion_CASE %s; %u native clicks\n", current.c_str(), steps);
        Send(app, B_QUIT_REQUESTED);
        Require(Wait([&] { team_info info; return get_team_info(team, &info) != B_OK; }), "Hit-region browser quits normally");
        std::puts(passed ? "HitRegion_RESULT PASS" : "HitRegion_RESULT FAIL");
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "HitRegion_FAILURE %s\n", error.what());
        if (owned) Cleanup(app, team);
        return 1;
    }
}
