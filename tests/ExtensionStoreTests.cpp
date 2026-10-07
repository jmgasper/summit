#include "core/ExtensionStore.h"
#include <iostream>

int main()
{
    using namespace summit;
    unsigned checks = 0;
    auto check = [&](bool value, const std::string& label) {
        ++checks;
        if (!value) { std::cerr << "FAIL: " << label << '\n'; std::exit(1); }
    };
    const std::string id = "eimadpbcbfnmbkopoojfekhnkhdbieeh";
    for (auto url : {"https://chromewebstore.google.com/detail/dark-reader/" + id,
            "https://chromewebstore.google.com/detail/" + id + "?hl=en#reviews",
            "https://chrome.google.com/webstore/detail/dark-reader/" + id + "/",
            "  https://CHROMEWEBSTORE.GOOGLE.COM:443/detail/" + id + "  "}) {
        auto item = ParseExtensionStoreURL(url);
        check(item && item->store == ExtensionStoreItem::Store::Chrome && item->identifier == id
            && item->requestURL.find("acceptformat=crx3") != std::string::npos, url);
    }
    for (auto url : {"https://addons.mozilla.org/en-US/firefox/addon/darkreader/",
            "https://addons.mozilla.org/firefox/addon/darkreader/?utm_source=summit",
            "https://addons.mozilla.org/de/firefox/addon/darkreader"}) {
        auto item = ParseExtensionStoreURL(url);
        check(item && item->store == ExtensionStoreItem::Store::Firefox && item->identifier == "darkreader"
            && item->requestURL == "https://addons.mozilla.org/api/v5/addons/addon/darkreader/", url);
    }
    for (auto url : {"http://addons.mozilla.org/firefox/addon/darkreader/",
            "https://addons.mozilla.org.evil.test/firefox/addon/darkreader/",
            "https://addons.mozilla.org@evil.test/firefox/addon/darkreader/",
            "https://evil.test@addons.mozilla.org/firefox/addon/darkreader/",
            "https://addons.mozilla.org:444/firefox/addon/darkreader/",
            "https://addons.mozilla.org/firefox/addon/../",
            "https://addons.mozilla.org/firefox/addon/%2e%2e/",
            "https://addons.mozilla.org/firefox/addon/darkreader/extra",
            "https://addons.mozilla.org/firefox/addon/dark\\reader/",
            "https://addons.mozilla.org/firefox/addon/darkreader\n/",
            "https://chromewebstore.google.com/detail/dark-reader/short",
            "https://chromewebstore.google.com/detail/qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq",
            kChromeExtensionStore, kFirefoxExtensionStore, "file:///extension.xpi"})
        check(!ParseExtensionStoreURL(url), url);
    std::cout << checks << " extension store URL checks passed\n";
}
