#include "config.h"
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

int main()
{
    BApplication application("application/x-vnd.Kunanyi-Summit-content-rule-pipeline-tests");
    WTF::initializeMainThread();
    JSRetainPtr context(Adopt, JSGlobalContextCreate(nullptr));

    auto compound = compile(R"JSON([
      {"trigger":{"url-filter":"^https://allowed\\.test/","url-filter-alternatives":["^https://allowed\\.test/TOKEN","^https://second\\.test/TOKEN","^https://foreign.invalid/TOKEN"],
        "url-filter-is-case-sensitive":true,"request-method":"post","resource-type":["script"],"url-conditions":[
          {"type":"if-request-url","urls":["\\.test/"]}, {"type":"unless-request-url","urls":["excluded"]},
          {"type":"if-top-url","urls":["^https://top\\.test/"]}, {"type":"unless-top-url","urls":["private"]},
          {"type":"if-frame-url","urls":["^https://frame\\.test/"]}, {"type":"unless-frame-url","urls":["blocked"]}]},
       "action":{"type":"modify-headers","priority":1,"request-headers":[{"header":"x-pipeline","operation":"append","value":"once"}]}},
      {"trigger":{"url-filter":".*"},"action":{"type":"block-cookies"}}
    ])JSON"_s);
    if (!compound)
        return 1;
    ContentExtensionsBackend backend;
    backend.addContentExtension("compound"_s, compound.releaseNonNull(), URL { }, ContentExtension::ShouldCompileCSS::No);
    auto selected = actions(backend, "https://allowed.test/TOKEN", "https://top.test/", "https://frame.test/");
    check(selected.size() == 2 && count<ModifyHeadersAction>(selected) == 1 && count<BlockCookiesAction>(selected) == 1,
        "real backend deserializes one header action for overlapping alternatives and retains an unconditional action");
    ResourceRequest request(URL { "https://allowed.test/TOKEN"_s });
    request.setHTTPHeaderField("x-pipeline"_s, "seed"_s);
    HashMap<String, ModifyHeadersAction::ModifyHeadersOperationType> applied;
    for (auto& action : selected) {
        if (auto* headerAction = std::get_if<ModifyHeadersAction>(&action.data())) {
            auto owned = headerAction->isolatedCopy();
            owned.applyToRequest(request, applied);
        }
    }
    check(request.httpHeaderField("x-pipeline"_s) == "seed; once"_s, "deserialized append action changes an actual ResourceRequest once");
    check(count<ModifyHeadersAction>(actions(backend, "https://second.test/TOKEN", "https://top.test/", "https://frame.test/")) == 1,
        "alternative-only match reaches the real backend");
    struct Case { const char* request; const char* top; const char* frame; };
    for (auto& test : { Case { "https://allowed.test/excluded", "https://top.test/", "https://frame.test/" },
        Case { "https://allowed.test/TOKEN", "https://other.test/", "https://frame.test/" },
        Case { "https://allowed.test/TOKEN", "https://top.test/private", "https://frame.test/" },
        Case { "https://allowed.test/TOKEN", "https://top.test/", "https://other.test/" },
        Case { "https://allowed.test/TOKEN", "https://top.test/", "https://frame.test/blocked" },
        Case { "https://foreign.invalid/TOKEN", "https://top.test/", "https://frame.test/" },
        Case { "https://second.test/token", "https://top.test/", "https://frame.test/" } }) {
        auto result = actions(backend, test.request, test.top, test.frame);
        check(result.size() == 1 && count<BlockCookiesAction>(result) == 1, "failed condition keeps unrelated serialized action intact");
    }
    for (auto& test : { std::pair { ResourceType::Image, RequestMethod::Post }, std::pair { ResourceType::Script, RequestMethod::Get } }) {
        auto result = actions(backend, "https://allowed.test/TOKEN", "https://top.test/", "https://frame.test/", test.first, test.second);
        check(result.size() == 1 && count<BlockCookiesAction>(result) == 1, "real load flags reject a mismatched type or method");
    }

    auto unflagged = compile(R"JSON([{"trigger":{"url-filter":"^https://allowed\\.test/","url-filter-alternatives":[".*"]},
      "action":{"type":"modify-headers","priority":1,"request-headers":[{"header":"x-universal","operation":"append","value":"once"}]}}])JSON"_s);
    if (!unflagged)
        return 1;
    ContentExtensionsBackend unionBackend;
    unionBackend.addContentExtension("union"_s, unflagged.releaseNonNull(), URL { }, ContentExtension::ShouldCompileCSS::No);
    for (const char* url : { "https://allowed.test/", "https://other.test/", "https://allowed.test/again" }) {
        auto result = actions(unionBackend, url);
        check(result.size() == 1 && count<ModifyHeadersAction>(result) == 1,
            "unflagged specific and universal alternatives deserialize one action across both backend match collections");
    }
    auto separate = compile(R"JSON([
      {"trigger":{"url-filter":".*","url-conditions":[{"type":"if-frame-url","urls":["/first"]}]},"action":{"type":"block"}},
      {"trigger":{"url-filter":".*","url-conditions":[{"type":"if-frame-url","urls":["/second"]}]},"action":{"type":"block"}}
    ])JSON"_s);
    if (!separate)
        return 1;
    ContentExtensionsBackend separateBackend;
    separateBackend.addContentExtension("separate"_s, separate.releaseNonNull(), URL { }, ContentExtension::ShouldCompileCSS::No);
    for (auto& test : { std::pair { "https://frame.test/first", 1u }, std::pair { "https://frame.test/second", 1u },
        std::pair { "https://frame.test/first/second", 2u }, std::pair { "https://frame.test/neither", 0u } }) {
        auto result = actions(separateBackend, "https://request.test/", nullptr, test.first);
        check(result.size() == test.second && count<BlockLoadAction>(result) == test.second,
            "serializer preserves distinct action locations for otherwise identical rules with different conditions");
    }

    // A null URL is a valid cache key. Its first lookup must evaluate universal
    // conditions, and switching away and back must produce the same result.
    for (auto type : { "if-top-url"_s, "unless-top-url"_s, "if-frame-url"_s, "unless-frame-url"_s }) {
        bool excluded = StringView(type).startsWith("unless"_s);
        auto json = makeString("[{\"trigger\":{\"url-filter\":\".*\",\"url-conditions\":[{\"type\":\""_s,
            type, "\",\"urls\":[\".*\"]}]},\"action\":{\"type\":\"block\"}}]"_s);
        auto rules = compile(json);
        if (!rules)
            return 1;
        ContentExtensionsBackend cache;
        cache.addContentExtension("cache"_s, rules.releaseNonNull(), URL { }, ContentExtension::ShouldCompileCSS::No);
        for (bool populated : { false, false, true, true, false }) {
            auto result = actions(cache, "https://request.test/", populated ? "https://top.test/" : nullptr, populated ? "https://frame.test/" : nullptr);
            check(count<BlockLoadAction>(result) == (excluded ? 0 : 1) && result.size() == (excluded ? 0 : 1),
                "real URL caches evaluate first null input, repeated input and transitions correctly");
        }
    }

    for (auto type : { "if-top-url"_s, "unless-top-url"_s, "if-frame-url"_s, "unless-frame-url"_s }) {
        bool excluded = StringView(type).startsWith("unless"_s);
        auto rules = compile(makeString("[{\"trigger\":{\"url-filter\":\".*\",\"request-method\":\"post\",\""_s,
            type, "\":[\"match\"]},\"action\":{\"type\":\"block\"}}]"_s));
        if (!rules)
            return 1;
        ContentExtensionsBackend legacy;
        legacy.addContentExtension("legacy"_s, rules.releaseNonNull(), URL { }, ContentExtension::ShouldCompileCSS::No);
        for (bool matched : { false, true, false }) {
            const char* page = matched ? "https://match.test/" : "https://other.test/";
            check(count<BlockLoadAction>(actions(legacy, "https://request.test/", page, page)) == (matched != excluded ? 1 : 0),
                "real legacy cache matches method-bearing action identities");
        }
        check(actions(legacy, "https://request.test/", "https://match.test/", "https://match.test/", ResourceType::Script, RequestMethod::Get).isEmpty(),
            "legacy cache does not bypass real request method filtering");
    }

    for (auto extra : { R"JSON("url-filter-alternatives":[])JSON"_s, R"JSON("url-filter-alternatives":[3])JSON"_s,
        R"JSON("url-conditions":[])JSON"_s, R"JSON("url-conditions":[{"type":"if-top-url","urls":["a"]}],"if-top-url":["b"])JSON"_s,
        R"JSON("url-conditions":[{"type":"if-top-url","urls":["a"]}],"top-url-filter-is-case-sensitive":true)JSON"_s,
        R"JSON("url-conditions":[{"type":"if-top-url","urls":["a"]},{"type":"if-top-url","urls":["b"]}])JSON"_s }) {
        auto json = makeString("[{\"trigger\":{\"url-filter\":\".*\","_s, extra, "},\"action\":{\"type\":\"block\"}}]"_s);
        check(!parseRuleList(json, CSSSelectorsAllowed::No), "real rule-list parser rejects invalid or mixed compound fields");
    }
    auto invalid = R"JSON([{"trigger":{"url-filter":".*","url-filter-alternatives":["a|b"]},"action":{"type":"block"}}])JSON"_s;
    auto parsed = parseRuleList(invalid, CSSSelectorsAllowed::No);
    check(!!parsed, "shape-valid unsupported alternative reaches actual compiler validation");
    if (parsed) {
        MemoryCompiler compiler;
        auto error = compileRuleList(compiler, String(invalid), WTF::move(*parsed));
        check(!!error && !compiler.finalized, "real compiler rejects unsupported alternative without finalizing a rule list");
    }
    backend.removeAllContentExtensions();
    check(actions(backend, "https://allowed.test/TOKEN", "https://top.test/", "https://frame.test/").isEmpty(), "removing actual rule lists clears backend actions");
    printf("%u checks passed, %u failed\n", passed, failed);
    return failed ? 1 : 0;
}
