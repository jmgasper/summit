// Exercise the real HTML color input and its native BColorControl picker.
#define SUMMIT_EXTENSION_MANAGER_HELPERS_ONLY
#include "ModernExtensionManagerTests.cpp"

int main(int argc, char** argv)
{
    if (argc != 5) return 2;
    status_t status;
    BApplication application("application/x-vnd.Summit-color-input-tests", &status);
    if (status != B_OK) return 1;
    const auto team = static_cast<team_id>(std::strtol(argv[1], nullptr, 10));
    BMessenger app;
    bool verified = false;
    try {
        app_info info;
        Require(Wait([&] { app = BMessenger(nullptr, team); return app.IsValid() && be_roster->GetRunningAppInfo(team, &info) == B_OK; }), "owned browser registers");
        BPath executable(&info.ref);
        Require(std::filesystem::canonical(executable.Path()) == std::filesystem::canonical(argv[2]), "owned executable matches the color-picker build");
        verified = true;
        const auto browser = Window(app, 0);
        auto snapshot = [&] {
            auto state = State(browser); BMessage tab;
            if (state.FindMessage("tab", Index(state, Selected(state)), &tab) != B_OK || tab.GetBool("loading", true)) return json();
            auto title = String(tab, "title");
            return title.starts_with("COLOR ") ? json::parse(title.substr(6), nullptr, false) : json();
        };
        auto picker = [&] { return NamedWindow(app, "Choose color — Summit"); };
        auto color = [&](const BMessenger& window) { return uint32(Property(View(window, "color-picker"), "Value").GetInt32("result", -1)); };
        auto setColor = [&](const BMessenger& window, uint32 value) {
            BMessage set(B_SET_PROPERTY); set.AddSpecifier("Value"); set.AddInt32("data", static_cast<int32>(value));
            Require(Query(View(window, "color-picker"), set).GetInt32("error", B_ERROR) == B_OK, "edit actual BColorControl value");
        };
        auto open = [&] {
            const auto page = Page(browser, Selected(State(browser)));
            Click(page, BPoint(60, 100));
            Require(Wait([&] { return picker().IsValid(); }), "genuine page input opens the native color picker");
            return picker();
        };
        Require(Wait([&] { return Has(snapshot(), "type", "color"); }), "HTML input reports the color type");
        auto report = snapshot();
        Require(Has(report, "value", "#123456") && Has(report, "noActivation", "NotAllowedError")
            && Has(report, "disabledPicker", "InvalidStateError"), "initial value, activation and disabled controls follow HTML rules");
        const auto originalTab = Selected(State(browser));
        auto window = open();
        Require(color(window) == 0x12345600, "BColorControl starts with the HTML value");
        setColor(window, 0x76543200);
        Button(window, "color-cancel");
        Require(Wait([&] { return !picker().IsValid(); }), "Cancel closes the native picker");
        Require(Has(snapshot(), "value", "#123456") && Has(snapshot(), "input", 0) && Has(snapshot(), "change", 0), "cancel preserves the DOM value and fires no events");
        window = open();
        setColor(window, 0x76543200);
        Button(window, "color-done");
        Require(Wait([&] { auto state = snapshot(); return !picker().IsValid() && Has(state, "value", "#765432")
            && Has(state, "input", 1) && Has(state, "change", 1); }), "Done commits the native RGB value with input and change events");
        window = open();
        Key(Page(browser, originalTab), "u");
        Require(Wait([&] { return color(window) == 0xfedcba00 && Has(snapshot(), "value", "#fedcba"); }), "script value updates the open native picker");
        Button(window, "color-cancel");
        Require(Wait([&] { return !picker().IsValid(); }), "picker cancels after script update");
        Require(Has(snapshot(), "value", "#fedcba") && Has(snapshot(), "change", 1), "cancel does not undo an author script's update");
        window = open();
        const auto suggestions = View(window, "suggested-colors");
        Require(suggestions.IsValid(), "datalist supplies native color suggestions");
        Key(Child(suggestions, 0), " ", 0x5e);
        Require(Wait([&] { return color(window) == 0x00ff0000; }), "native datalist swatch updates BColorControl");
        // Capture the real picker before committing the selected swatch.
        BScreen screen; auto frame = screen.Frame(); BBitmap bitmap(frame, B_RGBA32);
        Require(bitmap.InitCheck() == B_OK && screen.ReadBitmap(&bitmap, false, &frame) == B_OK, "capture native color-picker pixels");
        std::ofstream image(argv[4], std::ios::binary);
        image << "P6\n" << frame.IntegerWidth() + 1 << ' ' << frame.IntegerHeight() + 1 << "\n255\n";
        for (int y = 0; y <= frame.IntegerHeight(); ++y) for (int x = 0; x <= frame.IntegerWidth(); ++x) {
            auto* pixel = static_cast<const uint8*>(bitmap.Bits()) + y * bitmap.BytesPerRow() + x * 4;
            const char rgb[] { char(pixel[2]), char(pixel[1]), char(pixel[0]) }; image.write(rgb, 3);
        }
        image.close(); Require(bool(image), "write native screenshot");
        Button(window, "color-done");
        Require(Wait([&] { return !picker().IsValid() && Has(snapshot(), "value", "#00ff00") && Has(snapshot(), "change", 2); }), "suggested color commits to the DOM");
        open();
        Send(browser, summit::kNewTab, -1, "about:blank");
        Require(Wait([&] { return Selected(State(browser)) != originalTab && !picker().IsValid(); }), "switching away dismisses the hidden tab's picker");
        SelectTab(browser, originalTab);
        open();
        Send(browser, summit::kNavigate, -1, "about:blank");
        Require(Wait([&] { return !picker().IsValid(); }), "navigation cancels the old document's picker");
        Send(browser, summit::kNavigate, -1, argv[3]);
        Require(Wait([&] { return Has(snapshot(), "type", "color"); }), "fresh color document loads");
        open();
        Send(app, B_QUIT_REQUESTED);
        Require(Wait([&] { team_info info; return get_team_info(team, &info) == B_BAD_TEAM_ID; }), "browser exits normally with picker open");
        std::printf("COLOR_INPUT_RESULT PASS checks=%d\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "COLOR_INPUT_RESULT FAIL checks=%d error=%s\n", checks, error.what());
        State(Window(app, 0)).PrintToStream();
        if (verified) Cleanup(app, team);
        return 1;
    }
}
