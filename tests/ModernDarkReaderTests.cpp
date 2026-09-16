// Exercise the unmodified published extension through native installation and UI.
#define SUMMIT_EXTENSION_OVERFLOW_HELPERS_ONLY
#include "ModernExtensionOverflowTests.cpp"
#include <Window.h>

static void SaveScreen(const std::filesystem::path& path)
{
    BScreen screen;
    BRect frame = screen.Frame();
    BBitmap bitmap(BRect(0, 0, frame.Width(), frame.Height()), B_RGBA32);
    if (bitmap.InitCheck() != B_OK || screen.ReadBitmap(&bitmap, false, &frame) != B_OK)
        throw std::runtime_error("Cannot capture native screen");
    std::ofstream output(path, std::ios::binary);
    output << "P6\n" << int(frame.Width()) + 1 << ' ' << int(frame.Height()) + 1 << "\n255\n";
    for (int y = 0; y <= int(frame.Height()); ++y) for (int x = 0; x <= int(frame.Width()); ++x) {
        const auto* pixel = static_cast<const uint8*>(bitmap.Bits()) + y * bitmap.BytesPerRow() + x * 4;
        const char rgb[] = { char(pixel[2]), char(pixel[1]), char(pixel[0]) };
        output.write(rgb, 3);
    }
    if (!output) throw std::runtime_error("Cannot write native screenshot");
}

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--check-executable-unused") return SummitCloseHarnessEntry(argc, argv);
    if (argc != 8) return 2;
    status_t status;
    BApplication test("application/x-vnd.Kunanyi-Summit-DarkReader-tests", &status);
    if (status != B_OK) return 1;
    const auto team = static_cast<team_id>(std::strtol(argv[1], nullptr, 10));
    const std::filesystem::path control(argv[4]), profile(argv[6]);
    const bool watch = std::string(argv[7]) == "watch";
    BMessenger app, manager;
    bool verified = false;
    try {
        app_info info;
        Require(Wait([&] { app = BMessenger(nullptr, team); return app.IsValid() && be_roster->GetRunningAppInfo(team, &info) == B_OK; }), "owned published-extension browser registers");
        BPath executable(&info.ref);
        Require(std::filesystem::canonical(executable.Path()) == std::filesystem::canonical(argv[2]), "browser matches frozen executable");
        verified = true;
        auto window = Window(app, 0);
        const auto loaded = [&](const std::string& url) {
            const auto state = State(window); BMessage tab;
            const auto index = Index(state, Selected(state));
            return index >= 0 && state.FindMessage("tab", index, &tab) == B_OK
                && String(tab, "url") == url && !tab.GetBool("loading", true)
                && String(tab, "loadOutcome") == "succeeded" && tab.GetUInt64("loadSuccessSequence", 0) > 0;
        };
        const auto report = [&](const std::string& phase) { return ReadJSON(control / (phase + ".json")); };
        const auto light = [&](const json& value) {
            return value.is_object() && value.value("bodyBackground", "") == "rgb(255, 255, 255)"
                && value.value("textColor", "") == "rgb(17, 34, 51)"
                && value.value("panelBackground", "") == "rgb(240, 244, 248)"
                && value.value("mode", "").empty() && value.value("scheme", "").empty();
        };
        const auto dark = [&](const json& value) {
            return value.is_object() && value.value("mode", "") == "dynamic"
                && (value.value("scheme", "") == "dark" || value.value("scheme", "") == "dimmed")
                && value.value("bodyLuminance", -1.0) >= 0 && value.value("bodyLuminance", 255.0) < 80
                && value.value("panelLuminance", -1.0) >= 0 && value.value("panelLuminance", 255.0) < 100
                && value.value("textLuminance", 0.0) > 150;
        };
        Require(Wait([&] { return loaded(std::string(argv[5]) + "/setup") && light(report("setup")); }), "control page is light before extension installation");
        BMessage open(B_EXECUTE_PROPERTY); open.AddSpecifier("MenuItem", "Extensions…"); open.AddSpecifier("Menu", "Window");
        open.AddSpecifier("View", "menu"); open.AddSpecifier("Window", int32(0));
        const auto openManager = [&] {
            Require(Query(app, open).GetInt32("error", B_ERROR) == B_OK, "open native extension manager");
            Require(Wait([&] { manager = Manager(app); return manager.IsValid() && Enabled(manager, "extension-add"); }), "extension manager is ready");
        };
        openManager();
        InstallFolder(app, manager, argv[3], 1);
        const auto catalogPath = profile / "Extensions/catalog.json";
        const auto catalog = ReadJSON(catalogPath);
        Require(catalog.is_object() && catalog.at("extensions").size() == 1, "published package is the sole installed extension");
        const auto entry = catalog.at("extensions")[0];
        Require(entry.at("name") == "Dark Reader" && entry.at("version") == "4.9.131" && entry.at("enabled") == true, "catalog records the published version and approval");
        const std::string identity = entry.at("identifier"), actionName = "extension-action-" + identity;
        std::ofstream(control / "installed.json") << entry.dump(2) << '\n';
        Send(manager, B_QUIT_REQUESTED);
        Require(Wait([&] { return !manager.IsValid(); }), "manager closes after installation");
        const auto navigate = [&](const std::string& phase) {
            Send(window, summit::kNavigate, -1, std::string(argv[5]) + "/" + phase);
            Require(Wait([&] { return loaded(std::string(argv[5]) + "/" + phase) && report(phase).is_object(); }), phase + " observation page loads");
        };
        navigate("enabled");
        FocusWindow(window);
        Require(Wait([&] { return String(Property(View(window, actionName.c_str()), "Label"), "result") == "Dark Reader"; }), "published action title appears in native toolbar");
        Button(window, actionName.c_str());
        Require(Wait([&] { auto popup = ActionPopup(app); return popup.IsValid() && !Property(popup, "Hidden").GetBool("result", true); }), "published popup has a visible native host");
        snooze(watch ? 5000000 : 1500000);
        SaveScreen(control / "popup.ppm");
        auto popup = ActionPopup(app);
        auto content = View(popup, "extension-popup-content");
        const auto frame = Property(content, "Frame").GetRect("result", BRect());
        Require(content.IsValid() && frame.IsValid() && frame.Width() >= 250 && frame.Height() >= 100,
            "published popup exposes its native content view");
        FocusWindow(popup);
        // The pinned 4.9.131 popup places its On/Off control in the upper-right
        // header. Send real native pointer events, then observe the page's CSS.
        const auto beforeToggle = report("enabled").at("sequence").get<int>();
        Click(content, BPoint(frame.Width() - 45, 64));
        Require(Wait([&] { const auto value = report("enabled"); return light(value)
            && value.at("sequence").get<int>() > beforeToggle; }), "published popup Off restores page colors without a reload");
        SaveScreen(control / "popup-off.ppm");
        const auto offSequence = report("enabled").at("sequence").get<int>();
        Click(content, BPoint(frame.Width() - 95, 64));
        Require(Wait([&] { const auto value = report("enabled"); return dark(value)
            && value.at("sequence").get<int>() > offSequence; }), "published popup On restores the dynamic theme without a reload");
        SaveScreen(control / "popup-on.ppm");
        Send(ActionPopup(app), B_QUIT_REQUESTED);
        Require(Wait([&] { return !ActionPopup(app).IsValid(); }), "published popup closes cleanly");
        const bool themed = Wait([&] { return dark(report("enabled")); }, 30000000);
        SaveScreen(control / "enabled.ppm");
        Require(themed, "unmodified Dark Reader sets dynamic dark attributes and recolors actual page elements");
        for (const auto& phase : { "disabled", "reenabled" }) {
            const bool enabled = std::string(phase) == "reenabled";
            openManager(); FocusWindow(manager); Button(manager, "extension-toggle");
            Require(Wait([&] { auto c = ReadJSON(catalogPath); return c.is_object() && c.at("extensions")[0].at("enabled") == enabled && Enabled(manager, "extension-add"); }), std::string(phase) + " state persists in catalog");
            Send(manager, B_QUIT_REQUESTED);
            Require(Wait([&] { return !manager.IsValid(); }), "manager closes after toggle");
            navigate(phase);
            Require(Wait([&] { return enabled ? dark(report(phase)) : light(report(phase)); }, 30000000), std::string(phase) + " has the expected actual page colors");
            if (watch) snooze(2000000);
            SaveScreen(control / (std::string(phase) + ".ppm"));
        }
        Send(app, B_QUIT_REQUESTED);
        Require(Wait([&] { team_info current; return get_team_info(team, &current) == B_BAD_TEAM_ID; }), "published-extension browser quits cleanly");
        std::printf("DARKREADER_RESULT PASS checks=%d\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "DARKREADER_RESULT FAIL %s checks=%d\n", error.what(), checks);
        if (manager.IsValid()) { try { std::fprintf(stderr, "MANAGER %s\n", Text(manager, "extension-details").c_str()); } catch (...) { } }
        if (verified) { try { SaveScreen(control / "failure.ppm"); } catch (...) { } Cleanup(app, team); }
        return 1;
    }
}
