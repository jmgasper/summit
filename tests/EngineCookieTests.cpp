#include "config.h"
#include "Cookie.h"
#include "CookieUtil.h"
#include <wtf/MainThread.h>
#include <wtf/RunLoop.h>
#include <Application.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>

static int RunChecks()
{
    using namespace WebCore;
    int checks = 0, failures = 0;
    auto check = [&](bool passed, const char* label) {
        ++checks;
        failures += !passed;
        std::printf("%s %s\n", passed ? "PASS" : "FAIL", label);
    };
    check(CookieUtil::domainMatch("example.com"_s, "example.com"_s), "exact host cookie");
    check(CookieUtil::domainMatch(".example.com"_s, "example.com"_s), "domain cookie at its root host");
    check(CookieUtil::domainMatch(".example.com"_s, "www.example.com"_s), "domain cookie at a subdomain");
    check(CookieUtil::domainMatch(".example.com"_s, "a.b.example.com"_s), "domain cookie at a nested subdomain");
    check(!CookieUtil::domainMatch("example.com"_s, "www.example.com"_s), "host-only cookie stays on its host");
    check(!CookieUtil::domainMatch(".example.com.evil"_s, "example.com"_s), "a partial domain cannot authorize another site");
    check(!CookieUtil::domainMatch(".example.com"_s, "example.com.evil"_s), "a host prefix is not a domain suffix");
    check(!CookieUtil::domainMatch(".example.com"_s, "badexample.com"_s), "domain matching requires a label boundary");
    check(!CookieUtil::domainMatch(""_s, "example.com"_s), "empty cookie domain is rejected");
    check(!CookieUtil::domainMatch(".example.com"_s, ""_s), "empty request host is rejected");
    check(CookieUtil::domainMatch("127.0.0.1"_s, "127.0.0.1"_s), "exact IP address cookie");
    check(!CookieUtil::domainMatch(".0.0.1"_s, "127.0.0.1"_s), "an IP address cannot be a cookie subdomain");
    auto domain = CookieUtil::parseCookieHeader("Domain=example.com"_s);
    check(domain && domain->domain.isEmpty(), "cookie name is not a Domain attribute");
    auto secure = CookieUtil::parseCookieHeader("Secure=value"_s);
    check(secure && !secure->secure, "cookie name is not a Secure attribute");
    auto path = CookieUtil::parseCookieHeader("Path=/value"_s);
    check(path && path->path.isEmpty(), "cookie name is not a Path attribute");
    auto httpOnly = CookieUtil::parseCookieHeader("HttpOnly=value"_s);
    check(httpOnly && !httpOnly->httpOnly, "cookie name is not an HttpOnly attribute");
    auto maxAge = CookieUtil::parseCookieHeader("Max-Age=3600"_s);
    check(maxAge && maxAge->session && !maxAge->expires, "cookie name is not a Max-Age attribute");
    auto attributes = CookieUtil::parseCookieHeader("name=value; Domain=example.com; Path=/safe; Secure; HttpOnly"_s);
    check(attributes && attributes->name == "name"_s && attributes->value == "value"_s
        && attributes->domain == ".example.com"_s && attributes->path == "/safe"_s
        && attributes->secure && attributes->httpOnly, "real attributes remain effective after the cookie pair");
    std::printf("%d checks, %d failures\n", checks, failures);
    std::fflush(stdout);
    return failures ? 1 : 0;
}

class NativeTestApp : public BApplication {
public:
    NativeTestApp() : BApplication("application/x-vnd.Kunanyi-Summit-engine-cookies") { }
    void ReadyToRun() override
    {
        WTF::initializeMainThread();
        WTF::RunLoop::run();
        result = RunChecks();
        PostMessage(B_QUIT_REQUESTED);
    }
    int result = 1;
};

int main(int argc, char** argv)
{
    if (argc > 1 && !std::strcmp(argv[1], "--native-loop")) {
        NativeTestApp app;
        app.Run();
        return app.result;
    }
    WTF::initializeMainThread();
    if (argc > 1 && !std::strcmp(argv[1], "--main-thread-exit")) {
        static pthread_t original = pthread_self();
        pthread_t worker;
        if (pthread_create(&worker, nullptr, [](void*) -> void* {
            auto result = pthread_join(original, nullptr);
            std::exit(result ? 1 : 0);
        }, nullptr)) return 1;
        pthread_detach(worker);
        pthread_exit(nullptr);
    }
    return RunChecks();
}
