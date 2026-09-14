#include "config.h"
#include "WebExtensionWebRequestFilter.h"
#include "WebExtensionWebRequestBody.h"
#include "JSWebExtensionString.h"
#include <JavaScriptCore/InitializeThreading.h>
#include <Application.h>
#include <JavaScriptCore/JavaScript.h>
#include <cstdio>
#include <wtf/MainThread.h>

// API::Object uses this entry point. The isolated helper harness initializes
// JSC/WTF but deliberately does not initialize a full WebCore/WebKit runtime.
namespace WebKit {
void InitializeWebKit2()
{
    JSC::initialize();
    WTF::initializeMainThread();
}
}

using namespace WebKit;
static unsigned checks, failures;
static void check(bool passed, const char* message)
{
    ++checks;
    if (!passed) { ++failures; std::printf("FAIL: %s\n", message); }
}
static ResourceLoadInfo resource(ResourceLoadInfo::Type type = ResourceLoadInfo::Type::Document, bool child = false)
{
    return { NetworkResourceLoadIdentifier { 9007199254740993ULL }, WebCore::FrameIdentifier { 31 },
        child ? std::optional { WebCore::FrameIdentifier { 21 } } : std::nullopt, { },
        URL { "https://example.test/a/page?name=one#anchor"_s }, "GET"_s,
        WallTime::fromRawSeconds(12.345678), false, type };
}
static auto parse(const String& input)
{
    RefPtr value = JSON::Value::parseJSON(input);
    RELEASE_ASSERT(value);
    return WebExtensionWebRequestFilter::parse(*value);
}
static bool matches(const String& filter, const ResourceLoadInfo& resource, uint64_t tab = 7, uint64_t window = 9)
{
    auto parsed = parse(filter);
    check(!!parsed, "valid filter parses");
    return parsed && parsed->matches(resource, WebExtensionTabIdentifier { tab }, WebExtensionWindowIdentifier { window });
}
int main()
{
    BApplication application("application/x-vnd.Kunanyi-Summit-extension-web-request-tests");
    WTF::initializeMainThread();
    auto context = JSGlobalContextCreate(nullptr);
    RELEASE_ASSERT(context);
    auto main = resource();
    auto child = resource(ResourceLoadInfo::Type::Document, true);
    check(webExtensionWebRequestResourceType(main) == "main_frame"_s, "top-level document has main-frame type");
    check(webExtensionWebRequestResourceType(child) == "sub_frame"_s, "child document has subframe type");
    check(matches(R"({"urls":["https://example.test/*"]})"_s, main), "matching host and path receive a request");
    check(!matches(R"({"urls":["http://example.test/*"]})"_s, main), "scheme filter excludes different schemes");
    check(!matches(R"({"urls":["https://other.test/*"]})"_s, main), "host filter excludes different hosts");
    check(!matches(R"({"urls":["https://example.test/private/*"]})"_s, main), "path filter excludes different paths");
    check(matches(R"({"urls":["https://other.test/*","https://example.test/a/*"]})"_s, main), "URL patterns form a union");
    check(matches(R"({"urls":["*://*.test/*"]})"_s, main), "wildcard schemes and subdomains use actual match patterns");
    check(!matches(R"({"urls":[]})"_s, main), "empty URL union matches no requests");
    check(!matches(R"({"urls":["<all_urls>"],"types":[]})"_s, main), "empty resource-type union matches no requests");
    check(matches(R"({"urls":["<all_urls>"],"types":["main_frame"]})"_s, main), "main-frame filter matches top-level document");
    check(!matches(R"({"urls":["<all_urls>"],"types":["main_frame"]})"_s, child), "main-frame filter excludes child document");
    check(matches(R"({"urls":["<all_urls>"],"types":["sub_frame"]})"_s, child), "subframe filter matches child document");
    check(!matches(R"({"urls":["<all_urls>"],"types":["sub_frame"]})"_s, main), "subframe filter excludes top-level document");
    check(matches(R"({"urls":["<all_urls>"],"tabId":7,"windowId":9})"_s, main), "both identifier filters match");
    check(!matches(R"({"urls":["<all_urls>"],"tabId":8,"windowId":9})"_s, main), "different tab is excluded");
    check(!matches(R"({"urls":["<all_urls>"],"tabId":7,"windowId":10})"_s, main), "different window is excluded");
    auto unassociated = parse(R"({"urls":["<all_urls>"],"tabId":-1,"windowId":-1})"_s);
    check(unassociated && unassociated->matches(main, WebExtensionTabConstants::NoneIdentifier, WebExtensionWindowConstants::NoneIdentifier), "none identifiers match unassociated requests");
    check(unassociated && !unassociated->matches(main, WebExtensionTabIdentifier { 7 }, WebExtensionWindowIdentifier { 9 }), "none identifier does not become a wildcard");
    for (auto input : { "null"_s, "[]"_s, "{}"_s, R"({"urls":null})"_s, R"({"urls":"<all_urls>"})"_s,
        R"({"urls":[""]})"_s, R"({"urls":[1]})"_s, R"({"urls":[null]})"_s, R"({"urls":["bad"]})"_s,
        R"({"urls":["https://*bad.test/*"]})"_s, R"({"urls":["<all_urls>"],"types":null})"_s,
        R"({"urls":["<all_urls>"],"types":[4]})"_s, R"({"urls":["<all_urls>"],"types":["Main_Frame"]})"_s,
        R"({"urls":["<all_urls>"],"tabId":true})"_s, R"({"urls":["<all_urls>"],"tabId":"7"})"_s,
        R"({"urls":["<all_urls>"],"tabId":0})"_s, R"({"urls":["<all_urls>"],"tabId":7.2})"_s,
        R"({"urls":["<all_urls>"],"tabId":-2})"_s, R"({"urls":["<all_urls>"],"tabId":18446744073709551616})"_s,
        R"({"urls":["<all_urls>"],"windowId":-2})"_s, R"({"urls":["<all_urls>"],"windowId":0})"_s,
        R"({"urls":["<all_urls>"],"windowId":null})"_s, R"({"urls":["<all_urls>"],"windowId":8.1})"_s,
        R"({"urls":["<all_urls>"],"incognito":true})"_s, R"({"urls":["<all_urls>"],"unknown":true})"_s })
        check(!parse(input), "malformed or unsupported filter is rejected without broadening matches");
    struct TypeCase { ResourceLoadInfo::Type type; ASCIILiteral name; };
    for (auto test : { TypeCase { ResourceLoadInfo::Type::ApplicationManifest, "web_manifest"_s },
        TypeCase { ResourceLoadInfo::Type::Beacon, "beacon"_s }, TypeCase { ResourceLoadInfo::Type::CSPReport, "csp_report"_s },
        TypeCase { ResourceLoadInfo::Type::Font, "font"_s }, TypeCase { ResourceLoadInfo::Type::Image, "image"_s },
        TypeCase { ResourceLoadInfo::Type::Media, "media"_s }, TypeCase { ResourceLoadInfo::Type::Object, "object"_s },
        TypeCase { ResourceLoadInfo::Type::Ping, "ping"_s }, TypeCase { ResourceLoadInfo::Type::Script, "script"_s },
        TypeCase { ResourceLoadInfo::Type::Stylesheet, "stylesheet"_s }, TypeCase { ResourceLoadInfo::Type::Fetch, "xmlhttprequest"_s },
        TypeCase { ResourceLoadInfo::Type::XMLHTTPRequest, "xmlhttprequest"_s }, TypeCase { ResourceLoadInfo::Type::XSLT, "xslt"_s },
        TypeCase { ResourceLoadInfo::Type::Other, "other"_s } }) {
        auto input = JSON::Object::create(); auto urls = JSON::Array::create(); urls->pushString("<all_urls>"_s);
        auto types = JSON::Array::create(); types->pushString(test.name);
        input->setArray("urls"_s, WTF::move(urls)); input->setArray("types"_s, WTF::move(types));
        auto filter = WebExtensionWebRequestFilter::parse(input);
        auto load = resource(test.type, true);
        auto details = webExtensionWebRequestDetails(load, WebExtensionTabIdentifier { 7 });
        check(details->getString("type"_s) == test.name, "payload uses the resource's concrete type");
        check(filter && filter->matches(load, WebExtensionTabIdentifier { 7 }, WebExtensionWindowIdentifier { 9 }), "filter agrees with resource payload type");
    }
    auto input = JSON::Value::parseJSON(R"({"urls":["https://example.test/*"],"types":["main_frame"],"tabId":7})"_s)->asObject();
    auto snapshot = WebExtensionWebRequestFilter::parse(*input);
    input->getArray("urls"_s)->setString(0, "https://other.test/*"_s);
    input->getArray("types"_s)->setString(0, "image"_s);
    input->setDouble("tabId"_s, 8);
    check(snapshot && snapshot->matches(main, WebExtensionTabIdentifier { 7 }, WebExtensionWindowIdentifier { 9 }), "registration owns its filter values after caller mutation");
    auto details = webExtensionWebRequestDetails(main, WebExtensionTabIdentifier { 7 });
    check(details->getString("requestId"_s) == "9007199254740993"_s, "request ID serialization retains integers beyond JS number precision");
    check(details->getDouble("timeStamp"_s) == 12345, "time stamp is integer milliseconds");
    check(details->getDouble("frameId"_s) == 0 && details->getDouble("parentFrameId"_s) == -1, "main frame sentinels use WebExtension values");
    check(details->getDouble("tabId"_s) == 7 && details->getString("method"_s) == "GET"_s, "tab identifier and method are preserved");
    check(details->getString("url"_s) == main.originalURL.string(), "resource URL is preserved including query and fragment");
    check(!details->getValue("documentId"_s), "absent document UUID is omitted");
    child.documentID = WTF::UUID::createVersion4();
    auto childDetails = webExtensionWebRequestDetails(child, WebExtensionTabConstants::NoneIdentifier);
    check(childDetails->getString("type"_s) == "sub_frame"_s, "child document payload has subframe type");
    check(childDetails->getDouble("frameId"_s) == 31 && childDetails->getDouble("parentFrameId"_s) == 21, "actual child and parent IDs are preserved");
    check(childDetails->getString("documentId"_s) == child.documentID->toString(), "actual document UUID is preserved");
    check(childDetails->getDouble("tabId"_s) == -1, "unassociated tab is serialized as none");
    child.frameID = std::nullopt;
    check(webExtensionWebRequestDetails(child, WebExtensionTabConstants::NoneIdentifier)->getDouble("frameId"_s) == -1, "missing frame metadata does not assert or fabricate an ID");
    auto setBody = [&](const String& name, std::span<const WebCore::FormDataElement> elements, const String& contentType) {
        JSValueRef exception = nullptr;
        auto body = webExtensionWebRequestBody(context, elements, contentType, exception);
        check(body && !exception, "upload-body conversion returns an object without exception");
        if (body)
            JSObjectSetProperty(context, JSContextGetGlobalObject(context), toJSString(name).get(), body, kJSPropertyAttributeNone, &exception);
        check(!exception, "body can be retained in the test context");
    };
    auto evaluate = [&](const String& script, const char* message) {
        JSValueRef exception = nullptr;
        auto value = JSEvaluateScript(context, toJSString(script).get(), nullptr, nullptr, 1, &exception);
        check(value && !exception && JSValueToBoolean(context, value), message);
    };
    auto bytes = [](const String& text) {
        auto encoded = text.utf8();
        return WebCore::FormDataElement { Vector<uint8_t> { encoded.span() } };
    };
    Vector<WebCore::FormDataElement> encoded { bytes("a=hel"_s), bytes("lo&a=again&space=one+two&u=%E2%82%AC&empty=&bare&n=%00&__proto__=owned"_s) };
    setBody("form"_s, encoded.span(), " Application/X-WWW-Form-Urlencoded; charset=UTF-8 "_s);
    evaluate("form.formData.a.length === 2 && form.formData.a[0] === 'hello' && form.formData.a[1] === 'again'"_s, "split chunks and repeated URL-encoded keys retain complete values");
    evaluate("form.formData.space[0] === 'one two' && form.formData.u[0].length === 1 && form.formData.u[0].charCodeAt(0) === 0x20ac"_s, "form decoding handles plus and percent-encoded UTF-8");
    evaluate("form.formData.empty[0] === '' && form.formData.bare[0] === '' && form.formData.n[0].charCodeAt(0) === 0"_s, "empty values and embedded null characters survive decoding");
    evaluate("Object.hasOwn(form.formData, '__proto__') && form.formData.__proto__[0] === 'owned'"_s, "form keys cannot change the object prototype");
    Vector<WebCore::FormDataElement> raw { WebCore::FormDataElement { Vector<uint8_t> { 0, 255, 127, 3 } },
        WebCore::FormDataElement { "/boot/home/example.txt"_s, 0, -1, std::nullopt },
        WebCore::FormDataElement { Vector<uint8_t> { } } };
    evaluate("Object.defineProperty(Object.prototype, 'bytes', {set() {throw Error('prototype setter');}, configurable: true}); true"_s, "prototype setter trap is installed");
    setBody("first"_s, raw.span(), "application/octet-stream"_s);
    setBody("second"_s, raw.span(), "application/octet-stream"_s);
    evaluate("first.raw.length === 3 && first.raw[0].bytes instanceof ArrayBuffer && first.raw[0].bytes.byteLength === 4"_s, "raw bytes use the actual ArrayBuffer API");
    evaluate("new Uint8Array(first.raw[0].bytes).join(',') === '0,255,127,3'"_s, "binary bytes retain all values");
    evaluate("first.raw[1].file === '/boot/home/example.txt' && !Object.hasOwn(first.raw[1], 'bytes')"_s, "upload file entry exposes its path without reading its contents");
    evaluate("first.raw[2].bytes instanceof ArrayBuffer && first.raw[2].bytes.byteLength === 0"_s, "empty raw chunks produce empty ArrayBuffers");
    evaluate("new Uint8Array(first.raw[0].bytes)[1] = 8; new Uint8Array(second.raw[0].bytes)[1] === 255"_s, "listeners receive independent byte buffers");
    check(std::get<Vector<uint8_t>>(raw[0].data)[1] == 255, "listener mutation cannot change upload data");
    for (unsigned i = 0; i < 4; ++i)
        JSGarbageCollect(context);
    evaluate("new Uint8Array(second.raw[0].bytes)[1] === 255"_s, "retained upload bytes survive garbage collection");
    evaluate("delete Object.prototype.bytes; true"_s, "prototype trap is removed");
    Vector<WebCore::FormDataElement> unresolved { bytes("prefix"_s), WebCore::FormDataElement { URL { "blob:https://example.test/id"_s } } };
    setBody("unsupported"_s, unresolved.span(), "application/octet-stream"_s);
    evaluate("typeof unsupported.error === 'string' && !Object.hasOwn(unsupported, 'raw') && !Object.hasOwn(unsupported, 'formData')"_s, "unresolved blob fails as a whole instead of exposing partial data");
    Vector<WebCore::FormDataElement> pending { WebCore::FormDataElement { WebCore::FormDataElement::PendingStreamData { WebCore::PendingStreamIdentifier { 42 } } } };
    setBody("stream"_s, pending.span(), "text/plain"_s);
    evaluate("typeof stream.error === 'string' && !Object.hasOwn(stream, 'raw')"_s, "pending stream reports unavailable content without partial bytes");
    JSValueRef exception = nullptr;
    check(!webExtensionWebRequestBody(context, { }, "text/plain"_s, exception) && !exception, "absent body has no payload");
    JSGlobalContextRelease(context);
    std::printf("%u native web request helper checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
