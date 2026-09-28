#pragma once
#include "Profile.h"
#include <functional>
#include <string>
#include <vector>

namespace summit {
// Built-in pages. The browser writes each one to a file in the profile and
// shows it under its summit: address; links in them open in the same tab.
constexpr const char* kHistoryPage = "summit:history";
constexpr const char* kBookmarksPage = "summit:bookmarks";

struct HistoryDay {
    std::string label;
    std::vector<const PageRecord*> pages;
};
// Groups history (most recent first) by local calendar day: "Today",
// "Yesterday", then the date, e.g. "Thursday, 24 September 2026". Visits
// recorded before visits had times are grouped under "Earlier".
std::vector<HistoryDay> GroupHistoryByDay(const std::vector<PageRecord>& history, int64_t now);

// Returns an image URL (normally data:) for a page's favicon, or empty.
using IconSource = std::function<std::string(const std::string& pageURL)>;
std::string RenderHistoryPage(const std::vector<PageRecord>& history, int64_t now, const IconSource& icons);
std::string RenderBookmarksPage(const std::vector<PageRecord>& bookmarks, const IconSource& icons);
// What a private window's new tabs show: what private browsing keeps and
// forgets, and a search field for the chosen engine (SearchEngines()).
std::string RenderPrivateStartPage(const std::string& searchEngineName, const std::string& searchPrefix);
}
