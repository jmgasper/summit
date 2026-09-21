/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionWebRequestDecisionHaiku.h"
#include <Application.h>
#include <cstdio>
#include <string>
#include <wtf/MainThread.h>
#include <wtf/text/MakeString.h>

using namespace WebKit;
unsigned checks, failures;
static void check(bool passed, const char* label)
{
    ++checks;
    if (!passed) {
        ++failures;
        printf("FAIL %s\n", label);
    }
}
static auto parse(const String& input)
{
    return parseWebExtensionWebRequestDecisionHaiku(input);
}
int main()
{
    BApplication application("application/x-vnd.Kunanyi-Summit-WebRequestDecisionTests");
    WTF::initializeMainThread();
    for (auto input : { "null"_s, "{}"_s, R"({"futureOption":"ignored"})"_s }) {
        auto result = parse(input);
        check(result && !result->cancel && !result->redirectURL && !result->requestHeaders
            && !result->responseHeaders && !result->authCredentials, "no-action replies do not synthesize network mutations");
    }
    check(parse(R"({"cancel":true})"_s)->cancel, "cancellation survives decoding");
    check(!parse(R"({"cancel":false})"_s)->cancel, "explicit allow does not cancel");
    for (auto input : { ""_s, "["_s, "[]"_s, "true"_s, "0"_s, R"("text")"_s,
        R"({"cancel":1})"_s, R"({"cancel":null})"_s, R"({"cancel":"true"})"_s,
        R"({"cancel":true,"redirectUrl":"https://example.test/"})"_s,
        R"({"cancel":false,"unknown":0})"_s, R"({"redirectUrl":1})"_s,
        R"({"redirectUrl":null})"_s, R"({"redirectUrl":"relative/path"})"_s,
        R"({"redirectUrl":""})"_s, R"({"requestHeaders":null})"_s,
        R"({"requestHeaders":{}})"_s, R"({"requestHeaders":[false]})"_s,
        R"({"requestHeaders":[],"responseHeaders":[]})"_s,
        R"({"requestHeaders":[{}]})"_s, R"({"requestHeaders":[{"name":"X-Test"}]})"_s,
        R"({"requestHeaders":[{"name":"X-Test","value":4}]})"_s,
        R"({"requestHeaders":[{"name":"X-Test","value":"ok","binaryValue":[65]}]})"_s,
        R"({"authCredentials":null})"_s, R"({"authCredentials":{}})"_s,
        R"({"authCredentials":{"username":"user"}})"_s,
        R"({"authCredentials":{"username":1,"password":"secret"}})"_s })
        check(!parse(input), "invalid blocking response is rejected as a whole");

    for (auto url : { "https://example.test/target?q=1#f"_s, "data:text/plain,fixture"_s,
        "webkit-extension://owned-extension/resource.js"_s }) {
        Ref value = JSON::Object::create(); value->setString("redirectUrl"_s, url);
        auto result = parse(value->toJSONString());
        check(result && result->redirectURL && result->redirectURL->string() == url,
            "absolute redirect URL is preserved for later destination authorization");
    }
    auto absent = parse("{}"_s);
    auto empty = parse(R"({"requestHeaders":[]})"_s);
    check(absent && empty && !absent->requestHeaders && empty->requestHeaders && empty->requestHeaders->isEmpty(),
        "absent headers differ from an explicit empty header replacement");
    auto repeated = parse(R"({"responseHeaders":[{"name":"Set-Cookie","value":"one=1"},{"name":"set-cookie","value":"two=2"}]})"_s);
    check(repeated && repeated->responseHeaders && repeated->responseHeaders->size() == 2
        && repeated->responseHeaders->at(0).name == "Set-Cookie"_s
        && repeated->responseHeaders->at(1).name == "set-cookie"_s,
        "response duplicates, order and header-name spelling survive decoding");
    auto binary = parse(R"({"responseHeaders":[{"name":"X-Bytes","binaryValue":[9,32,65,127,128,255]}]})"_s);
    Vector<uint8_t> expected { 9, 32, 65, 127, 128, 255 };
    check(binary && binary->responseHeaders && binary->responseHeaders->at(0).value == expected,
        "binary header bytes are preserved without UTF-8 replacement");
    auto unicode = parse(R"({"requestHeaders":[{"name":"X-UTF8","value":"\u00e9"}]})"_s);
    Vector<uint8_t> utf8 { 0xc3, 0xa9 };
    check(unicode && unicode->requestHeaders && unicode->requestHeaders->at(0).value == utf8,
        "Unicode text header values are converted to their UTF-8 bytes");
    auto emptyValue = parse(R"({"requestHeaders":[{"name":"X-Empty","value":""}]})"_s);
    check(emptyValue && emptyValue->requestHeaders->at(0).value.isEmpty(), "empty header values remain valid");

    for (auto name : { ""_s, "Bad Name"_s, "Bad:Name"_s, "Bad\tName"_s, "Bad\nName"_s, "Bad\rName"_s,
        "Bad(Name)"_s, "Bad[Name]"_s, "Bad/Name"_s }) {
        Ref header = JSON::Object::create(); header->setString("name"_s, name); header->setString("value"_s, "ok"_s);
        Ref headers = JSON::Array::create(); headers->pushObject(WTF::move(header));
        Ref reply = JSON::Object::create(); reply->setArray("requestHeaders"_s, WTF::move(headers));
        check(!parse(reply->toJSONString()), "invalid HTTP token cannot become a header name");
    }
    for (auto bad : { R"({"name":"X-Test","value":"ok\r\nInjected: yes"})"_s,
        R"({"name":"X-Test","value":"ok\u0000bad"})"_s,
        R"({"name":"X-Test","binaryValue":[0]})"_s, R"({"name":"X-Test","binaryValue":[10]})"_s,
        R"({"name":"X-Test","binaryValue":[13]})"_s, R"({"name":"X-Test","binaryValue":[-1]})"_s,
        R"({"name":"X-Test","binaryValue":[256]})"_s, R"({"name":"X-Test","binaryValue":[1.5]})"_s,
        R"({"name":"X-Test","binaryValue":[true]})"_s, R"({"name":"X-Test","binaryValue":["65"]})"_s })
        check(!parse(makeString("{\"responseHeaders\":["_s, bad, "]}"_s)), "header injection and malformed byte values are rejected");
    auto credentials = parse(R"({"authCredentials":{"username":"","password":""}})"_s);
    check(credentials && credentials->authCredentials && credentials->authCredentials->username.isEmpty()
        && credentials->authCredentials->password.isEmpty(), "empty authentication credentials are distinct from absence");
    Ref tooMany = JSON::Array::create();
    for (unsigned i = 0; i < 1025; ++i) {
        Ref header = JSON::Object::create(); header->setString("name"_s, "X"_s); header->setString("value"_s, "v"_s);
        tooMany->pushObject(WTF::move(header));
    }
    Ref reply = JSON::Object::create(); reply->setArray("requestHeaders"_s, WTF::move(tooMany));
    check(!parse(reply->toJSONString()), "header-count exhaustion is rejected before network mutation");
    auto oversized = makeString("{\"futureOption\":\""_s, String::fromUTF8(std::string(1024 * 1024, 'x').c_str()), "\"}"_s);
    check(!parse(oversized), "oversized replies are rejected before JSON decoding");
    printf("WEB_REQUEST_DECISION_RESULT %s checks=%u failures=%u\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
