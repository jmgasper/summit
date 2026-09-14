// Runs the production URL filter against native WTF and JavaScriptCore.
#include "config.h"
#include "WebExtensionNavigationURLFilter.h"
#include "WebExtensionNavigationParameters.h"
#include <wtf/MainThread.h>
#include <wtf/text/MakeString.h>
#include <cstdio>
#include <Application.h>
#include <JavaScriptCore/JavaScript.h>
#include <JavaScriptCore/JSRetainPtr.h>

using namespace WebKit;
static unsigned passed;
static unsigned failed;
static void check(bool ok, const char* label)
{
    ++(ok ? passed : failed);
    printf("%s %s\n", ok ? "PASS" : "FAIL", label);
}
static void match(const char* filterJSON, const char* address, bool expected, const char* label)
{
    auto value = JSON::Value::parseJSON(String::fromUTF8(filterJSON));
    auto filter = WebExtensionNavigationURLFilter::parse(*value);
    check(filter && filter->matches(URL { String::fromUTF8(address) }) == expected, label);
}
static void invalid(const char* filterJSON)
{
    auto value = JSON::Value::parseJSON(String::fromUTF8(filterJSON));
    auto filter = WebExtensionNavigationURLFilter::parse(*value);
    check(!filter && !filter.error().isEmpty(), filterJSON);
}
int main()
{
    BApplication application("application/x-vnd.Kunanyi-Summit-extension-navigation-filter-tests");
    WTF::initializeMainThread();
    JSRetainPtr context(Adopt, JSGlobalContextCreate(nullptr));
    struct Case { const char* key; const char* value; const char* address; };
    const Case criteria[] {
        { "hostContains", "\".foo\"", "https://foo.example/x?name=value#frag" },
        { "hostEquals", "\"foo.example\"", "https://foo.example/x" },
        { "hostPrefix", "\"foo.\"", "https://foo.example/x" },
        { "hostSuffix", "\".example\"", "https://foo.example/x" },
        { "pathContains", "\"/x\"", "https://foo.example/x" },
        { "pathEquals", "\"/x\"", "https://foo.example/x" },
        { "pathPrefix", "\"/x\"", "https://foo.example/x" },
        { "pathSuffix", "\"x\"", "https://foo.example/x" },
        { "queryContains", "\"name=\"", "https://foo.example/x?name=value#frag" },
        { "queryEquals", "\"name=value\"", "https://foo.example/x?name=value#frag" },
        { "queryPrefix", "\"name\"", "https://foo.example/x?name=value#frag" },
        { "querySuffix", "\"value\"", "https://foo.example/x?name=value#frag" },
        { "urlContains", "\"example/x\"", "https://foo.example/x#frag" },
        { "urlEquals", "\"https://foo.example/x\"", "https://foo.example/x#frag" },
        { "urlPrefix", "\"https://foo.\"", "https://foo.example/x" },
        { "urlSuffix", "\"/x\"", "https://foo.example/x#frag" },
        { "urlMatches", "\"^https://foo\\\\.example/x$\"", "https://foo.example/x#frag" },
        { "originAndPathMatches", "\"^https://foo\\\\.example/x$\"", "https://foo.example/x?ignored=yes#frag" },
        { "schemes", "[\"https\"]", "https://foo.example/x" },
        { "ports", "[443]", "https://foo.example/x" },
    };
    for (auto& entry : criteria) {
        auto json = makeString("{\"url\":[{\""_s, String::fromUTF8(entry.key), "\":"_s, String::fromUTF8(entry.value), "}]}"_s).utf8();
        match(json.legacyCStringPointer(), entry.address, true, entry.key);
        match(json.legacyCStringPointer(), "http://other.invalid/different?wrong=yes", false, entry.key);
    }
    match(R"({"url":[]})", "https://example.org", true, "empty filter array matches all");
    match(R"({"url":[{}]})", "about:blank", true, "empty group matches all");
    match(R"({"url":[]})", "not a URL", false, "invalid target rejected");
    match(R"({"url":[{"hostEquals":"example.org","schemes":["https"]}]})", "http://example.org", false, "AND inside group");
    match(R"({"url":[{"hostEquals":"bad.invalid"},{"schemes":["https"]}]})", "https://example.org", true, "OR between groups");
    match(R"({"url":[{"hostEquals":"EXAMPLE.org"}]})", "https://example.org", false, "case sensitive host criterion");
    match(R"({"url":[{"schemes":["HTTPS"]}]})", "https://example.org", false, "case sensitive scheme criterion");
    match(R"({"url":[{"hostEquals":"example.org"}]})", "HTTPS://EXAMPLE.ORG", true, "canonical target hostname");
    match(R"({"url":[{"hostContains":".foo"}]})", "https://foo.com", true, "implicit leading host dot");
    match(R"({"url":[{"hostContains":"org."}]})", "https://example.org", false, "no implicit trailing host dot");
    match(R"({"url":[{"urlContains":"secret"}]})", "https://example.org/#secret", false, "fragment excluded");
    match(R"({"url":[{"urlEquals":"https://example.org/"}]})", "https://example.org:443/#fragment", true, "default port normalized and slash added");
    match(R"({"url":[{"urlSuffix":".org"}]})", "https://example.org", false, "hostname URL includes trailing slash");
    match(R"({"url":[{"ports":[80]}]})", "http://example.org", true, "default HTTP port");
    match(R"({"url":[{"ports":[21]}]})", "ftp://example.org", true, "default FTP port");
    match(R"({"url":[{"ports":[443]}]})", "wss://example.org", true, "default WSS port");
    match(R"({"url":[{"ports":[0]}]})", "https://example.org:0", true, "explicit zero port");
    match(R"({"url":[{"ports":[0]}]})", "file:///path", false, "absent port is not zero");
    match(R"({"url":[{"ports":[[1000,1200]]}]})", "https://example.org:1000", true, "range lower bound");
    match(R"({"url":[{"ports":[[1000,1200]]}]})", "https://example.org:1200", true, "range upper bound");
    match(R"({"url":[{"ports":[[1000,1200]]}]})", "https://example.org:1201", false, "outside range");
    match(R"({"url":[{"ports":[]}]})", "https://example.org", false, "empty ports matches none");
    match(R"({"url":[{"schemes":[]}]})", "https://example.org", false, "empty schemes matches none");
    match(R"({"url":[{"pathEquals":"/a%20b"}]})", "https://example.org/a%20b", true, "encoded path preserved");
    match(R"({"url":[{"queryEquals":"q=a%20b"}]})", "https://example.org/?q=a%20b", true, "encoded query preserved");
    match(R"({"url":[{"urlMatches":"example[.]org"}]})", "https://example.org/path", true, "partial regex search");
    match(R"({"url":[{"urlMatches":"^HTTPS"}]})", "https://example.org/path", false, "case sensitive regex");
    match(R"({"url":[{"urlMatches":""}]})", "https://example.org/path", true, "empty regex valid");
    match(R"({"url":[{"urlMatches":"^(?:https://)(?=example)[^/]+/"}]})", "https://example.org/path", true, "JavaScript regex lookahead");
    match(R"({"url":[{"originAndPathMatches":"secret"}]})", "https://example.org/?secret=yes#secret", false, "origin regex excludes query and fragment");
    for (auto text : { "null", "true", "[]", "{}", R"({"url":null})", R"({"url":{}})", R"({"url":[null]})", R"({"url":[[]]})", R"({"url":[{"unknown":true}]})", R"({"url":[{"hostContains":null}]})", R"({"url":[{"pathEquals":1}]})", R"({"url":[{"urlMatches":"["}]})", R"({"url":[{"originAndPathMatches":"("}]})", R"({"url":[{"schemes":"https"}]})", R"({"url":[{"schemes":[1]}]})", R"({"url":[{"ports":80}]})", R"({"url":[{"ports":[true]}]})", R"({"url":[{"ports":["80"]}]})", R"({"url":[{"ports":[-1]}]})", R"({"url":[{"ports":[65536]}]})", R"({"url":[{"ports":[80.5]}]})", R"({"url":[{"ports":[[80]]}]})", R"({"url":[{"ports":[[1,2,3]]}]})", R"({"url":[{"ports":[[90,80]]}]})", R"({"url":[{"ports":[[80,80]]}]})", R"({"url":[{"ports":[[1,65536]]}]})", R"({"url":[{"ports":[[1,2.5]]}]})" })
        invalid(text);
    auto input = JSON::Value::parseJSON(R"({"url":[{"hostEquals":"first.example"}]})"_s);
    auto frozen = WebExtensionNavigationURLFilter::parse(*input);
    input->asObject()->getArray("url"_s)->get(0)->asObject()->setString("hostEquals"_s, "second.example"_s);
    check(frozen && frozen->matches(URL { "https://first.example"_s }) && !frozen->matches(URL { "https://second.example"_s }), "parsed values survive input mutation");

    check(!parseWebExtensionNavigationTab(nullptr), "missing tab details rejected");
    check(!parseWebExtensionNavigationFrame(nullptr), "missing frame details rejected");
    for (auto text : { "null", "[]", "{}", R"({"tabId":true})", R"({"tabId":"1"})", R"({"tabId":0})", R"({"tabId":-1})", R"({"tabId":-2})", R"({"tabId":1.5})", R"({"tabId":18446744073709551615})" }) {
        auto value = JSON::Value::parseJSON(String::fromUTF8(text));
        check(!parseWebExtensionNavigationTab(value.get()), text);
    }
    for (auto text : { "null", "[]", "{}", R"({"frameId":true})", R"({"frameId":"1"})", R"({"frameId":-1})", R"({"frameId":-2})", R"({"frameId":1.5})", R"({"frameId":18446744073709551615})" }) {
        auto value = JSON::Value::parseJSON(String::fromUTF8(text));
        check(!parseWebExtensionNavigationFrame(value.get()), text);
    }
    auto identifiers = JSON::Value::parseJSON(R"({"tabId":12,"frameId":0,"ignored":true})"_s);
    auto tab = parseWebExtensionNavigationTab(identifiers.get());
    auto mainFrame = parseWebExtensionNavigationFrame(identifiers.get());
    check(tab && toWebAPI(*tab) == 12, "tab numeric identity");
    check(mainFrame && isMainFrame(*mainFrame), "zero maps to main frame sentinel");
    identifiers->asObject()->setDouble("frameId"_s, 27);
    auto childFrame = parseWebExtensionNavigationFrame(identifiers.get());
    check(childFrame && toWebAPI(*childFrame) == 27, "child frame numeric identity");
    identifiers->asObject()->setDouble("tabId"_s, std::numeric_limits<double>::quiet_NaN());
    check(!parseWebExtensionNavigationTab(identifiers.get()), "NaN tab ID rejected");
    identifiers->asObject()->setDouble("frameId"_s, std::numeric_limits<double>::infinity());
    check(!parseWebExtensionNavigationFrame(identifiers.get()), "infinite frame ID rejected");
    WebExtensionFrameParameters frame {
        .errorOccurred = false,
        .url = URL { "https://example.org/a?quoted=%22#fragment"_s },
        .parentFrameIdentifier = WebExtensionFrameConstants::NoneIdentifier,
        .frameIdentifier = WebExtensionFrameConstants::MainFrameIdentifier,
        .documentIdentifier = WTF::UUID::parse("d7e7b1fb-a7b1-416e-8d87-374c15194fd4"_s)
    };
    auto query = webExtensionNavigationFrameJSON(frame);
    check(query->getBoolean("errorOccurred"_s) == false && query->getDouble("frameId"_s) == 0 && query->getDouble("parentFrameId"_s) == -1, "frame query booleans and sentinel IDs");
    check(query->getString("url"_s) == frame.url->string() && query->getString("documentId"_s) == "d7e7b1fb-a7b1-416e-8d87-374c15194fd4"_s, "frame query URL and document ID");
    check(query->size() == 5, "frame query property set");
    auto time = WallTime::fromRawSeconds(1700000000.1239);
    auto event = webExtensionNavigationEventJSON(*tab, frame, time);
    check(event && event->getDouble("timeStamp"_s) == 1700000000123.0, "event timestamp uses floored milliseconds");
    check(event && event->getDouble("tabId"_s) == 12 && event->getDouble("frameId"_s) == 0 && event->getDouble("parentFrameId"_s) == -1, "navigation event identifiers");
    check(event && event->getString("url"_s) == frame.url->string() && event->size() == 6, "navigation event preserves fragment and excludes query-only fields");
    check(event && JSON::Value::parseJSON(event->toJSONString()), "navigation event JSON round trip");
    frame.frameIdentifier = std::nullopt;
    frame.documentIdentifier = { };
    frame.url = std::nullopt;
    frame.errorOccurred = true;
    query = webExtensionNavigationFrameJSON(frame);
    check(query->getString("url"_s).isEmpty() && query->getBoolean("errorOccurred"_s) == true, "redacted URL and error state");
    check(query->size() == 3 && !query->getValue("frameId"_s) && !query->getValue("documentId"_s), "absent optional query fields omitted");
    check(!webExtensionNavigationEventJSON(*tab, frame, time), "event without frame and URL rejected");
    frame.url = URL { "https://example.org"_s };
    check(!webExtensionNavigationEventJSON(*tab, frame, time), "event without frame rejected");
    frame.frameIdentifier = WebExtensionFrameIdentifier { 27 };
    frame.url = std::nullopt;
    check(!webExtensionNavigationEventJSON(*tab, frame, time), "event without URL rejected");
    printf("%u passed, %u failed\n", passed, failed);
    return failed ? 1 : 0;
}
