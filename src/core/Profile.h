#pragma once
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace summit {
struct PageRecord {
    std::string url;
    std::string title;
    // History: when the page was last visited, in seconds since the epoch
    // (0 for entries saved before visits were timed).
    int64_t visited = 0;
    // Bookmarks: shown on the bookmarks bar under the toolbar.
    bool bar = false;
};
struct Profile {
    std::vector<PageRecord> tabs;
    size_t selected = 0;
    std::vector<PageRecord> bookmarks;
    std::vector<PageRecord> history;
    // Where the Home button goes; empty means the built-in start page.
    std::string homeURL;
    bool showBookmarksBar = true;
    static Profile Load(const std::filesystem::path& path, std::string& error);
    bool Save(const std::filesystem::path& path, std::string& error) const;
    void Visit(const PageRecord& page, int64_t now = std::time(nullptr));
    PageRecord* FindBookmark(const std::string& url);
    // Adds the page, or moves an existing bookmark on to (or off) the bar.
    void AddBookmark(const PageRecord& page, bool bar);
    bool RemoveBookmark(const std::string& url);
};
}
