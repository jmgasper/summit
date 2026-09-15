// Exercise actual native controls and the real installer/extension runtime.
#define main SummitCloseHarnessEntry
#include "ModernCloseTests.cpp"
#undef main

static BMessenger NamedWindow(const BMessenger& app, const char* title)
{
    for (int32 i = 0; i < 16; ++i) {
        auto candidate = Window(app, i);
        if (!candidate.IsValid()) break;
        if (String(Property(candidate, "Title"), "result") == title) return candidate;
    }
    return {};
}
static BMessenger Manager(const BMessenger& app) { return NamedWindow(app, "Extensions — Summit"); }
static BMessenger Picker(const BMessenger& app) { return NamedWindow(app, "Choose extension package — Summit"); }
static json ReadJSON(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input) return json();
    return json::parse(input, nullptr, false);
}
static int Entries(const BMessenger& manager)
{
    return Property(View(manager, "extension-list"), "Item", B_COUNT_PROPERTIES).GetInt32("result", -1);
}
static bool Enabled(const BMessenger& manager, const char* name)
{
    return Property(View(manager, name), "Enabled").GetBool("result", false);
}
static std::string Text(const BMessenger& manager, const char* name)
{
    const auto view = View(manager, name);
    if (std::string(name) == "extension-details") {
        // BTextView exposes Text through a byte range, unlike BStringView.
        const auto length = Property(view, "Text", B_COUNT_PROPERTIES).GetInt32("result", -1);
        Require(length >= 0 && length <= 65536, "native permission/detail text has a bounded byte length");
        BMessage request(B_GET_PROPERTY);
        request.AddSpecifier("Text", int32(0), length);
        return String(Query(view, request), "result");
    }
    return String(Property(view, "Text"), "result");
}
static void Button(const BMessenger& manager, const char* name)
{
    Require(Wait([&] { return Enabled(manager, name); }), std::string("native control enabled: ") + name);
    Key(View(manager, name), " ", 0x5e);
}

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--check-executable-unused") return SummitCloseHarnessEntry(argc, argv);
    if (argc != 9) return 2;
    status_t status;
    BApplication application("application/x-vnd.Kunanyi-Summit-extension-manager-tests", &status);
    if (status != B_OK) return 1;
    const auto team = static_cast<team_id>(std::strtol(argv[1], nullptr, 10));
    const std::filesystem::path profile(argv[3]), package(argv[4]), observations(argv[5]);
    const std::string nonce(argv[6]), phase(argv[7]), identity(argv[8]);
    const auto catalogPath = profile / "Extensions/catalog.json";
    BMessenger app, manager;
    bool verified = false;
    try {
        app_info info;
        Require(Wait([&] {
            app = BMessenger(nullptr, team);
            return app.IsValid() && be_roster->GetRunningAppInfo(team, &info) == B_OK;
        }), "owned browser registers with native roster");
        BPath executable(&info.ref);
        Require(std::filesystem::canonical(executable.Path()) == std::filesystem::canonical(argv[2]), "browser executable matches frozen bundle");
        verified = true;
        const auto reports = [&]() {
            auto value = ReadJSON(observations);
            return value.is_object() && value.contains("reports") ? value["reports"] : json::array();
        };
        const auto count = [&]() { return reports().size(); };
        const auto boot = [&](int number) {
            Require(Wait([&] {
                auto list = reports();
                return list.size() == size_t(number) && list.back().value("boot", 0) == number
                    && list.back().value("nonce", "") == nonce && list.back().value("id", "") == identity
                    && list.back().value("granted", false) && !list.back().value("optionalGranted", true);
            }), "actual background reports boot " + std::to_string(number) + " with approved but no optional grants");
        };
        BMessage openManager(B_EXECUTE_PROPERTY);
        openManager.AddSpecifier("MenuItem", "Extensions…");
        openManager.AddSpecifier("Menu", "Window");
        openManager.AddSpecifier("View", "menu");
        openManager.AddSpecifier("Window", int32(0));
        Require(Query(app, openManager).GetInt32("error", B_ERROR) == B_OK, "open extension manager through browser Window menu");
        Require(Wait([&] { manager = Manager(app); return manager.IsValid() && Enabled(manager, "extension-add"); }), "native extension manager is ready");
        const auto openPicker = [&]() {
            Button(manager, "extension-add");
            BMessenger picker;
            Require(Wait([&] { picker = Picker(app); return picker.IsValid() && !Property(picker, "Hidden").GetBool("result", true)
                && View(picker, "ActualPoseView").IsValid(); }), "Add extension opens native file picker");
            return picker;
        };
        const auto selectPackage = [&](const std::filesystem::path& selectedPath) {
            const auto picker = openPicker();
            auto poses = View(picker, "ActualPoseView");
            BEntry parent(selectedPath.parent_path().c_str()), entry(selectedPath.c_str());
            entry_ref parentRef, ref;
            Require(parent.GetRef(&parentRef) == B_OK && entry.GetRef(&ref) == B_OK, "fixture package and parent have native entry references");
            // Tracker's native PoseView scripting 'sref' specifier opens the
            // directory inside the real picker, just like a folder activation.
            BMessage specifier('sref');
            specifier.AddString("property", "Entry");
            specifier.AddRef("refs", &parentRef);
            BMessage navigate(B_EXECUTE_PROPERTY);
            navigate.AddSpecifier(&specifier);
            Require(Query(poses, navigate).GetInt32("error", B_OK) == B_OK, "navigate native picker to package parent");
            Require(Wait([&] {
                poses = View(picker, "ActualPoseView");
                entry_ref actual;
                return Property(poses, "Path").FindRef("result", &actual) == B_OK && actual == parentRef;
            }), "native picker shows requested package directory");
            Require(Wait([&] {
                BMessage selection(B_SET_PROPERTY);
                selection.AddSpecifier("Selection");
                selection.AddRef("data", &ref);
                Query(poses, selection);
                entry_ref actual;
                return Property(poses, "Selection").FindRef("result", &actual) == B_OK && actual == ref;
            }), "select package in actual native file list");
            Button(picker, "default button");
            Require(Wait([&] { return Property(picker, "Hidden").GetBool("result", false); }), "Review extension accepts picker selection and hides picker");
        };
        const auto catalog = [&]() { return ReadJSON(catalogPath); };
        if (phase == "install") {
            Require(Entries(manager) == 0 && count() == 0, "fresh profile has no installation or extension execution");
            auto picker = openPicker();
            Button(picker, "cancel button");
            Require(Wait([&] { return Property(picker, "Hidden").GetBool("result", false); }) && Enabled(manager, "extension-add")
                && count() == 0 && !std::filesystem::exists(catalogPath), "cancelling file picker leaves importer and profile untouched");
            selectPackage(package.parent_path() / "package.xpi");
            Require(Wait([&] { return Enabled(manager, "extension-install"); }), "package preparation reaches native consent");
            const auto text = Text(manager, "extension-details");
            Require(text.find("storage") != std::string::npos && text.find("http://10.0.2.2/*") != std::string::npos
                && text.find("summitUnknownPermission") != std::string::npos && text.find("tabs") != std::string::npos,
                "consent displays recognized, unknown, host and optional requirements");
            Require(count() == 0 && !std::filesystem::exists(catalogPath), "review does not execute or install the package");
            BMessage stale(summit::kExtensionApprove);
            stale.AddMessenger("window", manager);
            stale.AddUInt64("generation", UINT64_MAX);
            stale.AddBool("allow_files", true);
            Require(app.SendMessage(&stale, static_cast<BHandler*>(nullptr), 500000) == B_OK, "submit stale approval generation");
            snooze(200000);
            Require(Enabled(manager, "extension-install") && count() == 0 && !std::filesystem::exists(catalogPath), "stale approval cannot activate the current package");
            Button(manager, "extension-cancel");
            Require(Wait([&] { return Enabled(manager, "extension-add") && !Enabled(manager, "extension-install"); }), "cancel dismisses consent and releases importer");
            Require(count() == 0 && std::filesystem::is_empty(profile / "Extensions/packages"), "cancelled import leaves no package or execution");
            selectPackage(package);
            Send(manager, B_QUIT_REQUESTED);
            Require(Wait([&] { return !manager.IsValid(); }), "manager can close immediately after selecting a package");
            Send(app, summit::kShowExtensions);
            Require(Wait([&] { manager = Manager(app); return manager.IsValid() && Enabled(manager, "extension-add"); }), "manager close cancels even an import whose first snapshot was pending");
            Require(count() == 0 && std::filesystem::is_empty(profile / "Extensions/packages"), "closing during import releases owned files without running code");
            selectPackage(package.parent_path() / "package.xpi");
            Require(Wait([&] { return Enabled(manager, "extension-install"); }), "XPI import can be reviewed after cancellation");
            Button(manager, "extension-install");
            boot(1);
            Require(Wait([&] { return Entries(manager) == 1 && Enabled(manager, "extension-toggle"); }), "approved extension appears in native manager");
            auto saved = catalog();
            Require(saved.is_object() && saved["extensions"].size() == 1
                && saved["extensions"][0]["identifier"] == identity
                && saved["extensions"][0]["enabled"] == true, "approval commits stable identity and enabled installation");
            Require(saved["extensions"][0]["allow_file_urls"] == false && saved["extensions"][0]["allow_private_browsing"] == false,
                "file and private access stay disabled by default");
            selectPackage(package);
            Require(Wait([&] { return Enabled(manager, "extension-add") && Text(manager, "extension-status").find("already installed") != std::string::npos; }),
                "duplicate declared identity is rejected before consent or activation");
            Require(count() == 1 && catalog() == saved, "duplicate import preserves runtime and saved installation");
            Button(manager, "extension-toggle");
            Require(Wait([&] {
                auto state = catalog();
                return Enabled(manager, "extension-toggle") && String(Property(View(manager, "extension-toggle"), "Label"), "result") == "Enable"
                    && state.is_object() && state["extensions"][0]["enabled"] == false;
            }), "disable unloads the runtime and persists disabled state");
            Button(manager, "extension-toggle");
            boot(2);
            Require(Wait([&] { return Enabled(manager, "extension-toggle") && catalog()["extensions"][0]["enabled"] == true; }),
                "enable reloads the approved package and preserves its storage");
        } else if (phase == "remove") {
            boot(3);
            Require(Entries(manager) == 1 && Text(manager, "extension-details").find("Running") != std::string::npos,
                "UI-installed extension restores automatically in another browser process");
            Button(manager, "extension-remove");
            Require(Wait([&] { auto state = catalog(); return Entries(manager) == 0 && Enabled(manager, "extension-add")
                && state.is_object() && state["extensions"].empty(); }), "remove unloads extension and removes its startup record");
        } else if (phase == "after-removal") {
            Require(Entries(manager) == 0 && catalog()["extensions"].empty(), "removed installation stays absent after another browser restart");
            snooze(300000);
            Require(count() == 3, "removed extension does not start again");
            openPicker();
        } else throw std::runtime_error("unknown test phase");
        Send(app, B_QUIT_REQUESTED);
        Require(Wait([&] { team_info info; return get_team_info(team, &info) != B_OK; }), "browser closes with extension manager and any file picker still open");
        std::printf("EXTENSION_MANAGER_RESULT PASS phase=%s checks=%d\n", phase.c_str(), checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "EXTENSION_MANAGER_RESULT FAIL %s\n", error.what());
        if (verified) Cleanup(app, team);
        return 1;
    }
}
