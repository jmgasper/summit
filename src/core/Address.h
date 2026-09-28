#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace summit {
struct Address {
    std::string url;
    std::string error;
    bool search = false;
};
std::string Trim(std::string_view text);
std::string PercentEncode(std::string_view text);
// Convert an absolute native path to a URL, preserving directory separators.
std::string FileURL(std::string_view path);
std::string EscapeHTML(std::string_view text);
// Text that is not an address is searched for with the chosen engine.
struct SearchEngine {
    const char* id;     // saved in the profile
    const char* name;   // shown to people
    const char* prefix; // the query is appended, percent-encoded
};
// DuckDuckGo (the default), Google and Bing, in the order menus list them.
const std::vector<SearchEngine>& SearchEngines();
// Unknown identifiers choose the default. Any thread.
void SetSearchEngine(std::string_view id);
const SearchEngine& CurrentSearchEngine();
std::string SearchURL(std::string_view text);
Address ResolveAddress(std::string_view input);
}
