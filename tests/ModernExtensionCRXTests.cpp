// Exercise signed CRX installation, consent, popup execution and browser startup.
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
    if (argc != 8) return 2;
    status_t status;
    BApplication test("application/x-vnd.Kunanyi-Summit-CRX-browser-tests", &status);
    if (status != B_OK) return 1;
    const auto team = static_cast<team_id>(std::strtol(argv[1], nullptr, 10));
    const std::filesystem::path fixtures(argv[3]), profile(argv[4]), control(argv[5]);
    const std::string phase(argv[6]); const bool watch = std::string(argv[7]) == "watch";
    BMessenger app, manager;
    bool verified = false;
    try {
        const std::string identity = ReadJSON(fixtures / "expected.json")["identities"]["rsa"].get<std::string>();
        app_info info;
        Require(Wait([&] { app = BMessenger(nullptr, team); return app.IsValid() && be_roster->GetRunningAppInfo(team, &info) == B_OK; }), "owned CRX test browser registers");
        BPath executable(&info.ref);
        Require(std::filesystem::canonical(executable.Path()) == std::filesystem::canonical(argv[2]), "browser matches the frozen executable");
        verified = true;
        const auto window = Window(app, 0);
        BMessage open(B_EXECUTE_PROPERTY); open.AddSpecifier("MenuItem", "Extensions…"); open.AddSpecifier("Menu", "Window");
        open.AddSpecifier("View", "menu"); open.AddSpecifier("Window", int32(0));
        Require(Query(app, open).GetInt32("error", B_ERROR) == B_OK, "open extension manager from browser menu");
        Require(Wait([&] { manager = Manager(app); return manager.IsValid() && Enabled(manager, "extension-add"); }), "extension startup finishes and manager is ready");
        const auto catalogPath = profile / "Extensions/catalog.json";
        const auto actionName = "extension-action-" + identity;
        const auto actionTitle = [&] { return String(Property(View(window, actionName.c_str()), "Label"), "result"); };
        if (phase == "install") {
            Require(Entries(manager) == 0, "isolated browser starts with no extensions");
            ChoosePackage(app, manager, fixtures / "corrupt.crx");
            Require(Wait([&] { return Enabled(manager, "extension-add") && !Enabled(manager, "extension-install")
                && !Text(manager, "extension-status").empty(); }), "corrupt CRX is rejected before consent");
            Require(Entries(manager) == 0 && !View(window, actionName.c_str()).IsValid(), "rejected package creates no catalog entry or action");
            for (int attempt = 0; attempt < 2; ++attempt) {
                ChoosePackage(app, manager, fixtures / "rsa.crx");
                Require(Wait([&] { return Enabled(manager, "extension-install"); }), "valid CRX reaches native consent");
                const auto body = Text(manager, "extension-details");
                Require(body.find("Package signature verified. Chrome Web Store provenance has not been verified.") != std::string::npos
                    && body.find("Requested access:") != std::string::npos && body.find("storage") != std::string::npos,
                    "consent distinguishes verified signature from store provenance and lists requested storage access");
                Require(Entries(manager) == 0 && !View(window, actionName.c_str()).IsValid(), "signature verification alone does not install or execute the extension");
                if (watch) snooze(2000000);
                Screen(control / "consent.ppm");
                if (!attempt) {
                    Button(manager, "extension-cancel");
                    Require(Wait([&] { return Enabled(manager, "extension-add") && !Enabled(manager, "extension-install"); }), "cancel releases the prepared signed package");
                    Require(Entries(manager) == 0, "cancelled CRX leaves no installation");
                } else Button(manager, "extension-install");
            }
            Require(Wait([&] { return Entries(manager) == 1 && Enabled(manager, "extension-add"); }), "approved signed extension joins the catalog");
            const auto catalog = ReadJSON(catalogPath);
            Require(catalog.is_object() && catalog["extensions"].size() == 1, "catalog contains exactly the approved extension");
            const auto entry = catalog["extensions"][0];
            Require(entry["identifier"].get<std::string>() == identity && entry["enabled"].get<bool>()
                && entry["name"].get<std::string>() == "Signed CRX runtime fixture" && entry["version"].get<std::string>() == "1.0",
                "catalog identity comes from the CRX signer despite the conflicting manifest identity");
            std::ofstream(control / "installed.json") << entry.dump(2) << '\n';
        }
        if (phase == "corrupt-startup" || phase == "updated-startup") {
            Require(Entries(manager) == 1, "rejected startup retains the installation record");
            const auto details = Text(manager, "extension-details");
            Require(details.find("Enabled, but not running") != std::string::npos && details.find(identity) != std::string::npos,
                "startup failure is visible for the retained signed identity");
            if (phase == "updated-startup")
                Require(details.find("changed and requires new approval") != std::string::npos, "valid newly signed resources require fresh approval");
            Require(!View(window, actionName.c_str()).IsValid(), "rejected startup creates no extension action");
            if (watch) snooze(2000000);
            Screen(control / (phase + ".ppm"));
        } else {
            const int boot = phase == "install" ? 1 : phase == "startup" ? 2 : 3;
            Require(Wait([&] { return actionTitle() == "CRX " + identity + " boot " + std::to_string(boot); }),
                "actual signed background reports chrome.runtime.id and persisted boot " + std::to_string(boot));
            Send(manager, B_QUIT_REQUESTED);
            Require(Wait([&] { return !manager.IsValid(); }), "manager closes before signed popup interaction");
            FocusWindow(window); Button(window, actionName.c_str());
            Require(Wait([&] { auto popup = ActionPopup(app); return popup.IsValid()
                && !Property(popup, "Hidden").GetBool("result", true)
                && actionTitle() == "CRX POPUP " + identity + " boot " + std::to_string(boot) + " PASS"; }),
                "signed popup runs with matching Chrome/Firefox IDs, signed resources and the background's stored boot");
            if (watch) snooze(2000000);
            Screen(control / (phase + "-popup.ppm"));
            Send(ActionPopup(app), B_QUIT_REQUESTED);
            Require(Wait([&] { return !ActionPopup(app).IsValid(); }), "signed popup closes cleanly");
        }
        Send(app, B_QUIT_REQUESTED);
        Require(Wait([&] { team_info info; return get_team_info(team, &info) == B_BAD_TEAM_ID; }), "CRX browser exits normally");
        std::printf("CRX_BROWSER_RESULT PASS phase=%s checks=%d\n", phase.c_str(), checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "CRX_BROWSER_RESULT FAIL phase=%s checks=%d error=%s\n", phase.c_str(), checks, error.what());
        if (manager.IsValid()) { try { std::fprintf(stderr, "MANAGER %s\n", Text(manager, "extension-details").c_str()); } catch (...) { } }
        if (verified) { try { Screen(control / (phase + "-failure.ppm")); } catch (...) { } Cleanup(app, team); }
        return 1;
    }
}
