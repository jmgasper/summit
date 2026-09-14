#include "config.h"
#include "JSWebExtensionWrapper.h"
#include "JSWebExtensionString.h"
#include <Application.h>
#include <JavaScriptCore/JavaScript.h>
#include <cstdio>
#include <wtf/MainThread.h>

using namespace WebKit;
static unsigned checks;
static unsigned failures;

static void check(bool passed, const char* message)
{
    ++checks;
    if (!passed) {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}

static JSValueRef sortedJSON(JSContextRef context, JSObjectRef, JSObjectRef, size_t count, const JSValueRef values[], JSValueRef*)
{
    auto result = toSortedJSONString(context, count ? values[0] : nullptr);
    return result.isNull() ? JSValueMakeUndefined(context) : JSValueMakeString(context, toJSString(result).get());
}

static void evaluate(JSContextRef context, const char* script, const char* message)
{
    JSValueRef exception = nullptr;
    auto source = toJSString(String::fromUTF8(script));
    auto result = JSEvaluateScript(context, source.get(), nullptr, nullptr, 1, &exception);
    check(!exception && result && JSValueToBoolean(context, result), message);
}

int main()
{
    BApplication application("application/x-vnd.Kunanyi-Summit-sorted-json-tests");
    WTF::initializeMainThread();
    JSRetainPtr context(Adopt, JSGlobalContextCreate(nullptr));
    auto name = toJSString("nativeSorted"_s);
    auto function = JSObjectMakeFunctionWithCallback(context.get(), name.get(), sortedJSON);
    JSObjectSetProperty(context.get(), JSContextGetGlobalObject(context.get()), name.get(), function, kJSPropertyAttributeNone, nullptr);

    check(toSortedJSONString(nullptr, nullptr).isNull(), "missing context returns null string");
    check(toSortedJSONString(context.get(), nullptr).isNull(), "missing value returns null string");
    evaluate(context.get(), R"JS(nativeSorted({z: 1, a: 2, m: 3}) === '{"a":2,"m":3,"z":1}')JS", "sorts object keys");
    evaluate(context.get(), R"JS(nativeSorted({a: 2, m: 3, z: 1}) === nativeSorted({z: 1, m: 3, a: 2}))JS", "insertion order does not affect output");
    evaluate(context.get(), R"JS(nativeSorted({z: [{b: 2, a: 1}, 3, 2, 1], a: {d: {}, c: []}}) === '{"a":{"c":[],"d":{}},"z":[{"a":1,"b":2},3,2,1]}')JS", "sorts recursively while preserving array order");
    evaluate(context.get(), R"JS(nativeSorted({'2': 2, '10': 10, A: 1, a: 2}) === '{"10":10,"2":2,"A":1,"a":2}')JS", "numeric-looking and case-sensitive keys sort lexicographically");
    evaluate(context.get(), R"JS(nativeSorted([, undefined, function(){}, Symbol('s'), NaN, Infinity, -Infinity]) === '[null,null,null,null,null,null,null]')JS", "array holes and unsupported values follow JSON rules");
    evaluate(context.get(), R"JS(nativeSorted({z: undefined, a: function(){}, s: Symbol('s'), keep: null}) === '{"keep":null}')JS", "omits nonserializable object properties");
    evaluate(context.get(), R"JS(nativeSorted(null) === 'null' && nativeSorted(true) === 'true' && nativeSorted(false) === 'false')JS", "primitive null and booleans are supported");
    evaluate(context.get(), R"JS(nativeSorted(-0) === '0' && nativeSorted(42) === '42')JS", "primitive numbers follow JSON semantics");
    evaluate(context.get(), R"JS(nativeSorted('') === '""' && JSON.parse(nativeSorted('雪\u0000"\\\n')).length === '雪\u0000"\\\n'.length)JS", "string fragments retain escapes and Unicode");
    evaluate(context.get(), R"JS(JSON.parse(nativeSorted('\ud800')) === '\ud800')JS", "unpaired surrogate values round trip");
    evaluate(context.get(), R"JS((() => { let value = {'雪': '\udfff', '\u0000': 1, '"': 2, '\ud800': 3}; let parsed = JSON.parse(nativeSorted(value)); return Object.keys(value).every(k => parsed[k] === value[k]); })())JS", "escaped and surrogate property names round trip");
    evaluate(context.get(), R"JS(nativeSorted(Object.assign(Object.create(null), {z: 1, a: 2})) === '{"a":2,"z":1}')JS", "null-prototype objects serialize");
    evaluate(context.get(), R"JS(nativeSorted(JSON.parse('{"z":1,"__proto__":{"b":2,"a":1}}')) === '{"__proto__":{"a":1,"b":2},"z":1}')JS", "prototype-looking keys remain ordinary JSON data");
    evaluate(context.get(), R"JS(globalThis.calls = 0; globalThis.value = {get z(){ ++calls; return {b:2,a:1}; }, a:3}; nativeSorted(value) === '{"a":3,"z":{"a":1,"b":2}}' && calls === 1)JS", "getters execute once during initial serialization");
    evaluate(context.get(), R"JS(globalThis.toJSONCalls = 0; nativeSorted({toJSON(){ ++toJSONCalls; return {z:1,a:2}; }}) === '{"a":2,"z":1}' && toJSONCalls === 1)JS", "toJSON runs once before sorting");
    evaluate(context.get(), R"JS(nativeSorted(new Date('2025-01-02T03:04:05Z')) === '"2025-01-02T03:04:05.000Z"')JS", "Date uses its JSON representation");
    evaluate(context.get(), R"JS(nativeSorted(undefined) === undefined && nativeSorted(function(){}) === undefined && nativeSorted(Symbol('s')) === undefined)JS", "unsupported root values return null string");
    evaluate(context.get(), R"JS(nativeSorted(1n) === undefined && nativeSorted({a:1n}) === undefined)JS", "BigInt serialization failure is reported");
    evaluate(context.get(), R"JS(globalThis.cycle = {}; cycle.self = cycle; nativeSorted(cycle) === undefined)JS", "cycles fail without recursion in the sorter");
    evaluate(context.get(), R"JS(nativeSorted({get value(){throw new Error('getter');}}) === undefined)JS", "throwing getters fail serialization");
    evaluate(context.get(), R"JS(nativeSorted({toJSON(){throw new Error('json');}}) === undefined)JS", "throwing toJSON fails serialization");
    evaluate(context.get(), R"JS(nativeSorted({z:2,a:1}) === '{"a":1,"z":2}')JS", "a failed call does not poison later calls");
    evaluate(context.get(), R"JS((() => { const values = [0.1, Number.MIN_VALUE, Number.MAX_VALUE, 9007199254740991, -3.141592653589793, 1e-100]; const copy = JSON.parse(nativeSorted(values)); return values.every((value, i) => value === copy[i]); })())JS", "finite number values round trip without precision loss");
    evaluate(context.get(), R"JS((() => { const original = JSON.stringify; JSON.stringify = () => {throw 1;}; try {return nativeSorted({z:1,a:2}) === '{"a":2,"z":1}';} finally {JSON.stringify = original;} })())JS", "serialization uses the JSC intrinsic");
    evaluate(context.get(), R"JS(globalThis.originalOrder = Object.keys(value).join(','); nativeSorted(value); Object.keys(value).join(',') === originalOrder)JS", "sorting does not mutate input property order");
    JSGarbageCollect(context.get());
    evaluate(context.get(), R"JS(nativeSorted({z:1,a:2}) === '{"a":2,"z":1}')JS", "serialization remains valid after collection");
    std::printf("%u native sorted JSON checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
