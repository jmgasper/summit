#include "ExtensionStore.h"
#include "Address.h"
#include <algorithm>
#include <vector>

namespace summit {
std::optional<ExtensionStoreItem> ParseExtensionStoreURL(std::string_view input)
{
    const std::string url = Trim(input);
    if (url.size() > 4096 || url.compare(0, 8, "https://") != 0
        || std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 32 || c == 127 || c == '\\'; }))
        return {};
    const auto slash = url.find('/', 8);
    if (slash == std::string::npos) return {};
    std::string host = url.substr(8, slash - 8);
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; });
    if (host.size() > 4 && host.compare(host.size() - 4, 4, ":443") == 0) host.resize(host.size() - 4);
    const auto end = url.find_first_of("?#", slash);
    auto path = url.substr(slash + 1, end == std::string::npos ? end : end - slash - 1);
    if (!path.empty() && path.back() == '/') path.pop_back();
    std::vector<std::string> parts;
    for (size_t start = 0; start <= path.size();) {
        const auto next = path.find('/', start);
        parts.push_back(path.substr(start, next == std::string::npos ? next : next - start));
        if (next == std::string::npos) break;
        start = next + 1;
    }
    auto validSlug = [](const std::string& slug) {
        return !slug.empty() && slug.size() <= 255 && slug != "." && slug != ".."
            && std::all_of(slug.begin(), slug.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
            });
    };
    if (host == "chromewebstore.google.com" || host == "chrome.google.com") {
        if (host == "chrome.google.com") {
            if (parts.empty() || parts.front() != "webstore") return {};
            parts.erase(parts.begin());
        }
        if ((parts.size() != 2 && parts.size() != 3) || parts[0] != "detail"
            || (parts.size() == 3 && !validSlug(parts[1]))) return {};
        const auto& id = parts.back();
        if (id.size() != 32 || !std::all_of(id.begin(), id.end(), [](char c) { return c >= 'a' && c <= 'p'; })) return {};
        return ExtensionStoreItem { ExtensionStoreItem::Store::Chrome, id,
            "https://clients2.google.com/service/update2/crx?response=redirect&prodversion=130.0&acceptformat=crx3&x=id%3D" + id + "%26uc" };
    }
    if (host == "addons.mozilla.org") {
        if (parts.size() == 4 && validSlug(parts[0])) parts.erase(parts.begin());
        if (parts.size() != 3 || parts[0] != "firefox" || parts[1] != "addon" || !validSlug(parts[2])) return {};
        return ExtensionStoreItem { ExtensionStoreItem::Store::Firefox, parts[2],
            "https://addons.mozilla.org/api/v5/addons/addon/" + parts[2] + "/" };
    }
    return {};
}
}
