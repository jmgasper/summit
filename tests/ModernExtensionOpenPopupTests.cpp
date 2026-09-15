// Programmatic commands arrive through a local HTTP poll, without toolbar input.
#define SUMMIT_EXTENSION_OVERFLOW_HELPERS_ONLY
#include "ModernExtensionOverflowTests.cpp"
#include <Window.h>
#include <map>

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--check-executable-unused") return SummitCloseHarnessEntry(argc, argv);
    if (argc != 6 && argc != 7) return 2;
    const bool watch = argc == 7 && std::string(argv[6]) == "watch";
    status_t status;
    BApplication test("application/x-vnd.Kunanyi-Summit-open-popup-tests", &status);
    if (status != B_OK) return 1;
    const auto team = static_cast<team_id>(std::strtol(argv[1], nullptr, 10));
    BMessenger app, competitor;
    bool verified = false;
    try {
        app_info info;
        Require(Wait([&] { app = BMessenger(nullptr, team); return app.IsValid() && be_roster->GetRunningAppInfo(team, &info) == B_OK; }), "owned popup browser registers");
        BPath executable(&info.ref);
        Require(std::filesystem::canonical(executable.Path()) == std::filesystem::canonical(argv[2]), "popup browser matches frozen executable");
        verified = true;
        auto window = Window(app, 0);
        const std::filesystem::path control(argv[4]);
        const std::string targetURL(argv[5]);
        std::map<std::string, bool> readinessNoted;
        const auto loaded = [&](const std::string& url) {
            auto state = State(window); BMessage tab;
            const int32 index = Index(state, Selected(state));
            if (index < 0 || state.FindMessage("tab", index, &tab) != B_OK
                || tab.GetBool("loading", true) || String(tab, "url") != url) return false;
            // A newly created tab already has its requested URL but starts
            // idle, before WebKit sends the first load notification.
            const bool complete = String(tab, "loadOutcome") == "succeeded"
                && tab.GetUInt64("loadSuccessSequence", 0) > 0;
            if (!complete && !readinessNoted[url]) {
                readinessNoted[url] = true;
                std::fprintf(stderr, "POPUP LOAD WAIT url=%s outcome=%s sequence=%llu\n", url.c_str(),
                    String(tab, "loadOutcome").c_str(), static_cast<unsigned long long>(tab.GetUInt64("loadSuccessSequence", 0)));
            }
            return complete;
        };
        Require(Wait([&] { return loaded(targetURL); }), "unpermitted localhost target loads before extension installation");
        BMessage open(B_EXECUTE_PROPERTY); open.AddSpecifier("MenuItem", "Extensions…"); open.AddSpecifier("Menu", "Window");
        open.AddSpecifier("View", "menu"); open.AddSpecifier("Window", int32(0));
        Require(Query(app, open).GetInt32("error", B_ERROR) == B_OK, "open native extension manager");
        BMessenger manager;
        Require(Wait([&] { manager = Manager(app); return manager.IsValid() && Enabled(manager, "extension-add"); }), "manager is ready for popup fixture");
        InstallFolder(app, manager, argv[3], 1);
        Send(manager, B_QUIT_REQUESTED);
        Require(Wait([&] { return !manager.IsValid(); }), "manager closes before programmatic requests");
        FocusWindow(window);
        constexpr auto actionName = "extension-action-summit-open-popup";
        Require(Wait([&] { return ReadJSON(control / "ready.json").is_object()
            && String(Property(View(window, actionName), "Label"), "result") == "OpenPopup ready"; }), "background reaches command polling through its permitted host");
        int sequence = 0;
        std::map<int, std::string> operations;
        const auto request = [&](json value) {
            value["id"] = ++sequence;
            operations[sequence] = value.at("operation").get<std::string>();
            const auto temporary = control / "command.tmp";
            { std::ofstream output(temporary); output << value.dump(); }
            std::filesystem::rename(temporary, control / "command.json");
            return sequence;
        };
        const auto response = [&](int id, bool success = true, bigtime_t duration = timeout) {
            json value;
            Require(Wait([&] { value = ReadJSON(control / "responses" / (std::to_string(id) + ".json")); return value.is_object(); }, duration), "background command " + std::to_string(id) + " completes");
            Require(value.value("id", -1) == id && value.value("ok", !success) == success,
                "command " + std::to_string(id) + (success ? " succeeds: " : " rejects: ") + value.value("error", ""));
            if (!success) Require(!value.value("error", "").empty(), "rejection includes an API error");
            if (success && operations.at(id) == "open") {
                const auto popup = ActionPopup(app);
                const auto hidden = Property(popup, "Hidden");
                const bool presented = popup.IsValid() && !hidden.GetBool("result", true);
                if (!presented) {
                    std::fprintf(stderr, "POPUP PRESENTATION DIAGNOSTIC command=%d valid=%d hidden_error=%d response=%s\n",
                        id, int(popup.IsValid()), hidden.GetInt32("error", B_ERROR), value.dump().c_str());
                    for (int sample = 0; sample < 2; ++sample) {
                        if (sample) snooze(200000);
                        for (int32 index = 0; index < 16; ++index) {
                            const auto candidate = Window(app, index);
                            std::fprintf(stderr, "POPUP WINDOW sample=%d index=%d valid=%d", sample, int(index), int(candidate.IsValid()));
                            if (!candidate.IsValid()) { std::fprintf(stderr, "\n"); break; }
                            const auto title = Property(candidate, "Title");
                            std::fprintf(stderr, " title=%s title_error=%d hidden=%d active=%d content=%d\n",
                                String(title, "result").c_str(), title.GetInt32("error", B_ERROR),
                                int(Property(candidate, "Hidden").GetBool("result", true)),
                                int(Property(candidate, "Active").GetBool("result", false)),
                                int(View(candidate, "extension-popup-content").IsValid()));
                        }
                    }
                }
                Require(presented,
                    "successful promise/callback is observed only after native presentation");
            }
            return value;
        };
        const auto command = [&](json value, bool success = true) { return response(request(value), success); };
        const auto emptyMetadata = [](const json& value) { return value.is_null() || value == ""; };
        const auto redacted = [&](const json& value, bool allowNoActiveTab = false) {
            const auto& snapshot = value.at("snapshot"); const auto& tabs = snapshot.at("tabs");
            Require(snapshot.at("clicked") == 0 && (tabs.size() == 1 || (allowNoActiveTab && tabs.empty()))
                && std::all_of(tabs.begin(), tabs.end(), [&](const auto& tab) {
                    return emptyMetadata(tab.at("url")) && emptyMetadata(tab.at("title"));
                }),
                "programmatic calls preserve URL/title redaction and emit no action click");
        };
        auto initial = command({{"operation", "inspect"}});
        Require(initial.at("value").at("api") == "function" && initial.at("value").at("hostPermission") == false,
            "openPopup exists without a localhost host permission");
        redacted(initial);
        const auto windowID = initial.at("snapshot").at("tabs")[0].at("windowId");
        const auto tabID = initial.at("snapshot").at("tabs")[0].at("id");
        const auto shown = [&](const std::string& id, const std::string& expectedURL = std::string()) {
            BMessenger popup;
            Require(Wait([&] { popup = ActionPopup(app); return popup.IsValid() && !Property(popup, "Hidden").GetBool("result", true); }),
                "completed request has a visible native popup");
            json document;
            Require(Wait([&] { document = ReadJSON(control / ("popup-" + id + ".json")); return document.is_object(); }), "popup document executes its extension API script");
            Require(document.at("text") == "Extension API ready" && document.at("extensionURL").get<std::string>().starts_with("webkit-extension://"),
                "popup runs a real extension document and updates its DOM");
            Require(document.at("tabs").size() == 1 && (expectedURL.empty() ? emptyMetadata(document.at("tabs")[0].at("url"))
                : document.at("tabs")[0].at("url") == expectedURL), "popup context has the expected tab access");
            if (watch) snooze(450000);
            return popup;
        };
        const auto closePopup = [&](const BMessenger& popup) {
            Send(popup, B_QUIT_REQUESTED);
            Require(Wait([&] { return !ActionPopup(app).IsValid(); }), "native popup and its content close");
            FocusWindow(window);
        };
        int id = request({{"operation", "open"}, {"popup", "popup.html"}});
        auto result = response(id);
        Require(result.at("value").at("resolvedUndefined") == true, "promise resolves with undefined after presentation");
        redacted(result); closePopup(shown(std::to_string(id)));
        id = request({{"operation", "open"}, {"popup", "popup.html"}, {"options", {{"windowId", windowID}}}, {"callback", true}});
        result = response(id);
        Require(result.at("value").at("callbackArguments") == 0, "explicit focused-window callback completes without arguments or lastError");
        redacted(result); closePopup(shown(std::to_string(id)));
        result = command({{"operation", "inspect"}, {"namespace", "chrome"}});
        Require(result.at("value").at("api") == "function" && result.at("value").at("hostPermission") == false,
            "chrome alias exposes openPopup with the same host permission state");
        redacted(result);
        for (bool callback : {false, true}) {
            id = request({{"operation", "open"}, {"namespace", "chrome"}, {"popup", "popup.html"},
                {"options", {{"windowId", windowID}}}, {"callback", callback}});
            result = response(id);
            Require(callback ? result.at("value").at("callbackArguments") == 0 : result.at("value").at("resolvedUndefined") == true,
                "chrome alias completes its promise or callback after presentation");
            redacted(result); closePopup(shown(std::to_string(id)));
        }
        Send(window, summit::kNewTab, -1, targetURL + "-second");
        Require(Wait([&] { return Count(State(window)) == 2 && loaded(targetURL + "-second"); }), "second browser tab becomes active");
        const auto secondNativeTab = Selected(State(window));
        redacted(command({{"operation", "open"}, {"options", {{"tabId", tabID}}}}, false));
        result = command({{"operation", "inspect"}}); redacted(result);
        const auto secondTabID = result.at("snapshot").at("tabs")[0].at("id");
        id = request({{"operation", "open"}, {"popup", "popup.html"}, {"options", {{"tabId", secondTabID}}}});
        redacted(response(id)); closePopup(shown(std::to_string(id)));
        Send(window, summit::kCloseTab, secondNativeTab);
        Require(Wait([&] { return Count(State(window)) == 1 && loaded(targetURL); }), "second tab closes and restores the first");
        redacted(command({{"operation", "open"}, {"options", {{"tabId", secondTabID}}}}, false));
        for (const auto& options : std::vector<json> {json::array(), false, {{"windowId", -999}}, {{"windowId", 1.5}}, {{"windowId", "bad"}}, {{"windowId", 9000000000LL}}, {{"windowId", windowID}, {"tabId", tabID}}}) {
            redacted(command({{"operation", "open"}, {"options", options}}, false));
            Require(!ActionPopup(app).IsValid(), "invalid popup target creates no native host");
        }
        command({{"operation", "configure"}, {"enabled", false}});
        redacted(command({{"operation", "open"}}, false));
        command({{"operation", "configure"}, {"enabled", true}, {"popup", ""}});
        redacted(command({{"operation", "open"}}, false));
        redacted(command({{"operation", "open"}, {"popup", "missing.html"}, {"callback", true}}, false));
        Require(Wait([&] { return !ActionPopup(app).IsValid(); }), "failed popup load releases its native host");
        FocusWindow(window);
        redacted(command({{"operation", "open"}, {"namespace", "chrome"}, {"popup", "missing.html"}, {"callback", true}}, false));
        Require(Wait([&] { return !ActionPopup(app).IsValid(); }), "chrome lastError rejection releases its native host");
        FocusWindow(window);
        id = request({{"operation", "open"}, {"popup", "popup.html"}});
        redacted(response(id)); auto popup = shown(std::to_string(id));
        redacted(command({{"operation", "open"}}, false));
        Require(ActionPopup(app) == popup, "a second request preserves the existing popup");
        closePopup(popup);
        const auto compete = [&] {
            auto* other = new BWindow(BRect(240, 230, 760, 520), "Summit popup focus competitor", B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS);
            competitor = BMessenger(other); other->Show();
            FocusWindow(competitor);
            Require(Wait([&] { return !Property(window, "Active").GetBool("result", true); }), "browser owner loses native focus");
        };
        const auto closeCompetitor = [&] {
            Send(competitor, B_QUIT_REQUESTED);
            Require(Wait([&] { return !competitor.IsValid(); }), "competing window closes");
            FocusWindow(window);
        };
        compete();
        redacted(command({{"operation", "open"}, {"options", {{"windowId", windowID}}}}, false));
        Require(!ActionPopup(app).IsValid(), "unfocused explicit window creates no popup");
        closeCompetitor();
        const auto pending = [&] {
            for (const auto name : {"hold-seen", "hold-finished", "release-load"}) std::filesystem::remove(control / name);
            const auto pendingID = request({{"operation", "open"}, {"popup", "pending.html"}});
            Require(Wait([&] { return std::filesystem::exists(control / "hold-seen"); }), "popup document reaches a controlled pending stylesheet");
            Require(!ReadJSON(control / "responses" / (std::to_string(pendingID) + ".json")).is_object(), "openPopup has not completed before document readiness");
            Require(Wait([&] { auto hidden = ActionPopup(app); return hidden.IsValid() && Property(hidden, "Hidden").GetBool("result", false); }), "pending native popup stays hidden");
            return pendingID;
        };
        const auto release = [&] {
            { std::ofstream output(control / "release-load"); output << "release\n"; }
            Require(Wait([&] { return std::filesystem::exists(control / "hold-finished"); }), "controlled document response finishes");
        };
        id = pending();
        redacted(command({{"operation", "open"}}, false));
        Require(!ReadJSON(control / "responses" / (std::to_string(id) + ".json")).is_object(),
            "first popup remains pending after another API round trip while its document is held");
        release(); redacted(response(id)); closePopup(shown(std::to_string(id)));
        id = pending(); compete(); release(); redacted(response(id, false));
        Require(Wait([&] { return !ActionPopup(app).IsValid(); }) && Property(competitor, "Active").GetBool("result", false),
            "delayed popup rejects after owner focus loss without stealing focus");
        closeCompetitor();
        id = pending();
        const auto navigated = targetURL + "-navigated";
        Send(window, summit::kNavigate, -1, navigated);
        redacted(response(id, false)); release();
        Require(Wait([&] { return !ActionPopup(app).IsValid() && loaded(navigated); }), "navigation cancels the pending popup and commits the new target");
        Send(window, summit::kNewTab, -1, targetURL + "-closing");
        Require(Wait([&] { return Count(State(window)) == 2 && loaded(targetURL + "-closing"); }), "disposable popup owner tab loads");
        const auto closingTab = Selected(State(window));
        id = pending();
        Send(window, summit::kCloseTab, closingTab);
        // The engine cancels the popup while committing the page close. The
        // browser window selects its replacement after receiving that commit,
        // so the cancellation response may precede replacement activation.
        redacted(response(id, false), true); release();
        Require(Wait([&] { return !ActionPopup(app).IsValid() && Count(State(window)) == 1 && loaded(navigated); }), "closing the owner tab cancels the pending popup");
        result = command({{"operation", "inspect"}}); redacted(result);
        Require(result.at("snapshot").at("tabs")[0].at("id") == tabID
            && result.at("snapshot").at("tabs")[0].at("windowId") == windowID,
            "completed closure restores the original active extension tab identity");
        id = pending();
        result = response(id, false, 40000000);
        Require(result.at("error").get<std::string>().find("deadline") != std::string::npos,
            "stalled popup rejects at the engine load deadline");
        redacted(result); release();
        Require(Wait([&] { return !ActionPopup(app).IsValid(); }), "load timeout releases the popup host");
        FocusWindow(window);
        id = pending();
        Require(Query(app, open).GetInt32("error", B_ERROR) == B_OK, "open manager while programmatic load is pending");
        Require(Wait([&] { manager = Manager(app); return manager.IsValid() && Enabled(manager, "extension-toggle"); }), "pending extension can be disabled through native controls");
        // A reloaded background starts with an empty command stream, so it
        // cannot replay the old pending request from the previous context.
        { std::ofstream output(control / "command.tmp"); output << "{\"id\":0}"; }
        std::filesystem::rename(control / "command.tmp", control / "command.json");
        Button(manager, "extension-toggle");
        Require(Wait([&] { return String(Property(View(manager, "extension-toggle"), "Label"), "result") == "Enable"
            && !View(window, actionName).IsValid() && !ActionPopup(app).IsValid(); }), "unload destroys pending popup and its action control");
        release();
        std::filesystem::remove(control / "ready.json");
        Button(manager, "extension-toggle");
        Require(Wait([&] { return ReadJSON(control / "ready.json").is_object() && View(window, actionName).IsValid(); }), "approved extension reloads with a fresh background");
        Send(manager, B_QUIT_REQUESTED);
        Require(Wait([&] { return !manager.IsValid(); }), "manager closes after reload");
        FocusWindow(window); redacted(command({{"operation", "inspect"}}));
        Require(!ActionPopup(app).IsValid(), "late readiness from the unloaded context cannot show a replacement popup");
        command({{"operation", "configure"}, {"popup", "popup.html"}});
        FocusWindow(window); Button(window, actionName);
        popup = shown("trusted", navigated);
        result = command({{"operation", "inspect"}});
        Require(result.at("snapshot").at("tabs")[0].at("url") == navigated && result.at("snapshot").at("clicked") == 0,
            "real toolbar popup click grants activeTab without firing onClicked");
        closePopup(popup);
        const auto retained = targetURL + "-same-origin";
        Send(window, summit::kNavigate, -1, retained);
        Require(Wait([&] { return loaded(retained); }), "trusted grant target navigates within its origin");
        result = command({{"operation", "inspect"}});
        Require(result.at("snapshot").at("tabs")[0].at("url") == retained && result.at("snapshot").at("clicked") == 0,
            "same-origin navigation retains the trusted activeTab grant without a new click");
        const auto revoked = ReadJSON(control / "targets.json").at("differentOrigin").get<std::string>();
        Send(window, summit::kNavigate, -1, revoked);
        Require(Wait([&] { return loaded(revoked); }), "trusted grant target navigates to a different port and origin");
        redacted(command({{"operation", "inspect"}}));
        command({{"operation", "configure"}, {"popup", ""}});
        FocusWindow(window); Button(window, actionName);
        Require(Wait([&] { return String(Property(View(window, actionName), "Label"), "result") == "Clicked 1"; }),
            "real no-popup toolbar click reaches the onClicked listener");
        result = command({{"operation", "inspect"}});
        Require(result.at("snapshot").at("clicked") == 1 && result.at("snapshot").at("tabs")[0].at("url") == revoked,
            "trusted click is a positive control for the event and activeTab observations");
        Send(app, B_QUIT_REQUESTED);
        Require(Wait([&] { team_info info; return get_team_info(team, &info) == B_BAD_TEAM_ID; }), "browser quits after programmatic popup tests");
        std::printf("EXTENSION_OPEN_POPUP_RESULT PASS checks=%d commands=%d\n", checks, sequence);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "EXTENSION_OPEN_POPUP_RESULT FAIL %s checks=%d\n", error.what(), checks);
        if (competitor.IsValid()) { BMessage quit(B_QUIT_REQUESTED); competitor.SendMessage(&quit); }
        if (verified) Cleanup(app, team);
        return 1;
    }
}
