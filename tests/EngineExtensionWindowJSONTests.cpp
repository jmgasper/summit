#include "config.h"
#include "WebExtensionWindowQueryParser.h"
#include "WebExtensionWindowParameters.h"
#include "WebExtensionTabParameters.h"
#include "JSWebExtensionWindowParameters.h"
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

static auto query(const String& source, WebExtensionWindowQueryKind kind = WebExtensionWindowQueryKind::Get)
{
    auto value = JSON::Value::parseJSON(source);
    return parseWebExtensionWindowQuery(value.get(), kind);
}

static void checkQuery()
{
    for (auto kind : { WebExtensionWindowQueryKind::Get, WebExtensionWindowQueryKind::Event }) {
        auto absent = parseWebExtensionWindowQuery(nullptr, kind);
        check(absent && absent->populate == WebExtensionWindow::PopulateTabs::No
            && absent->types == allWebExtensionWindowTypeFilters(), "omitted options include both supported types without populating tabs");
        auto empty = query("{}"_s, kind);
        check(empty && empty->types == allWebExtensionWindowTypeFilters(), "empty options retain default window types");
        for (auto invalid : { "null"_s, "[]"_s, "true"_s, "3"_s, "\"options\""_s })
            check(!query(invalid, kind), "window options reject non-objects");
        for (auto invalid : { R"({"windowTypes":null})"_s, R"({"windowTypes":[]})"_s,
                R"({"windowTypes":"normal"})"_s, R"({"windowTypes":{}})"_s,
                R"({"windowTypes":[1]})"_s, R"({"windowTypes":[true]})"_s,
                R"({"windowTypes":["normal",null]})"_s, R"({"windowTypes":["normal","unknown"]})"_s,
                R"({"windowTypes":["Normal"]})"_s, R"({"windowTypes":["normal\u0000"]})"_s,
                R"({"windowTypes":["panel"]})"_s, R"({"windowTypes":[["popup"]]})"_s,
                R"({"unknown":true})"_s })
            check(!query(invalid, kind), "invalid window type arrays and unsupported fields are rejected");
        auto normal = query(R"({"windowTypes":["normal"]})"_s, kind);
        auto popup = query(R"({"windowTypes":["popup"]})"_s, kind);
        auto both = query(R"({"windowTypes":["popup","normal","popup"]})"_s, kind);
        check(normal && normal->types.contains(WebExtensionWindowTypeFilter::Normal)
            && !normal->types.contains(WebExtensionWindowTypeFilter::Popup), "normal-only filter excludes popup windows");
        check(popup && popup->types.contains(WebExtensionWindowTypeFilter::Popup)
            && !popup->types.contains(WebExtensionWindowTypeFilter::Normal), "popup-only filter excludes normal windows");
        check(both && both->types == allWebExtensionWindowTypeFilters(), "duplicates and order do not change the type set");
    }
    for (auto invalid : { "null"_s, "0"_s, "1"_s, "\"true\""_s, "[]"_s, "{}"_s })
        check(!query(makeString("{\"populate\":"_s, invalid, '}')), "populate requires a boolean");
    auto yes = query(R"({"populate":true,"windowTypes":["normal"]})"_s);
    auto no = query(R"({"populate":false})"_s);
    check(yes && yes->populate == WebExtensionWindow::PopulateTabs::Yes
        && yes->types.contains(WebExtensionWindowTypeFilter::Normal), "populate and type filtering are independent");
    check(no && no->populate == WebExtensionWindow::PopulateTabs::No, "explicit false does not populate tabs");
    for (auto source : { R"({"populate":false})"_s, R"({"populate":true,"windowTypes":["normal"]})"_s })
        check(!query(source, WebExtensionWindowQueryKind::Event), "event filters reject the read-only populate option");
    auto original = JSON::Value::parseJSON(R"({"populate":true,"windowTypes":["popup","normal"]})"_s);
    auto before = original->toJSONString();
    check(parseWebExtensionWindowQuery(original.get(), WebExtensionWindowQueryKind::Get).has_value()
        && original->toJSONString() == before, "parsing does not mutate the caller's options");
}

static void checkWindowJSON()
{
    JSRetainPtr<JSGlobalContextRef> retained(Adopt, JSGlobalContextCreate(nullptr));
    auto context = retained.get();
    auto convert = [&](const WebExtensionWindowParameters& window) {
        Protected<JSValueRef> value(context, toWebAPI(context, window));
        JSRetainPtr<JSStringRef> text(Adopt, JSValueCreateJSONString(context, value.get(), 0, nullptr));
        auto json = text ? JSON::Value::parseJSON(toString(text.get())) : nullptr;
        RefPtr object = json ? json->asObject() : nullptr;
        check(!!object, "the actual JavaScriptCore window converter produces a serializable object");
        return object;
    };
    WebExtensionWindowParameters window;
    window.identifier = WebExtensionWindowIdentifier { 23 };
    window.type = WebExtensionWindow::Type::Normal;
    window.focused = false;
    window.privateBrowsing = false;
    auto basic = convert(window);
    check(basic->getDouble("id"_s) == 23 && basic->getString("type"_s) == "normal"_s, "window identity and type use public representations");
    check(basic->getBoolean("focused"_s) == false && basic->getBoolean("incognito"_s) == false
        && basic->getBoolean("alwaysOnTop"_s) == false, "explicit false window flags remain booleans");
    check(!basic->getValue("tabs"_s), "unpopulated windows omit tabs");
    for (auto key : { "state"_s, "left"_s, "top"_s, "width"_s, "height"_s })
        check(!basic->getValue(key), "unavailable native state and geometry are omitted");
    for (auto state : { WebExtensionWindow::State::Normal, WebExtensionWindow::State::Minimized,
            WebExtensionWindow::State::Maximized, WebExtensionWindow::State::Fullscreen }) {
        window.state = state;
        auto name = state == WebExtensionWindow::State::Normal ? "normal"_s
            : state == WebExtensionWindow::State::Minimized ? "minimized"_s
            : state == WebExtensionWindow::State::Maximized ? "maximized"_s : "fullscreen"_s;
        check(convert(window)->getString("state"_s) == name, "available window states map to their public names");
    }
    window.state = std::nullopt;
    window.type = WebExtensionWindow::Type::Popup;
    window.focused = true;
    window.privateBrowsing = true;
    auto popup = convert(window);
    check(popup->getString("type"_s) == "popup"_s && popup->getBoolean("focused"_s) == true
        && popup->getBoolean("incognito"_s) == true, "popup and true booleans survive conversion");
    window.tabs = Vector<WebExtensionTabParameters> { };
    auto empty = convert(window)->getArray("tabs"_s);
    check(empty && !empty->length(), "an explicitly populated empty window has an empty tabs array");
    WebExtensionTabParameters visible;
    visible.identifier = WebExtensionTabIdentifier { 17 };
    visible.windowIdentifier = window.identifier;
    visible.title = String::fromUTF8("雪 \"quoted\"\n title");
    visible.url = URL { "https://example.org/path?query=%25#fragment"_s };
    visible.selected = false;
    visible.muted = false;
    WebExtensionTabParameters redacted;
    redacted.identifier = WebExtensionTabIdentifier { 18 };
    redacted.url = URL { };
    redacted.title = nullString();
    window.tabs = Vector { visible, redacted };
    auto populated = convert(window);
    auto tabs = populated->getArray("tabs"_s);
    check(tabs && tabs->length() == 2, "populated windows preserve tab ordering and count");
    auto first = tabs->get(0)->asObject();
    auto second = tabs->get(1)->asObject();
    check(first->getDouble("id"_s) == 17 && second->getDouble("id"_s) == 18, "nested tab identifiers are numeric and ordered");
    check(first->getString("title"_s) == *visible.title && first->getString("url"_s) == visible.url->string(),
        "nested tab conversion preserves Unicode and URL delimiters");
    check(first->getBoolean("highlighted"_s) == false && first->getObject("mutedInfo"_s)->getBoolean("muted"_s) == false,
        "populated windows reuse the tab converter's nested API shape");
    check(second->getString("title"_s) == emptyString() && second->getString("url"_s) == emptyString(),
        "populated windows retain already-redacted tab metadata");
    Protected<JSValueRef> old(context, toWebAPI(context, window));
    auto object = JSValueToObject(context, old.get(), nullptr);
    JSObjectSetProperty(context, JSContextGetGlobalObject(context), toJSString("windowPayload"_s).get(), old.get(), kJSPropertyAttributeNone, nullptr);
    JSValueRef exception = nullptr;
    JSEvaluateScript(context, toJSString("windowPayload.tabs[0].title = 'mutated'; windowPayload.tabs.push({id:99}); windowPayload.focused = false;"_s).get(), nullptr, nullptr, 1, &exception);
    check(!exception, "JavaScript can mutate the returned nested window object");
    JSGarbageCollect(context);
    auto next = convert(window);
    auto nextTabs = next->getArray("tabs"_s);
    check(next->getBoolean("focused"_s) == true && nextTabs->length() == 2
        && nextTabs->get(0)->asObject()->getString("title"_s) == *visible.title,
        "mutation and garbage collection do not affect the next converted window payload");
    JSObjectDeleteProperty(context, JSContextGetGlobalObject(context), toJSString("windowPayload"_s).get(), nullptr);
    JSGarbageCollect(context);
    check(JSValueIsObject(context, JSObjectGetProperty(context, object, toJSString("tabs"_s).get(), nullptr)),
        "the protected window payload retains its nested array after collection");
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

class WindowJSONApplication final : public BApplication {
public:
    bool didRun { false };
    WindowJSONApplication() : BApplication("application/x-vnd.Kunanyi-Summit-window-json-tests") { }
    void ReadyToRun() override
    {
        WTF::initializeMainThread();
        checkQuery();
        checkTabJSON();
        checkWindowJSON();
        didRun = true;
        PostMessage(B_QUIT_REQUESTED);
    }
};

int main()
{
    WindowJSONApplication application;
    application.Run();
    check(application.didRun, "the native suite ran from BApplication::ReadyToRun");
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
