#include "config.h"
#include "JSWebExtensionString.h"
#include <Application.h>
#include <JavaScriptCore/JavaScript.h>
#include <cstdio>
#include <wtf/MainThread.h>
#include <wtf/Vector.h>

using namespace WebKit;

static unsigned checks;
static unsigned failures;

static void check(bool result, const char* message)
{
    ++checks;
    if (!result) {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}

int main()
{
    BApplication application("application/x-vnd.Kunanyi-Summit-extension-string-tests");
    WTF::initializeMainThread();
    JSRetainPtr context(Adopt, JSGlobalContextCreate(nullptr));

    check(WebKit::toString(nullptr).isNull(), "absent JavaScript string remains absent");
    auto empty = toJSString(nullString());
    check(empty && JSStringGetLength(empty.get()) == 0, "null native string maps to an empty JavaScript string");
    check(WebKit::toString(empty.get()).isEmpty() && !WebKit::toString(empty.get()).isNull(), "present empty string is distinct from absent string");

    for (const auto& units : {
        Vector<char16_t> { },
        Vector<char16_t> { 'a', 'b', 'c' },
        Vector<char16_t> { 'a', 0, 'b', 0 },
        Vector<char16_t> { 0, 0, 0 },
        Vector<char16_t> { 0xe9, 0xff },
        Vector<char16_t> { 0x96ea, 0x20ac, 0x0301 },
        Vector<char16_t> { 0xd83d, 0xde42 },
        Vector<char16_t> { 0xd800 },
        Vector<char16_t> { 0xdc00 },
        Vector<char16_t> { 0xd800, 'x', 0xdc00, 0, 0xffff },
    }) {
        String original = units.isEmpty() ? emptyString() : String { units.span() };
        auto string = toJSString(original);
        check(JSStringGetLength(string.get()) == units.size(), "native-to-JavaScript conversion preserves every UTF-16 code unit");
        bool same = JSStringGetLength(string.get()) == units.size();
        auto* characters = JSStringGetCharactersPtr(string.get());
        for (size_t i = 0; i < std::min(units.size(), JSStringGetLength(string.get())); ++i)
            same &= characters[i] == units[i];
        check(same, "conversion preserves NULs, Unicode and unpaired surrogates");
        check(WebKit::toString(string.get()) == original, "native string survives a JavaScript string round trip");

        JSRetainPtr fromJavaScript(Adopt, JSStringCreateWithCharacters(reinterpret_cast<const JSChar*>(units.span().data()), units.size()));
        check(WebKit::toString(fromJavaScript.get()) == original, "JavaScript-to-native conversion preserves original code units independently");
    }

    Vector<char16_t> everyCodeUnit;
    everyCodeUnit.reserveInitialCapacity(65536);
    for (unsigned i = 0; i < 65536; ++i)
        everyCodeUnit.append(static_cast<char16_t>(i));
    String everyString { everyCodeUnit.span() };
    auto everyJavaScriptString = toJSString(everyString);
    check(JSStringGetLength(everyJavaScriptString.get()) == 65536, "bridge preserves the complete 16-bit code-unit range");
    check(WebKit::toString(everyJavaScriptString.get()) == everyString, "every UTF-16 code unit survives a round trip");

    auto key = toJSString("payload"_s);
    Vector<char16_t> content { 'a', 0, 0xd800, 0xdc00, 'z' };
    auto value = toJSString(String { content.span() });
    JSObjectSetProperty(context.get(), JSContextGetGlobalObject(context.get()), key.get(), JSValueMakeString(context.get(), value.get()), kJSPropertyAttributeNone, nullptr);
    auto code = toJSString("payload.length === 5 && payload.charCodeAt(1) === 0 && payload.charCodeAt(2) === 0xd800 && payload.charCodeAt(4) === 122"_s);
    JSValueRef exception = nullptr;
    auto result = JSEvaluateScript(context.get(), code.get(), nullptr, nullptr, 1, &exception);
    check(!exception && JSValueToBoolean(context.get(), result), "JavaScript sees the full payload after conversion");

    std::printf("%u UTF-16 bridge checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
