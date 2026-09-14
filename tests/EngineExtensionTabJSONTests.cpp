#include "config.h"
#include "WebExtensionTabQueryParser.h"
#include "WebExtensionTabParameters.h"
#include "JSWebExtensionTabParameters.h"
#include "JSWebExtensionString.h"
#include "Protected.h"
#include <cmath>
#include <Application.h>
#include <cstdio>
#include <limits>
#include <wtf/MainThread.h>
#include <wtf/URL.h>
#include <wtf/text/MakeString.h>

using namespace WebKit;
static unsigned checks, failures;
static void check(bool result, const char* description)
{
    ++checks;
    failures += !result;
    std::printf("%s %s\n", result ? "PASS" : "FAIL", description);
}

static auto query(const String& source)
{
    auto value = JSON::Value::parseJSON(source);
    return parseWebExtensionTabQuery(value.get());
}

static void checkQuery()
{
    check(!parseWebExtensionTabQuery(nullptr), "missing query info is rejected");
    for (auto invalid : { "null"_s, "[]"_s, "true"_s, "3"_s, "\"active\""_s })
        check(!query(invalid), "query info must be an object");
    auto empty = query("{}"_s);
    check(empty && !empty->active && !empty->windowIdentifier && !empty->currentWindow && !empty->urlPatterns,
        "an empty query has no implicit filters");
    for (auto key : { "active"_s, "audible"_s, "currentWindow"_s, "hidden"_s, "highlighted"_s,
            "lastFocusedWindow"_s, "muted"_s, "pinned"_s, "selected"_s }) {
        for (auto invalid : { "null"_s, "0"_s, "1"_s, "\"true\""_s, "[]"_s, "{}"_s })
            check(!query(makeString("{\""_s, key, "\":"_s, invalid, '}')), "query flags reject non-boolean values");
        check(query(makeString("{\""_s, key, "\":false}"_s)).has_value(), "an explicit false query flag is valid");
    }
    auto flags = query(R"({"active":false,"audible":true,"currentWindow":false,"hidden":true,"highlighted":false,"lastFocusedWindow":true,"muted":false,"pinned":true})"_s);
    check(flags && flags->active == false && flags->audible == true && flags->currentWindow == false
        && flags->hidden == true && flags->selected == false && flags->frontmostWindow == true
        && flags->muted == false && flags->pinned == true, "true and false flags survive parsing distinctly from omitted flags");
    for (auto source : { R"({"selected":true,"highlighted":false})"_s, R"({"highlighted":false,"selected":true})"_s }) {
        auto result = query(source);
        check(result && result->selected == false, "highlighted takes precedence independently of key order");
    }
    for (auto number : { "-1"_s, "1.5"_s, "9007199254740992"_s, "true"_s, "\"0\""_s, "null"_s })
        check(!query(makeString("{\"index\":"_s, number, '}')), "index must be a nonnegative safe integer");
    auto first = query(R"({"index":0})"_s);
    auto largest = query(R"({"index":9007199254740991})"_s);
    check(first && first->index == 0 && largest && largest->index == 9007199254740991ULL, "index zero and the largest safe integer are preserved");
    for (auto number : { "-1"_s, "-3"_s, "0"_s, "1.5"_s, "9007199254740992"_s, "true"_s, "null"_s, "\"1\""_s })
        check(!query(makeString("{\"windowId\":"_s, number, '}')), "invalid window identifiers are rejected");
    auto current = query(R"({"windowId":-2,"currentWindow":false})"_s);
    check(current && isCurrent(current->windowIdentifier) && current->currentWindow == false,
        "current-window identifier and explicit false filter remain separate constraints");
    auto window = query(R"({"windowId":17,"windowType":"normal"})"_s);
    check(window && window->windowIdentifier->toUInt64() == 17 && window->windowType->contains(WebExtensionWindow::TypeFilter::Normal),
        "normal window filters retain a concrete identifier");
    auto popup = query(R"({"windowType":"popup"})"_s);
    check(popup && popup->windowType->contains(WebExtensionWindow::TypeFilter::Popup), "popup is a distinct window type filter");
    for (auto source : { R"({"windowType":"unknown"})"_s, R"({"windowType":[]})"_s,
            R"({"status":"unknown"})"_s, R"({"status":true})"_s,
            R"({"title":null})"_s, R"({"title":[]})"_s, R"({"title":"x\u0000y"})"_s,
            R"({"url":null})"_s, R"({"url":{}})"_s, R"({"url":[1]})"_s,
            R"({"url":""})"_s, R"({"url":["https://example.org/*",null]})"_s,
            R"({"url":"https://example.org/\u0000*"})"_s,
            R"({"groupId":5})"_s, R"({"cookieStoreId":"default"})"_s })
        check(!query(source), "malformed and unsupported query filters are rejected");
    auto loading = query(R"({"status":"loading"})"_s);
    auto complete = query(R"({"status":"complete"})"_s);
    check(loading && loading->loading == true && complete && complete->loading == false, "loading and complete are distinct status filters");
    auto source = JSON::Value::parseJSON(R"({"url":["https://example.org/*","file:///*"],"title":"*title*"})"_s);
    auto before = source->toJSONString();
    auto patterns = parseWebExtensionTabQuery(source.get());
    check(patterns && patterns->urlPatterns->size() == 2 && patterns->urlPatterns->at(1) == "file:///*"_s
        && patterns->titlePattern == "*title*"_s, "URL and title filters preserve their text for privileged validation");
    check(source->toJSONString() == before, "query parsing does not mutate the input");
    auto noURLs = query(R"({"url":[]})"_s);
    check(noURLs && noURLs->urlPatterns && noURLs->urlPatterns->isEmpty(), "an explicit empty URL set remains distinguishable from no URL filter");
    for (auto value : { std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() }) {
        auto object = JSON::Object::create();
        object->setDouble("index"_s, value);
        check(!parseWebExtensionTabQuery(object.ptr()), "non-finite numeric filters are rejected before IPC");
    }
}

static void checkTabJSON()
{
    JSRetainPtr<JSGlobalContextRef> retained(Adopt, JSGlobalContextCreate(nullptr));
    auto context = retained.get();
    auto convert = [&](const WebExtensionTabParameters& tab) {
        Protected<JSValueRef> value(context, toWebAPI(context, tab));
        JSRetainPtr<JSStringRef> text(Adopt, JSValueCreateJSONString(context, value.get(), 0, nullptr));
        auto json = text ? JSON::Value::parseJSON(toString(text.get())) : nullptr;
        RefPtr object = json ? json->asObject() : nullptr;
        check(!!object, "the actual JavaScriptCore tab converter produces a serializable object");
        return object;
    };
    check(convert({ })->size() == 0, "missing tab properties do not produce invented defaults");
    WebExtensionTabParameters tab;
    tab.identifier = WebExtensionTabIdentifier { 17 };
    tab.windowIdentifier = WebExtensionWindowIdentifier { 23 };
    tab.parentTabIdentifier = WebExtensionTabIdentifier { 4 };
    tab.index = 0;
    tab.url = URL { "https://example.org/path?query=%25#fragment"_s };
    tab.title = String::fromUTF8("雪 \"quoted\"\n title");
    tab.active = false;
    tab.selected = false;
    tab.pinned = false;
    tab.audible = true;
    tab.muted = false;
    tab.loading = false;
    tab.privateBrowsing = true;
    tab.readerModeAvailable = false;
    tab.showingReaderMode = false;
    auto object = convert(tab);
    check(object->getDouble("id"_s) == 17 && object->getDouble("windowId"_s) == 23 && object->getDouble("openerTabId"_s) == 4,
        "tab, window and parent identifiers are numeric");
    check(object->getDouble("index"_s) == 0, "first-tab index is not omitted as a false value");
    check(object->getBoolean("active"_s) == false && object->getBoolean("selected"_s) == false
        && object->getBoolean("highlighted"_s) == false && object->getBoolean("pinned"_s) == false,
        "explicit false booleans survive serialization");
    check(object->getBoolean("audible"_s) == true && object->getBoolean("incognito"_s) == true, "audio and private metadata retain boolean types");
    auto muted = object->getObject("mutedInfo"_s);
    check(muted && muted->size() == 1 && muted->getBoolean("muted"_s) == false, "mutedInfo uses the nested API shape");
    check(object->getString("status"_s) == "complete"_s, "finished loading uses the complete status string");
    auto roundTrip = JSON::Value::parseJSON(object->toJSONString())->asObject();
    check(roundTrip->getString("url"_s) == tab.url->string() && roundTrip->getString("title"_s) == *tab.title,
        "URL delimiters, Unicode, quotes and newlines survive JSON encoding");
    check(!roundTrip->getValue("width"_s) && !roundTrip->getValue("height"_s), "unavailable native dimensions are omitted");
    WebExtensionTabParameters changed;
    changed.loading = true;
    auto changes = convert(changed);
    check(changes->size() == 1 && changes->getString("status"_s) == "loading"_s, "a status-only change exposes only the changed property");
    changed.title = nullString();
    changed.url = URL { };
    changes = convert(changed);
    check(changes->getString("title"_s) == emptyString() && changes->getString("url"_s) == emptyString(),
        "permission-redacted metadata stays empty instead of recovering earlier values");
    check(object->getString("title"_s) == *tab.title, "serializing a later change does not mutate an earlier payload");
    WebExtensionTabParameters none;
    none.identifier = WebExtensionTabConstants::NoneIdentifier;
    none.windowIdentifier = WebExtensionWindowConstants::NoneIdentifier;
    none.index = notFound;
    auto sentinel = convert(none);
    check(sentinel->getDouble("id"_s) == -1 && sentinel->getDouble("windowId"_s) == -1, "internal none sentinels use public negative identifiers");
    check(sentinel->getValue("index"_s)->type() == JSON::Value::Type::Null, "an unknown native index serializes to null instead of a huge integer");
    Protected<JSValueRef> nativeNone(context, toWebAPI(context, none));
    auto noneObject = JSValueToObject(context, nativeNone.get(), nullptr);
    auto nativeIndex = JSObjectGetProperty(context, noneObject, toJSString("index"_s).get(), nullptr);
    check(JSValueIsNumber(context, nativeIndex) && std::isnan(JSValueToNumber(context, nativeIndex, nullptr)),
        "the existing JavaScript converter retains NaN for an unknown index");
    Protected<JSValueRef> first(context, toWebAPI(context, tab));
    auto firstObject = JSValueToObject(context, first.get(), nullptr);
    JSObjectSetProperty(context, firstObject, toJSString("title"_s).get(), JSValueMakeString(context, toJSString("mutated"_s).get()), kJSPropertyAttributeNone, nullptr);
    JSGarbageCollect(context);
    auto independent = convert(tab);
    check(independent->getString("title"_s) == *tab.title && independent->getDouble("id"_s) == 17,
        "a mutation and garbage collection do not alter the next converted tab object");
}

class TabJSONApplication final : public BApplication {
public:
    bool didRun { false };
    TabJSONApplication() : BApplication("application/x-vnd.Kunanyi-Summit-tab-json-tests") { }
    void ReadyToRun() override
    {
        WTF::initializeMainThread();
        checkQuery();
        checkTabJSON();
        didRun = true;
        PostMessage(B_QUIT_REQUESTED);
    }
};

int main()
{
    TabJSONApplication application;
    application.Run();
    check(application.didRun, "the native suite ran from BApplication::ReadyToRun");
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
