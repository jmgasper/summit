// Install through the native UI and inspect reports from real extension worlds.
#define SUMMIT_EXTENSION_OVERFLOW_HELPERS_ONLY
#include "ModernExtensionOverflowTests.cpp"
#include <Window.h>

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--check-executable-unused") return SummitCloseHarnessEntry(argc, argv);
    if (argc != 7) return 2;
    status_t status;
    BApplication test("application/x-vnd.Kunanyi-Summit-i18n-tests", &status);
    if (status != B_OK) return 1;
    const auto team = static_cast<team_id>(std::strtol(argv[1], nullptr, 10));
    BMessenger app;
    bool verified = false;
    try {
        app_info info;
        Require(Wait([&] { app = BMessenger(nullptr, team); return app.IsValid() && be_roster->GetRunningAppInfo(team, &info) == B_OK; }), "owned localization browser registers");
        BPath executable(&info.ref);
        Require(std::filesystem::canonical(executable.Path()) == std::filesystem::canonical(argv[2]), "localization browser matches frozen executable");
        verified = true;
        const std::filesystem::path control(argv[4]);
        const auto expected = ReadJSON(control / "expected.json");
        Require(expected.is_object() && expected.size() >= 70, "independent expected localization cases are present");
        auto window = Window(app, 0);
        const auto loaded = [&](const std::string& url) {
            const auto state = State(window); BMessage tab;
            auto index = Index(state, Selected(state));
            return index >= 0 && state.FindMessage("tab", index, &tab) == B_OK
                && String(tab, "url") == url && !tab.GetBool("loading", true)
                && String(tab, "loadOutcome") == "succeeded" && tab.GetUInt64("loadSuccessSequence", 0) > 0;
        };
        Require(Wait([&] { return loaded(std::string(argv[5]) + "/setup"); }), "setup page finishes loading before installation");
        BMessage open(B_EXECUTE_PROPERTY); open.AddSpecifier("MenuItem", "Extensions…"); open.AddSpecifier("Menu", "Window");
        open.AddSpecifier("View", "menu"); open.AddSpecifier("Window", int32(0));
        Require(Query(app, open).GetInt32("error", B_ERROR) == B_OK, "open native extension manager");
        BMessenger manager;
        Require(Wait([&] { manager = Manager(app); return manager.IsValid() && Enabled(manager, "extension-add"); }), "manager is ready for localized extension");
        InstallFolder(app, manager, argv[3], 1);
        Send(manager, B_QUIT_REQUESTED);
        Require(Wait([&] { return !manager.IsValid(); }), "manager closes after localization fixture installation");
        const auto verifyWorld = [&](const std::string& world, const json& expectedCases, const std::string& identity) {
            json report;
            Require(Wait([&] { report = ReadJSON(control / (world + ".json")); return report.is_object(); }), world + " produces a browser runtime report");
            Require(!report.contains("fatal"), world + " completes: " + report.value("fatal", ""));
            Require(report.at("world") == world && report.at("runtimeId") == identity, world + " has the expected extension identity");
            const auto& actual = report.at("cases");
            Require(actual.is_object() && actual.size() == expectedCases.size(), world + " executes every expected case exactly once");
            for (auto item = expectedCases.begin(); item != expectedCases.end(); ++item) {
                const auto observed = actual.find(item.key());
                Require(observed != actual.end() && *observed == item.value(), world + "/" + item.key()
                    + " expected=" + item.value().dump() + " actual=" + (observed == actual.end() ? "missing" : observed->dump()));
            }
            if (world != "unlocalized")
                Require(report.at("locale") == "en", world + " uses the installed default catalog");
        };
        verifyWorld("background", expected, "summit-i18n");
        constexpr auto actionName = "extension-action-summit-i18n";
        Require(Wait([&] { return String(Property(View(window, actionName), "Label"), "result") == "Localized popup"; }), "native action title comes from localized manifest");
        Send(window, summit::kNavigate, -1, std::string(argv[5]) + "/test");
        Require(Wait([&] { return loaded(std::string(argv[5]) + "/test"); }), "test page loads with installed content script and stylesheet");
        verifyWorld("content", expected, "summit-i18n");
        FocusWindow(window);
        Button(window, actionName);
        Require(Wait([&] { const auto popup = ActionPopup(app); return popup.IsValid() && !Property(popup, "Hidden").GetBool("result", true); }), "localized popup is visible in native browser");
        verifyWorld("popup", expected, "summit-i18n");
        Send(ActionPopup(app), B_QUIT_REQUESTED);
        Require(Wait([&] { return !ActionPopup(app).IsValid(); }), "localized popup closes cleanly");
        Require(Query(app, open).GetInt32("error", B_ERROR) == B_OK, "open manager for extension without a translation catalog");
        Require(Wait([&] { manager = Manager(app); return manager.IsValid() && Enabled(manager, "extension-add"); }), "manager is ready for unlocalized extension");
        InstallFolder(app, manager, argv[6], 2);
        const auto unlocalizedExpected = ReadJSON(control / "unlocalized-expected.json");
        Require(unlocalizedExpected.is_object() && unlocalizedExpected.size() == 13, "independent unlocalized expectations are present");
        verifyWorld("unlocalized", unlocalizedExpected, "summit-i18n-unlocalized");
        Send(manager, B_QUIT_REQUESTED);
        Require(Wait([&] { return !manager.IsValid(); }), "manager closes after unlocalized extension checks");
        Send(app, B_QUIT_REQUESTED);
        Require(Wait([&] { team_info info; return get_team_info(team, &info) == B_BAD_TEAM_ID; }), "localization browser quits cleanly");
        std::printf("EXTENSION_LOCALIZATION_RESULT PASS checks=%d worlds=3 unlocalized=1\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "EXTENSION_LOCALIZATION_RESULT FAIL %s checks=%d\n", error.what(), checks);
        if (verified) Cleanup(app, team);
        return 1;
    }
}
