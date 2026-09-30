#include "Profile.h"
#include "Address.h"
#include "Zoom.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace summit {
using nlohmann::json;
static json Encode(const std::vector<PageRecord>& pages)
{
    json result = json::array();
    for (const auto& page : pages) {
        json item = {{"url", page.url}, {"title", page.title}};
        if (page.visited) item["visited"] = page.visited;
        if (page.bar) item["bar"] = true;
        result.push_back(std::move(item));
    }
    return result;
}
static std::vector<PageRecord> Decode(const json& value, size_t limit)
{
    if (!value.is_array() || value.size() > limit) throw std::runtime_error("Invalid profile list");
    std::vector<PageRecord> pages;
    for (const auto& item : value) {
        auto url = item.at("url").get<std::string>();
        auto title = item.at("title").get<std::string>();
        if (url.size() > 65536 || title.size() > 65536 || url.find('\0') != std::string::npos)
            throw std::runtime_error("Invalid profile text");
        PageRecord page{url, title};
        if (auto visited = item.find("visited"); visited != item.end()) page.visited = visited->get<int64_t>();
        if (auto bar = item.find("bar"); bar != item.end()) page.bar = bar->get<bool>();
        pages.push_back(std::move(page));
    }
    return pages;
}
Profile Profile::Load(const std::filesystem::path& path, std::string& error)
{
    error.clear();
    Profile profile;
    try {
        if (!std::filesystem::exists(path)) return profile;
        if (std::filesystem::file_size(path) > 16 * 1024 * 1024) throw std::runtime_error("Profile is too large");
        std::ifstream stream(path);
        auto j = json::parse(stream);
        if (j.at("version") != 1) throw std::runtime_error("Unknown profile version");
        if (auto windows = j.find("windows"); windows != j.end()) {
            if (!windows->is_array() || windows->size() > 64) throw std::runtime_error("Invalid window list");
            for (const auto& item : *windows) {
                WindowSession window;
                window.tabs = Decode(item.at("tabs"), 512);
                window.selected = item.at("selected").get<size_t>();
                if (window.selected >= window.tabs.size()) window.selected = 0;
                if (auto frame = item.find("frame"); frame != item.end()) {
                    if (!frame->is_array() || frame->size() != 4) throw std::runtime_error("Invalid window frame");
                    for (size_t i = 0; i < 4; ++i) window.frame[i] = (*frame)[i].get<float>();
                }
                if (!window.tabs.empty()) profile.windows.push_back(std::move(window));
            }
        } else {
            WindowSession window;
            window.tabs = Decode(j.at("tabs"), 512);
            window.selected = j.at("selected").get<size_t>();
            if (window.selected >= window.tabs.size()) window.selected = 0;
            if (!window.tabs.empty()) profile.windows.push_back(std::move(window));
        }
        profile.bookmarks = Decode(j.at("bookmarks"), 10000);
        profile.history = Decode(j.at("history"), 2000);
        if (auto home = j.find("homeURL"); home != j.end()) {
            profile.homeURL = home->get<std::string>();
            if (profile.homeURL.size() > 65536 || profile.homeURL.find('\0') != std::string::npos)
                throw std::runtime_error("Invalid home page");
        }
        if (auto bar = j.find("showBookmarksBar"); bar != j.end()) profile.showBookmarksBar = bar->get<bool>();
        if (auto style = j.find("interfaceStyle"); style != j.end()) {
            profile.interfaceStyle = style->get<std::string>();
            if (profile.interfaceStyle != "haiku" && profile.interfaceStyle != "safari") profile.interfaceStyle = "haiku";
        }
        if (auto engine = j.find("searchEngine"); engine != j.end()) {
            const auto id = engine->get<std::string>();
            for (const auto& known : SearchEngines())
                if (id == known.id) profile.searchEngine = id;
        }
        if (auto zoom = j.find("siteZoom"); zoom != j.end()) {
            if (!zoom->is_object() || zoom->size() > 5000) throw std::runtime_error("Invalid site zoom list");
            for (const auto& [site, value] : zoom->items()) {
                const double factor = value.get<double>();
                // Unusable entries are dropped rather than failing the profile.
                if (site.empty() || site.size() > 255 || !(factor >= kMinimumZoom && factor <= kMaximumZoom)) continue;
                profile.siteZoom[site] = factor;
            }
        }
        if (auto unpinned = j.find("unpinnedExtensions"); unpinned != j.end()) {
            if (!unpinned->is_array() || unpinned->size() > 1000) throw std::runtime_error("Invalid unpinned extension list");
            for (const auto& identifier : *unpinned) {
                const auto text = identifier.get<std::string>();
                if (!text.empty() && text.size() <= 255) profile.unpinnedExtensions.insert(text);
            }
        }
    } catch (const std::exception& e) { error = e.what(); return {}; }
    return profile;
}
bool Profile::Save(const std::filesystem::path& path, std::string& error) const
{
    error.clear();
    int fd = -1;
    std::string temporary;
    try {
        std::filesystem::create_directories(path.parent_path());
        json sessions = json::array();
        for (const auto& window : windows) {
            json item = {{"tabs", Encode(window.tabs)}, {"selected", window.selected}};
            if (window.HasFrame()) item["frame"] = {window.frame[0], window.frame[1], window.frame[2], window.frame[3]};
            sessions.push_back(std::move(item));
        }
        const WindowSession first = windows.empty() ? WindowSession() : windows.front();
        std::string data = json{{"version", 1}, {"tabs", Encode(first.tabs)}, {"selected", first.selected},
            {"windows", std::move(sessions)},
            {"bookmarks", Encode(bookmarks)}, {"history", Encode(history)},
            {"homeURL", homeURL}, {"showBookmarksBar", showBookmarksBar},
            {"interfaceStyle", interfaceStyle}, {"searchEngine", searchEngine},
            {"siteZoom", siteZoom}, {"unpinnedExtensions", unpinnedExtensions}}.dump(2);
        if (data.size() > 16 * 1024 * 1024) throw std::runtime_error("Profile is too large");
        temporary = path.string() + ".XXXXXX";
        fd = mkstemp(temporary.data());
        if (fd < 0) throw std::runtime_error(std::strerror(errno));
        size_t written = 0;
        while (written < data.size()) {
            ssize_t count = write(fd, data.data() + written, data.size() - written);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw std::runtime_error(std::strerror(errno));
            written += count;
        }
        if (fsync(fd) != 0) throw std::runtime_error(std::strerror(errno));
        if (close(fd) != 0) { fd = -1; throw std::runtime_error(std::strerror(errno)); }
        fd = -1;
        if (rename(temporary.c_str(), path.c_str()) != 0) throw std::runtime_error(std::strerror(errno));
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        if (fd >= 0) close(fd);
        if (!temporary.empty()) unlink(temporary.c_str());
        return false;
    }
}
bool Profile::Visit(const PageRecord& page, int64_t now)
{
    if (page.url.rfind("https://", 0) != 0 && page.url.rfind("http://", 0) != 0) return false;
    history.erase(std::remove_if(history.begin(), history.end(), [&](const auto& old) {
        return old.url == page.url;
    }), history.end());
    history.insert(history.begin(), page);
    history.front().visited = now;
    history.front().bar = false;
    if (history.size() > 2000) history.resize(2000);
    return true;
}
PageRecord* Profile::FindBookmark(const std::string& url)
{
    auto found = std::find_if(bookmarks.begin(), bookmarks.end(), [&](const auto& bookmark) { return bookmark.url == url; });
    return found == bookmarks.end() ? nullptr : &*found;
}
void Profile::AddBookmark(const PageRecord& page, bool bar)
{
    if (auto* existing = FindBookmark(page.url)) {
        existing->bar = bar;
        return;
    }
    if (bookmarks.size() >= 10000) return;
    bookmarks.push_back({page.url, page.title, 0, bar});
}
bool Profile::RemoveBookmark(const std::string& url)
{
    const auto size = bookmarks.size();
    bookmarks.erase(std::remove_if(bookmarks.begin(), bookmarks.end(), [&](const auto& bookmark) {
        return bookmark.url == url;
    }), bookmarks.end());
    return bookmarks.size() != size;
}
}
