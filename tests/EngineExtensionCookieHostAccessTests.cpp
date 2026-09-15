#include "config.h"
#include "WebExtensionCookieHostAccess.h"
#include <Application.h>
#include <JavaScriptCore/InitializeThreading.h>
#include <cstdio>
#include <wtf/MainThread.h>

// Only API::Object construction uses this isolated initialization adapter.
// No full extension context, WebCore initialization or browser is exercised.
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
    HashMap<Ref<WebExtensionMatchPattern>, WallTime> grants, denials;
    auto now = WallTime::fromRawSeconds(100);
    auto add = [&](auto& map, const String& text, WallTime expiration = WallTime::infinity()) {
        RefPtr pattern = WebExtensionMatchPattern::getOrCreate(text);
        check(pattern && pattern->isSupported(), "the actual engine parses the host-permission fixture");
        if (pattern)
            map.set(pattern.releaseNonNull(), expiration);
    };
    auto allowed = [&](const String& value) { return hasWebExtensionCookieHostAccess(URL { value }, grants, denials, now); };
    check(!allowed("https://example.test/a"_s), "an empty grant set does not authorize a cookie host");
    add(grants, "https://example.test/private/*"_s);
    check(allowed("https://example.test/public?query=1"_s), "cookie host grants ignore match-pattern paths");
    check(allowed("https://example.test:8443/public"_s), "cookie host grants ignore URL ports");
    check(!allowed("http://example.test/public"_s), "a grant retains its scheme restriction");
    check(!allowed("https://sub.example.test/public"_s) && !allowed("https://badexample.test/public"_s), "exact host grants do not authorize unrelated or subdomain hosts");
    grants.clear();
    check(!allowed("https://example.test/a"_s), "removing a host grant revokes the next decision");
    add(grants, "*://*.example.test/*"_s);
    check(allowed("http://sub.example.test/a"_s) && allowed("https://example.test/a"_s), "a wildcard host grant covers its base domain and subdomains");
    check(!allowed("https://badexample.test/a"_s), "wildcard domains require a complete host-label boundary");
    add(denials, "https://sub.example.test/private/*"_s);
    check(!allowed("https://sub.example.test/public"_s), "an explicit host denial also ignores paths");
    check(allowed("http://sub.example.test/public"_s), "a denial does not cross its scheme boundary");
    grants.clear();
    denials.clear();
    add(grants, "<all_urls>"_s);
    add(denials, "https://example.test/*"_s);
    check(!allowed("https://example.test/a"_s), "a specific denial takes precedence over an all-host grant");
    check(allowed("https://other.test/a"_s), "the all-host grant still covers other hosts");
    for (auto value : { "file:///tmp/cookies"_s, "ftp://example.test/a"_s, "javascript:1"_s, ""_s })
        check(!allowed(value), "cookie host access is limited to valid HTTP-family URLs");
    grants.clear();
    denials.clear();
    add(grants, "https://example.test/*"_s);
    add(denials, "<all_urls>"_s);
    check(allowed("https://example.test/a"_s), "a specific grant remains an exception to the context's wildcard denial policy");
    add(denials, "https://example.test/private/*"_s);
    check(!allowed("https://example.test/a"_s), "a specific denial wins when both specific policies match");
    grants.clear();
    denials.clear();
    add(grants, "https://example.test/*"_s, now);
    check(!allowed("https://example.test/a"_s), "a grant expires exactly at its deadline");
    add(grants, "https://example.test/*"_s, WallTime::fromRawSeconds(101));
    check(allowed("https://example.test/a"_s), "an unexpired grant authorizes its host");
    add(denials, "https://example.test/*"_s, now);
    check(allowed("https://example.test/a"_s), "an expired denial does not override a current grant");
    add(grants, "https://example.test/*"_s, WallTime::nan());
    check(!allowed("https://example.test/a"_s), "an invalid grant deadline cannot authorize access");
    grants.clear();
    denials.clear();
    add(grants, "https://[::1]/*"_s);
    check(allowed("https://[::1]:8443/cookie"_s), "IPv6 host grants match the URL parser's bracketed host");
    check(!allowed("https://[::2]/cookie"_s), "an IPv6 host grant does not authorize another address");
    add(grants, "http://127.0.0.1/*"_s);
    check(allowed("http://127.0.0.1:8080/a"_s) && !allowed("http://127.0.0.2/a"_s), "IPv4 cookie grants retain exact address scope");
}
class CookieAccessApplication final : public BApplication {
public:
    bool didRun { false };
    CookieAccessApplication() : BApplication("application/x-vnd.Kunanyi-Summit-cookie-host-access-tests") { }
    void ReadyToRun() override
    {
        WTF::initializeMainThread();
        runChecks();
        didRun = true;
        PostMessage(B_QUIT_REQUESTED);
    }
};
int main()
{
    CookieAccessApplication application;
    application.Run();
    check(application.didRun, "the native host-access checks ran from ReadyToRun");
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
