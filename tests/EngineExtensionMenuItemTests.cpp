#include "config.h"
#include "JSWebExtensionMenuItemParameters.h"
#include "JSWebExtensionString.h"
#include "Protected.h"
#include "WebExtensionMenuItemTree.h"
#include <Application.h>
#include <JavaScriptCore/InitializeThreading.h>
#include <cstdio>
#include <limits>
#include <wtf/MainThread.h>
#include <wtf/HashMap.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/MakeString.h>

// This isolated helper initializes actual JSC/WTF for API::Object and match
// patterns. It does not create a WebExtensionContext or run the browser.
namespace WebKit {
void InitializeWebKit2() { JSC::initialize(); WTF::initializeMainThread(); }
}
using namespace WebKit;
static unsigned checks, failures;
static void check(bool value, const char* description)
{
    ++checks;
    failures += !value;
    std::printf("%s %s\n", value ? "PASS" : "FAIL", description);
}
static auto parse(const String& text, WebExtensionMenuItemOperation operation = WebExtensionMenuItemOperation::Create, bool version3 = true)
{
    auto value = JSON::Value::parseJSON(text);
    return parseWebExtensionMenuItemProperties(value.get(), operation, version3, URL { "webkit-extension://fixture/pages/background.html"_s });
}
static void checkProperties()
{
    using Operation = WebExtensionMenuItemOperation;
    auto basic = parse(R"({"id":"001","title":"Menu"})"_s);
    check(basic && basic->identifier == "s:001"_s && basic->title == "Menu"_s && !basic->checked && !basic->contexts && !basic->parentIdentifier,
        "explicit string IDs and omitted properties retain their identity and presence");
    auto numeric = parse(R"({"id":1,"title":"Number"})"_s);
    check(numeric && numeric->identifier == "n:1"_s && numeric->identifier != basic->identifier, "numeric IDs cannot alias string IDs");
    auto generated = parse(R"({"title":"Generated"})"_s);
    auto other = parse(R"({"title":"Generated"})"_s);
    check(generated && other && generated->identifier != other->identifier && decodeWebExtensionMenuIdentifier(generated->identifier), "omitted IDs produce distinct valid string identifiers");
    auto update = parse("{}"_s, Operation::Update);
    check(update && update->identifier.isNull() && update->title.isNull() && update->iconsJSON.isNull() && !update->contexts, "empty updates leave every property omitted");
    auto separator = parse(R"({"type":"separator"})"_s);
    check(separator && separator->type == WebExtensionMenuItemType::Separator && separator->title.isNull(), "separators can omit a title");
    auto checkbox = parse(R"({"id":-7,"title":"Check","type":"checkbox","checked":true,"enabled":false,"visible":false,"parentId":"01"})"_s);
    check(checkbox && checkbox->checked == true && checkbox->enabled == false && checkbox->visible == false && checkbox->parentIdentifier == "s:01"_s,
        "checkbox, false flags and typed parent identity survive parsing");
    auto clear = parse(R"({"icons":null,"documentUrlPatterns":[],"targetUrlPatterns":[]})"_s, Operation::Update);
    check(clear && !clear->iconsJSON.isNull() && clear->iconsJSON.isEmpty() && clear->documentURLPatterns && clear->documentURLPatterns->isEmpty() && clear->targetURLPatterns,
        "explicit clear operations remain distinct from omission");
    auto icon = parse(R"({"title":"Icon","icons":{"16":"../icons/small.png","32":"/icons/large.png"}})"_s);
    RefPtr iconValue = icon ? JSON::Value::parseJSON(icon->iconsJSON) : nullptr;
    RefPtr icons = iconValue ? iconValue->asObject() : nullptr;
    check(icons && icons->getString("16"_s) == "/icons/small.png"_s && icons->getString("32"_s) == "/icons/large.png"_s, "icon paths resolve relative to the extension document");
    auto contexts = parse(R"({"title":"Contexts","contexts":["link","image","selection","editable","frame","page","audio","video","tab","action"]})"_s);
    check(contexts && contexts->contexts == allWebExtensionMenuItemContextTypes(), "all native context types parse without dropping values");
    auto all = parse(R"({"title":"All","contexts":["all"]})"_s);
    check(all && all->contexts == allWebExtensionMenuItemContextTypes(), "all expands to the supported context set");
    check(!parse(R"({"title":"MV2","contexts":["browser_action"]})"_s) && parse(R"({"title":"MV2","contexts":["browser_action","page_action"]})"_s, Operation::Create, false), "action context aliases respect the manifest version");
    check(!parse(R"({"title":"MV3","contexts":["action"]})"_s, Operation::Create, false), "MV3 action context is rejected for MV2");
    auto patterns = parse(R"({"title":"Patterns","documentUrlPatterns":["https://*.example.test/private/*"],"targetUrlPatterns":["ftp://example.test/*"]})"_s);
    check(patterns && patterns->documentURLPatterns->size() == 1 && patterns->targetURLPatterns->size() == 1, "actual WebKit match patterns validate document and target filters");
    for (auto invalid : { ""_s, "null"_s, "[]"_s, "true"_s, "7"_s, "{}"_s, R"({"title":""})"_s, R"({"type":"bad","title":"M"})"_s,
            R"({"title":"M","checked":true})"_s, R"({"type":"separator","checked":true})"_s, R"({"title":"M","contexts":[]})"_s,
            R"({"title":"M","contexts":["made_up"]})"_s, R"({"title":"M","contexts":["all","made_up"]})"_s,
            R"({"title":"M","contexts":[1]})"_s, R"({"title":"M","documentUrlPatterns":["bad"]})"_s,
            R"({"title":"M","targetUrlPatterns":[false]})"_s, R"({"title":"M","icons":{"0":"icon.png"}})"_s,
            R"({"title":"M","icons":{"16":false}})"_s, R"({"title":"M","icons":"https://other.test/icon.png"})"_s,
            R"({"title":"M","icons":"data:image/png;base64,aA=="})"_s, R"({"title":"M","icons":{"16":""}})"_s,
            R"({"title":"M","icons":[]})"_s, R"({"title":"M","command":""})"_s, R"({"title":"M","onclick":false})"_s,
            R"({"title":"M","icon_variants":[]})"_s, R"({"title":"M","typo":true})"_s })
        check(!parse(invalid), "malformed or unsupported menu properties fail without a partial result");
    for (auto key : { "id"_s, "parentId"_s, "title"_s, "command"_s }) {
        for (auto invalid : { "null"_s, "true"_s, "[]"_s, "{}"_s, "\"a\\u0000b\""_s })
            check(!parse(makeString("{\"title\":\"M\",\""_s, key, "\":"_s, invalid, '}')), "menu strings and IDs reject invalid types and embedded nulls");
    }
    for (auto key : { "checked"_s, "enabled"_s, "visible"_s }) {
        for (auto invalid : { "null"_s, "0"_s, "\"false\""_s, "[]"_s })
            check(!parse(makeString("{\"title\":\"M\",\""_s, key, "\":"_s, invalid, '}')), "boolean menu flags cannot be coerced");
    }
    if (!basic)
        return;
    auto corrupt = *basic;
    corrupt.identifier = "001"_s;
    check(!validateWebExtensionMenuItemParameters(corrupt, Operation::Create), "the UI boundary rejects untagged native identifiers");
    corrupt = *basic;
    corrupt.type = static_cast<WebExtensionMenuItemType>(255);
    check(!validateWebExtensionMenuItemParameters(corrupt, Operation::Create), "the UI boundary rejects unknown item types");
    corrupt = *basic;
    corrupt.contexts = OptionSet<WebExtensionMenuItemContextType>::fromRaw(1 << 15);
    check(!validateWebExtensionMenuItemParameters(corrupt, Operation::Create), "the UI boundary rejects unknown context bits");
    corrupt = *basic;
    corrupt.iconsJSON = R"({"16":"https://other.test/icon.png"})"_s;
    check(!validateWebExtensionMenuItemParameters(corrupt, Operation::Create), "native icon paths cannot bypass resource validation");
}
static void checkIdentifiersAndValues()
{
    JSRetainPtr<JSGlobalContextRef> retained(Adopt, JSGlobalContextCreate(nullptr));
    auto context = retained.get();
    auto evaluate = [&](const String& script) { return JSEvaluateScript(context, toJSString(script).get(), nullptr, nullptr, 1, nullptr); };
    auto publish = [&](ASCIILiteral name, JSValueRef value) {
        Protected<JSValueRef> protection(context, value);
        JSObjectSetProperty(context, JSContextGetGlobalObject(context), toJSString(name).get(), value, kJSPropertyAttributeNone, nullptr);
    };
    auto verify = [&](const String& script, const char* description) { check(JSValueToBoolean(context, evaluate(script)), description); };
    auto input = [&](const String& script, WebExtensionMenuItemOperation operation = WebExtensionMenuItemOperation::Create) {
        return parseWebExtensionMenuItemInput(context, evaluate(script), operation, true, URL { "webkit-extension://fixture/pages/background.html"_s });
    };
    auto withCallback = input("({id:'001', title:'Callback', onclick:function(info) { return info.menuItemId; }})"_s);
    check(withCallback && withCallback->explicitIdentifier && withCallback->clickHandlerSpecified && withCallback->parameters.identifier == "s:001"_s && withCallback->clickHandler,
        "raw properties retain a function callback without JSON serialization or ID coercion");
    if (withCallback) {
        JSGarbageCollect(context);
        Protected<JSValueRef> argument(context, evaluate("({menuItemId:'survives'})"_s));
        JSValueRef argumentValue = argument.get(), exception = nullptr;
        auto returned = JSObjectCallAsFunction(context, withCallback->clickHandler.get(), nullptr, 1, &argumentValue, &exception);
        check(!exception && returned && JSValueIsStrictEqual(context, returned, evaluate("'survives'"_s)), "the extracted callback remains callable after garbage collection");
    }
    auto omitted = input("({title:'Generated', id:undefined, onclick:undefined})"_s);
    check(omitted && !omitted->explicitIdentifier && !omitted->clickHandler && !omitted->clickHandlerSpecified, "undefined optional properties stay omitted");
    auto clearCallback = input("({onclick:null})"_s, WebExtensionMenuItemOperation::Update);
    check(clearCallback && clearCallback->clickHandlerSpecified && !clearCallback->clickHandler, "explicit null clears an existing callback without supplying a replacement");
    auto emptyCallback = input("({title:'No callback',onclick:null})"_s);
    check(emptyCallback && emptyCallback->clickHandlerSpecified && !emptyCallback->clickHandler, "create accepts a nullable callback without retaining a function");
    auto keepCallback = input("({onclick:undefined})"_s, WebExtensionMenuItemOperation::Update);
    check(keepCallback && !keepCallback->clickHandlerSpecified, "an undefined update preserves the existing callback");
    auto updateInput = input("({checked:undefined, icons:null})"_s, WebExtensionMenuItemOperation::Update);
    check(updateInput && !updateInput->parameters.checked && !updateInput->parameters.iconsJSON.isNull() && updateInput->parameters.iconsJSON.isEmpty(), "raw updates distinguish undefined omission and explicit null clearing");
    auto readOnce = input("(globalThis.reads=0, {get title(){++reads;return 'Once';}, contexts:['page'], icons:{16:'icon.png'}})"_s);
    check(readOnce && JSValueToNumber(context, evaluate("reads"_s), nullptr) == 1, "raw conversion reads a property getter exactly once");
    check(!!input("new Proxy({title:'Proxy',contexts:new Proxy(['page'],{}),icons:new Proxy({16:'icon.png'},{})},{})"_s), "ordinary proxies retain dictionary, array and icon behavior");
    check(!!input("Object.create({title:'Inherited'})"_s), "enumerable inherited dictionary fields retain pinned conversion behavior");
    for (auto script : { "null"_s, "[]"_s, "function(){}"_s, "({title:new String('x')})"_s, "({title:'M',id:new Number(1)})"_s,
            "({title:'M',id:NaN})"_s, "({title:'M',checked:1})"_s, "({title:'M',onclick:7})"_s,
            "({title:'M',onclick:{call(){}}})"_s, "({title:'M',contexts:['page',undefined]})"_s,
            "({title:'M',icons:{16:{toJSON(){return 'icon.png';}}}})"_s, "({title:'M',toJSON(){return {title:'Changed'};}})"_s,
            "({get title(){throw Error('bad');}})"_s, "({title:'M',icons:{get 16(){throw Error('bad');}}})"_s,
            "({title:'M',contexts:new Proxy(['page'],{get(t,k){if(k==='0')throw Error('bad');return t[k];}})})"_s,
            "({title:'M',typo:function(){}})"_s })
        check(!input(script), "raw menu properties reject invalid callbacks, coercion, serialization hooks and throwing getters");
    for (auto script : { "new Proxy({}, {ownKeys(){throw Error('enumeration');}})"_s,
            "({icons:new Proxy({}, {ownKeys(){throw Error('icons');}})})"_s,
            "new Proxy({}, {getPrototypeOf(){throw Error('prototype');}})"_s,
            "(function(){let p=Proxy.revocable({},{});p.revoke();return p.proxy;})()"_s,
            "({contexts:(function(){let p=Proxy.revocable([],{});p.revoke();return p.proxy;})()})"_s }) {
        check(!input(script, WebExtensionMenuItemOperation::Update), "enumeration failure or a revoked proxy cannot become a successful empty update");
        verify("1+1 === 2"_s, "a rejected proxy leaves JavaScript usable without a pending exception");
    }
    for (auto script : { "'001'"_s, "'1'"_s, "'n:1'"_s, "'s:x'"_s, "'9007199254740993'"_s, "1"_s, "-7"_s, "0"_s, "9007199254740991"_s, "-9007199254740991"_s }) {
        Protected<JSValueRef> original(context, evaluate(script));
        auto key = parseWebExtensionMenuIdentifier(context, original.get());
        check(key && JSValueIsStrictEqual(context, original.get(), toWebAPIMenuIdentifier(context, *key)), "raw string/integer ID round trips preserve exact value and JavaScript type");
    }
    for (auto script : { "undefined"_s, "null"_s, "true"_s, "false"_s, "''"_s, "[]"_s, "({})"_s, "new String('a')"_s,
            "new Number(1)"_s, "1.5"_s, "NaN"_s, "Infinity"_s, "-Infinity"_s, "9007199254740992"_s, "'a\\0b'"_s })
        check(!parseWebExtensionMenuIdentifier(context, evaluate(script)), "raw ID conversion rejects coercion and unsafe numeric identities");
    for (auto key : { ""_s, "s:"_s, "n:"_s, "n:01"_s, "n:+1"_s, "n:-0"_s, "n:1.0"_s, "n:1e3"_s,
            "n:9007199254740992"_s, "n:-9007199254740992"_s, "n:18446744073709551615"_s, "n: 1"_s, "n:1x"_s, "x:a"_s })
        check(!decodeWebExtensionMenuIdentifier(key) && JSValueIsNull(context, toWebAPIMenuIdentifier(context, key)), "invalid or noncanonical internal IDs cannot alias a valid menu");
    WebExtensionMenuItemParameters item;
    item.identifier = "s:001"_s;
    item.parentIdentifier = "n:7"_s;
    item.type = WebExtensionMenuItemType::Checkbox;
    item.checked = true;
    WebExtensionMenuItemContextParameters details;
    details.frameIdentifier = WebExtensionFrameConstants::MainFrameIdentifier;
    details.frameURL = URL { "https://example.test/page"_s };
    details.linkURL = URL { "https://example.test/link"_s };
    details.linkText = "link"_s;
    details.selectionString = "selected"_s;
    details.sourceURL = URL { "https://example.test/image.png"_s };
    details.types = WebExtensionMenuItemContextType::Image;
    details.editable = true;
    publish("info"_s, toWebAPIMenuClickInfo(context, item, false, details));
    verify("info.menuItemId === '001' && info.parentMenuItemId === 7 && info.checked === true && info.wasChecked === false"_s, "click info preserves ID types and before/after checkbox state");
    verify("info.frameId === 0 && info.pageUrl === 'https://example.test/page' && !('frameUrl' in info) && info.editable === true"_s, "main-frame click context maps its sentinel to frame zero");
    verify("info.linkUrl === 'https://example.test/link' && info.linkText === 'link' && info.selectionText === 'selected' && info.mediaType === 'image' && info.srcUrl.endsWith('/image.png')"_s, "click info contains actual link, selection and media fields");
    publish("other"_s, toWebAPIMenuClickInfo(context, item, false, details));
    verify("other !== info && (info.menuItemId = 'changed', other.menuItemId === '001')"_s, "independent listeners get independent event objects");
    JSGarbageCollect(context);
    verify("other.menuItemId === '001' && other.selectionText === 'selected'"_s, "published event values survive garbage collection");
    item.type = WebExtensionMenuItemType::Normal;
    item.parentIdentifier = std::nullopt;
    details = { };
    publish("empty"_s, toWebAPIMenuClickInfo(context, item, false, details));
    verify("empty.menuItemId === '001' && empty.editable === false && Object.keys(empty).length === 2"_s, "absent frame, parent and checkbox metadata is omitted from ordinary clicks");
    details.frameIdentifier = WebExtensionFrameIdentifier { 42 };
    details.frameURL = URL { "https://example.test/frame"_s };
    details.selectionString = emptyString();
    publish("frame"_s, toWebAPIMenuClickInfo(context, item, false, details));
    verify("frame.frameId === 42 && frame.frameUrl.endsWith('/frame') && !('pageUrl' in frame) && frame.selectionText === ''"_s, "subframe and explicit empty selection metadata retain their meaning");
    details.frameIdentifier = WebExtensionFrameIdentifier { 9007199254740992ULL };
    check(JSValueIsNull(context, toWebAPIMenuClickInfo(context, item, false, details)), "unsafe frame numbers are not rounded into another frame identity");
    details = { };
    item.type = WebExtensionMenuItemType::Separator;
    check(JSValueIsNull(context, toWebAPIMenuClickInfo(context, item, false, details)), "separators cannot synthesize clicks");
    item.type = std::nullopt;
    check(JSValueIsNull(context, toWebAPIMenuClickInfo(context, item, false, details)), "clicks require a known item type");
    item.type = WebExtensionMenuItemType::Normal;
    item.parentIdentifier = "n:01"_s;
    check(JSValueIsNull(context, toWebAPIMenuClickInfo(context, item, false, details)), "corrupt parent identity rejects the entire event");
}
static void checkTreeValidation()
{
    HashMap<String, String> parents;
    parents.set("child"_s, "root"_s);
    parents.set("grandchild"_s, "child"_s);
    Function<std::optional<String>(const String&)> parentForItem = [&](const String& identifier) -> std::optional<String> {
        auto found = parents.find(identifier);
        if (found == parents.end())
            return std::nullopt;
        return found->value;
    };
    auto canMove = [&](const String& item, const String& parent) { return canReparentWebExtensionMenuItem(item, parent, parentForItem); };
    check(!canMove("root"_s, "root"_s), "a menu item cannot parent itself");
    check(!canMove("root"_s, "child"_s) && !canMove("root"_s, "grandchild"_s), "a menu item cannot move below an immediate or distant descendant");
    check(canMove("grandchild"_s, "root"_s), "moving a menu item higher in its existing tree is allowed");
    check(canMove("grandchild"_s, "child"_s), "retaining an existing parent is allowed");
    check(canMove("child"_s, "other"_s), "moving to an unrelated tree is allowed");
    check(parents.get("child"_s) == "root"_s && parents.get("grandchild"_s) == "child"_s, "validation leaves the original tree unchanged");
    parents.set("root"_s, "grandchild"_s);
    check(!canMove("new"_s, "root"_s), "an existing corrupt cycle fails instead of looping");
}
class MenuTestApplication final : public BApplication {
public:
    bool didRun { false };
    MenuTestApplication() : BApplication("application/x-vnd.Kunanyi-Summit-menu-item-tests") { }
    void ReadyToRun() override
    {
        WebKit::InitializeWebKit2();
        checkProperties();
        checkIdentifiersAndValues();
        checkTreeValidation();
        didRun = true;
        PostMessage(B_QUIT_REQUESTED);
    }
};
int main()
{
    MenuTestApplication application;
    application.Run();
    check(application.didRun, "the native menu checks ran from ReadyToRun");
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
