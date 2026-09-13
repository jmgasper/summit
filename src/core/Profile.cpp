#include "Profile.h"
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
    for (const auto& page : pages) result.push_back({{"url", page.url}, {"title", page.title}});
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
        pages.push_back({url, title});
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
        profile.tabs = Decode(j.at("tabs"), 512);
        profile.bookmarks = Decode(j.at("bookmarks"), 10000);
        profile.history = Decode(j.at("history"), 2000);
        profile.selected = j.at("selected").get<size_t>();
        if (profile.selected >= profile.tabs.size()) profile.selected = 0;
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
        std::string data = json{{"version", 1}, {"tabs", Encode(tabs)}, {"selected", selected},
            {"bookmarks", Encode(bookmarks)}, {"history", Encode(history)}}.dump(2);
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
void Profile::Visit(const PageRecord& page)
{
    if (page.url.rfind("https://", 0) != 0 && page.url.rfind("http://", 0) != 0) return;
    history.erase(std::remove_if(history.begin(), history.end(), [&](const auto& old) {
        return old.url == page.url;
    }), history.end());
    history.insert(history.begin(), page);
    if (history.size() > 2000) history.resize(2000);
}
}
