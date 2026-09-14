#include "config.h"
#include "JSWebExtensionString.h"
#include "JSWebExtensionTabParameters.h"
#include "WebExtensionTabParameters.h"
#include <Application.h>
#include <cstdio>
#include <wtf/MainThread.h>

using namespace WebKit;

static unsigned checks;
static unsigned failures;

static void expect(JSContextRef context, const char* expression, const char* message)
{
    ++checks;
    auto script = toJSString(String::fromUTF8(expression));
    JSValueRef exception = nullptr;
    auto result = JSEvaluateScript(context, script.get(), nullptr, nullptr, 1, &exception);
    if (!result || exception || !JSValueToBoolean(context, result)) {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}

static void expose(JSContextRef context, const WebExtensionTabParameters& parameters)
{
    auto value = toWebAPI(context, parameters);
    auto name = toJSString("tab"_s);
    JSObjectSetProperty(context, JSContextGetGlobalObject(context), name.get(), value, kJSPropertyAttributeNone, nullptr);
}

int main()
{
    BApplication application("application/x-vnd.Kunanyi-Summit-extension-tab-parameter-tests");
    WTF::initializeMainThread();
    JSRetainPtr context(Adopt, JSGlobalContextCreate(nullptr));
    WebExtensionTabParameters tab;
    expose(context.get(), tab);
    expect(context.get(), "Object.keys(tab).length === 0", "unset metadata does not invent properties");
    expect(context.get(), "Object.getPrototypeOf(tab) === Object.prototype", "tab metadata is an ordinary JavaScript object");

    tab.identifier = WebExtensionTabIdentifier { 42 };
    tab.windowIdentifier = WebExtensionWindowIdentifier { 7 };
    tab.parentTabIdentifier = WebExtensionTabIdentifier { 31 };
    tab.index = 0;
    expose(context.get(), tab);
    expect(context.get(), "tab.id === 42 && tab.windowId === 7 && tab.openerTabId === 31", "identifiers retain numeric values");
    expect(context.get(), "tab.index === 0", "zero index remains present");
    expect(context.get(), "Object.keys(tab).length === 4", "partial metadata contains only supplied properties");

    tab.identifier = WebExtensionTabConstants::NoneIdentifier;
    tab.windowIdentifier = WebExtensionWindowConstants::NoneIdentifier;
    tab.parentTabIdentifier = WebExtensionTabConstants::NoneIdentifier;
    tab.index = notFound;
    expose(context.get(), tab);
    expect(context.get(), "tab.id === -1 && tab.windowId === -1 && tab.openerTabId === -1", "no-tab and no-window sentinels match the extension API");
    expect(context.get(), "Number.isNaN(tab.index)", "unknown index retains the upstream NaN value");

    tab = { };
    tab.url = URL { };
    tab.title = nullString();
    expose(context.get(), tab);
    expect(context.get(), "Object.hasOwn(tab, 'url') && tab.url === ''", "present null URL becomes an empty string");
    expect(context.get(), "Object.hasOwn(tab, 'title') && tab.title === ''", "present null title becomes an empty string");
    tab.url = URL { "https://example.test/path?q=one%20two#fragment"_s };
    const char16_t title[] { 'a', 0, 'b', 0xd800, 0xdc00, 0xdc00 };
    tab.title = String { std::span(title) };
    expose(context.get(), tab);
    expect(context.get(), "tab.url === 'https://example.test/path?q=one%20two#fragment'", "URL query and fragment survive metadata conversion");
    expect(context.get(), "tab.title.length === 6 && tab.title.charCodeAt(1) === 0 && tab.title.charCodeAt(5) === 0xdc00", "tab title preserves NULs and unpaired UTF-16 surrogates");

    for (bool flag : { false, true }) {
        tab = { };
        tab.active = flag;
        tab.selected = flag;
        tab.pinned = flag;
        tab.audible = flag;
        tab.muted = flag;
        tab.loading = flag;
        tab.privateBrowsing = flag;
        tab.readerModeAvailable = flag;
        tab.showingReaderMode = flag;
        expose(context.get(), tab);
        auto flagName = toJSString("expectedFlag"_s);
        JSObjectSetProperty(context.get(), JSContextGetGlobalObject(context.get()), flagName.get(), JSValueMakeBoolean(context.get(), flag), kJSPropertyAttributeNone, nullptr);
        expect(context.get(), "tab.active === expectedFlag", "active preserves explicit boolean values");
        expect(context.get(), "tab.selected === expectedFlag && tab.highlighted === expectedFlag", "selected and highlighted stay consistent");
        expect(context.get(), "tab.pinned === expectedFlag && tab.audible === expectedFlag", "pinned and audible preserve explicit boolean values");
        expect(context.get(), "tab.mutedInfo.muted === expectedFlag && Object.keys(tab.mutedInfo).length === 1", "muted state uses the nested API object");
        expect(context.get(), "tab.status === (expectedFlag ? 'loading' : 'complete')", "load state uses the API status strings");
        expect(context.get(), "tab.incognito === expectedFlag", "private browsing maps to incognito");
        expect(context.get(), "tab.isArticle === expectedFlag && tab.isInReaderMode === expectedFlag", "reader metadata preserves availability and active state");
        expect(context.get(), "Object.keys(tab).length === 10", "false values remain present and no metadata is added");
    }

    std::printf("%u native tab metadata checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
