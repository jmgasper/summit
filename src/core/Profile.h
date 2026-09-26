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
// One browser window of a saved session.
struct WindowSession {
    std::vector<PageRecord> tabs;
    size_t selected = 0;
    // Window frame in screen coordinates (left, top, right, bottom); all zero
    // when the window was never placed.
    float frame[4] = {0, 0, 0, 0};
    bool HasFrame() const { return frame[2] > frame[0] && frame[3] > frame[1]; }
};
struct Profile {
    // Open windows, front to back as they were created. Older builds only
    // read "tabs" and "selected", which hold the first window.
    std::vector<WindowSession> windows;
    std::vector<PageRecord> bookmarks;
    std::vector<PageRecord> history;
    // Where the Home button goes; empty means the built-in start page.
    std::string homeURL;
    bool showBookmarksBar = true;
    // "haiku" draws the window like other Haiku applications (real buttons and
    // tabs); "safari" is the flat, Safari-like look.
    std::string interfaceStyle = "haiku";
    static Profile Load(const std::filesystem::path& path, std::string& error);
    bool Save(const std::filesystem::path& path, std::string& error) const;
    // Records a visit to an http(s) page; returns false for other pages.
    bool Visit(const PageRecord& page, int64_t now = std::time(nullptr));
    PageRecord* FindBookmark(const std::string& url);
    // Adds the page, or moves an existing bookmark on to (or off) the bar.
    void AddBookmark(const PageRecord& page, bool bar);
    bool RemoveBookmark(const std::string& url);
};
}
