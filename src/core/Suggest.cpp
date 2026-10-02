#include "Suggest.h"
#include <algorithm>
#include <cctype>
#include <map>

namespace summit {
namespace {
std::string Lower(std::string_view text)
{
    std::string result(text);
    for (auto& character : result) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return result;
}

bool StartsWith(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

// An http(s) address split where type-ahead needs it. The authority keeps its
// port; it and the scheme are lower case, the path keeps its case.
struct Parts {
    std::string scheme;
    std::string authority;
    std::string path;
};

bool Split(std::string_view url, Parts& parts)
{
    const auto lower = Lower(url.substr(0, std::min<size_t>(url.size(), 8)));
    size_t start;
    if (StartsWith(lower, "https://")) parts.scheme = "https", start = 8;
    else if (StartsWith(lower, "http://")) parts.scheme = "http", start = 7;
    else return false;
    const size_t end = url.find_first_of("/?#", start);
    parts.authority = Lower(url.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
    parts.path = end == std::string_view::npos ? "/" : std::string(url.substr(end));
    if (parts.path[0] != '/') parts.path.insert(0, "/");
    // User information never completes into the field.
    return !parts.authority.empty() && parts.authority.find('@') == std::string::npos;
}

std::string_view WithoutWWW(std::string_view authority)
{
    return StartsWith(authority, "www.") ? authority.substr(4) : authority;
}

// A bookmark without visits still counts, as a page visited once recently.
constexpr double kBookmarkWeight = 100;

struct Candidate {
    const PageRecord* page;
    double weight;
};

std::vector<Candidate> Candidates(const std::vector<PageRecord>& history, const std::vector<PageRecord>& bookmarks, int64_t now)
{
    std::vector<Candidate> result;
    result.reserve(history.size() + bookmarks.size());
    for (const auto& page : history) result.push_back({&page, Frecency(page, now)});
    for (const auto& page : bookmarks) result.push_back({&page, kBookmarkWeight});
    return result;
}

std::vector<std::string> Words(std::string_view text)
{
    std::vector<std::string> words;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        size_t end = i;
        while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end]))) ++end;
        if (end > i) words.push_back(Lower(text.substr(i, end - i)));
        i = end;
    }
    return words;
}
}

double Frecency(const PageRecord& page, int64_t now)
{
    const double visits = std::max(1, page.visits);
    const double days = page.visited ? std::max<int64_t>(0, now - page.visited) / 86400.0 : 365;
    const double weight = days < 4 ? 100 : days < 14 ? 70 : days < 31 ? 50 : days < 90 ? 30 : 10;
    return visits * weight;
}

Autofill AutofillAddress(const std::vector<PageRecord>& history, const std::vector<PageRecord>& bookmarks,
    std::string_view typed, int64_t now)
{
    if (typed.empty() || typed.size() > 2048) return {};
    for (const char character : typed)
        if (std::isspace(static_cast<unsigned char>(character))) return {};
    // A typed scheme stays as typed and limits completion to that scheme.
    std::string scheme;
    std::string_view rest = typed;
    const auto lowerTyped = Lower(typed);
    if (StartsWith(lowerTyped, "https://")) scheme = "https", rest = typed.substr(8);
    else if (StartsWith(lowerTyped, "http://")) scheme = "http", rest = typed.substr(7);
    const auto wanted = Lower(rest);
    if (wanted.empty() || wanted[0] == '/') return {};
    const size_t slash = wanted.find('/');
    const bool typedWWW = StartsWith(wanted, "www.");

    // Completions are grouped by what the field would show; the group with
    // the most weight wins, and its heaviest page decides where Enter goes.
    struct Group {
        double weight = 0;
        double best = -1;
        std::string url;
    };
    std::map<std::string, Group> groups;
    for (const auto& candidate : Candidates(history, bookmarks, now)) {
        Parts parts;
        if (!Split(candidate.page->url, parts) || (!scheme.empty() && parts.scheme != scheme)) continue;
        const std::string shown(typedWWW ? std::string_view(parts.authority) : WithoutWWW(parts.authority));
        std::string completion;
        std::string target;
        if (slash == std::string::npos) {
            // A site: complete its name and the slash after it.
            if (!StartsWith(shown, wanted)) continue;
            completion = shown + "/";
            target = parts.scheme + "://" + parts.authority + "/";
        } else {
            // Inside a path: complete the next segment of a page under it.
            if (shown != wanted.substr(0, slash)) continue;
            const std::string full = shown + parts.path;
            if (full.size() <= wanted.size() || Lower(full.substr(0, wanted.size())) != wanted) continue;
            size_t end = full.find('/', wanted.size());
            if (end != std::string::npos) ++end;
            else end = std::min(full.find_first_of("?#", wanted.size()), full.size());
            if (end <= wanted.size()) continue;
            completion = full.substr(0, end);
            target = parts.scheme + "://" + parts.authority + completion.substr(shown.size());
        }
        auto& group = groups[completion];
        group.weight += candidate.weight;
        if (candidate.weight > group.best) {
            group.best = candidate.weight;
            group.url = std::move(target);
        }
    }
    const std::map<std::string, Group>::value_type* winner = nullptr;
    for (const auto& entry : groups) {
        if (!winner || entry.second.weight > winner->second.weight
            || (entry.second.weight == winner->second.weight && entry.first.size() < winner->first.size()))
            winner = &entry;
    }
    if (!winner || winner->first.size() <= wanted.size()) return {};
    return {std::string(typed) + winner->first.substr(wanted.size()), winner->second.url};
}

std::vector<Suggestion> SuggestPages(const std::vector<PageRecord>& history, const std::vector<PageRecord>& bookmarks,
    std::string_view typed, int64_t now, size_t limit)
{
    const auto words = Words(typed);
    if (words.empty() || limit == 0) return {};
    std::map<std::string, size_t> seen;
    std::vector<Suggestion> result;
    for (const auto& candidate : Candidates(history, bookmarks, now)) {
        const auto& page = *candidate.page;
        Parts parts;
        if (!Split(page.url, parts)) continue;
        const std::string site(WithoutWWW(parts.authority));
        const auto haystack = site + Lower(parts.path) + "\n" + Lower(page.title);
        bool all = true;
        bool boundary = false;
        for (const auto& word : words) {
            const size_t found = haystack.find(word);
            if (found == std::string::npos) { all = false; break; }
            if (found == 0 || !std::isalnum(static_cast<unsigned char>(haystack[found - 1]))) boundary = true;
        }
        if (!all) continue;
        double score = candidate.weight;
        if (StartsWith(site, words[0])) score *= 4;
        else if (boundary) score *= 1.5;
        const bool bookmark = candidate.page >= bookmarks.data() && candidate.page < bookmarks.data() + bookmarks.size();
        if (auto existing = seen.find(page.url); existing != seen.end()) {
            // A visited bookmark: one row, marked as a bookmark, with both weights.
            auto& row = result[existing->second];
            row.score += score;
            if (bookmark) row.kind = Suggestion::Kind::Bookmark;
            if (row.title.empty()) row.title = page.title;
            continue;
        }
        seen.emplace(page.url, result.size());
        result.push_back({page.url, page.title, bookmark ? Suggestion::Kind::Bookmark : Suggestion::Kind::History, score});
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
    if (result.size() > limit) result.resize(limit);
    return result;
}

std::string ShortAddress(std::string_view url)
{
    Parts parts;
    if (!Split(url, parts)) return std::string(url);
    std::string result(WithoutWWW(parts.authority));
    if (parts.path != "/") result += parts.path;
    return result;
}
}
