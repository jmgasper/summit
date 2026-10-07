// Native BCalendar, canonical HTML values, constraints, and picker lifecycle.
#define SUMMIT_EXTENSION_MANAGER_HELPERS_ONLY
#include "ModernExtensionManagerTests.cpp"

int main(int argc, char** argv)
{
    if (argc != 5) return 2;
    status_t status;
    BApplication application("application/x-vnd.Summit-date-input-tests", &status);
    if (status != B_OK) return 1;
    const auto team = static_cast<team_id>(std::strtol(argv[1], nullptr, 10));
    BMessenger app;
    bool verified = false;
    try {
        app_info info;
        Require(Wait([&] { app = BMessenger(nullptr, team); return app.IsValid() && be_roster->GetRunningAppInfo(team, &info) == B_OK; }), "owned browser registers");
        BPath executable(&info.ref);
        Require(std::filesystem::canonical(executable.Path()) == std::filesystem::canonical(argv[2]), "owned executable matches date/time build");
        verified = true;
        const auto browser = Window(app, 0);
        auto snapshot = [&] {
            auto state = State(browser); BMessage tab;
            if (state.FindMessage("tab", Index(state, Selected(state)), &tab) != B_OK || tab.GetBool("loading", true)) return json();
            const auto title = String(tab, "title");
            return title.starts_with("DATE ") ? json::parse(title.substr(5), nullptr, false) : json();
        };
        auto picker = [&] { return NamedWindow(app, "Choose date and time — Summit"); };
        auto nativeValue = [&](const BMessenger& window) { return String(Property(View(window, "date-value"), "Value"), "result"); };
        auto setValue = [&](const BMessenger& window, const std::string& value, bool valid = true) {
            BMessage set(B_SET_PROPERTY); set.AddSpecifier("Value"); set.AddString("data", value.c_str());
            Require(Query(View(window, "date-value"), set).GetInt32("error", B_ERROR) == B_OK, "edit native value: " + value);
            Require(Wait([&] { return nativeValue(window) == value && Enabled(window, "date-done") == valid
                && (valid || !Text(window, "date-error").empty()); }), valid ? "native value satisfies constraints" : "invalid value has an explanation and disables Done");
        };
        auto configure = [&](const char* key, const char* type, const char* value) {
            Key(Page(browser, Selected(State(browser))), key);
            Require(Wait([&] { return Has(snapshot(), "type", type) && Has(snapshot(), "value", value)
                && Has(snapshot(), "change", 0); }), std::string("configure real ") + type + " input");
        };
        auto open = [&](bool direct = false) {
            Click(Page(browser, Selected(State(browser))), BPoint(direct ? 100 : 435, 100));
            Require(Wait([&] { return picker().IsValid(); }), "real activation opens native date/time picker");
            return picker();
        };
        auto done = [&](const BMessenger& window, const std::string& expected) {
            Button(window, "date-done");
            Require(Wait([&] { auto state = snapshot(); return !picker().IsValid() && Has(state, "value", expected)
                && Has(state, "input", 1) && Has(state, "change", 1) && Has(state, "valid", true); }), "native selection reaches the DOM with normal events: " + expected);
        };
        Require(Wait([&] { return Has(snapshot(), "type", "date"); }), "date fixture loads");
        const auto report = snapshot();
        for (auto type : {"date", "month", "week", "time", "datetime-local"})
            Require(Has(report["capabilities"], type, true), std::string("HTML supports input type ") + type);
        Require(Has(report, "noActivation", "NotAllowedError") && Has(report, "disabledPicker", "InvalidStateError"), "showPicker retains activation and disabled guards");
        const auto originalTab = Selected(State(browser));
        auto window = open();
        Require(nativeValue(window) == "2024-02-28", "native picker initializes from the HTML date");
        auto calendar = View(window, "date-calendar");
        Require(calendar.IsValid(), "date picker contains the real BCalendarView");
        Key(calendar, std::string(1, B_RIGHT_ARROW));
        Key(calendar, " ", 0x5e);
        Require(Wait([&] { return nativeValue(window) == "2024-02-29"; }), "BCalendar keyboard selection advances to leap day");
        BScreen screen; auto frame = screen.Frame(); BBitmap bitmap(frame, B_RGBA32);
        Require(bitmap.InitCheck() == B_OK && screen.ReadBitmap(&bitmap, false, &frame) == B_OK, "capture native calendar pixels");
        std::ofstream image(argv[4], std::ios::binary);
        image << "P6\n" << frame.IntegerWidth() + 1 << ' ' << frame.IntegerHeight() + 1 << "\n255\n";
        for (int y = 0; y <= frame.IntegerHeight(); ++y) for (int x = 0; x <= frame.IntegerWidth(); ++x) {
            const auto* pixel = static_cast<const uint8*>(bitmap.Bits()) + y * bitmap.BytesPerRow() + x * 4;
            const char rgb[] { char(pixel[2]), char(pixel[1]), char(pixel[0]) }; image.write(rgb, 3);
        }
        image.close(); Require(bool(image), "write calendar screenshot");
        done(window, "2024-02-29");
        configure("1", "date", "2024-02-28"); window = open();
        setValue(window, "2023-02-29", false);
        setValue(window, "1582-10-10");
        done(window, "1582-10-10");
        configure("1", "date", "2024-02-28"); window = open();
        setValue(window, "2025-01-01"); Button(window, "date-cancel");
        Require(Wait([&] { return !picker().IsValid(); }), "Cancel closes the calendar");
        Require(Has(snapshot(), "value", "2024-02-28") && Has(snapshot(), "change", 0), "Cancel preserves the original value and events");
        configure("2", "month", "2024-02"); window = open(true);
        setValue(window, "2025-13", false); setValue(window, "2025-03"); done(window, "2025-03");
        configure("3", "week", "2020-W53"); window = open(true);
        Require(View(window, "date-calendar").IsValid(), "week picker uses BCalendar");
        setValue(window, "2021-W53", false); setValue(window, "2021-W01"); done(window, "2021-W01");
        configure("4", "time", "12:34:56.789"); window = open(true);
        Require(!View(window, "date-calendar").IsValid(), "time-only picker has no unrelated calendar");
        setValue(window, "24:00", false); setValue(window, "23:59:59.999"); done(window, "23:59:59.999");
        configure("5", "datetime-local", "2024-02-29T12:34:56.789"); window = open();
        setValue(window, "2024-03-10T02:30:00.125"); done(window, "2024-03-10T02:30:00.125");
        configure("b", "date", "2024-02-01"); window = open();
        setValue(window, "2024-01-31", false); setValue(window, "2024-03-02", false);
        setValue(window, "2024-02-02", false); setValue(window, "2024-02-03"); done(window, "2024-02-03");
        configure("r", "time", "23:00"); window = open();
        setValue(window, "12:00", false); setValue(window, "01:17", false); setValue(window, "01:15"); done(window, "01:15");
        configure("2", "month", "2024-02"); window = open();
        Button(window, "date-clear");
        Require(Wait([&] { return !picker().IsValid() && Has(snapshot(), "value", "") && Has(snapshot(), "change", 1); }), "Clear removes the HTML value and sends change");
        open(); Send(browser, summit::kNewTab, -1, "about:blank");
        Require(Wait([&] { return Selected(State(browser)) != originalTab && !picker().IsValid(); }), "tab switch dismisses hidden calendar");
        SelectTab(browser, originalTab); open(); Send(browser, summit::kNavigate, -1, "about:blank");
        Require(Wait([&] { return !picker().IsValid(); }), "navigation dismisses old document's calendar");
        Send(browser, summit::kNavigate, -1, argv[3]);
        Require(Wait([&] { return Has(snapshot(), "type", "date"); }), "fresh date document loads");
        open(); Send(app, B_QUIT_REQUESTED);
        Require(Wait([&] { team_info info; return get_team_info(team, &info) == B_BAD_TEAM_ID; }), "browser exits normally with calendar open");
        std::printf("DATE_INPUT_RESULT PASS checks=%d\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "DATE_INPUT_RESULT FAIL checks=%d error=%s\n", checks, error.what());
        State(Window(app, 0)).PrintToStream();
        if (verified) Cleanup(app, team);
        return 1;
    }
}
