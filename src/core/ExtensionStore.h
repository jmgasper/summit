#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace summit {
inline constexpr const char* kChromeExtensionStore = "https://chromewebstore.google.com/category/extensions";
inline constexpr const char* kFirefoxExtensionStore = "https://addons.mozilla.org/en-US/firefox/extensions/";
struct ExtensionStoreItem {
    enum class Store { Chrome, Firefox } store;
    std::string identifier;
    std::string requestURL;
};
// Only extension detail pages on the two official HTTPS stores are accepted.
std::optional<ExtensionStoreItem> ParseExtensionStoreURL(std::string_view);
}
