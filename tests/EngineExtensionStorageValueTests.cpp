#include "config.h"
#include "JSWebExtensionStorageValues.h"
#include "JSWebExtensionString.h"
#include "Protected.h"
#include <Application.h>
#include <JavaScriptCore/JavaScript.h>
#include <cstdio>
#include <limits>
#include <wtf/MainThread.h>

using namespace WebKit;
static unsigned checks;
static unsigned failures;
static std::optional<WebExtensionStorageKeySelection> pendingSelection;

static JSValueRef selectKeys(JSContextRef context, JSObjectRef, JSObjectRef, size_t count, const JSValueRef values[], JSValueRef*)
{
    auto mode = count > 1 ? static_cast<WebExtensionStorageKeyMode>(static_cast<unsigned>(JSValueToNumber(context, values[1], nullptr))) : WebExtensionStorageKeyMode::Get;
    auto result = parseWebExtensionStorageKeys(context, count ? values[0] : nullptr, mode);
    if (!result)
        return JSValueMakeString(context, toJSString(result.error()).get());
    pendingSelection = WTF::move(*result);
    Ref output = JSON::Object::create();
    Ref keys = JSON::Array::create();
    for (auto& key : pendingSelection->keys)
        keys->pushString(key);
    output->setArray("keys"_s, WTF::move(keys));
    output->setBoolean("all"_s, pendingSelection->allKeys);
    return JSValueMakeFromJSONString(context, toJSString(output->toJSONString()).get());
}

static JSValueRef readValues(JSContextRef context, JSObjectRef, JSObjectRef, size_t count, const JSValueRef values[], JSValueRef*)
{
    Vector<String> chunks;
    for (size_t index = 0; index < count; ++index)
        chunks.append(toString(JSRetainPtr(Adopt, JSValueToStringCopy(context, values[index], nullptr)).get()));
    auto result = deserializeWebExtensionStorageValues(context, chunks, pendingSelection ? pendingSelection->defaultValues : HashMap<String, Protected<JSValueRef>> { });
    if (!result)
        return JSValueMakeString(context, toJSString(result.error()).get());
    return result->get();
}

static JSValueRef serializeValues(JSContextRef context, JSObjectRef, JSObjectRef, size_t count, const JSValueRef values[], JSValueRef*)
{
    auto quota = count > 1 && !JSValueIsNull(context, values[1]) ? std::optional<size_t>(JSValueToNumber(context, values[1], nullptr)) : std::nullopt;
    size_t maximum = count > 2 ? JSValueToNumber(context, values[2], nullptr) : std::numeric_limits<size_t>::max();
    auto result = serializeWebExtensionStorageValues(context, count ? values[0] : nullptr, quota, maximum);
    if (!result)
        return JSValueMakeString(context, toJSString(result.error()).get());
    Ref output = JSON::Object::create();
    output->setObject("values"_s, result->serializedValues.copyRef());
    output->setDouble("size"_s, result->sizeInBytes);
    return JSValueMakeFromJSONString(context, toJSString(output->toJSONString()).get());
}

static void evaluate(JSContextRef context, const char* script, const char* message)
{
    JSValueRef exception = nullptr;
    auto result = JSEvaluateScript(context, toJSString(String::fromUTF8(script)).get(), nullptr, nullptr, 1, &exception);
    ++checks;
    if (exception || !result || !JSValueToBoolean(context, result)) {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}

int main()
{
    BApplication application("application/x-vnd.Kunanyi-Summit-storage-value-tests");
    WTF::initializeMainThread();
    JSRetainPtr context(Adopt, JSGlobalContextCreate(nullptr));
    auto name = toJSString("nativeSerialize"_s);
    auto function = JSObjectMakeFunctionWithCallback(context.get(), name.get(), serializeValues);
    JSObjectSetProperty(context.get(), JSContextGetGlobalObject(context.get()), name.get(), function, kJSPropertyAttributeNone, nullptr);
    for (auto entry : { std::pair { "nativeSelect"_s, &selectKeys }, std::pair { "nativeRead"_s, &readValues } }) {
        auto functionName = toJSString(entry.first);
        JSObjectSetProperty(context.get(), JSContextGetGlobalObject(context.get()), functionName.get(), JSObjectMakeFunctionWithCallback(context.get(), functionName.get(), entry.second), kJSPropertyAttributeNone, nullptr);
    }
    evaluate(context.get(), R"JS(nativeSerialize({}).size === 0 && Object.keys(nativeSerialize({}).values).length === 0)JS", "empty payload stays empty");
    evaluate(context.get(), R"JS(nativeSerialize({a: 1}).values.a === '1' && nativeSerialize({a: 1}).size === 2)JS", "wire map contains individual JSON strings");
    evaluate(context.get(), R"JS(nativeSerialize({a: undefined}).size === 0 && Object.keys(nativeSerialize({a: undefined}).values).length === 0)JS", "undefined properties are omitted");
    evaluate(context.get(), R"JS(nativeSerialize({a: null}).values.a === 'null' && nativeSerialize({a: null}).size === 5)JS", "null remains a stored value");
    evaluate(context.get(), R"JS(nativeSerialize({a: true, b: false}).values.b === 'false' && nativeSerialize({a: true, b: false}).size === 11)JS", "booleans are independently encoded and counted");
    evaluate(context.get(), R"JS(nativeSerialize({a: ''}).values.a === '""' && nativeSerialize({a: ''}).size === 3)JS", "empty strings include JSON quotes in quota");
    evaluate(context.get(), R"JS(nativeSerialize({'': 1}).size === 1 && nativeSerialize({'': 1}).values[''] === '1')JS", "empty keys are retained");
    evaluate(context.get(), R"JS(nativeSerialize({'雪': 'é'}).size === 7)JS", "quotas count UTF-8 bytes rather than UTF-16 units");
    evaluate(context.get(), R"JS(nativeSerialize({'😀': 'a'}).size === 7)JS", "supplementary characters count as four UTF-8 bytes");
    evaluate(context.get(), R"JS(nativeSerialize({'a\u0000': '\u0000'}).size === 10 && JSON.parse(nativeSerialize({k:'\u0000'}).values.k).charCodeAt(0) === 0)JS", "NUL key bytes and escaped value bytes are counted");
    evaluate(context.get(), R"JS(JSON.parse(nativeSerialize({k:'\ud800'}).values.k).charCodeAt(0) === 0xd800)JS", "unpaired surrogate values survive serialization");
    evaluate(context.get(), R"JS(nativeSerialize({a: 1}, 2, 2).size === 2)JS", "exact item and aggregate quota boundaries pass");
    evaluate(context.get(), R"JS(typeof nativeSerialize({a: 1}, 1) === 'string' && nativeSerialize({a: 1}, 1).includes('single item'))JS", "one byte above item quota fails");
    evaluate(context.get(), R"JS(typeof nativeSerialize({a: 1}, null, 1) === 'string' && nativeSerialize({a: 1}, null, 1).includes('per call'))JS", "one byte above aggregate quota fails");
    evaluate(context.get(), R"JS(nativeSerialize({a: 1, b: 2}, 2, 4).size === 4 && typeof nativeSerialize({a: 1, b: 2}, 2, 3) === 'string')JS", "aggregate quota spans independent entries");
    evaluate(context.get(), R"JS(nativeSerialize({}, 0, 0).size === 0 && typeof nativeSerialize({a: 1}, 0, 0) === 'string')JS", "zero quotas allow only empty stored data");
    evaluate(context.get(), R"JS(typeof nativeSerialize({'long key': 1}, 2) === 'string' && typeof nativeSerialize({'long key': 1}, null, 2) === 'string')JS", "keys larger than quota fail without subtraction underflow");
    evaluate(context.get(), R"JS(nativeSerialize({a:[1,undefined,null,NaN]}).values.a === '[1,null,null,null]')JS", "nested arrays follow JSON semantics");
    evaluate(context.get(), R"JS(nativeSerialize({a:{keep:1, omit:undefined}}).values.a === '{"keep":1}')JS", "nested object omissions follow JSON semantics");
    evaluate(context.get(), R"JS(nativeSerialize({a:new Date('2025-01-02T03:04:05Z')}).values.a === '"2025-01-02T03:04:05.000Z"')JS", "nested Date uses its JSON representation");
    evaluate(context.get(), R"JS(globalThis.calls = 0; nativeSerialize({get a(){ ++calls; return 1; }}).values.a === '1' && calls === 1)JS", "each value getter runs once");
    evaluate(context.get(), R"JS(globalThis.jsonCalls = 0; nativeSerialize({a:{toJSON(){ ++jsonCalls; return {ok:1}; }}}).values.a === '{"ok":1}' && jsonCalls === 1)JS", "each stored value invokes toJSON once");
    evaluate(context.get(), R"JS(nativeSerialize(Object.assign(Object.create({inherited:1}), {own:2})).size === 4)JS", "inherited enumerable keys are excluded");
    evaluate(context.get(), R"JS(nativeSerialize(Object.assign(Object.create(null), {own:2})).values.own === '2')JS", "null-prototype dictionaries are supported");
    evaluate(context.get(), R"JS(nativeSerialize(Object.defineProperty({a:1}, 'hidden', {value:2})).size === 2)JS", "non-enumerable keys are excluded");
    evaluate(context.get(), R"JS(nativeSerialize({a:1, [Symbol('key')]:2}).size === 2)JS", "symbol keys are excluded");
    evaluate(context.get(), R"JS(nativeSerialize(JSON.parse('{"__proto__":7}')).values.__proto__ === '7')JS", "prototype-looking keys remain stored data");
    evaluate(context.get(), R"JS(typeof nativeSerialize({a: function(){}}) === 'string' && typeof nativeSerialize({a: Symbol('value')}) === 'string')JS", "nonserializable values reject the payload");
    evaluate(context.get(), R"JS(typeof nativeSerialize({a:1n}) === 'string')JS", "BigInt rejects the payload");
    evaluate(context.get(), R"JS(globalThis.cycle = {}; cycle.self = cycle; typeof nativeSerialize({a:cycle}) === 'string')JS", "cyclic values reject the payload");
    evaluate(context.get(), R"JS(typeof nativeSerialize({ok:1, get bad(){throw 1;}}) === 'string')JS", "throwing getter rejects the entire payload");
    evaluate(context.get(), R"JS(typeof nativeSerialize({a:{toJSON(){throw 1;}}}) === 'string')JS", "throwing toJSON rejects the payload");
    evaluate(context.get(), R"JS(typeof nativeSerialize(new Proxy({}, {ownKeys(){throw 1;}})) === 'string')JS", "throwing key enumeration is handled");
    evaluate(context.get(), R"JS(typeof nativeSerialize(new Proxy({a:1}, {getOwnPropertyDescriptor(){throw 1;}})) === 'string')JS", "throwing descriptor lookup is handled");
    evaluate(context.get(), R"JS([undefined,null,1,'text',[],function(){}].every(value => typeof nativeSerialize(value) === 'string'))JS", "invalid dictionary roots reject cleanly");
    evaluate(context.get(), R"JS(nativeSerialize({after:1}).values.after === '1')JS", "a failed call does not leave a pending JS exception");
    evaluate(context.get(), R"JS((() => { const original = JSON.stringify; JSON.stringify = () => {throw 1;}; try {return nativeSerialize({a:1}).values.a === '1';} finally {JSON.stringify=original;} })())JS", "serialization uses the native intrinsic");
    JSGarbageCollect(context.get());
    evaluate(context.get(), R"JS(nativeSerialize({afterGC:1}).values.afterGC === '1')JS", "payload construction works after collection");
    evaluate(context.get(), R"JS(nativeSelect().all && nativeSelect(null).all && nativeSelect(undefined).all)JS", "omitted and null get keys select all");
    evaluate(context.get(), R"JS(!nativeSelect([]).all && nativeSelect([]).keys.length === 0 && !nativeSelect({}).all)JS", "empty get arrays and dictionaries select no keys");
    evaluate(context.get(), R"JS(nativeSelect('').keys[0] === '' && nativeSelect(['a','雪','a']).keys.join('|') === 'a|雪|a')JS", "string and array keys preserve empty Unicode and duplicate entries");
    evaluate(context.get(), R"JS(nativeSelect(null,1).all && nativeSelect([],1).all && !nativeSelect('a',1).all)JS", "byte-count selection preserves existing empty-array behavior");
    evaluate(context.get(), R"JS([null,undefined,{},1,true].every(v => typeof nativeSelect(v,2) === 'string') && nativeSelect([],2).keys.length === 0)JS", "remove accepts strings and arrays only");
    evaluate(context.get(), R"JS([{},1,true,()=>{}].every(v => typeof nativeSelect(v,1) === 'string'))JS", "byte-count selection rejects invalid roots");
    evaluate(context.get(), R"JS([1,true,()=>{},Symbol(),1n].every(v => typeof nativeSelect(v) === 'string'))JS", "get selection rejects invalid roots");
    evaluate(context.get(), R"JS([[1],['a',null],new Array(1)].every(v => typeof nativeSelect(v) === 'string'))JS", "non-string elements and array holes reject");
    evaluate(context.get(), R"JS(typeof nativeSelect(Object.defineProperty([],0,{get(){throw 1;}})) === 'string')JS", "throwing array getters are caught");
    evaluate(context.get(), R"JS(typeof nativeSelect(new Proxy([], {get(t,k){ if(k==='length') throw 1; return Reflect.get(t,k); }})) === 'string')JS", "throwing array length lookup is caught");
    evaluate(context.get(), R"JS(nativeSelect(new Proxy(['a','b'],{})).keys.join() === 'a,b' && typeof nativeSerialize(new Proxy([],{})) === 'string')JS", "array proxies remain arrays in get and set validation");
    evaluate(context.get(), R"JS(typeof nativeSelect(new Proxy([1],{})) === 'string')JS", "array proxy elements must be strings");
    evaluate(context.get(), R"JS((() => { const pair=Proxy.revocable([],{}); pair.revoke(); return typeof nativeSelect(pair.proxy)==='string' && typeof nativeSerialize(pair.proxy)==='string'; })())JS", "revoked proxies reject and clear ordinary exceptions");
    evaluate(context.get(), R"JS([NaN,-1,1.5,4294967296,'1'].every(length => typeof nativeSelect(new Proxy([], {get(t,k){return k==='length' ? length : Reflect.get(t,k);}})) === 'string'))JS", "array proxies cannot provide invalid lengths");
    evaluate(context.get(), R"JS(nativeSelect(1).includes('an object or a string or an array of strings or null is expected, but a number was provided') && nativeSelect([1],2).includes('a string or an array of strings is expected, but an array of other values was provided'))JS", "existing storage validation error wording is retained");
    evaluate(context.get(), R"JS(nativeSelect(Object.assign(Object.create({inherited:1}), {own:2})).keys.join() === 'own')JS", "default keys exclude inherited properties");
    evaluate(context.get(), R"JS(nativeSelect(Object.defineProperty({a:1,[Symbol()]:2},'hidden',{value:3})).keys.join() === 'a')JS", "default keys exclude symbols and non-enumerable properties");
    evaluate(context.get(), R"JS(globalThis.defaultCalls=0; nativeSelect({get value(){++defaultCalls; return 2;}}); nativeRead().value === 2 && defaultCalls === 1)JS", "default getters are evaluated once");
    evaluate(context.get(), R"JS(nativeSelect({present:undefined}); Object.hasOwn(nativeRead(),'present') && nativeRead().present === undefined)JS", "undefined defaults remain own properties");
    evaluate(context.get(), R"JS(globalThis.identity={a:1}; nativeSelect({object:identity, fn:nativeSelect, symbol:Symbol.for('s'), bigint:1n}); nativeRead().object === identity && nativeRead().fn === nativeSelect && nativeRead().symbol === Symbol.for('s') && nativeRead().bigint === 1n)JS", "default values retain JS identity without JSON coercion");
    evaluate(context.get(), R"JS(nativeSelect({value:{toJSON(){throw 1;}}}); typeof nativeRead().value.toJSON === 'function')JS", "defaults do not invoke toJSON");
    evaluate(context.get(), R"JS(typeof nativeSelect({get bad(){throw 1;}}) === 'string' && typeof nativeSelect(new Proxy({}, {ownKeys(){throw 1;}})) === 'string')JS", "default getter and enumeration failures are caught");
    evaluate(context.get(), R"JS(nativeSelect(JSON.parse('{"__proto__":{"default":1}}')); Object.hasOwn(nativeRead(),'__proto__') && nativeRead().__proto__.default === 1 && Object.getPrototypeOf(nativeRead()) === Object.prototype)JS", "prototype-looking defaults are own data properties");
    evaluate(context.get(), R"JS(nativeSelect({a:1,missing:2}); nativeRead('{"a":"null"}').a === null && nativeRead('{"a":"null"}').missing === 2)JS", "stored null overrides defaults while missing keys retain them");
    evaluate(context.get(), R"JS(nativeSelect({}); nativeRead('{"a":"1"}','{"b":"[2,true]"}').b[1] === true && nativeRead('{"a":"1"}','{"b":"2"}').a === 1)JS", "separately serialized entries and chunks decode");
    evaluate(context.get(), R"JS(nativeRead(JSON.stringify({['__proto__']:JSON.stringify({stored:1})})).__proto__.stored === 1 && Object.getPrototypeOf(nativeRead('{"__proto__":"1"}')) === Object.prototype)JS", "stored prototype-looking keys do not change the object prototype");
    evaluate(context.get(), R"JS(nativeRead('{"0":"1","12":"2","01":"3","4294967294":"4","4294967295":"5"}')[4294967294] === 4 && Object.keys(nativeRead('{"12":"2","0":"1","01":"3"}')).join() === '0,12,01')JS", "numeric-looking stored keys use indexed own properties and JS ordering");
    evaluate(context.get(), R"JS(nativeSelect({'0':7,'12':8,'01':9}); nativeRead()['0'] === 7 && nativeRead('{"12":"2"}')[12] === 2 && Object.keys(nativeRead()).join() === '0,12,01')JS", "numeric-looking defaults remain accessible and stored entries override them");
    evaluate(context.get(), R"JS(nativeSelect({}); (() => { let calls=0; Object.defineProperty(Object.prototype,'0',{set(){++calls;},configurable:true}); try {return nativeRead('{"0":"1"}')[0]===1 && calls===0;} finally {delete Object.prototype[0];} })())JS", "indexed stored values bypass inherited setters");
    evaluate(context.get(), R"JS(nativeSelect({'':1,['a\u0000']:2,['\ud800']:3}); nativeRead()['']===1 && nativeRead()['a\u0000']===2 && nativeRead()['\ud800']===3)JS", "empty NUL and unpaired-surrogate default keys retain their UTF-16 identity");
    evaluate(context.get(), R"JS(['','invalid','[]','null','{"a":1}','{"a":"undefined"}','{"a":"{"}'].every(v => typeof nativeRead(v) === 'string'))JS", "malformed wire data rejects instead of returning partial values");
    evaluate(context.get(), R"JS(nativeRead('{"ok":"true"}').ok === true)JS", "decoding recovers after failures");
    evaluate(context.get(), R"JS((() => {const original=JSON.parse; JSON.parse=()=>{throw 1;}; try{return nativeRead('{"a":"1"}').a===1;} finally{JSON.parse=original;}})())JS", "decoding uses native JSON intrinsics");
    evaluate(context.get(), R"JS(nativeSelect({retained:{text:'survives'}, callback:()=>17}); true)JS", "capture temporary defaults before returning to the event loop");
    JSGarbageCollect(context.get());
    evaluate(context.get(), R"JS(nativeRead().retained.text === 'survives' && nativeRead().callback() === 17)JS", "defaults survive garbage collection between native calls");
    pendingSelection.reset();
    std::printf("%u native storage value checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
