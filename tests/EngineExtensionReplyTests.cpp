#include "config.h"
#include "JSWebExtensionMessageReply.h"
#include <Application.h>
#include <JavaScriptCore/JSRetainPtr.h>
#include <atomic>
#include <cstdio>
#include <wtf/MainThread.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/text/MakeString.h>

using namespace WebKit;

extern "C" void JSSynchronousGarbageCollectForDebugging(JSContextRef);

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

struct ReplyState : ThreadSafeRefCounted<ReplyState> {
    std::atomic<unsigned> calls { 0 };
    String result;
};

static Ref<WebExtensionMessageReply> handler(Ref<ReplyState> state)
{
    return createWebExtensionMessageReply([state = WTF::move(state)](String&& reply) {
        state->result = WTF::move(reply);
        ++state->calls;
    });
}

static void install(JSContextRef context, const char* name, Ref<WebExtensionMessageReply>&& callback)
{
    auto function = createWebExtensionMessageReplyFunction(context, WTF::move(callback));
    JSRetainPtr key(Adopt, JSStringCreateWithUTF8CString(name));
    JSObjectSetProperty(context, JSContextGetGlobalObject(context), key.get(), function, kJSPropertyAttributeNone, nullptr);
}

static bool evaluate(JSContextRef context, const char* code)
{
    JSRetainPtr script(Adopt, JSStringCreateWithUTF8CString(code));
    JSValueRef exception = nullptr;
    auto result = JSEvaluateScript(context, script.get(), nullptr, nullptr, 1, &exception);
    return result && !exception && JSValueToBoolean(context, result);
}

static JSValueRef thenable(JSContextRef context, const char* code)
{
    JSRetainPtr script(Adopt, JSStringCreateWithUTF8CString(code));
    JSValueRef exception = nullptr;
    auto result = JSEvaluateScript(context, script.get(), nullptr, nullptr, 1, &exception);
    check(result && !exception, "thenable fixture evaluates");
    return exception ? JSValueMakeUndefined(context) : result;
}

int main()
{
    BApplication application("application/x-vnd.Kunanyi-Summit-extension-reply-tests");
    WTF::initializeMainThread();
    JSRetainPtr context(Adopt, JSGlobalContextCreate(nullptr));

    auto absent = adoptRef(*new ReplyState);
    { auto callback = handler(absent.copyRef()); }
    check(absent->calls == 1 && absent->result.isNull(), "unused handler completes once with no reply");

    auto state = adoptRef(*new ReplyState);
    auto callback = handler(state.copyRef());
    install(context.get(), "reply", callback.copyRef());
    install(context.get(), "otherReply", callback.copyRef());
    check(evaluate(context.get(), "typeof reply === 'function' && reply instanceof Function && reply.length === 1"), "reply is a real JavaScript function");
    check(evaluate(context.get(), "reply.call(null, 'first'); otherReply.apply(null, ['second']); true"), "callbacks support call and apply");
    check(state->calls == 1 && state->result == "\"first\""_s, "first reply wins across different callback functions");
    check(evaluate(context.get(), "globalThis.conversions = 0; otherReply({toJSON() { conversions++; return 2; }}); conversions === 0"), "discarded replies do not run serialization or user code");

    struct Case { const char* expression; const char* expected; };
    for (auto test : {
        Case { "{a: 3, b: [true, null]}", "{\"a\":3,\"b\":[true,null]}" },
        Case { "'snow 雪 \\u0000 end'", "\"snow 雪 \\u0000 end\"" },
        Case { "false", "false" },
        Case { "0", "0" },
        Case { "null", "null" },
        Case { "undefined", "" },
        Case { "NaN", "null" },
        Case { "1n", "" },
        Case { "(()=>{let x={}; x.self=x; return x;})()", "" },
        Case { "{toJSON(){throw new Error('bad reply');}}", "" },
    }) {
        auto item = adoptRef(*new ReplyState);
        install(context.get(), "reply", handler(item.copyRef()));
        String code = makeString("reply("_s, String::fromUTF8(test.expression), "); true"_s);
        check(evaluate(context.get(), code.utf8().legacyCStringPointer()), "reply serialization does not escape as a script exception");
        check(item->calls == 1 && !item->result.isNull() && item->result == String::fromUTF8(test.expected), "JSON or non-null empty reply preserves message semantics");
    }

    auto recursive = adoptRef(*new ReplyState);
    install(context.get(), "reply", handler(recursive.copyRef()));
    check(evaluate(context.get(), "reply({toJSON(){reply('inner'); return 'outer';}}); true"), "serialization can reenter the reply function");
    check(recursive->calls == 1 && recursive->result == "\"outer\""_s, "reentrant reply cannot replace the first reply");

    auto promise = adoptRef(*new ReplyState);
    install(context.get(), "reply", handler(promise.copyRef()));
    check(evaluate(context.get(), "Promise.resolve({asynchronous: true}).then(reply); true"), "promise fulfillment accepts the native reply function");
    check(promise->calls == 1 && promise->result == "{\"asynchronous\":true}"_s, "promise callback retains the handler until its reply");

    auto bound = adoptRef(*new ReplyState);
    install(context.get(), "reply", handler(bound.copyRef()));
    check(evaluate(context.get(), "globalThis.savedReply = reply.bind(null); reply = null; true"), "bound callback takes ownership of the reply function");
    JSSynchronousGarbageCollectForDebugging(context.get());
    check(evaluate(context.get(), "savedReply('retained'); true"), "a bound callback survives garbage collection");
    check(bound->calls == 1 && bound->result == "\"retained\""_s, "bound callback preserves its native handler");

    auto missingArgument = adoptRef(*new ReplyState);
    install(context.get(), "reply", handler(missingArgument.copyRef()));
    check(evaluate(context.get(), "reply(); true"), "reply accepts no argument");
    check(missingArgument->calls == 1 && !missingArgument->result.isNull() && missingArgument->result.isEmpty(), "explicit undefined reply differs from no listener reply");

    auto abandoned = adoptRef(*new ReplyState);
    {
        JSRetainPtr temporary(Adopt, JSGlobalContextCreate(nullptr));
        install(temporary.get(), "reply", handler(abandoned.copyRef()));
        check(abandoned->calls == 0, "JavaScript ownership keeps an unanswered handler alive");
    }
    check(abandoned->calls == 1 && abandoned->result.isNull(), "destroying an unanswered context completes with no reply");

    auto custom = adoptRef(*new ReplyState);
    JSValueRef exception = nullptr;
    auto object = thenable(context.get(), "({marker: 17, then(resolve, reject) { if (this.marker !== 17) throw Error('wrong receiver'); resolve({marker:this.marker}); reject('ignored'); resolve(99); }})");
    invokeWebExtensionMessageReplyThenable(context.get(), object, handler(custom.copyRef()), exception);
    check(!exception && custom->calls == 1 && custom->result == "{\"marker\":17}"_s, "custom thenable keeps its receiver and first fulfillment");

    auto rejected = adoptRef(*new ReplyState);
    auto rejectCallback = handler(rejected.copyRef());
    exception = nullptr;
    object = thenable(context.get(), "({then(resolve, reject) { reject('ignored'); }})");
    invokeWebExtensionMessageReplyThenable(context.get(), object, rejectCallback.copyRef(), exception);
    check(!exception && rejected->calls == 0, "rejection does not produce a message reply");
    rejectCallback.get()(nullptr, nullptr);

    auto getter = adoptRef(*new ReplyState);
    exception = nullptr;
    object = thenable(context.get(), "({get then() { throw Error('getter'); }})");
    invokeWebExtensionMessageReplyThenable(context.get(), object, handler(getter.copyRef()), exception);
    check(exception && getter->calls == 1 && getter->result.isNull(), "throwing then getter returns an exception and releases an unanswered handler");

    auto late = adoptRef(*new ReplyState);
    exception = nullptr;
    object = thenable(context.get(), "({then(resolve) { globalThis.lateReply = resolve; throw Error('then'); }})");
    invokeWebExtensionMessageReplyThenable(context.get(), object, handler(late.copyRef()), exception);
    check(exception && late->calls == 0, "throwing then method can retain its reply callback");
    check(evaluate(context.get(), "lateReply('later'); true") && late->calls == 1 && late->result == "\"later\""_s, "retained callback can reply after its then method threw");

    auto asynchronous = adoptRef(*new ReplyState);
    exception = nullptr;
    object = thenable(context.get(), "Promise.resolve('promise transport')");
    invokeWebExtensionMessageReplyThenable(context.get(), object, handler(asynchronous.copyRef()), exception);
    check(!exception && asynchronous->calls == 1 && asynchronous->result == "\"promise transport\""_s, "thenable adapter handles a native promise fulfillment");

    auto maximum = adoptRef(*new ReplyState);
    install(context.get(), "reply", handler(maximum.copyRef()));
    check(evaluate(context.get(), "reply('x'.repeat(64 * 1024 * 1024 - 2)); true"), "maximum reply serializes");
    check(maximum->calls == 1 && maximum->result.length() == 64 * 1024 * 1024, "64 MiB JSON limit includes quote characters");
    auto excessive = adoptRef(*new ReplyState);
    install(context.get(), "reply", handler(excessive.copyRef()));
    check(evaluate(context.get(), "reply('x'.repeat(64 * 1024 * 1024 - 1)); true"), "oversized reply completes without a script exception");
    check(excessive->calls == 1 && !excessive->result.isNull() && excessive->result.isEmpty(), "reply above the limit becomes a non-null empty string");

    std::printf("%u native JavaScript reply checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
