#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace summit {
struct PageRecord {
    std::string url;
    std::string title;
};
struct Profile {
    std::vector<PageRecord> tabs;
    size_t selected = 0;
    std::vector<PageRecord> bookmarks;
    std::vector<PageRecord> history;
    static Profile Load(const std::filesystem::path& path, std::string& error);
    bool Save(const std::filesystem::path& path, std::string& error) const;
    void Visit(const PageRecord& page);
};
}
