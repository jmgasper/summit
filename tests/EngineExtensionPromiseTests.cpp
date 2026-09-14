#include "config.h"
#include "JSWebExtensionPromise.h"
#include <Application.h>
#include <JavaScriptCore/JSRetainPtr.h>
#include <atomic>
#include <cstdio>
#include <memory>
#include <wtf/MainThread.h>

using namespace WebKit;
static unsigned checks, failures;
static std::atomic<unsigned> liveCompletions { 0 };
struct CompletionLifetime {
    CompletionLifetime() { ++liveCompletions; }
    ~CompletionLifetime() { --liveCompletions; }
};

static void check(bool condition, const char* message)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}

static JSValueRef transform(JSContextRef context, JSObjectRef, JSObjectRef, size_t count, const JSValueRef arguments[], JSValueRef* exception)
{
    auto input = count ? arguments[0] : JSValueMakeUndefined(context);
    auto extra = count > 1 ? arguments[1] : JSValueMakeUndefined(context);
    auto mode = count > 2 ? static_cast<int>(JSValueToNumber(context, arguments[2], exception)) : 0;
    if (*exception)
        return JSValueMakeUndefined(context);
    return transformWebExtensionPromise(context, input, extra,
        [mode, lifetime = std::make_shared<CompletionLifetime>()](JSContextRef context, bool fulfilled, JSValueRef value, JSValueRef extra, JSValueRef& exception) -> JSValueRef {
            switch (mode) {
            case 1:
                exception = value;
                return JSValueMakeUndefined(context);
            case 2:
                return extra;
            case 3:
                return JSObjectCallAsFunction(context, JSValueToObject(context, extra, &exception), nullptr, 1, &value, &exception);
            case 4:
                return nullptr;
            default:
                JSValueRef values[] { JSValueMakeBoolean(context, fulfilled), value, extra };
                return JSObjectMakeArray(context, 3, values, &exception);
            }
        });
}

static JSValueRef errorMessage(JSContextRef context, JSObjectRef, JSObjectRef, size_t count, const JSValueRef arguments[], JSValueRef* exception)
{
    return webExtensionPromiseErrorMessage(context, count ? arguments[0] : JSValueMakeUndefined(context), *exception);
}

static void evaluate(JSContextRef context, const char* script, const char* message)
{
    JSRetainPtr source(Adopt, JSStringCreateWithUTF8CString(script));
    JSValueRef exception = nullptr;
    auto result = JSEvaluateScript(context, source.get(), nullptr, nullptr, 1, &exception);
    check(!exception && result && JSValueToBoolean(context, result), message);
}

int main()
{
    BApplication application("application/x-vnd.Kunanyi-Summit-extension-promise-tests");
    WTF::initializeMainThread();
    {
        JSRetainPtr context(Adopt, JSGlobalContextCreate(nullptr));
        JSRetainPtr name(Adopt, JSStringCreateWithUTF8CString("nativeTransform"));
        auto function = JSObjectMakeFunctionWithCallback(context.get(), name.get(), transform);
        JSObjectSetProperty(context.get(), JSContextGetGlobalObject(context.get()), name.get(), function, kJSPropertyAttributeNone, nullptr);
        JSRetainPtr errorName(Adopt, JSStringCreateWithUTF8CString("nativeErrorMessage"));
        JSObjectSetProperty(context.get(), JSContextGetGlobalObject(context.get()), errorName.get(), JSObjectMakeFunctionWithCallback(context.get(), errorName.get(), errorMessage), kJSPropertyAttributeNone, nullptr);
        evaluate(context.get(), "nativeErrorMessage('text') === 'text' && nativeErrorMessage(undefined) === undefined && nativeErrorMessage(null) === null", "primitive rejection reasons remain unchanged");
        evaluate(context.get(), "nativeErrorMessage(new Error('reason')) === 'reason'", "error objects yield their message");
        evaluate(context.get(), "nativeErrorMessage(Object.create({message: 'inherited'})) === 'inherited'", "inherited error messages are read");
        evaluate(context.get(), "nativeErrorMessage({message: undefined}) === undefined", "an explicit undefined message stays undefined");
        evaluate(context.get(), "globalThis.noMessage = {}; nativeErrorMessage(noMessage) === noMessage", "objects without message properties retain identity");
        evaluate(context.get(), "(() => { const reason = {}; try { nativeErrorMessage({get message() { throw reason; }}); return false; } catch (error) { return error === reason; } })()", "message getter exceptions propagate");
        evaluate(context.get(), "(() => { const reason = {}; try { nativeErrorMessage(new Proxy({}, {has() { throw reason; }})); return false; } catch (error) { return error === reason; } })()", "message existence-trap exceptions propagate");
        evaluate(context.get(), "(() => { let gets = 0; const value = new Proxy({}, {has() { return false; }, get() { ++gets; return 1; }}); return nativeErrorMessage(value) === value && gets === 0; })()", "absent message properties are not read");
        evaluate(context.get(), "globalThis.results = []; nativeTransform(42, 'extra').then(x => results.push(x)); results.length === 0", "primitive completion runs asynchronously");
        evaluate(context.get(), "results.length === 1 && results[0][0] === true && results[0][1] === 42 && results[0][2] === 'extra'", "primitive value and captured argument are preserved");
        evaluate(context.get(), "globalThis.fragments = []; for (const value of [undefined, null, false, 0, '', 123n, Symbol.for('test')]) nativeTransform(value).then(x => fragments.push(x)); true", "all primitive kinds are accepted");
        evaluate(context.get(), "fragments.length === 7 && fragments.every(x => x[0] === true) && fragments[0][1] === undefined && fragments[1][1] === null && fragments[2][1] === false && fragments[3][1] === 0 && fragments[4][1] === '' && fragments[5][1] === 123n && fragments[6][1] === Symbol.for('test')", "primitive settlement retains exact JavaScript types");
        evaluate(context.get(), "globalThis.object = {answer: 7}; nativeTransform(Promise.resolve(object), object).then(x => globalThis.identity = x); true", "native fulfillment is observed");
        evaluate(context.get(), "identity[0] && identity[1] === object && identity[2] === object", "fulfillment and capture preserve object identity");
        evaluate(context.get(), "globalThis.reason = new Error('reason'); nativeTransform(Promise.reject(reason)).then(x => globalThis.rejection = x); true", "native rejection is observed without rejecting the transformed result by default");
        evaluate(context.get(), "rejection[0] === false && rejection[1] === reason", "rejection reason identity is preserved");
        evaluate(context.get(), "nativeTransform(Promise.reject(undefined)).then(x => globalThis.undefinedReason = x); true", "undefined rejection is observed");
        evaluate(context.get(), "undefinedReason[0] === false && undefinedReason[1] === undefined", "undefined rejection differs from fulfillment");
        evaluate(context.get(), "globalThis.gets = 0; globalThis.calls = 0; nativeTransform({get then() { ++gets; return function(resolve, reject) { ++calls; resolve(1); reject(2); resolve(3); throw 4; }; }}).then(x => globalThis.once = x); true", "hostile thenable is assimilated");
        evaluate(context.get(), "gets === 1 && calls === 1 && once[0] === true && once[1] === 1", "then is read once and the first settlement wins");
        evaluate(context.get(), "nativeTransform({get then() { throw reason; }}).then(x => globalThis.getterError = x); nativeTransform({then() { throw reason; }}).then(x => globalThis.callError = x); true", "throwing then getters and calls become rejections");
        evaluate(context.get(), "!getterError[0] && getterError[1] === reason && !callError[0] && callError[1] === reason", "thenable exceptions preserve their reasons");
        evaluate(context.get(), "globalThis.notThenable = {then: 5}; nativeTransform(notThenable).then(x => globalThis.noncallable = x); true", "noncallable then is a normal object");
        evaluate(context.get(), "noncallable[0] && noncallable[1] === notThenable", "nonthenable object identity survives resolution");
        evaluate(context.get(), "nativeTransform(reason, undefined, 1).catch(x => globalThis.thrown = x); nativeTransform(undefined, undefined, 1).then(() => globalThis.wrong = true, x => globalThis.thrownUndefined = x === undefined); true", "completion exceptions reject the returned promise");
        evaluate(context.get(), "thrown === reason && thrownUndefined === true && !globalThis.wrong", "both object and undefined completion exceptions reject correctly");
        evaluate(context.get(), "nativeTransform(1, {then(resolve) { resolve(9); }}, 2).then(x => globalThis.returnedThenable = x); nativeTransform(1, undefined, 4).then(x => globalThis.nullReturn = x); true", "completion return values use promise resolution");
        evaluate(context.get(), "returnedThenable === 9 && nullReturn === undefined", "returned thenables are assimilated and null callback pointers mean undefined");
        evaluate(context.get(), "nativeTransform(5, value => Promise.resolve(value + 1), 3).then(x => globalThis.callbackValue = x); nativeTransform(5, () => { throw reason; }, 3).catch(x => globalThis.callbackError = x); true", "a JavaScript completion can return a promise or throw");
        evaluate(context.get(), "callbackValue === 6 && callbackError === reason", "completion promise and exception propagation are correct");
        evaluate(context.get(), R"JS((() => {
            const Original = Promise, originalThen = Original.prototype.then;
            const species = Object.getOwnPropertyDescriptor(Original, Symbol.species);
            globalThis.Promise = function() { throw new Error('global constructor used'); };
            Original.prototype.then = function() { throw new Error('prototype then used'); };
            Object.defineProperty(Original, Symbol.species, {get() { throw new Error('species used'); }, configurable: true});
            let result;
            try { result = nativeTransform(8, 'intrinsic'); }
            finally { globalThis.Promise = Original; Original.prototype.then = originalThen; Object.defineProperty(Original, Symbol.species, species); }
            result.then(value => globalThis.intrinsic = value);
            return result instanceof Original;
        })())JS", "intrinsic hook ignores replaced global constructor, then and species");
        evaluate(context.get(), "intrinsic[0] && intrinsic[1] === 8 && intrinsic[2] === 'intrinsic'", "intrinsic completion still settles normally");
        evaluate(context.get(), "globalThis.pending = new Promise(resolve => globalThis.finish = resolve); globalThis.capture = {data: 'retained'}; globalThis.weakCapture = new WeakRef(capture); nativeTransform(pending, capture, 2).then(x => globalThis.afterGC = x); capture = null; true", "pending callback captures are retained by JavaScript tracing");
        JSGarbageCollect(context.get());
        evaluate(context.get(), "finish(0); true", "pending promise resolves after collection");
        evaluate(context.get(), "afterGC === weakCapture.deref() && afterGC.data === 'retained'", "captured object identity survives collection before settlement");
        evaluate(context.get(), "globalThis.count = 0; for (let i = 0; i < 128; ++i) nativeTransform({then(resolve, reject) { resolve(i); reject(i); }}).then(x => { if (x[0] && x[1] === i) ++count; }); true", "batch of competing thenable settlements is scheduled");
        evaluate(context.get(), "count === 128", "each transformed promise completes exactly once");
        evaluate(context.get(), "nativeTransform(new Promise(() => {}), {pending: true}); true", "an unreferenced pending graph can be released with its context");
        check(liveCompletions.load() > 0, "the fixture actually has native callback state to release");
    }
    check(liveCompletions.load() == 0, "releasing the context releases all captured native callback state");
    std::printf("%u native extension promise checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
