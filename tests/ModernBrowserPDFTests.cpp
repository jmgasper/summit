// Full Summit shell check, supplementing the engine's PDF rendering harness.
#define SUMMIT_EXTENSION_MANAGER_HELPERS_ONLY
#include "ModernExtensionManagerTests.cpp"
#include <FindDirectory.h>

static BMessage PDFTab(const BMessenger& browser)
{
    auto state = State(browser); BMessage tab;
    state.FindMessage("tab", Index(state, Selected(state)), &tab); return tab;
}
static json PDFInspector(const BMessenger& tools, const char* action, const std::string& argument = {})
{
    BMessage message(summit::kDeveloperToolsCommand);
    message.AddString("action", action); message.AddString("argument", argument.c_str());
    return json::parse(Query(tools, message).GetString("json", "{}"), nullptr, false);
}
static json PDFEvaluate(const BMessenger& browser, const std::string& expression)
{
    Send(browser, summit::kShowDeveloperTools); BMessenger tools;
    Require(Wait([&] { return PDFTab(browser).FindMessenger("devtools", &tools) == B_OK && tools.IsValid(); }), "native PDF inspector opens");
    auto marker = "PDF_TEST_" + std::to_string(system_time());
    PDFInspector(tools, "clear-console");
    PDFInspector(tools, "evaluate", "'" + marker + "'+JSON.stringify((" + expression + "))");
    json result;
    Require(Wait([&] {
        auto state = PDFInspector(tools, "state"); if (!state.is_object() || !state.contains("entries")) return false;
        for (auto& entry : state["entries"]) {
            if (entry.value("kind", "") != "result") continue;
            auto text = entry.value("text", ""); auto decoded = json::parse(text, nullptr, false);
            if (decoded.is_string()) text = decoded.get<std::string>();
            if (!text.starts_with(marker)) continue;
            result = json::parse(text.substr(marker.size()), nullptr, false); return !result.is_discarded();
        }
        return false;
    }), "inspect the actual PDF document wrapper");
    Send(tools, B_QUIT_REQUESTED); Require(Wait([&] { return !tools.IsValid(); }), "close native PDF inspector");
    return result;
}
static std::string ReadPDF(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}
static void CapturePDF(const char* path)
{
    BScreen screen; auto frame = screen.Frame(); BBitmap bitmap(frame, B_RGBA32);
    Require(bitmap.InitCheck() == B_OK && screen.ReadBitmap(&bitmap, false, &frame) == B_OK, "capture full browser PDF pixels");
    std::ofstream image(path, std::ios::binary);
    image << "P6\n" << frame.IntegerWidth() + 1 << ' ' << frame.IntegerHeight() + 1 << "\n255\n";
    for (int y = 0; y <= frame.IntegerHeight(); ++y) for (int x = 0; x <= frame.IntegerWidth(); ++x) {
        auto* pixel = static_cast<const uint8*>(bitmap.Bits()) + y * bitmap.BytesPerRow() + x * 4;
        const char rgb[] {char(pixel[2]), char(pixel[1]), char(pixel[0])}; image.write(rgb, 3);
    }
    Require(bool(image), "save full browser PDF screenshot");
}
int main(int argc, char** argv)
{
    if (argc != 8) return 2;
    status_t status; BApplication application("application/x-vnd.Summit-browser-pdf-tests", &status);
    if (status != B_OK) return 1;
    team_id team = std::strtol(argv[1], nullptr, 10); BMessenger app; bool verified = false;
    const std::string source = argv[3], filename = "summit-pdf-" + std::string(argv[4]) + ".pdf";
    try {
        app_info info;
        Require(Wait([&] { app = BMessenger(nullptr, team); return app.IsValid() && be_roster->GetRunningAppInfo(team, &info) == B_OK; }), "owned PDF browser registers");
        BPath executable(&info.ref);
        Require(std::filesystem::canonical(executable.Path()) == std::filesystem::canonical(argv[2]), "target is the exact frozen PDF browser"); verified = true;
        auto browser = Window(app, 0);
        auto loaded = [&] { auto tab = PDFTab(browser); return !tab.GetBool("loading", true) && !tab.GetBool("loadError", true) && String(tab, "url") == source; };
        Require(Wait(loaded), "PDF loads inline in the full Summit browser");
        Require(String(State(browser), "address") == source && String(PDFTab(browser), "title") == filename,
            "address retains PDF URL and title uses the response filename");
        Require(!PDFTab(browser).GetBool("readerAvailable", false), "PDF does not offer article Reader mode");
        auto wrapper = PDFEvaluate(browser, "({url:document.URL,frame:document.querySelector('iframe')?.src,width:document.querySelector('iframe')?.getBoundingClientRect().width})");
        Require(wrapper.value("url", "") == source && wrapper.value("frame", "").starts_with("webkit-pdfjs-viewer://pdfjs/web/viewer.html"),
            "browser uses the engine's embedded PDF viewer");
        double width = wrapper.value("width", 0.0);
        Require(width >= 850, "full PDF toolbar fits the native test window");
        BPath home; Require(find_directory(B_USER_DIRECTORY, &home) == B_OK, "locate the native Downloads directory");
        auto download = std::filesystem::path(home.Path()) / "Downloads" / filename;
        auto golden = ReadPDF(argv[5]);
        Require(!golden.empty() && !std::filesystem::exists(download), "unique PDF save cannot replace an existing user file");
        FocusWindow(browser); snooze(2000000); CapturePDF(argv[6]);
        // PDF.js's right toolbar has the Save button 50 CSS pixels from the
        // edge; its same frozen layout is measured precisely by ModernPDFTests.
        Click(Page(browser, Selected(State(browser))), BPoint(float(width - 50), 16));
        Require(Wait([&] { return std::filesystem::exists(download) && ReadPDF(download) == golden
            && State(browser).GetInt32("download_count", -1) == 0; }), "PDF Save writes exact original bytes through Summit's download UI");
        Require(loaded() && String(State(browser), "address") == source, "saving keeps the PDF open at its original address");
        auto generation = PDFTab(browser).GetUInt64("loadGeneration", 0);
        Send(browser, summit::kReload);
        Require(Wait([&] { return PDFTab(browser).GetUInt64("loadGeneration", 0) > generation && loaded(); }), "browser Reload reopens the PDF");
        Send(browser, summit::kNavigate, -1, "about:blank");
        Require(Wait([&] { return String(PDFTab(browser), "url") == "about:blank"; }), "navigate away from PDF");
        Send(browser, summit::kBack); Require(Wait(loaded), "browser Back restores the PDF's original URL");
        Send(app, B_QUIT_REQUESTED);
        Require(Wait([&] { team_info current; return get_team_info(team, &current) != B_OK; }), "browser exits cleanly with a PDF open");
        auto profile = ReadJSON(std::filesystem::path(argv[7]) / "profile.json");
        bool found = false, internal = false;
        if (profile.is_object() && profile.contains("history")) for (const auto& item : profile["history"]) {
            auto url = item.value("url", ""); found |= url == source; internal |= url.starts_with("webkit-pdfjs-viewer:") || url.starts_with("blob:");
        }
        Require(found && !internal, "history stores original PDF visits without private viewer resource URLs");
        Require(ReadPDF(download) == golden && std::filesystem::remove(download), "remove only the unchanged test PDF download");
        std::printf("PDF_UI_RESULT PASS checks=%d\n", checks); return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "PDF_UI_RESULT FAIL: %s\n", error.what());
        if (verified) Cleanup(app, team); return 1;
    }
}
