// Verify real content/background messaging after native disable and reenable.
#define SUMMIT_EXTENSION_OVERFLOW_HELPERS_ONLY
#include "ModernExtensionOverflowTests.cpp"

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--check-executable-unused") return SummitCloseHarnessEntry(argc, argv);
    if (argc != 7) return 2;
    status_t status;
    BApplication test("application/x-vnd.Kunanyi-Summit-messaging-lifecycle-tests", &status);
    if (status != B_OK) return 1;
    const auto team = static_cast<team_id>(std::strtol(argv[1], nullptr, 10));
    const std::filesystem::path control(argv[4]), profile(argv[6]);
    const std::string target(argv[5]);
    BMessenger app, manager;
    bool verified = false;
    try {
        app_info info;
        Require(Wait([&] { app = BMessenger(nullptr, team); return app.IsValid() && be_roster->GetRunningAppInfo(team, &info) == B_OK; }), "owned lifecycle browser registers");
        BPath executable(&info.ref);
        Require(std::filesystem::canonical(executable.Path()) == std::filesystem::canonical(argv[2]), "lifecycle browser matches frozen executable");
        verified = true;
        auto window = Window(app, 0);
        const auto loaded = [&](const std::string& url) {
            const auto state = State(window); BMessage tab;
            auto index = Index(state, Selected(state));
            return index >= 0 && state.FindMessage("tab", index, &tab) == B_OK
                && String(tab, "url") == url && !tab.GetBool("loading", true)
                && String(tab, "loadOutcome") == "succeeded";
        };
        const auto navigate = [&](const std::string& path) {
            Send(window, summit::kNavigate, -1, target + "/" + path);
            Require(Wait([&] { return loaded(target + "/" + path); }), path + " loads");
        };
        Require(Wait([&] { return loaded(target + "/setup"); }), "setup loads before installation");
        BMessage open(B_EXECUTE_PROPERTY); open.AddSpecifier("MenuItem", "Extensions…"); open.AddSpecifier("Menu", "Window");
        open.AddSpecifier("View", "menu"); open.AddSpecifier("Window", int32(0));
        const auto openManager = [&] {
            Require(Query(app, open).GetInt32("error", B_ERROR) == B_OK, "open native extension manager");
            Require(Wait([&] { manager = Manager(app); return manager.IsValid() && Enabled(manager, "extension-add"); }), "manager is ready");
        };
        const auto closeManager = [&] {
            Send(manager, B_QUIT_REQUESTED);
            Require(Wait([&] { return !manager.IsValid(); }), "manager closes");
        };
        openManager();
        InstallFolder(app, manager, argv[3], 1);
        const auto catalogPath = profile / "Extensions/catalog.json";
        auto catalog = ReadJSON(catalogPath);
        Require(catalog.is_object() && catalog.at("extensions").size() == 1, "fixture is the sole installed extension");
        const auto originalEntry = catalog.at("extensions")[0];
        std::ofstream(control / "installed.json") << originalEntry.dump(2) << '\n';
        closeManager();
        std::string previousStartup;
        for (const auto& phase : {"initial", "reenabled", "again", "third", "fourth"}) {
            if (std::string(phase) != "initial") {
                for (bool enabled : {false, true}) {
                    openManager(); FocusWindow(manager); Button(manager, "extension-toggle");
                    Require(Wait([&] {
                        auto current = ReadJSON(catalogPath);
                        return current.is_object() && current.at("extensions")[0].at("enabled") == enabled
                            && Enabled(manager, "extension-add")
                            && Text(manager, "extension-details").find(enabled ? "\nRunning\n" : "\nDisabled\n") != std::string::npos;
                    }), enabled ? "reenable finishes with a running extension" : "disable finishes with an unloaded extension");
                    auto entry = ReadJSON(catalogPath).at("extensions")[0];
                    Require(entry.at("identifier") == originalEntry.at("identifier") && entry.at("fingerprint") == originalEntry.at("fingerprint"), "toggle preserves package identity");
                    closeManager();
                    if (!enabled) navigate("disabled");
                }
            }
            navigate(std::string(phase) + "/main");
            json result;
            Require(Wait([&] {
                result = ReadJSON(control / (std::string(phase) + ".json"));
                if (!result.is_object()) return false;
                if (result.contains("failure")) return true;
                for (const auto& frame : {"main", "child"})
                    if (!result.contains(frame) || !result.at(frame).contains("routing") || !result.at(frame).contains("handshake")) return false;
                return true;
            }), std::string(phase) + " receives both frame reports and handshake callbacks");
            Require(!result.contains("failure"), std::string(phase) + " background completes all routing attempts");
            std::string startup;
            for (const auto& frame : {"main", "child"}) {
                const auto& routing = result.at(frame).at("routing");
                const auto& handshake = result.at(frame).at("handshake");
                Require(routing.at("runtimeId") == "summit-messaging-lifecycle" && handshake.at("runtimeId") == "summit-messaging-lifecycle", "reports have the installed extension identity");
                Require(routing.at("senderURL") == target + "/" + phase + "/" + frame, "sender identifies the actual document URL");
                Require(routing.at("documentId").is_string() && routing.at("documentId").get<std::string>().size() == 36, "sender supplies a document UUID");
                Require((routing.at("frameId") == 0) == (std::string(frame) == "main"), "sender distinguishes main and child frames");
                for (const auto& route : {"document", "frame", "combined", "callback"})
                    Require(routing.at("routes").at(route) == true, std::string(phase) + "/" + frame + "/" + route + ": " + routing.at("routes").at(route).dump());
                Require(handshake.at("error").is_null() && handshake.at("reply").at("startup") == routing.at("startup"), "original runtime callback receives the same background's response");
                if (startup.empty()) startup = routing.at("startup");
                else Require(startup == routing.at("startup").get<std::string>(), "both documents reach the same background instance");
            }
            Require(startup != previousStartup, "each enable uses a new background instance");
            previousStartup = startup;
        }
        Send(app, B_QUIT_REQUESTED);
        Require(Wait([&] { team_info current; return get_team_info(team, &current) == B_BAD_TEAM_ID; }), "lifecycle browser quits cleanly");
        std::printf("EXTENSION_MESSAGING_LIFECYCLE_RESULT PASS checks=%d\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "EXTENSION_MESSAGING_LIFECYCLE_RESULT FAIL %s checks=%d\n", error.what(), checks);
        if (manager.IsValid()) { try { std::fprintf(stderr, "MANAGER %s\n", Text(manager, "extension-details").c_str()); } catch (...) { } }
        if (verified) Cleanup(app, team);
        return 1;
    }
}
