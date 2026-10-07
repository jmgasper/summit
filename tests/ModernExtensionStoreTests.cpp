// Real store downloads, native consent, cancellation and restart persistence.
#define SUMMIT_EXTENSION_MANAGER_HELPERS_ONLY
#include "ModernExtensionManagerTests.cpp"

int main(int argc, char** argv)
{
    if (argc != 7) return 2;
    status_t status;
    BApplication test("application/x-vnd.Summit-extension-store-tests", &status);
    if (status != B_OK) return 1;
    const auto team = static_cast<team_id>(std::strtol(argv[1], nullptr, 10));
    const std::filesystem::path profile(argv[3]);
    const std::string url(argv[4]), identity(argv[5]), phase(argv[6]);
    BMessenger app, manager;
    bool verified = false;
    try {
        app_info info;
        Require(Wait([&] { app = BMessenger(nullptr, team); return app.IsValid()
            && be_roster->GetRunningAppInfo(team, &info) == B_OK; }), "owned browser registers");
        BPath executable(&info.ref);
        Require(std::filesystem::canonical(executable.Path()) == std::filesystem::canonical(argv[2]), "owned browser matches the built executable");
        verified = true;
        const auto browser = Window(app, 0);
        Send(app, summit::kShowExtensions);
        Require(Wait([&] { manager = Manager(app); return manager.IsValid() && Enabled(manager, "extension-store-install"); }), "store controls are ready");
        const auto input = View(manager, "extension-store-url");
        auto enterURL = [&](const std::string& value) {
            BMessage set(B_SET_PROPERTY); set.AddSpecifier("Value"); set.AddString("data", value.c_str());
            Require(Query(input, set).GetInt32("error", B_ERROR) == B_OK, "enter URL in native store field");
            Button(manager, "extension-store-install");
        };
        if (phase == "browse") {
            for (const auto& store : std::vector<std::pair<const char*, const char*>> {
                { "Chrome Web Store", "https://chromewebstore.google.com/category/extensions" },
                { "Firefox Add-ons", "https://addons.mozilla.org/en-US/firefox/extensions/" } }) {
                const auto before = Count(State(browser));
                Button(manager, store.first);
                Require(Wait([&] {
                    const auto state = State(browser);
                    BMessage tab;
                    return Count(state) == before + 1 && state.FindMessage("tab", Index(state, Selected(state)), &tab) == B_OK
                        && String(tab, "url").find(store.second) == 0;
                }), std::string("store browse control opens official site: ") + store.first);
            }
            Send(manager, B_QUIT_REQUESTED);
            Require(Wait([&] { return !manager.IsValid(); }), "manager closes before browsing extension detail page");
            Send(browser, summit::kNewTab, -1, url);
            Require(Wait([&] {
                return !Property(View(browser, "install-store-extension"), "Hidden").GetBool("result", true);
            }), "native install button appears on extension detail page");
            FocusWindow(browser);
            Button(browser, "install-store-extension");
            Require(Wait([&] {
                manager = Manager(app);
                return manager.IsValid() && Enabled(manager, "extension-store-install")
                    && Text(manager, "extension-status").find("already installed") != std::string::npos;
            }, 180000000), "detail-page install button downloads and reviews the selected identity");
        }
        if (phase == "install") {
            Require(Entries(manager) == 0, "isolated profile has no installed extension");
            for (const char* label : {"Chrome Web Store", "Firefox Add-ons"})
                Require(View(manager, label).IsValid(), std::string("browse button exists: ") + label);
            enterURL("https://addons.mozilla.org.evil.test/firefox/addon/darkreader/");
            Require(Wait([&] { return Text(manager, "extension-status").find("detail-page link") != std::string::npos; }), "lookalike store URL is rejected with useful feedback");
            Require(Entries(manager) == 0 && !Enabled(manager, "extension-install"), "invalid store URL cannot reach installation");
            for (int attempt = 0; attempt < 2; ++attempt) {
                enterURL(url);
                Require(Wait([&] { return Enabled(manager, "extension-install"); }, 180000000), "real store package reaches native permission review");
                const auto body = Text(manager, "extension-details");
                Require(body.find("Downloaded from ") != std::string::npos && body.find("Requested access:") != std::string::npos,
                    "review identifies source and requested access");
                Require(Entries(manager) == 0 && !View(browser, ("extension-action-" + identity).c_str()).IsValid(), "download and verification do not execute the extension");
                if (!attempt) {
                    Button(manager, "extension-cancel");
                    Require(Wait([&] { return Enabled(manager, "extension-store-install") && !Enabled(manager, "extension-install"); }), "cancelling discards the prepared package");
                    Require(Entries(manager) == 0, "cancelled installation leaves no catalog entry");
                } else Button(manager, "extension-install");
            }
            Require(Wait([&] { return Entries(manager) == 1 && Enabled(manager, "extension-store-install"); }, 60000000), "approved store extension installs");
            const auto catalog = ReadJSON(profile / "Extensions/catalog.json");
            Require(catalog.is_object() && catalog["extensions"].size() == 1 && catalog["extensions"][0]["identifier"] == identity,
                "persisted installation has the identity selected in the store");
            enterURL(url);
            Require(Wait([&] { return Enabled(manager, "extension-store-install") && Text(manager, "extension-status").find("already installed") != std::string::npos; }, 180000000),
                "duplicate store identity is rejected before approval");
            Require(Entries(manager) == 1, "duplicate does not alter existing installation");
        }
        Require(Entries(manager) == 1 && Text(manager, "extension-details").find("Running") != std::string::npos,
            phase == "restart" ? "approved extension restores and runs after browser restart" : "approved extension runs");
        Require(View(browser, ("extension-action-" + identity).c_str()).IsValid(), "real extension action appears in the native toolbar");
        const auto downloads = profile / "Extensions/downloads";
        Require(!std::filesystem::exists(downloads) || std::filesystem::is_empty(downloads), "temporary store downloads are removed");
        Require(Wait([&] {
            const auto state = State(browser);
            if (Count(state) < 1) return false;
            BMessage tab;
            for (int32 i = 0; state.FindMessage("tab", i, &tab) == B_OK; ++i)
                if (tab.GetBool("loading", true)) return false;
            return true;
        }), "initial browser navigation finishes before requesting shutdown");
        Send(app, B_QUIT_REQUESTED);
        Require(Wait([&] { team_info info; return get_team_info(team, &info) == B_BAD_TEAM_ID; }), "test browser exits normally");
        std::printf("STORE_BROWSER_RESULT PASS phase=%s identity=%s checks=%d\n", phase.c_str(), identity.c_str(), checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "STORE_BROWSER_RESULT FAIL checks=%d error=%s\n", checks, error.what());
        if (manager.IsValid()) {
            std::fprintf(stderr, "STATUS: %s\nDETAILS: %s\n", Text(manager, "extension-status").c_str(), Text(manager, "extension-details").c_str());
        }
        State(Window(app, 0)).PrintToStream();
        if (verified) Cleanup(app, team);
        return 1;
    }
}
