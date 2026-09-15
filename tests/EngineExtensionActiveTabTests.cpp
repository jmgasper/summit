/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionActiveTabGrant.h"
#include <Application.h>
#include <JavaScriptCore/InitializeThreading.h>
#include <cstdio>
#include <wtf/MainThread.h>

// Only API::Object construction uses this initialization adapter. The real
// grant and URL/match-pattern implementations run without a browser context.
namespace WebKit {
void InitializeWebKit2()
{
    JSC::initialize();
    WTF::initializeMainThread();
}
}
using namespace WebKit;
static unsigned checks, failures;
static void check(bool value, const char* description)
{
    ++checks;
    failures += !value;
    std::printf("%s %s\n", value ? "PASS" : "FAIL", description);
}
static void runChecks()
{
    WebExtensionActiveTabGrant firstTab, secondTab;
    URL origin { "https://example.test/page?query=1#fragment"_s };
    auto allows = [&](const String& url) { return firstTab.matchesURL(origin, URL { url }, false); };
    check(!firstTab.isGranted() && !allows("https://example.test/page"_s), "a new tab has no temporary access");
    check(firstTab.grant(origin, false), "a user-approved HTTPS origin can be granted");
    check(firstTab.isGranted(), "a successful grant is retained");
    check(firstTab.pattern()->string() == "https://example.test/*"_s, "the permission descriptor has an exact host and scheme with all paths");
    check(allows("https://example.test/other?new=1#section"_s), "temporary host access ignores path, query and fragment");
    check(allows("https://EXAMPLE.test:443/other"_s), "host case and an explicit default port have the same origin");
    check(allows("https://name:password@example.test/other"_s), "URL credentials do not change origin identity");
    check(!allows("http://example.test/"_s), "HTTPS access does not grant HTTP");
    check(!allows("https://example.test:8443/"_s), "temporary access does not cross an origin port");
    check(!allows("https://sub.example.test/"_s), "temporary access does not include subdomains");
    check(!allows("https://badexample.test/"_s), "temporary access retains host-label boundaries");
    check(!allows("https://other.test/"_s), "an unrelated origin remains inaccessible");
    check(!allows("webkit-extension://example.test/"_s), "a web origin cannot authorize an extension origin");
    check(!allows("file:///tmp/example.test"_s), "a web origin cannot authorize local files");
    check(!secondTab.matchesURL(origin, origin, false), "a grant in one tab does not authorize another tab");
    check(!firstTab.matchesURL(URL { "https://other.test/"_s }, origin, false), "a stale grant cannot authorize the old origin after the committed tab origin changes");
    check(!firstTab.matchesURL(URL { }, origin, false), "an opaque or absent committed origin cannot consume a grant");
    check(!firstTab.revokeForNavigation(URL { "https://example.test/next"_s }), "same-origin navigation preserves temporary access");
    check(allows("https://example.test/after-navigation"_s), "the grant still authorizes the same origin after navigation");
    check(firstTab.revokeForNavigation(URL { "http://example.test/"_s }), "a scheme change revokes temporary access");
    check(!firstTab.isGranted() && !allows("https://example.test/"_s), "returning to the previous origin cannot restore a revoked grant");
    check(!firstTab.revokeForNavigation(origin), "revoking an empty grant is harmless");
    check(firstTab.grant(origin, false), "a later invocation may grant the origin again");
    check(firstTab.revokeForNavigation(URL { "https://example.test:8443/"_s }), "a port change revokes temporary access");
    check(firstTab.grant(origin, false) && firstTab.revokeForNavigation(URL { }), "navigation to an opaque origin revokes temporary access");
    check(firstTab.grant(origin, false) && firstTab.revokeForNavigation(URL { "https://sub.example.test/"_s }), "navigation to a subdomain revokes temporary access");
    for (auto url : { ""_s, "not a url"_s, "about:blank"_s, "data:text/html,test"_s, "javascript:1"_s, "webkit-extension://extension/page.html"_s, "ftp://example.test/file"_s }) {
        firstTab.grant(origin, false);
        check(!firstTab.grant(URL { url }, true) && !firstTab.isGranted(), "unsupported or opaque URLs cannot create or retain a grant");
    }
    URL file { "file:///tmp/first.html"_s };
    check(!firstTab.grant(file, false), "file URLs require the separate browser file-access setting");
    check(firstTab.grant(file, true), "file access can be granted when the browser explicitly allows it");
    check(firstTab.matchesURL(file, URL { "file:///tmp/second.html"_s }, true), "an allowed file origin covers file paths");
    check(!firstTab.matchesURL(file, file, false), "disabling file access immediately blocks a retained grant");
    check(!firstTab.matchesURL(file, URL { "https://example.test/"_s }, true), "file permission does not authorize a web origin");
    firstTab.clear();
    check(!firstTab.isGranted() && !firstTab.pattern(), "explicit revocation drops both the grant and its descriptor");
    firstTab.clear();
    check(!firstTab.matchesURL(file, file, true), "repeated revocation cannot restore access");
    for (auto text : { "https://[::1]:8443/a"_s, "http://127.0.0.1:8080/a"_s, "https://xn--bcher-kva.test/a"_s }) {
        URL address { text };
        check(firstTab.grant(address, false) && firstTab.matchesURL(address, address, false), "IPv6, IPv4 and internationalized hosts retain exact origin access");
        URL changed { address }; changed.setPort(9999);
        check(!firstTab.matchesURL(address, changed, false), "numeric and internationalized origins retain the port restriction");
    }
    firstTab.grant(origin, false);
    WebExtensionMatchPattern::MatchPatternSet removed;
    auto pattern = [&](const String& text) {
        RefPtr result = WebExtensionMatchPattern::getOrCreate(text);
        check(result && result->isSupported(), "the engine parses the revocation pattern fixture");
        return result;
    };
    check(!firstTab.matchesAnyPattern(removed), "an empty revocation does not match the grant");
    removed.add(pattern("https://other.test/*"_s).releaseNonNull());
    check(!firstTab.matchesAnyPattern(removed), "revocation of a different host leaves this tab grant alone");
    removed.add(pattern("http://example.test/*"_s).releaseNonNull());
    check(!firstTab.matchesAnyPattern(removed), "revocation retains the match-pattern scheme restriction");
    removed.add(pattern("https://example.test/private/*"_s).releaseNonNull());
    check(firstTab.matchesAnyPattern(removed), "host-permission revocation ignores path restrictions");
    removed.clear(); removed.add(pattern("*://*.example.test/*"_s).releaseNonNull());
    check(firstTab.matchesAnyPattern(removed), "a parent wildcard revokes an exact-host temporary grant");
    removed.clear(); removed.add(pattern("<all_urls>"_s).releaseNonNull());
    check(firstTab.matchesAnyPattern(removed), "all-URL revocation includes temporary web access");
    firstTab.grant(file, true);
    check(firstTab.matchesAnyPattern(removed), "all-URL revocation includes an explicitly enabled file grant");
    removed.clear(); removed.add(pattern("file:///*"_s).releaseNonNull());
    check(firstTab.matchesAnyPattern(removed), "file-pattern revocation includes an enabled file grant");
    firstTab.clear();
    check(!firstTab.matchesAnyPattern(removed), "a revoked grant no longer matches revocation patterns");
}
class ActiveTabApplication final : public BApplication {
public:
    bool didRun { false };
    ActiveTabApplication() : BApplication("application/x-vnd.Kunanyi-Summit-active-tab-tests") { }
    void ReadyToRun() override
    {
        WTF::initializeMainThread();
        runChecks();
        std::printf("%u checks, %u failures\n", checks, failures);
        didRun = true;
        PostMessage(B_QUIT_REQUESTED);
    }
};
int main()
{
    JSC::initialize();
    ActiveTabApplication application;
    application.Run();
    return !application.didRun || failures ? 1 : 0;
}
