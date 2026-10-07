// Full browser UI, isolated extraction, sanitization, history and private mode.
#define SUMMIT_EXTENSION_MANAGER_HELPERS_ONLY
#include "ModernExtensionManagerTests.cpp"

static BMessage SelectedTab(const BMessenger& browser)
{
    auto state = State(browser); BMessage tab;
    state.FindMessage("tab", Index(state, Selected(state)), &tab);
    return tab;
}
static json Inspector(const BMessenger& tools, const char* action, const std::string& argument = {})
{
    BMessage message(summit::kDeveloperToolsCommand);
    message.AddString("action", action); message.AddString("argument", argument.c_str());
    auto reply = Query(tools, message);
    return json::parse(reply.GetString("json", "{}"), nullptr, false);
}
static json Evaluate(const BMessenger& browser, const std::string& expression)
{
    Send(browser, summit::kShowDeveloperTools);
    BMessenger tools;
    Require(Wait([&] { return SelectedTab(browser).FindMessenger("devtools", &tools) == B_OK && tools.IsValid(); }), "actual native inspector opens");
    const std::string marker = "READER_TEST_" + std::to_string(system_time());
    Inspector(tools, "clear-console");
    Inspector(tools, "evaluate", "'" + marker + "'+JSON.stringify((" + expression + "))");
    json result;
    Require(Wait([&] {
        auto state = Inspector(tools, "state");
        if (!state.is_object() || !state.contains("entries")) return false;
        for (auto& entry : state["entries"]) {
            if (entry.value("kind", "") != "result") continue;
            auto text = entry.value("text", "");
            auto quoted = json::parse(text, nullptr, false);
            if (quoted.is_string()) text = quoted.get<std::string>();
            if (!text.starts_with(marker)) continue;
            result = json::parse(text.substr(marker.size()), nullptr, false);
            return !result.is_discarded();
        }
        return false;
    }), "inspect the real rendered DOM");
    Send(tools, B_QUIT_REQUESTED);
    Require(Wait([&] { return !tools.IsValid(); }), "close native inspector");
    return result;
}
static void CaptureReader(const char* output)
{
    BScreen screen; auto frame = screen.Frame(); BBitmap bitmap(frame, B_RGBA32);
    Require(bitmap.InitCheck() == B_OK && screen.ReadBitmap(&bitmap, false, &frame) == B_OK, "capture rendered Reader and address button");
    std::ofstream image(output, std::ios::binary);
    image << "P6\n" << frame.IntegerWidth() + 1 << ' ' << frame.IntegerHeight() + 1 << "\n255\n";
    for (int y = 0; y <= frame.IntegerHeight(); ++y) for (int x = 0; x <= frame.IntegerWidth(); ++x) {
        const auto* pixel = static_cast<const uint8*>(bitmap.Bits()) + y * bitmap.BytesPerRow() + x * 4;
        const char rgb[] { char(pixel[2]), char(pixel[1]), char(pixel[0]) }; image.write(rgb, 3);
    }
    Require(bool(image), "write Reader screenshot");
}
int main(int argc, char** argv)
{
    if (argc != 6) return 2;
    status_t status; BApplication application("application/x-vnd.Summit-reader-tests", &status);
    if (status != B_OK) return 1;
    const auto team = static_cast<team_id>(std::strtol(argv[1], nullptr, 10));
    const std::string source(argv[3]);
    BMessenger app; bool verified = false;
    try {
        app_info info;
        Require(Wait([&] { app = BMessenger(nullptr, team); return app.IsValid() && be_roster->GetRunningAppInfo(team, &info) == B_OK; }), "owned browser registers");
        BPath executable(&info.ref);
        Require(std::filesystem::canonical(executable.Path()) == std::filesystem::canonical(argv[2]), "owned executable matches Reader build");
        verified = true;
        auto browser = Window(app, 0);
        auto readable = [&](const BMessenger& window) { return Wait([&] {
            auto tab = SelectedTab(window); return !tab.GetBool("loading", true) && tab.GetBool("readerAvailable", false);
        }); };
        auto reader = [&](const BMessenger& window, bool active) { return Wait([&] {
            auto tab = SelectedTab(window); return !tab.GetBool("loading", true) && !tab.GetBool("loadError", true)
                && tab.GetBool("readerActive", false) == active;
        }); };
        auto hidden = [&](const BMessenger& window) { return Property(View(window, "reader"), "Hidden").GetBool("result", true); };
        Require(readable(browser), "article detected despite hostile page-world overrides");
        auto address = View(browser, "address"), button = View(browser, "reader");
        Require(button.IsValid() && View(address, "reader") == button && !hidden(browser), "Reader button is inside native address control");
        BRect buttonFrame, addressFrame;
        Require(Property(button, "Frame").FindRect("result", &buttonFrame) == B_OK
            && Property(address, "Frame").FindRect("result", &addressFrame) == B_OK
            && BRect(0, 0, addressFrame.Width(), addressFrame.Height()).Contains(buttonFrame), "Reader button fits entirely within URL field bounds");
        Click(button, BPoint(14, 10));
        Require(reader(browser, true), "native Reader button opens simplified document");
        Require(String(SelectedTab(browser), "url") == source && String(State(browser), "address") == source,
            "address and bookmark/session URL remain the original article");
        Require(String(SelectedTab(browser), "title").find("A walk above the clouds") != std::string::npos,
            "Reader retains article title");
        const auto dom = Evaluate(browser, R"((() => ({
            url:document.URL, title:document.title, text:document.body.innerText,
            active:document.querySelectorAll('script,iframe,form,input,svg,math,object,embed').length,
            handlers:[...document.querySelectorAll('*')].some(e=>[...e.attributes].some(a=>/^on/i.test(a.name))),
            unsafe:[...document.querySelectorAll('[href],[src]')].some(e=>['href','src'].some(a=>/^javascript:|^file:|^data:/i.test(e.getAttribute(a)||''))),
            link:document.querySelector('article a[href]')?.href,
            image:document.querySelector('article img')?.src,
            extracted:typeof Readability, sanitizer:typeof DOMPurify,
            width:getComputedStyle(document.querySelector('main')).maxWidth,
            csp:document.querySelector('meta[http-equiv="Content-Security-Policy"]')?.content
        }))())");
        Require(dom.is_object() && dom.value("url", "").starts_with("summit-reader://"), "article renders under dedicated internal origin");
        Require(dom.value("text", "").find("READER_ARTICLE_MARKER") != std::string::npos
            && dom.value("text", "").find("READER_REMOVE_") == std::string::npos, "article paragraphs retained; navigation, advertisements and embedded content removed");
        Require(Has(dom, "active", 0) && Has(dom, "handlers", false) && Has(dom, "unsafe", false), "sanitizer strips active elements, event handlers and unsafe URLs");
        const auto base = source.substr(0, source.rfind('/') + 1);
        Require(Has(dom, "link", base + "reader-next.html") && Has(dom, "image", base + "reader-mountain.svg"), "relative links and images resolve against original document");
        Require(Has(dom, "extracted", "undefined") && Has(dom, "sanitizer", "undefined") && Has(dom, "width", "760px")
            && dom.value("csp", "").find("default-src 'none'") != std::string::npos, "Reader uses isolated scripts, readable width and restrictive policy");
        FocusWindow(browser); snooze(500000); CaptureReader(argv[4]);
        const auto csp = Evaluate(browser, R"((() => { let s=document.createElement('script');s.textContent='window.__readerAttack=1';document.body.appendChild(s);return {blocked:!window.__readerAttack}; })())");
        Require(Has(csp, "blocked", true), "CSP blocks inline script execution in actual Reader document");
        const auto beforeReload = SelectedTab(browser).GetUInt64("loadGeneration", 0);
        Send(browser, summit::kReload);
        Require(Wait([&] {return SelectedTab(browser).GetUInt64("loadGeneration", 0) > beforeReload;}), "Reader reload starts a new navigation");
        Require(reader(browser, true), "Reader reload serves its in-memory article");
        const auto selected = Selected(State(browser));
        Send(browser, summit::kNewTab, -1, "about:blank");
        Require(Wait([&] { return Selected(State(browser)) != selected && hidden(browser); }), "blank tab hides the Reader control");
        SelectTab(browser, selected);
        Require(reader(browser, true) && !hidden(browser), "original tab retains Reader state");
        Evaluate(browser, "(() => {setTimeout(() => location.href=document.querySelector('article a[href]').href, 1500); return true;})()");
        Require(Wait([&] { return String(SelectedTab(browser), "title") == "Next trail" && !SelectedTab(browser).GetBool("loading", true); }), "article hyperlink opens its real target");
        Require(hidden(browser), "non-article page hides Reader control");
        Send(browser, summit::kBack);
        Require(reader(browser, true), "Back restores in-memory Reader document");
        Button(browser, "reader");
        Require(readable(browser) && reader(browser, false), "Reader toggle restores original article");
        Send(browser, summit::kNavigate, -1, source + "?second-article=1");
        Require(Wait([&] {return String(SelectedTab(browser), "url") == source + "?second-article=1";}) && readable(browser), "second article loads");
        Button(browser, "reader"); Require(reader(browser, true), "second article replaces the tab's preview cache");
        Send(browser, summit::kBack);
        Require(reader(browser, false) && readable(browser), "Back reaches second original article");
        Send(browser, summit::kBack);
        Require(Wait([&] {return String(SelectedTab(browser), "url") == source;}) && readable(browser), "Back reaches first original article");
        const auto beforeOldReader = SelectedTab(browser).GetUInt64("loadGeneration", 0);
        Send(browser, summit::kBack);
        Require(Wait([&] {return SelectedTab(browser).GetUInt64("loadGeneration",0) > beforeOldReader;})
            && readable(browser) && reader(browser,false) && String(SelectedTab(browser), "url") == source,
            "expired Reader history reopens its original URL without an internal address or error");
        Button(browser, "reader"); Send(browser, summit::kNavigate, -1, base + "reader-next.html");
        Require(Wait([&] { return String(SelectedTab(browser), "title") == "Next trail" && !SelectedTab(browser).GetBool("loading", true); }), "navigation wins over in-flight extraction");
        snooze(300000);
        Require(!SelectedTab(browser).GetBool("readerActive", true) && hidden(browser), "stale extraction cannot replace a new document");
        const std::string privateURL = source + "?private-reader-fixture=1";
        BMessage createPrivate(summit::kNewWindow); createPrivate.AddBool("private", true);
        createPrivate.AddString("url", privateURL.c_str());
        Require(app.SendMessage(&createPrivate) == B_OK, "request private browser window");
        BMessenger privateWindow;
        Require(Wait([&] { for (int i=0;i<8;++i) { auto w=Window(app,i); if (w.IsValid() && State(w).GetBool("private",false)) { privateWindow=w;return true; } }return false; }), "private browser window opens");
        Require(readable(privateWindow), "private article is detected"); Button(privateWindow, "reader");
        Require(reader(privateWindow, true), "Reader works in private browsing");
        Send(app, B_QUIT_REQUESTED);
        Require(Wait([&] { team_info info; return get_team_info(team, &info) == B_BAD_TEAM_ID; }), "browser exits with private Reader open");
        bool leaked = false;
        for (const auto& item : std::filesystem::recursive_directory_iterator(argv[5])) {
            if (!item.is_regular_file() || item.path().extension() != ".json" || item.file_size() > 8000000) continue;
            std::ifstream file(item.path()); std::string data((std::istreambuf_iterator<char>(file)), {});
            leaked |= data.find("private-reader-fixture") != std::string::npos || data.find("summit-reader://") != std::string::npos;
        }
        Require(!leaked, "saved profile contains no private article or temporary Reader URLs");
        std::printf("READER_RESULT PASS checks=%d\n", checks); return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "READER_RESULT FAIL checks=%d error=%s\n", checks, error.what());
        State(Window(app,0)).PrintToStream(); if (verified) Cleanup(app,team); return 1;
    }
}
