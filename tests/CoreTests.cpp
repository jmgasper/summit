#include "core/Address.h"
#include "core/Profile.h"
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
    profile.Visit({"file:///boot/home/private", "Local file"});
    CHECK(profile.history.size() == 2);
    CHECK(profile.history.front().title == "Updated title");
    CHECK(profile.Save(path, error) && error.empty());
    auto loaded = Profile::Load(path, error);
    CHECK(error.empty() && loaded.tabs.size() == 2 && loaded.selected == 1);
    CHECK(loaded.bookmarks.at(0).title == "WebKit — 浏览器");
    CHECK(loaded.history.size() == 2 && loaded.history.front().title == "Updated title");
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
