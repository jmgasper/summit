// Multiple independently loaded extensions using the real native picker/menu.
#define SUMMIT_EXTENSION_MANAGER_HELPERS_ONLY
#include "ModernExtensionManagerTests.cpp"

static void InstallFolder(const BMessenger& app, const BMessenger& manager, const std::filesystem::path& path, int count)
{
    FocusWindow(manager);
    Button(manager, "extension-add");
    BMessenger picker;
    Require(Wait([&] { picker = Picker(app); return picker.IsValid() && !Property(picker, "Hidden").GetBool("result", true); }), "real installation picker opens");
    BEntry parent(path.parent_path().c_str()), entry(path.c_str());
    entry_ref parentRef, ref;
    Require(parent.GetRef(&parentRef) == B_OK && entry.GetRef(&ref) == B_OK, "extension folder has a native reference");
    auto poses = View(picker, "ActualPoseView");
    BMessage specifier('sref'); specifier.AddString("property", "Entry"); specifier.AddRef("refs", &parentRef);
    BMessage navigate(B_EXECUTE_PROPERTY); navigate.AddSpecifier(&specifier);
    Query(poses, navigate);
    Require(Wait([&] { entry_ref actual; return Property(poses, "Path").FindRef("result", &actual) == B_OK && actual == parentRef; }), "picker navigates to extension folders");
    Require(Wait([&] {
        BMessage selection(B_SET_PROPERTY); selection.AddSpecifier("Selection"); selection.AddRef("data", &ref);
        Query(poses, selection);
        entry_ref actual;
        return Property(poses, "Selection").FindRef("result", &actual) == B_OK && actual == ref;
    }), "extension is selected in the real file list");
    Button(picker, "default button");
    Require(Wait([&] { return Enabled(manager, "extension-install"); }), "extension reaches native consent");
    Button(manager, "extension-install");
    Require(Wait([&] { return Entries(manager) == count && Enabled(manager, "extension-add"); }), "approved extension joins the running catalog");
}

static BMessage ExtensionAction(const BMessenger& window, const std::string& identity)
{
    auto state = State(window);
    BMessage item;
    for (int32 i = 0; state.FindMessage("extension_action", i, &item) == B_OK; ++i)
        if (String(item, "extension_identifier") == identity) return item;
    return {};
}

static BMessenger OverflowMenu(const BMessenger& app)
{
    // BApplication::WindowAt deliberately excludes BMenuWindow. Enumerate
    // native loopers so the test also finds menus after their host is marked
    // as a menu window, rather than depending on that construction race.
    const auto loopers = Property(app, "Loopers");
    BMessenger looper;
    for (int32 i = 0; loopers.FindMessenger("result", i, &looper) == B_OK; ++i) {
        auto menu = View(looper, "Extension actions");
        if (menu.IsValid()) return menu;
    }
    return {};
}

static BMessage MenuItemProperty(const BMessenger& menu, int32 index, const char* property)
{
    BMessage request(B_GET_PROPERTY); request.AddSpecifier(property); request.AddSpecifier("MenuItem", index);
    return Query(menu, request);
}

#ifndef SUMMIT_EXTENSION_OVERFLOW_HELPERS_ONLY
int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--check-executable-unused") return SummitCloseHarnessEntry(argc, argv);
    if (argc != 4) return 2;
    status_t status; BApplication test("application/x-vnd.Kunanyi-Summit-overflow-tests", &status);
    if (status != B_OK) return 1;
    auto team = static_cast<team_id>(std::strtol(argv[1], nullptr, 10));
    BMessenger app;
    bool verified = false;
    try {
        app_info info;
        Require(Wait([&] { app = BMessenger(nullptr, team); return app.IsValid() && be_roster->GetRunningAppInfo(team, &info) == B_OK; }), "owned browser registers");
        BPath executable(&info.ref);
        Require(std::filesystem::canonical(executable.Path()) == std::filesystem::canonical(argv[2]), "browser matches frozen executable");
        verified = true;
        auto window = Window(app, 0);
        BMessage open(B_EXECUTE_PROPERTY); open.AddSpecifier("MenuItem", "Extensions…"); open.AddSpecifier("Menu", "Window");
        open.AddSpecifier("View", "menu"); open.AddSpecifier("Window", int32(0));
        Require(Query(app, open).GetInt32("error", B_ERROR) == B_OK, "open manager through the browser menu");
        BMessenger manager;
        Require(Wait([&] { manager = Manager(app); return manager.IsValid() && Enabled(manager, "extension-add"); }), "manager ready for six independent extensions");
        for (int i = 1; i <= 5; ++i) InstallFolder(app, manager, std::filesystem::path(argv[3]) / std::to_string(i), i);
        Require(Wait([&] { return !ExtensionAction(window, "summit-overflow-5").GetBool("enabled", true); }), "single overflow action becomes disabled");
        FocusWindow(window);
        Button(window, "extension-actions-more");
        BMessenger singleMenu;
        Require(Wait([&] { singleMenu = OverflowMenu(app); return singleMenu.IsValid(); }), "single-action overflow opens");
        Require(Property(singleMenu, "MenuItem", B_COUNT_PROPERTIES).GetInt32("result", -1) == 3
            && String(MenuItemProperty(singleMenu, 2, "Label"), "result") == "Manage extensions…",
            "overflow keeps extension management reachable when its only action is disabled");
        snooze(75000);
        Key(singleMenu, std::string(1, B_DOWN_ARROW));
        Key(singleMenu, std::string(1, B_ENTER));
        Require(Wait([&] { return !OverflowMenu(app).IsValid() && Property(manager, "Active").GetBool("result", false); }),
            "keyboard navigation skips disabled action and opens the manager");
        InstallFolder(app, manager, std::filesystem::path(argv[3]) / "6", 6);
        Require(Wait([&] { return !ExtensionAction(window, "summit-overflow-5").GetBool("enabled", true)
            && ExtensionAction(window, "summit-overflow-6").GetBool("enabled", false); }), "fifth action is disabled while sixth stays enabled");
        const auto fifthTitle = String(ExtensionAction(window, "summit-overflow-5"), "title");
        Send(manager, B_QUIT_REQUESTED);
        Require(Wait([&] { return !manager.IsValid(); }), "manager closes before toolbar interaction");
        FocusWindow(window);
        for (int i = 1; i <= 4; ++i) {
            const auto name = "extension-action-summit-overflow-" + std::to_string(i);
            Require(View(window, name.c_str()).IsValid() && Enabled(window, name.c_str()), "visible toolbar action " + std::to_string(i));
        }
        Require(!View(window, "extension-action-summit-overflow-5").IsValid() && !View(window, "extension-action-summit-overflow-6").IsValid()
            && Enabled(window, "extension-actions-more"), "extra actions use a single overflow control");
        BRect address, frame;
        Require(Property(View(window, "address"), "Frame").FindRect("result", &address) == B_OK && address.Width() >= 240
            && Property(window, "Frame").FindRect("result", &frame) == B_OK && frame.Width() < 1250, "six extensions preserve usable address field and window width");
        const auto beforeChoice = State(window).GetUInt64("extension_action_result_identifier", 0);
        const auto openingStarted = system_time();
        Button(window, "extension-actions-more");
        BMessenger menu;
        Require(Wait([&] { menu = OverflowMenu(app); return menu.IsValid(); }), "native overflow menu opens");
        Require(Property(menu, "MenuItem", B_COUNT_PROPERTIES).GetInt32("result", -1) == 4, "overflow contains two remaining actions and management footer");
        const auto fifthLabel = String(MenuItemProperty(menu, 0, "Label"), "result");
        Require(fifthLabel.starts_with("Action 5 ") && fifthLabel.size() < fifthTitle.size()
            && !MenuItemProperty(menu, 0, "Enabled").GetBool("result", true), "disabled action keeps its label and native disabled state");
        BRect menuFrame;
        Require(Property(menu, "Frame").FindRect("result", &menuFrame) == B_OK && menuFrame.Width() <= 420,
            "long Unicode extension title stays within a usable menu width");
        Require(String(MenuItemProperty(menu, 1, "Label"), "result") == "Action 6"
            && MenuItemProperty(menu, 1, "Enabled").GetBool("result", false), "enabled overflow action is reachable");
        // Exercise a rapid keyboard choice within the opening-click interval,
        // after the native tracking thread's initial 50ms delay.
        bigtime_t clickSpeed = 0;
        Require(get_click_speed(&clickSpeed) == B_OK, "read native menu opening click interval");
        Require(clickSpeed >= 200000, "guest permits a rapid keyboard choice during menu opening");
        snooze(75000);
        Key(menu, std::string(1, B_DOWN_ARROW));
        Key(menu, std::string(1, B_ENTER));
        Require(Wait([&] { const auto state = State(window);
            return state.GetUInt64("extension_action_result_identifier", 0) > beforeChoice
                && state.GetInt32("extension_action_result_error", B_ERROR) == B_OK;
        }) && system_time() - openingStarted < clickSpeed,
            "engine accepts the keyboard choice before the native opening-click interval ends");
        const bool opened = Wait([&] { auto popup = ActionPopup(app); return popup.IsValid() && !Property(popup, "Hidden").GetBool("result", true)
            && String(ExtensionAction(window, "summit-overflow-6"), "title") == "Overflow popup 6"; });
        if (!opened) {
            auto state = State(window);
            std::fprintf(stderr, "Overflow diagnostics: snapshot=%llu receipt=%llu error=%d active=%d popup=%d menu=%d title=%s\n",
                static_cast<unsigned long long>(state.GetUInt64("extension_action_snapshot", 0)),
                static_cast<unsigned long long>(state.GetUInt64("extension_action_result_identifier", 0)),
                state.GetInt32("extension_action_result_error", B_OK), int(Property(window, "Active").GetBool("result", false)),
                int(ActionPopup(app).IsValid()), int(OverflowMenu(app).IsValid()), String(ExtensionAction(window, "summit-overflow-6"), "title").c_str());
        }
        Require(opened,
            "native menu keyboard input executes the sixth extension's real popup JavaScript");
        Require(!OverflowMenu(app).IsValid(), "invoked overflow menu releases its native host");
        Require(String(ExtensionAction(window, "summit-overflow-5"), "title") == fifthTitle
            && !ExtensionAction(window, "summit-overflow-5").GetBool("enabled", true), "popup action update leaves the other extension unchanged");
        Send(ActionPopup(app), B_QUIT_REQUESTED);
        Require(Wait([&] { return !ActionPopup(app).IsValid(); }), "overflow popup closes cleanly");
        const auto lastInvocation = State(window).GetUInt64("extension_action_result_identifier", 0);
        FocusWindow(window);
        const auto dismissOpeningStarted = system_time();
        Button(window, "extension-actions-more");
        Require(Wait([&] { menu = OverflowMenu(app); return menu.IsValid(); }), "overflow reopens for keyboard dismissal");
        snooze(75000);
        Key(menu, std::string(1, B_ESCAPE));
        Require(Wait([&] { return !OverflowMenu(app).IsValid(); }), "rapid Escape releases the native menu host");
        Require(system_time() - dismissOpeningStarted < clickSpeed, "Escape finishes before the native opening-click interval ends");
        snooze(clickSpeed + 100000);
        Require(!OverflowMenu(app).IsValid() && !ActionPopup(app).IsValid()
            && State(window).GetUInt64("extension_action_result_identifier", 0) == lastInvocation,
            "dismissed menu stays closed and does not invoke an extension");
        FocusWindow(window);
        Button(window, "extension-actions-more");
        Require(Wait([&] { return OverflowMenu(app).IsValid(); }), "overflow can reopen before application shutdown");
        Send(app, B_QUIT_REQUESTED);
        Require(Wait([&] { team_info info; return get_team_info(team, &info) != B_OK; }), "application drains with overflow menu still open");
        std::printf("EXTENSION_OVERFLOW_RESULT PASS checks=%d\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "EXTENSION_OVERFLOW_RESULT FAIL %s\n", error.what());
        if (verified) Cleanup(app, team);
        return 1;
    }
}
#endif
