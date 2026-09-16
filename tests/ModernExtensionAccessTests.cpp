// Native extension installation and real page APIs; settings are reloaded on browser restart.
#define SUMMIT_EXTENSION_OVERFLOW_HELPERS_ONLY
#include "ModernExtensionOverflowTests.cpp"
#include <map>

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--check-executable-unused") return SummitCloseHarnessEntry(argc, argv);
    if (argc != 8) return 2;
    status_t status;
    BApplication test("application/x-vnd.Kunanyi-Summit-extension-access-tests", &status);
    if (status != B_OK) return 1;
    const auto team = static_cast<team_id>(std::strtol(argv[1], nullptr, 10));
    const std::filesystem::path packages(argv[3]), control(argv[4]), profile(argv[6]);
    const std::string target(argv[5]), phase(argv[7]);
    BMessenger app, manager;
    bool verified = false;
    try {
        app_info info;
        Require(Wait([&] { app = BMessenger(nullptr, team); return app.IsValid() && be_roster->GetRunningAppInfo(team, &info) == B_OK; }), "owned API browser registers");
        BPath executable(&info.ref);
        Require(std::filesystem::canonical(executable.Path()) == std::filesystem::canonical(argv[2]), "API browser matches frozen executable");
        verified = true;
        auto window = Window(app, 0);
        const auto loaded = [&](const std::string& url) {
            const auto state = State(window); BMessage tab;
            const auto index = Index(state, Selected(state));
            return index >= 0 && state.FindMessage("tab", index, &tab) == B_OK && String(tab, "url") == url
                && !tab.GetBool("loading", true) && String(tab, "loadOutcome") == "succeeded";
        };
        Require(Wait([&] { return loaded(target + "/setup"); }), "API setup page loads");
        if (phase == "initial") {
            BMessage open(B_EXECUTE_PROPERTY); open.AddSpecifier("MenuItem", "Extensions…"); open.AddSpecifier("Menu", "Window");
            open.AddSpecifier("View", "menu"); open.AddSpecifier("Window", int32(0));
            Require(Query(app, open).GetInt32("error", B_ERROR) == B_OK, "open native extension manager");
            Require(Wait([&] { manager = Manager(app); return manager.IsValid() && Enabled(manager, "extension-add"); }), "manager is ready");
            InstallFolder(app, manager, packages / "a", 1);
            InstallFolder(app, manager, packages / "b", 2);
            Send(manager, B_QUIT_REQUESTED);
            Require(Wait([&] { return !manager.IsValid(); }), "manager closes");
        }
        const auto catalog = ReadJSON(profile / "Extensions/catalog.json");
        Require(catalog.is_object() && catalog.at("extensions").size() == 2, "both independent extensions are installed");
        std::ofstream(control / "installed.json") << catalog.dump(2) << '\n';
        const auto expected = ReadJSON(control / "expected.json");
        const auto document = target + "/" + phase;
        Send(window, summit::kNavigate, -1, document);
        Require(Wait([&] { return loaded(document); }), "fresh content document loads in this browser session");
        std::map<std::string, int> sequences;
        const auto command = [&](const std::string& id, json request) {
            const auto sequence = ++sequences[id]; request["id"] = sequence;
            const auto directory = control / id;
            { std::ofstream output(directory / "command.tmp"); output << request.dump(); }
            std::filesystem::rename(directory / "command.tmp", directory / "command.json");
            json response;
            Require(Wait([&] { response = ReadJSON(directory / "responses" / (std::to_string(sequence) + ".json")); return response.is_object(); }), id + " command " + std::to_string(sequence) + " completes");
            Require(response.value("id", -1) == sequence && response.value("ok", false), id + " API command succeeds: " + response.value("error", ""));
            return response.at("value");
        };
        const auto permissions = [&](const std::string& id, const json& value) {
            for (const auto& api : {"browser", "chrome"}) {
                for (const auto& name : {"files", "private"}) {
                    for (const auto& mode : {"Promise", "Callback"})
                        Require(value.at(api).at(std::string(name) + mode).is_boolean()
                            && value.at(api).at(std::string(name) + mode) == expected.at(id).at(name), id + "/" + api + "/" + name + mode + " matches saved access");
                }
            }
        };
        const auto views = [&](const std::string& id, const json& value, size_t count) {
            Require(value.at("array") == true && value.at("unique") == true && value.at("views").size() == count, id + " view list has the expected unique Window count: " + value.dump());
            for (const auto& view : value.at("views"))
                Require(view.at("realWindow") == true && view.at("sameBackgroundObject") == true
                    && view.at("identity") == "summit-extension-access-" + id, id + " view belongs to this extension and shares the real background object");
        };
        for (const auto& id : {std::string("a"), std::string("b")}) {
            Require(Wait([&] { return ReadJSON(control / id / "ready.json").is_object(); }), id + " background starts");
            json state;
            Require(Wait([&] { state = command(id, {{"operation", "inspect"}}); for (const auto& record : state.at("content")) if (record.at("message").at("url") == document) return true; return false; }), id + " background receives its content script report");
            Require(state.at("identity") == "summit-extension-access-" + id && state.at("selfBackground") == true && state.at("chromeBackground") == true, id + " background APIs return the actual current Window");
            Require(state.at("incognito") == false && state.at("getURL") == state.at("expectedURL") && state.at("normalizedURL") == state.at("expectedURL"), id + " normal context and normalized MV2 resource URL");
            permissions(id, state.at("permissions"));
            views(id, state.at("all"), 1); views(id, state.at("popup"), 0); views(id, state.at("tabs"), 0);
            for (const auto& rejected : state.at("rejects").items()) Require(rejected.value() == true, "invalid getViews filter rejects: " + rejected.key());
            for (const auto& record : state.at("content")) {
                if (record.at("message").at("url") != document) continue;
                Require(record.at("sender").at("id") == "summit-extension-access-" + id && record.at("sender").at("url") == document, "content message preserves real sender identity");
                for (const auto& api : {"browser", "chrome"}) {
                    const auto& exposure = record.at("message").at("namespaces").at(api);
                    Require(exposure.at("present") == true && exposure.at("urlMatches") == true && exposure.at("incognito") == false, "content extension namespace exposes resource URL and normal context state");
                    for (const auto& method : exposure.at("restricted").items()) Require(method.value() == "undefined", "content script cannot call privileged " + method.key());
                }
            }
            if (phase != "initial") continue;
            auto unavailableURL = state.at("getURL").get<std::string>();
            unavailableURL.insert(unavailableURL.find('/', std::string("webkit-extension://").size()), ".invalid");
            Send(window, summit::kNewTab, -1, unavailableURL);
            BMessenger unavailable;
            Require(Wait([&] { unavailable = NamedWindow(app, "Summit"); return unavailable.IsValid(); }),
                "unknown extension origin displays a native error");
            const auto errorText = View(unavailable, "_tv_");
            const auto errorLength = Property(errorText, "Text", B_COUNT_PROPERTIES).GetInt32("result", -1);
            Require(errorLength > 0 && errorLength < 1024, "native extension error has bounded text");
            BMessage readError(B_GET_PROPERTY); readError.AddSpecifier("Text", int32(0), errorLength);
            Require(String(Query(errorText, readError), "result") == "This extension is not available.",
                "a hostname prefix cannot select a loaded extension");
            FocusWindow(unavailable);
            Button(unavailable, "_b0_");
            Require(Wait([&] { return !unavailable.IsValid(); }), "native extension error closes");
            Require(Count(State(window)) == 1 && loaded(document), "unknown extension origin leaves the existing tab intact");
            FocusWindow(window);
            command(id, {{"operation", "open"}});
            const auto popupSequence = sequences.at(id);
            json popupResult;
            Require(Wait([&] { popupResult = ReadJSON(control / id / ("view-" + std::to_string(popupSequence) + ".json")); return popupResult.is_object(); }), id + " popup reports its document APIs");
            Require(!popupResult.contains("error"), "popup APIs complete: " + popupResult.dump());
            Require(popupResult.at("realBackground") == true && popupResult.at("roundTripIdentity") == true
                && popupResult.at("startup") == state.at("startup") && popupResult.at("incognito") == false, "popup shares the live background Window and JS object identity");
            permissions(id, popupResult.at("permissions")); views(id, popupResult.at("all"), 2); views(id, popupResult.at("filtered"), 1);
            state = command(id, {{"operation", "inspect"}}); views(id, state.at("all"), 2); views(id, state.at("popup"), 1);
            const auto nativeTab = state.at("nativeTabs")[0];
            views(id, command(id, {{"operation", "inspect"}, {"filter", {{"type", "popup"}, {"tabId", nativeTab.at("id")}, {"windowId", nativeTab.at("windowId")}}}}).at("filtered"), 1);
            views(id, command(id, {{"operation", "inspect"}, {"filter", {{"type", "popup"}, {"windowId", 999999}}}}).at("filtered"), 0);
            auto popup = ActionPopup(app);
            Require(popup.IsValid() && !Property(popup, "Hidden").GetBool("result", true), "real popup host is visible");
            Send(popup, B_QUIT_REQUESTED);
            Require(Wait([&] { return !ActionPopup(app).IsValid(); }), "popup closes");
            FocusWindow(window);
            const auto url = state.at("getURL").get<std::string>() + "?kind=tab";
            Send(window, summit::kNewTab, -1, url);
            const bool extensionTabLoaded = Wait([&] { return loaded(url); });
            if (!extensionTabLoaded) {
                auto snapshot = State(window);
                snapshot.PrintToStream();
            }
            Require(extensionTabLoaded, "native browser opens the packaged extension tab");
            const auto nativeIndex = Selected(State(window));
            json tabResult;
            Require(Wait([&] { tabResult = ReadJSON(control / id / "view-tab.json"); return tabResult.is_object(); }), "extension tab reports its document APIs");
            Require(!tabResult.contains("error"), "extension tab APIs complete: " + tabResult.dump());
            Require(tabResult.at("realBackground") == true && tabResult.at("roundTripIdentity") == true
                && tabResult.at("startup") == state.at("startup"), "extension tab shares the real background Window");
            permissions(id, tabResult.at("permissions")); views(id, tabResult.at("filtered"), 1);
            state = command(id, {{"operation", "inspect"}}); views(id, state.at("all"), 2); views(id, state.at("popup"), 0); views(id, state.at("tabs"), 1);
            json tab;
            for (const auto& item : state.at("nativeTabs")) if (item.value("url", "") == url) tab = item;
            Require(tab.is_object(), "extension API tab ID corresponds to the actual native extension page");
            views(id, command(id, {{"operation", "inspect"}, {"filter", {{"type", "tab"}, {"tabId", tab.at("id")}, {"windowId", tab.at("windowId")}}}}).at("filtered"), 1);
            views(id, command(id, {{"operation", "inspect"}, {"filter", {{"type", "tab"}, {"tabId", nativeTab.at("id")}}}}).at("filtered"), 0);
            Send(window, summit::kCloseTab, nativeIndex);
            Require(Wait([&] { return loaded(document) && Count(State(window)) == 1; }), "extension tab closes and original document is restored");
            Require(Wait([&] { state = command(id, {{"operation", "inspect"}}); return state.at("tabs").at("views").empty(); }), "closed extension tab is removed from getViews");
            views(id, state.at("all"), 1);
        }
        Require(ReadJSON(profile / "Extensions/catalog.json") == catalog, "read-only APIs preserve saved grants and package identity");
        Send(app, B_QUIT_REQUESTED);
        Require(Wait([&] { team_info current; return get_team_info(team, &current) == B_BAD_TEAM_ID; }), "API browser quits cleanly");
        std::printf("EXTENSION_ACCESS_RESULT PASS phase=%s checks=%d\n", phase.c_str(), checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "EXTENSION_ACCESS_RESULT FAIL phase=%s %s checks=%d\n", phase.c_str(), error.what(), checks);
        if (verified) Cleanup(app, team);
        return 1;
    }
}
