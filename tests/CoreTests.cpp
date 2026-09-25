#include "core/Address.h"
#include "core/Profile.h"
#include "core/Favicon.h"
#include "core/InternalPages.h"
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr << __FILE__ << ':' << __LINE__ << ": " #x "\n"; } } while (0)

int main()
{
    using namespace summit;
    CHECK(ResolveAddress("").url == "summit:home");
    CHECK(ResolveAddress("  example.com  ").url == "https://example.com");
    CHECK(ResolveAddress("localhost:8080/test").url == "http://localhost:8080/test");
    CHECK(ResolveAddress("[::1]:8000/test").url == "http://[::1]:8000/test");
    CHECK(ResolveAddress("example.com:8443/a").url == "https://example.com:8443/a");
    CHECK(ResolveAddress("HTTPS://example.com/a").url == "https://example.com/a");
    CHECK(ResolveAddress("http://10.0.2.2:8765/").url == "http://10.0.2.2:8765/");
    CHECK(ResolveAddress("about:blank").url == "about:blank");
    CHECK(ResolveAddress("webkit-extension://12345678-1234-4234-8234-123456789abc/view.html?kind=tab").url
        == "webkit-extension://12345678-1234-4234-8234-123456789abc/view.html?kind=tab");
    CHECK(ResolveAddress("WEBKIT-EXTENSION://12345678-1234-4234-8234-123456789abc/options.html").url
        == "webkit-extension://12345678-1234-4234-8234-123456789abc/options.html");
    CHECK(ResolveAddress("/boot/home/test.html").url == "file:///boot/home/test.html");
    CHECK(ResolveAddress("/").url == "file:///");
    CHECK(ResolveAddress("/boot/home/a #?%/café.html").url == "file:///boot/home/a%20%23%3F%25/caf%C3%A9.html");
    CHECK(FileURL("/boot/home/ report .html ") == "file:///boot/home/%20report%20.html%20");
    CHECK(ResolveAddress("file:///boot/home/a%20%23.html").url == "file:///boot/home/a%20%23.html");
    CHECK(FileURL("relative.html").empty());
    CHECK(ResolveAddress("kunanyi walking tracks").search);
    CHECK(ResolveAddress("site:example.com").error.size() > 0);
    CHECK(ResolveAddress("hello & goodbye").url == "https://duckduckgo.com/?q=hello%20%26%20goodbye");
    CHECK(ResolveAddress("example.com query").search);
    CHECK(!ResolveAddress("javascript:alert(1)").error.empty());
    CHECK(!ResolveAddress("https://example.com\r\nother").error.empty());
    CHECK(!ResolveAddress(std::string("https://host\0evil", 17)).error.empty());
    CHECK(!ResolveAddress(std::string(65537, 'x')).error.empty());
    CHECK(PercentEncode("café") == "caf%C3%A9");
    CHECK(EscapeHTML("<&\"'>") == "&lt;&amp;&quot;&#39;&gt;");
    CHECK(ResolveAddress("summit:history").url == "summit:history");
    CHECK(ResolveAddress("Summit:Bookmarks").url == "summit:bookmarks");
    CHECK(!ResolveAddress("summit:nothing").error.empty());

    // Favicons.
    CHECK(FaviconKey("https://WWW.Example.com:8443/a?b") == "www.example.com");
    CHECK(FaviconKey("http://user@[::1]:80/") == "___1_");
    CHECK(FaviconKey("file:///boot/home").empty() && FaviconKey("summit:history").empty());
    CHECK(Base64("") == "" && Base64("f") == "Zg==" && Base64("fo") == "Zm8=" && Base64("foobar") == "Zm9vYmFy");
    {
        // A 2x2 32-bit icon (bottom-up rows) and a 1-bit one, both in one .ico.
        auto le16 = [](std::string& s, unsigned v) { s += char(v & 255); s += char(v >> 8); };
        auto le32 = [&](std::string& s, unsigned v) { le16(s, v & 0xffff); le16(s, v >> 16); };
        auto dib = [&](int width, int bpp, const std::string& pixels, const std::string& palette) {
            std::string d;
            le32(d, 40); le32(d, width); le32(d, width * 2); le16(d, 1); le16(d, bpp); le32(d, 0);
            le32(d, 0); le32(d, 0); le32(d, 0); le32(d, palette.size() / 4); le32(d, 0);
            return d + palette + pixels;
        };
        // BGRA rows, bottom row first: bottom = blue, green; top = red, transparent.
        std::string rgba32 = std::string("\xff\0\0\xff\0\xff\0\xff", 8) + std::string("\0\0\xff\xff\0\0\0\0", 8);
        std::string first = dib(2, 32, rgba32 + std::string(8, '\0'), { });
        // 1-bit 16x16: palette black/white, all white, mask transparent on the top row.
        std::string bits(16 * 4, '\xff'), mask(16 * 4, '\0');
        mask[15 * 4] = mask[15 * 4 + 1] = '\xff';
        std::string second = dib(16, 1, bits + mask, std::string("\0\0\0\0\xff\xff\xff\0", 8));
        std::string ico;
        le16(ico, 0); le16(ico, 1); le16(ico, 2);
        const unsigned base = 6 + 32;
        ico += char(2); ico += char(2); ico += char(0); ico += char(0); le16(ico, 1); le16(ico, 32);
        le32(ico, first.size()); le32(ico, base);
        ico += char(16); ico += char(16); ico += char(0); ico += char(0); le16(ico, 1); le16(ico, 1);
        le32(ico, second.size()); le32(ico, base + first.size());
        ico += first + second;
        CHECK(IsIco(ico));
        auto entry = SelectIcoEntry(ico, 32);
        CHECK(entry.kind == IcoEntryKind::DIB && entry.width == 16);
        Image image;
        CHECK(DecodeDIB(entry.bytes, image) && image.width == 16 && image.height == 16);
        CHECK(image.rgba[3] == 0 && image.rgba[0 + 16 * 4 * 1 + 3] == 255 && image.rgba[16 * 4 + 0] == 255);
        entry = SelectIcoEntry(ico, 2);
        CHECK(entry.width == 2 && DecodeDIB(entry.bytes, image));
        CHECK(image.rgba[0] == 255 && image.rgba[3] == 255);        // top-left red
        CHECK(image.rgba[7] == 0);                                   // top-right transparent
        CHECK(image.rgba[8 + 2] == 255 && image.rgba[12 + 1] == 255); // blue, green
        auto scaled = ScaleImage(image, 1);
        CHECK(scaled.width == 1 && scaled.rgba[3] > 180 && scaled.rgba[3] < 200);
        CHECK(scaled.rgba[0] > 80 && scaled.rgba[1] > 80 && scaled.rgba[2] > 80);
        auto wide = ScaleImage(Image{4, 2, std::vector<uint8_t>(32, 255)}, 4);
        CHECK(wide.rgba[3] == 0 && wide.rgba[(1 * 4) * 4 + 3] == 255 && wide.rgba[(3 * 4) * 4 + 3] == 0);
        auto png = EncodePNG(scaled);
        CHECK(png.size() > 8 && png.substr(1, 3) == "PNG" && png.find("IEND") != std::string::npos);
        CHECK(!IsIco("\x89PNG") && SelectIcoEntry("junk").kind == IcoEntryKind::None);
    }

    // History grouped by day, and the built-in pages.
    setenv("TZ", "UTC", 1);
    tzset();
    const int64_t now = 1790380800 + 15 * 3600; // Saturday, 26 September 2026, 15:00 UTC
    std::vector<PageRecord> visits = {
        {"https://a.example/", "A <b>", now - 60}, {"https://b.example/", "", now - 3600},
        {"https://c.example/", "C", now - 86400}, {"https://d.example/", "D", now - 2 * 86400},
        {"https://e.example/", "E", 0}};
    auto days = GroupHistoryByDay(visits, now);
    CHECK(days.size() == 4 && days[0].label == "Today" && days[0].pages.size() == 2);
    CHECK(days[1].label == "Yesterday" && days[2].label == "Thursday, 24 September 2026" && days[3].label == "Earlier");
    auto html = RenderHistoryPage(visits, now, [](const std::string& url) {
        return url == "https://a.example/" ? std::string("data:image/png;base64,QUJD") : std::string();
    });
    CHECK(html.find("<title>History</title>") != std::string::npos && html.find("A &lt;b&gt;") != std::string::npos);
    CHECK(html.find("href=\"https://b.example/\"") != std::string::npos && html.find(">14:59<") != std::string::npos);
    CHECK(html.find(".i0{background-image:url(data:image/png;base64,QUJD)}") != std::string::npos);
    CHECK(html.find("icon letter\">B<") != std::string::npos);
    CHECK(RenderHistoryPage({}, now, nullptr).find("Pages you visit") != std::string::npos);
    auto bookmarksHTML = RenderBookmarksPage({{"https://x.example/", "X", 0, true}, {"javascript:bad", "J"}},
        [](const std::string&) { return std::string("data:image/png;base64,\"evil"); });
    CHECK(bookmarksHTML.find("Bookmarks Bar") != std::string::npos && bookmarksHTML.find("Other Bookmarks") != std::string::npos);
    CHECK(bookmarksHTML.find("href=\"javascript") == std::string::npos && bookmarksHTML.find("evil") == std::string::npos);

    char directory[] = "/tmp/summit-core-XXXXXX";
    auto* created = mkdtemp(directory);
    if (!created) return 2;
    const auto path = std::filesystem::path(created) / "profile/profile.json";
    std::string error;
    CHECK(Profile::Load(path, error).tabs.empty() && error.empty());
    Profile profile;
    profile.tabs = {{"https://example.com/", "Example"}, {"summit:home", "Start Page"}};
    profile.selected = 1;
    profile.bookmarks = {{"https://webkit.org", "WebKit — 浏览器"}};
    profile.Visit({"https://example.com/", "First title"});
    profile.Visit({"https://webkit.org/", "WebKit"});
    profile.Visit({"https://example.com/", "Updated title"});
    CHECK(!profile.Visit({"file:///boot/home/private", "Local file"}));
    CHECK(profile.history.size() == 2);
    CHECK(profile.history.front().title == "Updated title");
    CHECK(profile.history.front().visited > 0);
    profile.AddBookmark({"https://bar.example/", "Bar"}, true);
    profile.AddBookmark({"https://webkit.org", "ignored"}, true);
    CHECK(profile.bookmarks.size() == 2 && profile.bookmarks[0].bar && profile.bookmarks[0].title == "WebKit — 浏览器");
    profile.homeURL = "https://home.example/";
    profile.showBookmarksBar = false;
    CHECK(profile.Save(path, error) && error.empty());
    auto loaded = Profile::Load(path, error);
    CHECK(error.empty() && loaded.tabs.size() == 2 && loaded.selected == 1);
    CHECK(loaded.bookmarks.at(0).title == "WebKit — 浏览器");
    CHECK(loaded.history.size() == 2 && loaded.history.front().title == "Updated title");
    CHECK(loaded.history.front().visited == profile.history.front().visited);
    CHECK(loaded.bookmarks.size() == 2 && loaded.bookmarks[1].bar && loaded.bookmarks[1].url == "https://bar.example/");
    CHECK(loaded.homeURL == "https://home.example/" && !loaded.showBookmarksBar);
    CHECK(loaded.RemoveBookmark("https://bar.example/") && !loaded.RemoveBookmark("https://bar.example/"));
    CHECK(!loaded.FindBookmark("https://bar.example/") && loaded.FindBookmark("https://webkit.org"));
    struct stat mode{};
    CHECK(stat(path.c_str(), &mode) == 0 && (mode.st_mode & 0777) == 0600);
    profile.tabs = {{"https://different.example/", "Replacement"}};
    profile.selected = 999;
    CHECK(profile.Save(path, error));
    loaded = Profile::Load(path, error);
    CHECK(loaded.tabs.size() == 1 && loaded.tabs[0].title == "Replacement" && loaded.selected == 0);
    { std::ofstream out(path); out << "{broken"; }
    loaded = Profile::Load(path, error);
    CHECK(!error.empty() && loaded.tabs.empty());
    CHECK(std::filesystem::file_size(path) == 7);
    CHECK(!profile.Save(path / "impossible.json", error) && !error.empty());
    CHECK(std::filesystem::file_size(path) == 7);
    std::filesystem::remove_all(created);
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
