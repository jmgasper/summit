// Exercise the unmodified published uBlock CRX and real controlled HTTP requests.
#define SUMMIT_EXTENSION_MANAGER_HELPERS_ONLY
#include "ModernExtensionManagerTests.cpp"

static void ChoosePackage(const BMessenger& app, const BMessenger& manager, const std::filesystem::path& path)
{
    FocusWindow(manager); Button(manager, "extension-add");
    BMessenger picker;
    Require(Wait([&] { picker = Picker(app); return picker.IsValid() && !Property(picker, "Hidden").GetBool("result", true); }), "native package picker opens");
    BEntry parent(path.parent_path().c_str()), entry(path.c_str());
    entry_ref parentRef, ref;
    Require(parent.GetRef(&parentRef) == B_OK && entry.GetRef(&ref) == B_OK, "CRX file has a native reference");
    auto poses = View(picker, "ActualPoseView");
    BMessage specifier('sref'); specifier.AddString("property", "Entry"); specifier.AddRef("refs", &parentRef);
    BMessage navigate(B_EXECUTE_PROPERTY); navigate.AddSpecifier(&specifier); Query(poses, navigate);
    Require(Wait([&] { entry_ref actual; return Property(poses, "Path").FindRef("result", &actual) == B_OK && actual == parentRef; }), "picker reaches the CRX directory");
    Require(Wait([&] {
        BMessage selection(B_SET_PROPERTY); selection.AddSpecifier("Selection"); selection.AddRef("data", &ref); Query(poses, selection);
        entry_ref actual; return Property(poses, "Selection").FindRef("result", &actual) == B_OK && actual == ref;
    }), "picker selects the actual signed archive file");
    Button(picker, "default button");
    Require(Wait([&] { return !picker.IsValid() || Property(picker, "Hidden").GetBool("result", false); }), "picker submits the selected package");
}
static void Screen(const std::filesystem::path& path)
{
    BScreen screen; BRect frame = screen.Frame();
    BBitmap bitmap(BRect(0, 0, frame.Width(), frame.Height()), B_RGBA32);
    if (bitmap.InitCheck() != B_OK || screen.ReadBitmap(&bitmap, false, &frame) != B_OK) throw std::runtime_error("screen capture failed");
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << int(frame.Width()) + 1 << ' ' << int(frame.Height()) + 1 << "\n255\n";
    for (int y = 0; y <= int(frame.Height()); ++y) for (int x = 0; x <= int(frame.Width()); ++x) {
        auto* pixel = static_cast<const uint8*>(bitmap.Bits()) + y * bitmap.BytesPerRow() + x * 4;
        const char rgb[] { char(pixel[2]), char(pixel[1]), char(pixel[0]) }; out.write(rgb, 3);
    }
    if (!out) throw std::runtime_error("screen output failed");
}
int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--check-executable-unused") return SummitCloseHarnessEntry(argc, argv);
    if (argc != 9) return 2;
    status_t status;
    BApplication test("application/x-vnd.Kunanyi-Summit-UBlock-tests", &status);
    if (status != B_OK) return 1;
    const auto team = static_cast<team_id>(std::strtol(argv[1], nullptr, 10));
    const std::filesystem::path archive(argv[3]), control(argv[4]), profile(argv[6]);
    const std::string target(argv[5]), mode(argv[7]), identity = "fkgkibajhfbepljeaefdnfnegdcjomkh";
    const bool watch = std::string(argv[8]) == "watch";
    BMessenger app, manager, window;
    bool verified = false;
    try {
        app_info info;
        Require(Wait([&] { app = BMessenger(nullptr, team); return app.IsValid() && be_roster->GetRunningAppInfo(team, &info) == B_OK; }), "owned uBlock browser registers");
        BPath executable(&info.ref);
        Require(std::filesystem::canonical(executable.Path()) == std::filesystem::canonical(argv[2]), "browser matches frozen executable");
        verified = true; window = Window(app, 0);
        auto observation = [&](const std::string& phase) { return ReadJSON(control / (phase + ".json")); };
        const auto requested = [&](const std::string& phase, const char* resource) {
            auto all = ReadJSON(control / "requests.json");
            return all.is_object() && all.contains(phase) ? all[phase].value(resource, 0) : 0;
        };
        const auto outcome = [&](const std::string& phase, bool blocked) {
            auto value = observation(phase);
            return value.is_object() && !value.value("timedOut", true) && value.value("allowed", false)
                && value.value("allowedEvent", "") == "load" && value.value("advert", false) == !blocked
                && value.value("advertEvent", "") == (blocked ? "error" : "load")
                && requested(phase, "allowed") == 1 && requested(phase, "advert") == (blocked ? 0 : 1);
        };
        const auto navigate = [&](const std::string& phase, bool blocked) {
            Send(window, summit::kNavigate, -1, target + "/" + phase);
            Require(Wait([&] { return outcome(phase, blocked); }, 25000000), phase + (blocked
                ? ": allowed script executes, EasyList-matched request never reaches server"
                : ": both scripts reach server and execute their unique response bytes"));
            Screen(control / (mode + "-" + phase + ".ppm"));
        };
        const auto actionName = "extension-action-" + identity;
        const auto title = [&] { return String(Property(View(window, actionName.c_str()), "Label"), "result"); };
        const auto openManager = [&] {
            BMessage open(B_EXECUTE_PROPERTY); open.AddSpecifier("MenuItem", "Extensions…"); open.AddSpecifier("Menu", "Window");
            open.AddSpecifier("View", "menu"); open.AddSpecifier("Window", int32(0));
            Require(Query(app, open).GetInt32("error", B_ERROR) == B_OK, "open native extension manager");
            Require(Wait([&] { manager = Manager(app); return manager.IsValid() && Enabled(manager, "extension-add"); }), "manager finishes extension startup");
        };
        if (mode == "install") {
            Require(Wait([&] { return outcome("setup", false); }), "both controlled resources work before installation");
            openManager();
            ChoosePackage(app, manager, archive);
            Require(Wait([&] { return Enabled(manager, "extension-install"); }), "published CRX reaches native consent");
            const auto consent = Text(manager, "extension-details");
            Require(consent.find("uBlock Origin") != std::string::npos && consent.find("Package signature verified.") != std::string::npos
                && consent.find("webRequestBlocking") != std::string::npos && consent.find("<all_urls>") != std::string::npos,
                "consent reports the actual signature and requested network access");
            if (watch) snooze(2000000);
            Screen(control / "consent.ppm");
            Button(manager, "extension-install");
            Require(Wait([&] { return Entries(manager) == 1 && Enabled(manager, "extension-add"); }), "unmodified published CRX installs");
            const auto catalog = ReadJSON(profile / "Extensions/catalog.json");
            Require(catalog.is_object() && catalog["extensions"].size() == 1, "uBlock is the only installed extension");
            const auto entry = catalog["extensions"][0];
            Require(entry["identifier"].get<std::string>() == identity && entry["name"].get<std::string>() == "uBlock Origin"
                && entry["version"].get<std::string>() == "1.74.0" && entry["enabled"].get<bool>(), "published version and verified signer are retained");
            std::ofstream(control / "installed.json") << entry.dump(2) << '\n';
            Send(manager, B_QUIT_REQUESTED);
            Require(Wait([&] { return !manager.IsValid(); }), "manager closes after installation");
        }
        // The manifest title alone does not prove background execution. uBlock's
        // actual setIcon implementation appends its current request count.
        Require(Wait([&] { return title().starts_with("uBlock Origin ("); }, 45000000), "published background updates the active tab's native action title");
        if (mode == "restart") navigate("restart", true);
        else {
            navigate("enabled", true);
            auto togglePopup = [&](bool enabled) {
                FocusWindow(window); Button(window, actionName.c_str());
                Require(Wait([&] { auto popup = ActionPopup(app); return popup.IsValid() && !Property(popup, "Hidden").GetBool("result", true); }), "published uBlock popup has a visible native host");
                snooze(watch ? 3000000 : 1200000);
                Screen(control / (enabled ? "popup-before-on.ppm" : "popup-before-off.ppm"));
                auto popup = ActionPopup(app), content = View(popup, "extension-popup-content");
                const auto frame = Property(content, "Frame").GetRect("result", BRect());
                Require(content.IsValid() && frame.IsValid(), "published popup exposes native input view");
                FocusWindow(popup); Click(content, BPoint(frame.Width() / 2, 70));
                Require(Wait([&] { return enabled ? title().starts_with("uBlock Origin (") && title() != "uBlock Origin (off)" : title() == "uBlock Origin (off)"; }), "published popup changes the real background filtering switch");
                Send(popup, B_QUIT_REQUESTED);
                Require(Wait([&] { return !ActionPopup(app).IsValid(); }), "uBlock popup closes cleanly");
            };
            togglePopup(false); navigate("popup-disabled", false);
            togglePopup(true); navigate("popup-reenabled", true);
            openManager(); FocusWindow(manager); Button(manager, "extension-toggle");
            Require(Wait([&] { return Enabled(manager, "extension-add") && !View(window, actionName.c_str()).IsValid(); }), "disabling uBlock removes its live action");
            Send(manager, B_QUIT_REQUESTED); Require(Wait([&] { return !manager.IsValid(); }), "manager closes after disable");
            navigate("disabled", false);
            openManager(); FocusWindow(manager); Button(manager, "extension-toggle");
            Require(Wait([&] { return Enabled(manager, "extension-add") && title().starts_with("uBlock Origin ("); }), "reenabling uBlock restores its background");
            Send(manager, B_QUIT_REQUESTED); Require(Wait([&] { return !manager.IsValid(); }), "manager closes after reenable");
            navigate("reenabled", true);
        }
        Send(app, B_QUIT_REQUESTED);
        Require(Wait([&] { team_info info; return get_team_info(team, &info) == B_BAD_TEAM_ID; }), "published uBlock browser exits normally");
        std::printf("UBLOCK_RESULT PASS mode=%s checks=%d\n", mode.c_str(), checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "UBLOCK_RESULT FAIL mode=%s checks=%d error=%s\n", mode.c_str(), checks, error.what());
        if (verified) {
            try {
                if (manager.IsValid()) { std::fprintf(stderr, "MANAGER %s\n", Text(manager, "extension-details").c_str()); Send(manager, B_QUIT_REQUESTED); }
                const auto name = "extension-action-" + identity;
                std::fprintf(stderr, "ACTION_TITLE %s\n", String(Property(View(window, name.c_str()), "Label"), "result").c_str());
                if (Enabled(window, name.c_str()) && !ActionPopup(app).IsValid()) { FocusWindow(window); Button(window, name.c_str()); snooze(2500000); }
                Screen(control / (mode + "-failure.ppm"));
            } catch (...) { }
            Cleanup(app, team);
        }
        return 1;
    }
}
