// Pixel observations are synchronized by actual native toolbar clicks.
#define SUMMIT_EXTENSION_OVERFLOW_HELPERS_ONLY
#include "ModernExtensionOverflowTests.cpp"

static bool IconMatches(const BMessage& action, const json& color)
{
    const void* data = nullptr;
    ssize_t size = 0;
    if (!action.GetUInt64("load_identifier", 0) || !action.GetUInt64("page_identifier", 0)
        || action.FindData("icon_bgra", B_RAW_TYPE, &data, &size) != B_OK || size != 1024) return false;
    const auto* pixels = static_cast<const uint8*>(data);
    for (int y = 4; y < 12; ++y) for (int x = 4; x < 12; ++x) {
        const auto* pixel = pixels + (y * 16 + x) * 4;
        for (int channel = 0; channel < 4; ++channel)
            if (std::abs(int(pixel[channel == 3 ? 3 : 2 - channel]) - color[channel].get<int>()) > 2) return false;
    }
    return true;
}

static void IconScreenPixels(const BMessenger& window, const char* name, const json& color)
{
    auto target = View(window, name);
    BRect windowFrame, frame;
    Require(Property(window, "Frame").FindRect("result", &windowFrame) == B_OK, "icon window has screen bounds");
    std::function<bool(const BMessenger&, BPoint, int)> find = [&](const BMessenger& parent, BPoint origin, int depth) {
        if (depth > 12) return false;
        const auto count = Property(parent, "View", B_COUNT_PROPERTIES).GetInt32("result", 0);
        for (int32 i = 0; i < count; ++i) {
            auto child = Child(parent, i);
            BRect local;
            if (Property(child, "Frame").FindRect("result", &local) != B_OK) continue;
            local.OffsetBy(origin);
            if (child == target) { frame = local; return true; }
            if (find(child, local.LeftTop(), depth + 1)) return true;
        }
        return false;
    };
    BScreen screen;
    Require(find(window, windowFrame.LeftTop(), 0) && frame.Width() >= 24 && frame.Height() >= 24 && screen.Frame().Contains(frame),
        "dynamic icon capture is inside the actual native control");
    BBitmap bitmap(BRect(0, 0, frame.Width(), frame.Height()), B_RGBA32);
    Require(Wait([&] {
        if (bitmap.InitCheck() != B_OK || screen.ReadBitmap(&bitmap, false, &frame) != B_OK) return false;
        int cx = int(frame.Width()) / 2, cy = int(frame.Height()) / 2;
        auto* pixels = static_cast<const uint8*>(bitmap.Bits());
        auto* background = pixels + cy * bitmap.BytesPerRow() + 4 * 4;
        const int alpha = color[3].get<int>();
        for (int y = cy - 2; y <= cy + 2; ++y) for (int x = cx - 2; x <= cx + 2; ++x) {
            auto* pixel = pixels + y * bitmap.BytesPerRow() + x * 4;
            for (int channel = 0; channel < 3; ++channel) {
                const int bgra = 2 - channel;
                const int expected = (color[channel].get<int>() * alpha + int(background[bgra]) * (255 - alpha) + 127) / 255;
                if (std::abs(int(pixel[bgra]) - expected) > 3) return false;
            }
        }
        return true;
    }), "actual desktop pixels render the dynamic icon with its alpha");
}

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--check-executable-unused") return SummitCloseHarnessEntry(argc, argv);
    if (argc != 5 && argc != 6) return 2;
    const bool watch = argc == 6 && std::string(argv[5]) == "watch";
    status_t status;
    BApplication test("application/x-vnd.Kunanyi-Summit-icon-tests", &status);
    if (status != B_OK) return 1;
    const auto team = static_cast<team_id>(std::strtol(argv[1], nullptr, 10));
    BMessenger app;
    bool verified = false;
    try {
        app_info info;
        Require(Wait([&] { app = BMessenger(nullptr, team); return app.IsValid() && be_roster->GetRunningAppInfo(team, &info) == B_OK; }), "owned icon browser registers");
        BPath executable(&info.ref);
        Require(std::filesystem::canonical(executable.Path()) == std::filesystem::canonical(argv[2]), "icon browser matches frozen executable");
        verified = true;
        auto window = Window(app, 0);
        BMessage open(B_EXECUTE_PROPERTY); open.AddSpecifier("MenuItem", "Extensions…"); open.AddSpecifier("Menu", "Window");
        open.AddSpecifier("View", "menu"); open.AddSpecifier("Window", int32(0));
        Require(Query(app, open).GetInt32("error", B_ERROR) == B_OK, "open native extension manager");
        BMessenger manager;
        Require(Wait([&] { manager = Manager(app); return manager.IsValid() && Enabled(manager, "extension-add"); }), "manager is ready for the icon fixture");
        InstallFolder(app, manager, argv[3], 1);
        Send(manager, B_QUIT_REQUESTED);
        Require(Wait([&] { return !manager.IsValid(); }), "manager closes before icon capture");
        FocusWindow(window);
        constexpr auto control = "extension-action-summit-action-icons";
        const auto title = [&] { return String(Property(View(window, control), "Label"), "result"); };
        Require(Wait([&] { return title() == "Icons ready" && Enabled(window, control); }), "real background page is ready for icon changes");
        const auto first = Selected(State(window));
        const auto pages = std::filesystem::path(argv[3]).parent_path() / "pages";
        int32 second = -1;
        auto plan = ReadJSON(argv[4]);
        Require(plan.is_array() && !plan.empty(), "independent native pixel expectations are available");
        int step = 0;
        for (const auto& item : plan) {
            const auto operation = item.value("operation", "click");
            if (operation == "click") {
                Button(window, control);
                const auto expected = "Icon " + std::to_string(++step) + ": " + item["label"].get<std::string>();
                Require(Wait([&] {
                    const auto actual = title();
                    if (actual.starts_with("ICON ERROR")) throw std::runtime_error(actual);
                    return actual == expected;
                }), expected + " completes through the real extension API");
            } else if (operation == "new-tab") {
                Send(window, summit::kNewTab, -1, (pages / "second.html").string());
                Require(Wait([&] { return Count(State(window)) == 2 && Selected(State(window)) != first; }), "second native tab is selected");
                second = Selected(State(window));
            } else if (operation == "first-tab") SelectTab(window, first);
            else if (operation == "second-tab") SelectTab(window, second);
            else if (operation == "navigate") Send(window, summit::kNavigate, -1, (pages / "navigated.html").string());
            else if (operation == "close-first") {
                Send(window, summit::kCloseTab, first);
                Require(Wait([&] { return Count(State(window)) == 1 && Selected(State(window)) == second; }), "original native tab closes");
            } else Require(operation == "initial", "known native icon test operation");
            const auto& color = item["rgba"];
            BMessage observedIcon;
            const bool pixelsMatch = Wait([&] {
                observedIcon = ExtensionAction(window, "summit-action-icons");
                return IconMatches(observedIcon, color);
            });
            if (!pixelsMatch) {
                const void* data = nullptr;
                ssize_t size = 0;
                if (observedIcon.FindData("icon_bgra", B_RAW_TYPE, &data, &size) == B_OK && size == 1024) {
                    const auto* center = static_cast<const uint8*>(data) + (8 * 16 + 8) * 4;
                    std::printf("ICON_PIXEL_MISMATCH expected=%s center_rgba=[%u,%u,%u,%u]\n", color.dump().c_str(), center[2], center[1], center[0], center[3]);
                }
            }
            Require(pixelsMatch, item["label"].get<std::string>() + " has the expected decoded SDK pixels");
            IconScreenPixels(window, control, color);
            if (watch) snooze(400000);
        }
        Send(app, B_QUIT_REQUESTED);
        Require(Wait([&] { team_info info; return get_team_info(team, &info) == B_BAD_TEAM_ID; }), "icon browser quits after all image transitions");
        std::printf("EXTENSION_ICONS_RESULT PASS cases=%d observations=%zu checks=%d\n", step, plan.size(), checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "EXTENSION_ICONS_RESULT FAIL %s checks=%d\n", error.what(), checks);
        if (verified) Cleanup(app, team);
        return 1;
    }
}
