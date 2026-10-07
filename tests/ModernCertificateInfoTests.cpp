// Certificate details from actual HTTPS loads, including a saved exception.
#define SUMMIT_EXTENSION_MANAGER_HELPERS_ONLY
#include "ModernExtensionManagerTests.cpp"

static std::string Details(const BMessenger& window)
{
    auto view = View(window, "certificate-details");
    const auto length = Property(view, "Text", B_COUNT_PROPERTIES).GetInt32("result", -1);
    Require(length > 0 && length < 65536, "certificate text is available in the native viewer");
    BMessage read(B_GET_PROPERTY); read.AddSpecifier("Text", int32(0), length);
    return String(Query(view, read), "result");
}
static void Capture(const char* output)
{
    BScreen screen; BRect frame = screen.Frame();
    BBitmap bitmap(frame, B_RGBA32);
    Require(bitmap.InitCheck() == B_OK && screen.ReadBitmap(&bitmap, false, &frame) == B_OK, "capture actual certificate viewer pixels");
    std::ofstream file(output, std::ios::binary);
    file << "P6\n" << frame.IntegerWidth() + 1 << ' ' << frame.IntegerHeight() + 1 << "\n255\n";
    for (int y = 0; y <= frame.IntegerHeight(); ++y) for (int x = 0; x <= frame.IntegerWidth(); ++x) {
        const auto* pixel = static_cast<const uint8*>(bitmap.Bits()) + y * bitmap.BytesPerRow() + x * 4;
        const char rgb[] { char(pixel[2]), char(pixel[1]), char(pixel[0]) }; file.write(rgb, 3);
    }
    Require(bool(file), "write native viewer screenshot");
}
int main(int argc, char** argv)
{
    if (argc != 7) return 2;
    status_t status;
    BApplication test("application/x-vnd.Summit-certificate-tests", &status);
    if (status != B_OK) return 1;
    const auto team = static_cast<team_id>(std::strtol(argv[1], nullptr, 10));
    const std::string base(argv[3]), plain(argv[4]), fingerprint(argv[5]);
    BMessenger app;
    bool verified = false;
    try {
        app_info info;
        Require(Wait([&] { app = BMessenger(nullptr, team); return app.IsValid() && be_roster->GetRunningAppInfo(team, &info) == B_OK; }), "owned browser registers");
        BPath executable(&info.ref);
        Require(std::filesystem::canonical(executable.Path()) == std::filesystem::canonical(argv[2]), "owned browser matches the certificate build");
        verified = true;
        const auto browser = Window(app, 0);
        auto hidden = [&] { return Property(View(browser, "connection-certificate"), "Hidden").GetBool("result", true); };
        auto loaded = [&](const std::string& prefix) {
            return Wait([&] {
                const auto state = State(browser); BMessage tab;
                return state.FindMessage("tab", Index(state, Selected(state)), &tab) == B_OK
                    && !tab.GetBool("loading", true) && !tab.GetBool("loadError", true) && String(tab, "url").find(prefix) == 0;
            }, 60000000);
        };
        auto popup = [&] { return NamedWindow(app, "Connection security — Summit"); };
        auto open = [&] {
            Require(Wait([&] { return !hidden(); }), "loaded HTTPS page has a certificate button");
            Button(browser, "connection-certificate");
            Require(Wait([&] { return popup().IsValid(); }), "certificate button opens native details");
            return popup();
        };
        Require(loaded("https://example.com/"), "public HTTPS page loads");
        const auto publicTab = Selected(State(browser));
        auto details = open();
        Require(Text(details, "certificate-heading") == "Connection is encrypted and verified", "valid public certificate is identified as verified");
        auto text = Details(details);
        Require(text.find("example.com") != std::string::npos && text.find("Issued by:") != std::string::npos
            && text.find("SHA-256 fingerprint:") != std::string::npos && text.find("UTC") != std::string::npos,
            "viewer includes actual subject, issuer, dates and fingerprint");
        Button(details, "certificate-close");
        Require(Wait([&] { return !popup().IsValid(); }), "Close button dismisses certificate viewer");
        details = open();
        Send(browser, summit::kNewTab, -1, plain + "/plain");
        Require(loaded(plain + "/plain"), "plain HTTP fixture loads");
        Require(Wait([&] { return hidden() && !popup().IsValid(); }), "HTTP tab has no lock and closes the old certificate viewer");
        Send(browser, summit::kNavigate, -1, base + "/secure");
        BMessenger warning;
        Require(Wait([&] { warning = NamedWindow(app, "Certificate Not Trusted"); return warning.IsValid(); }), "self-signed server still requires explicit trust");
        Require(hidden(), "rejected certificate cannot display a verified lock");
        Button(warning, "_b1_");
        Require(loaded(base + "/secure"), "explicit certificate exception loads the fixture");
        details = open();
        Require(Text(details, "certificate-heading") == "Certificate not verified", "saved exception is not misrepresented as a verified certificate");
        text = Details(details);
        std::string compact;
        for (char c : text) if (c != ':' && c != '\n') compact += c;
        Require(text.find("saved exception") != std::string::npos && text.find("Summit certificate fixture") != std::string::npos
            && compact.find(fingerprint) != std::string::npos, "viewer contains the exact fixture fingerprint and identifies the exception");
        Capture(argv[6]);
        const auto fixtureTab = Selected(State(browser));
        SelectTab(browser, publicTab);
        Require(Wait([&] { return !popup().IsValid(); }), "switching tabs closes certificate details from the previous tab");
        details = open();
        Require(Details(details).find("Summit certificate fixture") == std::string::npos, "public tab retains its own certificate");
        SelectTab(browser, fixtureTab);
        Require(Wait([&] { return !popup().IsValid(); }), "returning to fixture closes the public certificate viewer");
        details = open();
        Send(browser, summit::kNavigate, -1, base + "/redirect");
        Require(loaded(plain + "/plain"), "HTTPS redirect reaches plain HTTP");
        Require(hidden() && !popup().IsValid(), "redirect clears the lock and certificate details");
        Send(browser, summit::kNavigate, -1, base + "/secure");
        Require(loaded(base + "/secure"), "saved exception reloads without another trust prompt");
        details = open();
        text = Details(details);
        compact.clear();
        for (char c : text) if (c != ':' && c != '\n') compact += c;
        Require(compact.find(fingerprint) != std::string::npos, "resumed TLS session retains its actual peer certificate");
        Send(app, B_QUIT_REQUESTED);
        Require(Wait([&] { team_info info; return get_team_info(team, &info) == B_BAD_TEAM_ID; }), "browser exits normally with certificate viewer open");
        std::printf("CERTIFICATE_BROWSER_RESULT PASS checks=%d\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "CERTIFICATE_BROWSER_RESULT FAIL checks=%d error=%s\n", checks, error.what());
        State(Window(app, 0)).PrintToStream();
        if (verified) Cleanup(app, team);
        return 1;
    }
}
