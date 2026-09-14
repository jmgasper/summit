#include "config.h"
#include "WebExtensionWebsiteData.h"
#include "WebExtensionMatchPattern.h"
#include <Application.h>
#include <cstdio>
#include <wtf/MainThread.h>

using namespace WebKit;
using WebCore::SecurityOriginData;
static unsigned checks, failures;

static void check(bool passed, const char* message)
{
    ++checks;
    if (!passed) {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}

static WebsiteDataRecord record(std::initializer_list<SecurityOriginData> origins)
{
    WebsiteDataRecord result;
    result.types = { WebsiteDataType::LocalStorage, WebsiteDataType::IndexedDBDatabases, WebsiteDataType::DiskCache };
    for (auto& origin : origins)
        result.origins.addVoid(origin);
    return result;
}

int main()
{
    BApplication application("application/x-vnd.Kunanyi-Summit-extension-website-data-tests");
    WTF::initializeMainThread();
    SecurityOriginData keep { "webkit-extension"_s, "keep"_s, std::nullopt };
    SecurityOriginData stale { "webkit-extension"_s, "removed"_s, std::nullopt };
    SecurityOriginData web { "https"_s, "example.test"_s, std::nullopt };
    HashSet<String> active { "webkit-extension://keep"_s };
    check(staleWebExtensionWebsiteData({ }, active).isEmpty(), "empty records produce no deletion candidates");
    check(staleWebExtensionWebsiteData({ record({ keep }) }, active).isEmpty(), "an installed extension origin is retained");
    check(staleWebExtensionWebsiteData({ record({ web }) }, { }).isEmpty(), "an ordinary website is retained even with no installed extensions");
    auto removed = staleWebExtensionWebsiteData({ record({ stale }) }, active);
    check(removed.size() == 1 && removed[0].origins == HashSet<SecurityOriginData> { stale }, "an unknown extension origin is selected");
    check(removed.size() == 1 && removed[0].types == record({ stale }).types, "origin deletion retains the record's data-type mask");
    auto mixed = record({ keep, stale, web });
    mixed.displayName = "shared display group"_s;
    mixed.cookieHostNames.addVoid("example.test"_s);
    mixed.HSTSCacheHostNames.addVoid("example.test"_s);
    mixed.alternativeServicesHostNames.addVoid("example.test"_s);
    removed = staleWebExtensionWebsiteData({ mixed }, active);
    check(removed.size() == 1 && removed[0].origins == HashSet<SecurityOriginData> { stale }, "mixed records only select stale extension origins");
    check(removed.size() == 1 && removed[0].cookieHostNames.isEmpty() && removed[0].HSTSCacheHostNames.isEmpty()
        && removed[0].alternativeServicesHostNames.isEmpty() && removed[0].resourceLoadStatisticsRegistrableDomains.isEmpty(),
        "host and domain deletion metadata is not copied from mixed records");
    check(mixed.origins.size() == 3 && mixed.cookieHostNames.contains("example.test"_s) && mixed.displayName == "shared display group"_s,
        "selection leaves the source record unchanged");
    check(active.size() == 1 && active.contains("webkit-extension://keep"_s), "selection leaves the installed-origin set unchanged");
    check(staleWebExtensionWebsiteData({ record({ }), record({ web }), record({ keep }) }, active).isEmpty(),
        "records with no stale origins are omitted");
    auto empty = record({ });
    empty.cookieHostNames.addVoid("ordinary.test"_s);
    check(staleWebExtensionWebsiteData({ empty }, { }).isEmpty(), "host-only records cannot become deletion candidates");
    SecurityOriginData upper { "webkit-extension"_s, "KEEP"_s, std::nullopt };
    check(staleWebExtensionWebsiteData({ record({ upper }) }, active).isEmpty(), "origin matching normalizes ASCII case");
    SecurityOriginData prefix { "webkit-extension"_s, "keep-extra"_s, std::nullopt };
    SecurityOriginData child { "webkit-extension"_s, "keep.example"_s, std::nullopt };
    removed = staleWebExtensionWebsiteData({ record({ prefix, child }) }, active);
    check(removed.size() == 1 && removed[0].origins.size() == 2, "installed origins require exact host matches");
    SecurityOriginData port { "webkit-extension"_s, "keep"_s, 8123 };
    check(staleWebExtensionWebsiteData({ record({ port }) }, active).size() == 1, "a different port is a distinct origin");
    check(staleWebExtensionWebsiteData({ record({ port }) }, { "webkit-extension://keep:8123"_s }).isEmpty(),
        "an installed origin with an explicit port is retained");
    for (auto scheme : { "http"_s, "https"_s, "file"_s, "ftp"_s, "custom-app"_s }) {
        SecurityOriginData other { scheme, "removed"_s, std::nullopt };
        check(staleWebExtensionWebsiteData({ record({ other }) }, { }).isEmpty(), "non-extension schemes are retained");
    }
    removed = staleWebExtensionWebsiteData({ record({ keep }), record({ stale }), record({ prefix }), record({ web }) }, active);
    check(removed.size() == 2 && removed[0].origins.contains(stale) && removed[1].origins.contains(prefix),
        "multiple candidates retain record order without retaining unrelated records");
    SecurityOriginData custom { "summit-extension-test"_s, "removed"_s, std::nullopt };
    check(staleWebExtensionWebsiteData({ record({ custom }) }, { }).isEmpty(), "an unregistered custom scheme is retained");
    WebExtensionMatchPattern::extensionSchemes().addVoid("summit-extension-test"_s);
    check(staleWebExtensionWebsiteData({ record({ custom }) }, { }).size() == 1, "selection honors the actual extension-scheme registry");
    check(staleWebExtensionWebsiteData({ record({ custom }) }, { "summit-extension-test://removed"_s }).isEmpty(),
        "an installed custom extension origin is retained");
    WebExtensionMatchPattern::extensionSchemes().remove("summit-extension-test"_s);
    check(staleWebExtensionWebsiteData({ record({ custom }) }, { }).isEmpty(), "scheme-registry changes are observed between selections");

    auto types = webExtensionWebsiteDataTypes();
    check(types.containsAll({ WebsiteDataType::LocalStorage, WebsiteDataType::IndexedDBDatabases, WebsiteDataType::SessionStorage }),
        "cleanup fetches extension origin storage");
    check(types.containsAll({ WebsiteDataType::DOMCache, WebsiteDataType::ServiceWorkerRegistrations, WebsiteDataType::FileSystem }),
        "cleanup fetches extension worker and filesystem data");
    check(types.containsAll({ WebsiteDataType::DiskCache, WebsiteDataType::MemoryCache, WebsiteDataType::DeviceIdHashSalt }),
        "cleanup fetches caches and origin salts");
    check(!types.contains(WebsiteDataType::LegacyOfflineWebApplicationCachePlaceholder), "legacy placeholder data is not requested");
    std::printf("%u native extension website-data checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
