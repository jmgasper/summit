#include "config.h"
#include "WebExtensionActionDetails.h"
#include "JSWebExtensionActionDetails.h"
#include "JSWebExtensionString.h"
#include "JSWebExtensionWrapper.h"
#include "Protected.h"
#include <Application.h>
#include <cmath>
#include <cstdio>
#include <limits>
#include <wtf/MainThread.h>
#include <wtf/text/MakeString.h>

using namespace WebKit;
static unsigned checks, failures;
static void check(bool result, const char* description)
{
    ++checks;
    failures += !result;
    std::printf("%s %s\n", result ? "PASS" : "FAIL", description);
}

static auto parse(const String& source, WebExtensionActionDetailsKind kind = WebExtensionActionDetailsKind::Get)
{
    auto value = JSON::Value::parseJSON(source);
    return parseWebExtensionActionDetails(value.get(), kind);
}

static void checkDetails()
{
    auto absent = parseWebExtensionActionDetails(nullptr, WebExtensionActionDetailsKind::Get);
    check(absent && !absent->window && !absent->tab, "omitted get details use the default action");
    for (auto value : { "null"_s, "{}"_s }) {
        auto result = parse(value);
        check(result && !result->window && !result->tab, "null and empty get details use the default action");
    }
    for (auto value : { "[]"_s, "true"_s, "4"_s, "\"text\""_s })
        check(!parse(value), "get details reject non-objects");
    for (auto key : { "tabId"_s, "windowId"_s }) {
        for (auto invalid : { "null"_s, "true"_s, "\"7\""_s, "[]"_s, "{}"_s, "0"_s, "-1"_s, "-3"_s, "1.5"_s, "9007199254740992"_s })
            check(!parse(makeString("{\""_s, key, "\":"_s, invalid, '}')), "action targets reject malformed and unsafe identifiers");
        auto largest = parse(makeString("{\""_s, key, "\":9007199254740991}"_s));
        check(largest && (key == "tabId"_s ? largest->tab->toUInt64() : largest->window->toUInt64()) == 9007199254740991ULL,
            "largest safe integer is preserved before privileged lookup");
    }
    check(!parse(R"({"tabId":-2})"_s), "current-window sentinel is not a tab identifier");
    auto current = parse(R"({"windowId":-2})"_s);
    check(current && isCurrent(current->window), "current-window target remains a distinct sentinel");
    check(!parse(R"({"tabId":7,"windowId":9})"_s) && !parse(R"({"windowId":9,"tabId":7})"_s),
        "conflicting scopes are rejected independently of property order");
    for (auto value : { R"({"unknown":true})"_s, R"({"title":"label"})"_s, R"({"windowID":7})"_s })
        check(!parse(value), "get details reject unsupported fields and misspelled scope keys");
    for (auto kind : { WebExtensionActionDetailsKind::Title, WebExtensionActionDetailsKind::BadgeText, WebExtensionActionDetailsKind::Popup }) {
        auto key = kind == WebExtensionActionDetailsKind::Title ? "title"_s : kind == WebExtensionActionDetailsKind::BadgeText ? "text"_s : "popup"_s;
        check(!parseWebExtensionActionDetails(nullptr, kind) && !parse("null"_s, kind) && !parse("{}"_s, kind),
            "set operations require an object and the value field");
        for (auto invalid : { "true"_s, "4"_s, "[]"_s, "{}"_s, "\"x\\u0000y\""_s })
            check(!parse(makeString("{\""_s, key, "\":"_s, invalid, '}'), kind), "set text rejects non-strings and embedded null characters");
        auto reset = parse(makeString("{\""_s, key, "\":null,\"tabId\":7}"_s), kind);
        auto empty = parse(makeString("{\""_s, key, "\":\"\",\"windowId\":9}"_s), kind);
        check(reset && reset->value.isNull() && reset->tab->toUInt64() == 7, "null text resets scoped inheritance");
        check(empty && !empty->value.isNull() && empty->value.isEmpty() && empty->window->toUInt64() == 9,
            "empty text remains an explicit scoped override");
        auto text = parse(makeString("{\""_s, key, "\":\"hello \\\"world\\\"\\n\\u96ea\"}"_s), kind);
        check(text && text->value == String::fromUTF8("hello \"world\"\n雪"), "action text preserves Unicode, quotes and line breaks");
        check(!parse(makeString("{\""_s, key, "\":null,\"unknown\":true}"_s), kind), "set operations reject unsupported extra fields");
    }
    auto value = JSON::Value::parseJSON(R"({"title":null,"windowId":9})"_s);
    auto original = value->toJSONString();
    check(parseWebExtensionActionDetails(value.get(), WebExtensionActionDetailsKind::Title).has_value()
        && value->toJSONString() == original, "parsing does not mutate the caller's action details");
    auto omittedTab = parseWebExtensionActionTab(nullptr);
    check(omittedTab && !*omittedTab, "omitted scalar target selects the default action");
    for (double value : { std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity() }) {
        auto number = JSON::Value::create(value);
        check(!parseWebExtensionActionTab(number.ptr()), "non-finite scalar identifiers cannot alter the default action");
    }
}

static void checkJavaScriptArguments()
{
    JSRetainPtr<JSGlobalContextRef> retained(Adopt, JSGlobalContextCreate(nullptr));
    auto context = retained.get();
    auto evaluate = [&](const String& source) {
        JSValueRef exception = nullptr;
        Protected<JSValueRef> value(context, JSEvaluateScript(context, toJSString(source).get(), nullptr, nullptr, 1, &exception));
        check(!exception && value.get(), "test JavaScript evaluates in a real context");
        return value;
    };
    for (auto source : { "7"_s, "9007199254740991"_s }) {
        auto value = evaluate(source);
        auto tab = parseWebExtensionActionTabIdentifier(context, value.get());
        check(tab && *tab && toWebAPI(**tab) == JSValueToNumber(context, value.get(), nullptr), "raw JavaScript numeric identifiers survive validation");
    }
    auto omitted = parseWebExtensionActionTabIdentifier(context, JSValueMakeUndefined(context));
    check(omitted && !*omitted, "raw undefined preserves an omitted scalar argument");
    for (auto source : { "NaN"_s, "Infinity"_s, "-Infinity"_s, "null"_s, "true"_s, "'7'"_s, "[]"_s, "({})"_s,
            "0"_s, "-0"_s, "-1"_s, "-2"_s, "7.5"_s, "9007199254740992"_s }) {
        auto value = evaluate(source);
        check(!parseWebExtensionActionTabIdentifier(context, value.get()), "invalid raw JavaScript values are rejected without numeric coercion");
    }
    for (auto source : { "({ title: null, tabId: 7 })"_s, "({ title: '', windowId: 9 })"_s }) {
        auto value = evaluate(source);
        auto dictionary = toJSONValue(context, value.get(), NullValuePolicy::Allowed);
        auto result = parseWebExtensionActionDetails(dictionary.get(), WebExtensionActionDetailsKind::Title);
        check(result && (result->tab ? result->value.isNull() : (!result->value.isNull() && result->value.isEmpty())),
            "the actual binding dictionary converter preserves null reset versus empty override");
    }
    auto nullTitle = evaluate("({ title: null })"_s);
    auto discardedNull = toJSONValue(context, nullTitle.get(), NullValuePolicy::NotAllowed);
    check(!parseWebExtensionActionDetails(discardedNull.get(), WebExtensionActionDetailsKind::Title),
        "the non-null dictionary policy would lose a required reset field");
    for (auto source : { "({ title: undefined })"_s, "({ title: 'ok', tabId: NaN })"_s, "({ title: 'ok', windowId: Infinity })"_s,
            "({ title: 7 })"_s, "({ title: 'ok', tabId: '7' })"_s, "({ title: 'ok', tabId: 7, windowId: 9 })"_s }) {
        auto value = evaluate(source);
        auto dictionary = toJSONValue(context, value.get(), NullValuePolicy::Allowed);
        check(!parseWebExtensionActionDetails(dictionary.get(), WebExtensionActionDetailsKind::Title), "invalid values stay invalid through the actual binding dictionary converter");
    }
    auto text = evaluate(String::fromUTF8("({ title: '雪 \\\"quoted\\\"\\n text' })"));
    auto dictionary = toJSONValue(context, text.get(), NullValuePolicy::Allowed);
    auto parsed = parseWebExtensionActionDetails(dictionary.get(), WebExtensionActionDetailsKind::Title);
    check(parsed && parsed->value == String::fromUTF8("雪 \"quoted\"\n text"), "the binding converter preserves JavaScript text exactly");
    JSGarbageCollect(context);
    check(parsed && parsed->value == String::fromUTF8("雪 \"quoted\"\n text"), "parsed text survives collection of JavaScript temporaries");
}

class ActionDetailsApplication final : public BApplication {
public:
    bool didRun { false };
    ActionDetailsApplication() : BApplication("application/x-vnd.Kunanyi-Summit-action-details-tests") { }
    void ReadyToRun() override
    {
        WTF::initializeMainThread();
        checkDetails();
        checkJavaScriptArguments();
        didRun = true;
        PostMessage(B_QUIT_REQUESTED);
    }
};
int main()
{
    ActionDetailsApplication application;
    application.Run();
    check(application.didRun, "the native suite ran from BApplication::ReadyToRun");
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
