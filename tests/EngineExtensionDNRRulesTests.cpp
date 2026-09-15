#include "config.h"
#include "WebExtensionDeclarativeNetRequestRulesHaiku.cpp"
#include "WebExtensionDeclarativeNetRequestURLFilter.cpp"
#include <Application.h>
#include <JavaScriptCore/JavaScript.h>
#include <JavaScriptCore/JSRetainPtr.h>
#include <WebCore/CompiledContentExtension.h>
#include <WebCore/ContentExtensionCompiler.h>
#include <WebCore/ContentExtensionParser.h>
#include <WebCore/ContentExtensionsBackend.h>
#include <WebCore/ResourceLoadInfo.h>
#include <WebCore/ResourceRequest.h>
#include <wtf/MainThread.h>
#include <wtf/text/MakeString.h>
#include <cstdio>

using namespace WebCore;
using namespace WebCore::ContentExtensions;
static unsigned passed;
static unsigned failed;
static void check(bool ok, const char* description)
{
    ++(ok ? passed : failed);
    printf("%s %s\n", ok ? "PASS" : "FAIL", description);
}

class MemoryRules final : public CompiledContentExtension {
public:
    static Ref<MemoryRules> create() { return adoptRef(*new MemoryRules); }
    std::span<const uint8_t> urlFiltersBytecode() const final { return request.span(); }
    std::span<const uint8_t> topURLFiltersBytecode() const final { return top.span(); }
    std::span<const uint8_t> frameURLFiltersBytecode() const final { return frame.span(); }
    std::span<const uint8_t> serializedActions() const final { return actions.span(); }
    Vector<DFABytecode> request;
    Vector<DFABytecode> top;
    Vector<DFABytecode> frame;
    Vector<SerializedActionByte> actions;
};

class MemoryCompiler final : public ContentExtensionCompilationClient {
public:
    void writeSource(String&& value) final { source = WTF::move(value); }
    void writeActions(Vector<SerializedActionByte>&& value) final { output->actions = WTF::move(value); }
    void writeURLFiltersBytecode(Vector<DFABytecode>&& value) final { output->request.appendVector(WTF::move(value)); }
    void writeTopURLFiltersBytecode(Vector<DFABytecode>&& value) final { output->top.appendVector(WTF::move(value)); }
    void writeFrameURLFiltersBytecode(Vector<DFABytecode>&& value) final { output->frame.appendVector(WTF::move(value)); }
    void finalize() final { finalized = true; }
    Ref<MemoryRules> output { MemoryRules::create() };
    String source;
    bool finalized { false };
};

static RefPtr<MemoryRules> compile(const String& json)
{
    auto parsed = parseRuleList(json, CSSSelectorsAllowed::No);
    check(!!parsed, "real rule-list parser accepts the fixture");
    if (!parsed) {
        printf("Parser error: %s\n", parsed.error().message().c_str());
        return nullptr;
    }
    MemoryCompiler compiler;
    auto error = compileRuleList(compiler, String(json), WTF::move(*parsed));
    check(!error && compiler.finalized && compiler.source == json
        && !compiler.output->request.isEmpty() && !compiler.output->top.isEmpty() && !compiler.output->frame.isEmpty(),
        "real compiler finalizes source, actions and all three bytecode streams");
    if (error || !compiler.finalized) {
        printf("Compiler error: %s\n", error.message().c_str());
        return nullptr;
    }
    return compiler.output.copyRef();
}

static auto actions(ContentExtensionsBackend& backend, const char* request, const char* top = nullptr, const char* frame = nullptr,
    ResourceType type = ResourceType::Script, RequestMethod method = RequestMethod::Post)
{
    ResourceLoadInfo info { URL { String::fromUTF8(request) }, top ? URL { String::fromUTF8(top) } : URL { },
        frame ? URL { String::fromUTF8(frame) } : URL { }, { type }, false, method };
    Vector<DeserializedAction> result;
    for (auto& list : backend.actionsForResourceLoad(info))
        result.appendVector(WTF::move(list.actions));
    return result;
}

template<typename T> static unsigned count(const Vector<DeserializedAction>& actions)
{
    unsigned result = 0;
    for (auto& action : actions)
        result += std::holds_alternative<T>(action.data());
    return result;
}

using namespace WebKit;
static std::expected<WebExtensionDeclarativeNetRequestTranslationHaiku, String> translate(const String& input,
    WebExtensionDeclarativeNetRequestLimitsHaiku limits = { })
{
    auto parsed = JSON::Value::parseJSON(input);
    auto array = parsed ? parsed->asArray() : nullptr;
    if (!array)
        return makeUnexpected("Invalid test input"_s);
    Vector<WebExtensionDeclarativeNetRequestRulesetHaiku> sources;
    sources.append({ "rules"_s, array.releaseNonNull() });
    auto before = sources[0].rules->toJSONString();
    auto result = translateDeclarativeNetRequestRulesHaiku(sources, limits);
    check(before == sources[0].rules->toJSONString(), "translation preserves source rules");
    return result;
}
static RefPtr<MemoryRules> compileDNR(const String& input)
{
    auto result = translate(input);
    check(!!result, "native DNR translation accepts valid rules");
    if (!result) { printf("DNR error: %s\n", result.error().utf8().legacyCStringPointer()); return nullptr; }
    return compile(result->json);
}
int main()
{
    BApplication application("application/x-vnd.Kunanyi-Summit-dnr-rules-tests");
    WTF::initializeMainThread();
    JSRetainPtr context(Adopt, JSGlobalContextCreate(nullptr));
    ContentExtensionsBackend backend;
    auto rules = compileDNR(R"JSON([
        {"id":1,"priority":1,"action":{"type":"block"},"condition":{"urlFilter":"||ads.test^","resourceTypes":["script"]}},
        {"id":2,"priority":2,"action":{"type":"allow"},"condition":{"urlFilter":"||ads.test/safe","resourceTypes":["script"]}},
        {"id":3,"priority":3,"action":{"type":"block"},"condition":{"urlFilter":"||ads.test/safe/blocked","resourceTypes":["script"]}}
    ])JSON"_s);
    if (!rules) return 1;
    backend.addContentExtension("dnr"_s, rules.releaseNonNull(), URL { }, ContentExtension::ShouldCompileCSS::No);
    for (auto url : { "https://ads.test/tracker", "https://sub.ads.test/tracker", "http://ads.test:8080/tracker", "https://ads.test/safe/blocked" })
        check(count<BlockLoadAction>(actions(backend, url)) == 1, "highest matching blocker applies once through real backend");
    for (auto url : { "https://ads.test/safe", "https://ads.test/safe/okay", "https://notads.test/tracker", "https://ads.test.evil/tracker", "https://site.test/?host=ads.test", "https://ads.test@site.test/tracker" })
        check(actions(backend, url).isEmpty(), "allow priority and URL boundaries prevent unintended blocks");
    check(actions(backend, "https://ads.test/tracker", nullptr, nullptr, ResourceType::Image).isEmpty(), "script-only rule does not block images");
    check(actions(backend, "https://ads.test/tracker", nullptr, nullptr, ResourceType::TopDocument).isEmpty(), "script-only rule does not block navigation");
    for (bool allowFirst : { false, true }) {
        auto allow = R"JSON({"id":1,"action":{"type":"allow"},"condition":{}})JSON"_s;
        auto block = R"JSON({"id":2,"action":{"type":"block"},"condition":{}})JSON"_s;
        auto compiled = compileDNR(makeString('[', allowFirst ? allow : block, ',', allowFirst ? block : allow, ']'));
        if (!compiled) return 1;
        backend.removeAllContentExtensions();
        backend.addContentExtension("tie"_s, compiled.releaseNonNull(), URL { }, ContentExtension::ShouldCompileCSS::No);
        check(actions(backend, "https://site.test/").isEmpty(), "equal-priority allow wins regardless of declaration order");
    }
    auto defaults = compileDNR(R"JSON([{ "id":1,"action":{"type":"block"},"condition":{} }])JSON"_s);
    if (!defaults) return 1;
    backend.removeAllContentExtensions();backend.addContentExtension("defaults"_s, defaults.releaseNonNull(), URL { }, ContentExtension::ShouldCompileCSS::No);
    check(actions(backend, "https://site.test/", nullptr, nullptr, ResourceType::TopDocument).isEmpty(), "omitted resourceTypes excludes main-frame navigation");
    for (auto type : { ResourceType::ChildDocument, ResourceType::Script, ResourceType::Image, ResourceType::Font, ResourceType::StyleSheet, ResourceType::Fetch, ResourceType::Media, ResourceType::WebSocket, ResourceType::Ping, ResourceType::CSPReport, ResourceType::Other, ResourceType::SVGDocument, ResourceType::Popup })
        check(count<BlockLoadAction>(actions(backend, "https://site.test/", nullptr, nullptr, type)) == 1, "default resource selection blocks each non-main resource once");
    auto caseRule = compileDNR(R"JSON([{ "id":1,"action":{"type":"block"},"condition":{"urlFilter":"Tracker","isUrlFilterCaseSensitive":true} }])JSON"_s);
    if (!caseRule) return 1;
    backend.removeAllContentExtensions();backend.addContentExtension("case"_s, caseRule.releaseNonNull(), URL { }, ContentExtension::ShouldCompileCSS::No);
    check(count<BlockLoadAction>(actions(backend, "https://site.test/Tracker")) == 1 && actions(backend, "https://site.test/tracker").isEmpty(), "case-sensitive URL condition survives compilation");
    auto empty = translate("[]"_s);
    check(empty && !empty->ruleCount && empty->json == "[]"_s, "empty ruleset remains explicitly empty");
    for (auto id : { "0"_s, "-1"_s, "1.5"_s, "2147483648"_s, "true"_s, "null"_s, "\"1\""_s })
        check(!translate(makeString("[{\"id\":"_s, id, ",\"action\":{\"type\":\"block\"},\"condition\":{}}]"_s)), "invalid rule id cannot reach compilation");
    for (auto input : {
        R"JSON([null])JSON"_s,
        R"JSON([{"id":1,"priority":false,"action":{"type":"block"},"condition":{}}])JSON"_s,
        R"JSON([{"id":1,"action":{"type":"block"},"condition":{}},{"id":1,"action":{"type":"allow"},"condition":{}}])JSON"_s,
        R"JSON([{"id":1,"action":{"type":"redirect","redirect":{"url":"https://site.test/"}},"condition":{}}])JSON"_s,
        R"JSON([{"id":1,"action":{"type":"allowAllRequests"},"condition":{"resourceTypes":["main_frame"]}}])JSON"_s,
        R"JSON([{"id":1,"action":{"type":"block"},"condition":{"requestMethods":["post"]}}])JSON"_s,
        R"JSON([{"id":1,"action":{"type":"block"},"condition":{"initiatorDomains":["site.test"]}}])JSON"_s,
        R"JSON([{"id":1,"action":{"type":"block"},"condition":{"resourceTypes":["other"]}}])JSON"_s,
        R"JSON([{"id":1,"action":{"type":"block"},"condition":{"excludedResourceTypes":["ping"]}}])JSON"_s,
        R"JSON([{"id":1,"action":{"type":"block"},"condition":{"resourceTypes":[]}}])JSON"_s,
        R"JSON([{"id":1,"action":{"type":"block"},"condition":{"resourceTypes":["script","script"]}}])JSON"_s,
        R"JSON([{"id":1,"action":{"type":"block"},"condition":{"urlFilter":"","regexFilter":".*"}}])JSON"_s })
        check(!translate(input), "malformed or unimplemented semantics reject the entire ruleset");
    auto one = R"JSON([{"id":1,"action":{"type":"block"},"condition":{}}])JSON"_s;
    check(!translate(one, { 0, 100000 }), "rule-count budget is enforced before conversion");
    check(!translate(one, { 10, 10 }), "output-size budget is enforced");
    auto array = JSON::Value::parseJSON(one)->asArray();
    Vector<WebExtensionDeclarativeNetRequestRulesetHaiku> multiple;
    multiple.append({ "static"_s, Ref { *array } });multiple.append({ "_dynamic"_s, Ref { *array } });
    auto distinct = translateDeclarativeNetRequestRulesHaiku(multiple);
    check(distinct && distinct->ruleCount == 2 && distinct->identifiers.get("static"_s).contains(1) && distinct->identifiers.get("_dynamic"_s).contains(1), "rule identifiers are independent across named rulesets");
    multiple.append({ "static"_s, Ref { *array } });
    check(!translateDeclarativeNetRequestRulesHaiku(multiple), "duplicate ruleset identity is rejected");
    printf("%u checks passed, %u failed\n", passed, failed);
    return failed ? 1 : 0;
}
