#include "config.h"
#include "WebExtensionUtilities.h"
#include "JSWebExtensionString.h"
#include <Application.h>
#include <JavaScriptCore/JavaScript.h>
#include <cstdio>
#include <wtf/MainThread.h>

using namespace WebKit;
static unsigned checks;
static unsigned failures;

static JSValueRef makeRejected(JSContextRef context, JSObjectRef, JSObjectRef, size_t count, const JSValueRef values[], JSValueRef* exception)
{
    String arguments[3];
    for (size_t i = 0; i < std::min(count, size_t { 3 }); ++i) {
        JSRetainPtr value(Adopt, JSValueToStringCopy(context, values[i], exception));
        if (*exception)
            return JSValueMakeUndefined(context);
        arguments[i] = WebKit::toString(value.get());
    }
    return toJSRejectedPromise(context, arguments[0], arguments[1], arguments[2]);
}

static void evaluate(JSContextRef context, const char* script, const char* message)
{
    JSValueRef exception = nullptr;
    auto source = toJSString(String::fromUTF8(script));
    auto result = JSEvaluateScript(context, source.get(), nullptr, nullptr, 1, &exception);
    ++checks;
    if (exception || !result || !JSValueToBoolean(context, result)) {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}

int main()
{
    BApplication application("application/x-vnd.Kunanyi-Summit-rejected-promise-tests");
    WTF::initializeMainThread();
    JSRetainPtr context(Adopt, JSGlobalContextCreate(nullptr));
    auto name = toJSString("nativeRejected"_s);
    auto function = JSObjectMakeFunctionWithCallback(context.get(), name.get(), makeRejected);
    JSObjectSetProperty(context.get(), JSContextGetGlobalObject(context.get()), name.get(), function, kJSPropertyAttributeNone, nullptr);

    evaluate(context.get(), "globalThis.outcomes = []; globalThis.p = nativeRejected('runtime.sendMessage()', 'message', 'Bad value.'); p.catch(e => outcomes.push(e)); p instanceof Promise", "returns a Promise immediately");
    evaluate(context.get(), "outcomes.length === 1 && outcomes[0] instanceof Error", "rejects once with an Error object");
    evaluate(context.get(), "outcomes[0].message === \"Invalid call to runtime.sendMessage(). The 'message' value is invalid, because bad value.\"", "preserves API and argument error formatting");
    evaluate(context.get(), "p.then(() => outcomes.push('resolved'), e => outcomes.push(e)); true", "can attach another rejection observer");
    evaluate(context.get(), "outcomes.length === 2 && outcomes[0] === outcomes[1]", "observers receive the same rejection reason");
    evaluate(context.get(), "nativeRejected('runtime.connect()', '', 'failed...').catch(e => globalThis.apiOnly = e.message); true", "API-only rejection is created");
    evaluate(context.get(), "apiOnly === 'Invalid call to runtime.connect(). Failed.'", "API-only formatting preserves punctuation rules");
    evaluate(context.get(), "nativeRejected('', 'id', 'Empty.').catch(e => globalThis.keyOnly = e.message); true", "argument-only rejection is created");
    evaluate(context.get(), "keyOnly === \"The 'id' value is invalid, because empty.\"", "argument-only formatting preserved");
    evaluate(context.get(), "nativeRejected('', '', 'Raw error...').catch(e => globalThis.raw = e.message); true", "plain rejection is created");
    evaluate(context.get(), "raw === 'Raw error'", "plain reason preserves text and trims periods");
    evaluate(context.get(), "nativeRejected('', '', '雪\\u0000\\ud800').catch(e => globalThis.unicode = e.message); true", "Unicode rejection is created");
    evaluate(context.get(), "unicode.length === 3 && unicode.charCodeAt(0) === 0x96ea && unicode.charCodeAt(1) === 0 && unicode.charCodeAt(2) === 0xd800", "rejection preserves UTF-16 including NUL and unpaired surrogate");
    evaluate(context.get(), "globalThis.OriginalPromise = Promise; globalThis.OriginalError = Error; globalThis.Promise = function() { throw 1; }; globalThis.Error = function() { throw 2; }; nativeRejected('', '', 'intrinsic').catch(e => globalThis.intrinsic = e); true", "construction tolerates overwritten global constructors");
    evaluate(context.get(), "intrinsic instanceof OriginalError && intrinsic.message === 'intrinsic'", "rejection uses native Error and Promise constructors");
    evaluate(context.get(), "globalThis.Promise = OriginalPromise; globalThis.Error = OriginalError; globalThis.retained = nativeRejected('', '', 'after GC'); retained.catch(() => {}); true", "retain a rejection before collection");
    JSGarbageCollect(context.get());
    evaluate(context.get(), "retained.catch(e => globalThis.afterGC = e.message); true", "attach an observer after collection");
    evaluate(context.get(), "afterGC === 'after GC'", "promise retains its reason across collection");
    evaluate(context.get(), "globalThis.batch = 0; for (let i = 0; i < 64; ++i) nativeRejected('', '', 'batch').catch(e => { if (e.message === 'batch') ++batch; }); true", "construct a batch of independent rejections");
    evaluate(context.get(), "batch === 64", "every rejection settles exactly once");
    std::printf("%u native rejected promise checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
