// Exercise navigation outcomes through the real native browser and HTTP loader.
#define main SummitCloseHarnessEntry
#include "ModernCloseTests.cpp"
#undef main

static BMessage SelectedPageState(const BMessenger& window)
{
    auto state = State(window);
    BMessage tab;
    state.FindMessage("tab", Index(state, Selected(state)), &tab);
    return tab;
}

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--check-executable-unused")
        return SummitCloseHarnessEntry(argc, argv);
    if (argc != 6) return 2;
    status_t status;
    BApplication application("application/x-vnd.Kunanyi-Summit-load-error-tests", &status);
    if (status != B_OK) return 1;
    BMessenger app, window;
    const auto team = static_cast<team_id>(std::strtol(argv[1], nullptr, 10));
    bool verified = false;
    try {
        Require(team > 0, "explicit browser team supplied");
        app_info info;
        Require(Wait([&] {
            app = BMessenger(nullptr, team);
            return app.IsValid() && be_roster->GetRunningAppInfo(team, &info) == B_OK;
        }), "test browser registered with the native roster");
        BPath executable(&info.ref);
        Require(std::filesystem::canonical(executable.Path()) == std::filesystem::canonical(argv[2]),
            "target belongs to the exact frozen browser executable");
        verified = true;
        Require(Wait([&] {
            for (int32 index = 0; index < 8; ++index) {
                auto candidate = Window(app, index);
                if (!candidate.IsValid()) break;
                if (String(State(candidate), "backend") == "modern") { window = candidate; return true; }
            }
            return false;
        }), "native browser exposes modern state");
        const auto url = [&](const std::string& name) {
            return std::string(argv[3]) + "/" + name + "?run=" + argv[4];
        };
        const auto navigate = [&](const std::string& address) {
            Send(window, summit::kNavigate, -1, address);
        };
        const auto succeeds = [&](const std::string& address, const std::string& title) {
            Require(Wait([&] {
                auto page = SelectedPageState(window);
                return String(page, "url") == address && String(page, "title") == title
                    && String(page, "loadOutcome") == "succeeded" && !page.GetBool("loading", true)
                    && !page.GetBool("loadError", true);
            }), "successful navigation and JavaScript document: " + title);
        };
        const auto fails = [&](const std::string& address, bool provisional) {
            Require(Wait([&] {
                auto page = SelectedPageState(window);
                return String(page, "loadOutcome") == "failed" && page.GetBool("loadError", false)
                    && String(page, "loadErrorURL") == address && !page.GetBool("loading", true);
            }), "failed request reaches a terminal native error state");
            auto page = SelectedPageState(window);
            Require(page.GetBool("loadErrorProvisional", !provisional) == provisional,
                provisional ? "failure is before document commit" : "failure is after document commit");
            Require(!String(page, "loadErrorDescription").empty() && !String(page, "loadErrorDomain").empty()
                && page.GetInt32("loadErrorCode", 0) != 0, "native failure retains its actual engine diagnostic");
            Require(String(State(window), "status").find(provisional ? "Could not load" : "Loading was interrupted") != std::string::npos,
                "selected tab displays the failure in native status text");
        };

        navigate(url("good"));
        succeeds(url("good"), "ERROR TEST good");
        navigate(url("drop"));
        fails(url("drop"), true);
        Require(String(SelectedPageState(window), "title") == "ERROR TEST good",
            "provisional failure retains the previous document");
        const auto failedTab = Selected(State(window));
        Send(window, summit::kNewTab, -1, url("good"));
        Require(Wait([&] { return Count(State(window)) == 2 && Selected(State(window)) != failedTab; }),
            "a second tab opens independently of the failed request");
        succeeds(url("good"), "ERROR TEST good");
        const auto healthyTab = Selected(State(window));
        Require(String(State(window), "status").find("Could not load") == std::string::npos,
            "the healthy tab does not inherit another tab's error");
        SelectTab(window, failedTab);
        Require(SelectedPageState(window).GetBool("loadError", false)
            && String(State(window), "status").find("Could not load") != std::string::npos,
            "selecting the failed tab restores its error status");
        Send(window, summit::kCloseTab, healthyTab);
        Require(Wait([&] { return Count(State(window)) == 1 && Selected(State(window)) == failedTab; }),
            "closing the healthy background tab preserves the failed page");

        navigate(url("retry"));
        fails(url("retry"), true);
        Send(window, summit::kFocusAddress);
        auto address = Child(View(window, "address"), 0);
        Require(address.IsValid(), "native address editor is available for retry");
        Key(address, url("retry"));
        Key(address, "\n", 0x47);
        succeeds(url("retry"), "ERROR TEST retry");

        navigate(url("reload-failure"));
        succeeds(url("reload-failure"), "ERROR TEST reload-failure");
        Send(window, summit::kReload);
        Require(Wait([&] {
            auto page = SelectedPageState(window);
            return String(page, "title") == "ERROR TEST partial-reload" && page.GetBool("loading", false);
        }), "failed reload receives a new title before its connection fails");
        fails(url("reload-failure"), false);

        navigate(url("partial"));
        Require(Wait([&] {
            auto page = SelectedPageState(window);
            return String(page, "title") == "ERROR TEST partial" && page.GetBool("loading", false);
        }), "truncated main resource commits and executes before the connection closes");
        fails(url("partial"), false);

        navigate(url("missing"));
        succeeds(url("missing"), "ERROR TEST missing");
        navigate(url("slow"));
        Require(Wait([&] {
            auto page = SelectedPageState(window);
            return String(page, "url") == url("slow") && page.GetBool("loading", false);
        }), "slow request starts loading");
        // Loading is reported before the network process dispatches the HTTP
        // request. The host independently requires actual fixture receipt.
        snooze(1000000);
        Send(window, summit::kReload); // The loading toolbar action is Stop.
        Require(Wait([&] {
            auto page = SelectedPageState(window);
            return String(page, "loadOutcome") == "cancelled" && !page.GetBool("loadError", true)
                && !page.GetBool("loading", true);
        }), "Stop finishes without a spurious error");

        navigate(url("same"));
        Require(Wait([&] {
            auto page = SelectedPageState(window);
            return String(page, "url") == url("same") && page.GetBool("loading", false);
        }), "first same-URL request is pending");
        snooze(1000000);
        navigate(url("same"));
        succeeds(url("same"), "ERROR TEST same");
        snooze(2500000);
        succeeds(url("same"), "ERROR TEST same");

        navigate(url("good"));
        succeeds(url("good"), "ERROR TEST good");
        navigate(url("good") + "#fragment");
        succeeds(url("good") + "#fragment", "ERROR TEST good");
        navigate(url("good") + "#superseded");
        navigate(url("drop"));
        fails(url("drop"), true);
        navigate(url("good") + "#fragment");
        succeeds(url("good") + "#fragment", "ERROR TEST good");
        navigate(url("final"));
        succeeds(url("final"), "ERROR TEST final");
        Send(window, summit::kBack);
        succeeds(url("good") + "#fragment", "ERROR TEST good");
        Send(window, summit::kForward);
        succeeds(url("final"), "ERROR TEST final");
        Send(app, B_QUIT_REQUESTED);
        Require(Wait([&] { team_info current; return get_team_info(team, &current) != B_OK; }),
            "browser exits normally after failure, cancellation and recovery");

        std::ifstream stream(std::filesystem::path(argv[5]) / "profile.json");
        const json profile = json::parse(stream, nullptr, false);
        Require(profile.is_object() && profile.contains("history") && profile["history"].is_array(),
            "browser saved its real history");
        const auto hasHistory = [&](const std::string& address) {
            for (const auto& page : profile["history"])
                if (page.value("url", std::string()) == address) return true;
            return false;
        };
        for (const auto& name : { "drop", "partial", "slow" })
            Require(!hasHistory(url(name)), "failed or stopped request is absent from history: " + std::string(name));
        for (const auto& name : { "good", "retry", "reload-failure", "missing", "same", "final" })
            Require(hasHistory(url(name)), "successful document is present in history: " + std::string(name));
        for (const auto& page : profile["history"])
            if (page.value("url", std::string()) == url("reload-failure"))
                Require(page.value("title", std::string()) == "ERROR TEST reload-failure",
                    "failed reload does not overwrite the previous successful history title");
        Require(hasHistory(url("good") + "#fragment"), "same-document navigation is present in history");
        std::printf("LOAD_ERROR_RESULT PASS checks=%d\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "LOAD_ERROR_RESULT FAIL: %s\n", error.what());
        if (window.IsValid()) State(window).PrintToStream();
        if (verified) Cleanup(app, team);
        return 1;
    }
}
