// Install real extensions after loading the target documents, then verify injection.
#define SUMMIT_EXTENSION_OVERFLOW_HELPERS_ONLY
#include "ModernExtensionOverflowTests.cpp"

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--check-executable-unused") return SummitCloseHarnessEntry(argc, argv);
    if (argc != 8) return 2;
    status_t status;
    BApplication test("application/x-vnd.Kunanyi-Summit-tab-script-tests", &status);
    if (status != B_OK) return 1;
    const auto team = static_cast<team_id>(std::strtol(argv[1], nullptr, 10));
    BMessenger app;
    bool verified = false;
    try {
        app_info info;
        Require(Wait([&] { app = BMessenger(nullptr, team); return app.IsValid() && be_roster->GetRunningAppInfo(team, &info) == B_OK; }), "owned script browser registers");
        BPath executable(&info.ref);
        Require(std::filesystem::canonical(executable.Path()) == std::filesystem::canonical(argv[2]), "script browser matches frozen executable");
        verified = true;
        const std::filesystem::path control(argv[4]);
        const auto command = [&](const char* stage) {
            const auto temporary = control / "command.tmp";
            std::ofstream output(temporary);
            output << json{{"stage", stage}}.dump() << '\n';
            output.close();
            Require(output.good(), std::string("write extension command: ") + stage);
            std::filesystem::rename(temporary, control / "command.json");
        };
        const auto expected = ReadJSON(control / "expected.json");
        Require(expected.is_object() && expected.at("initial").size() >= 60, "independent injection expectations are present");
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
        const auto a = Selected(State(window));
        Send(window, summit::kNavigate, -1, target + "/a/main");
        Require(Wait([&] { return loaded(target + "/a/main"); }), "tab A and its frames load before extension installation");
        Send(window, summit::kNewTab, -1, target + "/b/main");
        Require(Wait([&] { return Count(State(window)) == 2 && loaded(target + "/b/main"); }), "tab B loads before extension installation");
        const auto b = Selected(State(window));
        BMessage open(B_EXECUTE_PROPERTY); open.AddSpecifier("MenuItem", "Extensions…"); open.AddSpecifier("Menu", "Window");
        open.AddSpecifier("View", "menu"); open.AddSpecifier("Window", int32(0));
        Require(Query(app, open).GetInt32("error", B_ERROR) == B_OK, "open native extension manager");
        BMessenger manager;
        Require(Wait([&] { manager = Manager(app); return manager.IsValid() && Enabled(manager, "extension-add"); }), "manager is ready for injection fixtures");
        InstallFolder(app, manager, argv[3], 1);
        InstallFolder(app, manager, argv[6], 2);
        InstallFolder(app, manager, argv[7], 3);
        Send(manager, B_QUIT_REQUESTED);
        Require(Wait([&] { return !manager.IsValid(); }), "manager closes after installing three extensions");
        // Omitted tab IDs must resolve against the ordinary selected tab.
        FocusWindow(window);
        SelectTab(window, b);
        command("initial");
        const auto verifyPhase = [&](const std::string& phase) {
            json report, failure;
            Require(Wait([&] {
                report = ReadJSON(control / (phase + ".json"));
                failure = ReadJSON(control / "failure.json");
                return report.is_object() || failure.is_object();
            }, 90000000), phase + " produces a real extension report");
            Require(!failure.is_object(), phase + " completes: " + (failure.is_object() ? failure.value("fatal", "unknown failure") : ""));
            Require(report.at("phase") == phase && report.at("runtimeId") == "summit-tab-script", phase + " has the expected extension identity");
            const auto& actual = report.at("cases");
            const auto& wanted = expected.at(phase);
            Require(actual.is_object() && actual.size() == wanted.size(), phase + " executes all expected cases");
            for (auto item = wanted.begin(); item != wanted.end(); ++item)
                Require(actual.contains(item.key()) && actual.at(item.key()) == item.value(), phase + "/" + item.key());
        };
        verifyPhase("initial");
        SelectTab(window, a);
        Send(window, summit::kNavigate, -1, target + "/loading");
        Require(Wait([&] { const auto held = ReadJSON(control / "parser-held.json"); return held.is_object() && held.value("held", false); }), "timing page blocks its parser on a real pending resource");
        command("timing");
        verifyPhase("timing");
        Require(Wait([&] { return loaded(target + "/loading"); }), "timing page completes after extension releases its resources");
        Send(window, summit::kNavigate, -1, target + "/empty");
        Require(Wait([&] { return loaded(target + "/empty"); }), "navigation removes the original injected documents");
        Send(window, summit::kNavigate, -1, target + "/a/main");
        Require(Wait([&] { return loaded(target + "/a/main"); }), "replacement documents load at the same URL");
        command("navigated");
        verifyPhase("navigated");
        Send(window, summit::kCloseTab, b);
        Require(Wait([&] { return Count(State(window)) == 1 && Index(State(window), b) < 0; }), "native browser closes tab B");
        Send(window, summit::kNavigate, -1, "about:blank");
        Require(Wait([&] { return loaded("about:blank"); }), "tab A navigates to a top-level blank document");
        command("complete");
        verifyPhase("complete");
        Send(app, B_QUIT_REQUESTED);
        Require(Wait([&] { team_info info; return get_team_info(team, &info) == B_BAD_TEAM_ID; }), "script browser quits cleanly");
        std::printf("EXTENSION_TAB_SCRIPT_RESULT PASS checks=%d\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "EXTENSION_TAB_SCRIPT_RESULT FAIL %s checks=%d\n", error.what(), checks);
        if (verified) Cleanup(app, team);
        return 1;
    }
}
