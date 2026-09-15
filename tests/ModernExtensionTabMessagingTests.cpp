// Exercise installed backgrounds and content scripts in a real native browser.
#define SUMMIT_EXTENSION_OVERFLOW_HELPERS_ONLY
#include "ModernExtensionOverflowTests.cpp"

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--check-executable-unused") return SummitCloseHarnessEntry(argc, argv);
    if (argc != 7) return 2;
    status_t status;
    BApplication test("application/x-vnd.Kunanyi-Summit-tab-messaging-tests", &status);
    if (status != B_OK) return 1;
    const auto team = static_cast<team_id>(std::strtol(argv[1], nullptr, 10));
    BMessenger app;
    bool verified = false;
    try {
        app_info info;
        Require(Wait([&] { app = BMessenger(nullptr, team); return app.IsValid() && be_roster->GetRunningAppInfo(team, &info) == B_OK; }), "owned messaging browser registers");
        BPath executable(&info.ref);
        Require(std::filesystem::canonical(executable.Path()) == std::filesystem::canonical(argv[2]), "messaging browser matches frozen executable");
        verified = true;
        const std::filesystem::path control(argv[4]);
        const auto expected = ReadJSON(control / "expected.json");
        Require(expected.is_object() && expected.at("initial").size() >= 40, "independent messaging expectations are present");
        auto window = Window(app, 0);
        const auto loaded = [&](const std::string& url) {
            const auto state = State(window); BMessage tab;
            auto index = Index(state, Selected(state));
            return index >= 0 && state.FindMessage("tab", index, &tab) == B_OK
                && String(tab, "url") == url && !tab.GetBool("loading", true)
                && String(tab, "loadOutcome") == "succeeded";
        };
        const std::string target(argv[5]);
        Require(Wait([&] { return loaded(target + "/setup"); }), "setup page loads before installation");
        BMessage open(B_EXECUTE_PROPERTY); open.AddSpecifier("MenuItem", "Extensions…"); open.AddSpecifier("Menu", "Window");
        open.AddSpecifier("View", "menu"); open.AddSpecifier("Window", int32(0));
        Require(Query(app, open).GetInt32("error", B_ERROR) == B_OK, "open native extension manager");
        BMessenger manager;
        Require(Wait([&] { manager = Manager(app); return manager.IsValid() && Enabled(manager, "extension-add"); }), "manager is ready for messaging fixtures");
        InstallFolder(app, manager, argv[3], 1);
        InstallFolder(app, manager, argv[6], 2);
        Send(manager, B_QUIT_REQUESTED);
        Require(Wait([&] { return !manager.IsValid(); }), "manager closes after installing both extensions");
        const auto a = Selected(State(window));
        Send(window, summit::kNavigate, -1, target + "/a/main");
        Require(Wait([&] { return loaded(target + "/a/main"); }), "tab A loads main frame and child frame");
        Send(window, summit::kNewTab, -1, target + "/b/main");
        Require(Wait([&] { return Count(State(window)) == 2 && loaded(target + "/b/main"); }), "tab B loads independently");
        const auto b = Selected(State(window));
        const auto verifyPhase = [&](const std::string& phase) {
            json report, failure;
            Require(Wait([&] {
                report = ReadJSON(control / (phase + ".json"));
                failure = ReadJSON(control / "failure.json");
                return report.is_object() || failure.is_object();
            }), phase + " produces a real extension report");
            Require(!failure.is_object(), phase + " completes: " + (failure.is_object() ? failure.value("fatal", "unknown failure") : ""));
            Require(report.at("phase") == phase && report.at("runtimeId") == "summit-tab-messaging", phase + " has the expected extension identity");
            const auto& actual = report.at("cases");
            const auto& wanted = expected.at(phase);
            Require(actual.is_object() && actual.size() == wanted.size(), phase + " executes all expected cases");
            for (auto item = wanted.begin(); item != wanted.end(); ++item)
                Require(actual.contains(item.key()) && actual.at(item.key()) == item.value(), phase + "/" + item.key());
        };
        verifyPhase("initial");
        SelectTab(window, a);
        Send(window, summit::kNavigate, -1, target + "/empty");
        Require(Wait([&] { return loaded(target + "/empty"); }), "navigation removes original content documents");
        Send(window, summit::kNavigate, -1, target + "/a/main");
        Require(Wait([&] { return loaded(target + "/a/main"); }), "replacement documents load in the same tab");
        verifyPhase("navigated");
        Send(window, summit::kCloseTab, b);
        Require(Wait([&] { return Count(State(window)) == 1 && Index(State(window), b) < 0; }), "native browser closes tab B");
        Send(window, summit::kNavigate, -1, target + "/empty");
        Require(Wait([&] { return loaded(target + "/empty"); }), "tab A navigates to a document without listeners");
        std::ofstream(control / "command.json") << "{\"stage\":\"closed\"}\n";
        verifyPhase("complete");
        Send(app, B_QUIT_REQUESTED);
        Require(Wait([&] { team_info info; return get_team_info(team, &info) == B_BAD_TEAM_ID; }), "messaging browser quits cleanly");
        std::printf("EXTENSION_TAB_MESSAGING_RESULT PASS checks=%d\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "EXTENSION_TAB_MESSAGING_RESULT FAIL %s checks=%d\n", error.what(), checks);
        if (verified) Cleanup(app, team);
        return 1;
    }
}
