#include "config.h"
#include "ContentExtensionURLConditions.h"
#include <Application.h>
#include <JavaScriptCore/JavaScript.h>
#include <JavaScriptCore/JSRetainPtr.h>
#include <WebCore/CombinedURLFilters.h>
#include <WebCore/ContentExtensionRule.h>
#include <WebCore/DFABytecodeCompiler.h>
#include <WebCore/DFABytecodeInterpreter.h>
#include <WebCore/NFAToDFA.h>
#include <wtf/MainThread.h>
#include <wtf/text/MakeString.h>
#include <cstdio>

using namespace WebCore::ContentExtensions;
static unsigned passed;
static unsigned failed;
static void check(bool ok, const char* description)
{
    ++(ok ? passed : failed);
    printf("%s %s\n", ok ? "PASS" : "FAIL", description);
}

static auto parse(const char* json)
{
    return parseURLConditionGroups(*JSON::Value::parseJSON(String::fromUTF8(json)));
}

static uint64_t tag(uint32_t location, URLConditionType type)
{
    return (static_cast<uint64_t>(type) << 32) | location;
}

static uint64_t candidate(uint32_t location, const URLConditionGroups& groups, ResourceFlags flags = 0)
{
    return (static_cast<uint64_t>(flags | static_cast<ResourceFlags>(ActionCondition::Conjunction) | urlConditionRequirements(groups)) << 32) | location;
}

struct Program {
    CombinedURLFilters filters;
    URLFilterParser parser { filters };
    URLConditionActions universal;
    Vector<DFABytecode> bytecode;

    bool compile()
    {
        bool result = filters.processNFAs(50000, [&](NFA&& nfa) {
            auto dfa = NFAToDFA::convert(WTF::move(nfa));
            if (!dfa)
                return false;
            dfa->minimize();
            DFABytecodeCompiler(*dfa, bytecode).compile();
            return true;
        });
        // Match-all condition tags only carry condition bits, so they do not
        // need resource filtering. Test the actual bytecode encoding of those
        // bits too by putting them on an otherwise empty DFA root.
        if (!universal.isEmpty()) {
            auto dfa = DFA::empty();
            auto& root = dfa.nodes[dfa.root];
            root.setActions(dfa.actions.size(), static_cast<uint16_t>(universal.size()));
            for (auto action : universal)
                dfa.actions.append(action);
            DFABytecodeCompiler(dfa, bytecode).compile();
        }
        return result;
    }

    URLConditionActions run(const String& url, ResourceFlags flags = AllResourceFlags) const
    {
        if (bytecode.isEmpty())
            return { };
        DFABytecodeInterpreter interpreter(bytecode.span());
        auto result = interpreter.interpret(url, flags);
        for (auto action : interpreter.actionsMatchingEverything())
            result.add(action);
        return result;
    }
};

struct Programs {
    Program request;
    Program top;
    Program frame;
    bool add(const URLConditionGroups& groups, uint32_t location)
    {
        return compileURLConditions(groups, location, request.parser, request.universal,
            top.parser, top.universal, frame.parser, frame.universal) == URLFilterParser::Ok;
    }
    bool compile() { return request.compile() && top.compile() && frame.compile(); }
    URLConditionActions evaluate(const String& url, const String& topURL, const String& frameURL, ResourceFlags flags) const
    {
        auto actions = request.run(url, flags);
        filterActionsByURLConditions(actions, top.run(topURL), frame.run(frameURL));
        return actions;
    }
};

int main()
{
    BApplication application("application/x-vnd.Kunanyi-Summit-extension-dnr-conditions-tests");
    WTF::initializeMainThread();
    JSRetainPtr context(Adopt, JSGlobalContextCreate(nullptr));

    constexpr URLConditionType types[] { URLConditionType::IfRequestURL, URLConditionType::UnlessRequestURL,
        URLConditionType::IfTopURL, URLConditionType::UnlessTopURL, URLConditionType::IfFrameURL, URLConditionType::UnlessFrameURL };
    ResourceFlags allTypes = 0;
    for (auto type : types)
        allTypes |= static_cast<ResourceFlags>(type);
    check(allTypes == URLConditionMask && !(ActionFlagMask & (uint64_t { 1 } << 63)), "condition tags fit 31 flag bits and avoid hash sentinels");

    for (const char* json : { "null", "{}", "[]", "[null]", "[{}]", R"([{"type":"unknown","urls":["a"]}])",
        R"([{"type":3,"urls":["a"]}])", R"([{"type":"if-request-url"}])", R"([{"type":"if-request-url","urls":[]}])",
        R"([{"type":"if-request-url","urls":"a"}])", R"([{"type":"if-request-url","urls":[null]}])",
        R"([{"type":"if-request-url","urls":[3]}])", R"([{"type":"if-request-url","urls":[""]}])",
        R"([{"type":"if-request-url","urls":["é"]}])", R"([{"type":"if-request-url","urls":["a\u0000b"]}])",
        R"([{"type":"if-request-url","urls":["a"],"case-sensitive":null}])", R"([{"type":"if-request-url","urls":["a"],"case-sensitive":1}])",
        R"([{"type":"if-request-url","urls":["a"],"extra":true}])",
        R"([{"type":"if-request-url","urls":["a"]},{"type":"if-request-url","urls":["b"]}])" }) {
        auto result = parse(json);
        check(!result && !result.error().isEmpty(), "malformed condition group reports an error");
    }

    auto groups = parse(R"([
        {"type":"if-request-url","urls":["^https://allowed\\.test/","^https://second\\.test/"]},
        {"type":"unless-request-url","urls":["/excluded"]},
        {"type":"if-top-url","urls":["^https://top\\.test/"]},
        {"type":"unless-top-url","urls":["/private"]},
        {"type":"if-frame-url","urls":["^https://frame\\.test/"]},
        {"type":"unless-frame-url","urls":["/blocked"]}
    ])");
    check(groups && groups->size() == 6 && urlConditionRequirements(*groups) == URLConditionMask, "all six independent condition kinds parse together");
    Programs program;
    auto flags = static_cast<ResourceFlags>(ResourceType::Script) | static_cast<ResourceFlags>(RequestMethod::Post);
    uint64_t action = candidate(7, *groups, flags);
    check(program.add(*groups, 7) && program.request.parser.addPattern("^https://"_s, false, action) == URLFilterParser::Ok,
        "production condition compiler routes six groups into three bytecode streams");
    // This unrelated action must survive an exclusion affecting action 7.
    uint64_t unrelated = (static_cast<uint64_t>(flags) << 32) | 19;
    program.request.parser.addPattern("^https://"_s, false, unrelated);
    check(program.compile(), "condition streams compile through actual NFA/DFA bytecode");
    auto accepted = program.evaluate("https://allowed.test/a"_s, "https://top.test/"_s, "https://frame.test/"_s, flags);
    check(accepted.contains(action) && accepted.contains(unrelated) && accepted.size() == 2, "matching request returns only real actions with no auxiliary tags");
    auto second = program.evaluate("https://second.test/a"_s, "https://top.test/"_s, "https://frame.test/"_s, flags);
    check(second.contains(action), "patterns within one inclusion group form a union");
    struct Failure { const char* request; const char* top; const char* frame; };
    for (auto& test : { Failure { "https://other.test/a", "https://top.test/", "https://frame.test/" },
        Failure { "https://allowed.test/excluded", "https://top.test/", "https://frame.test/" },
        Failure { "https://allowed.test/a", "https://other.test/", "https://frame.test/" },
        Failure { "https://allowed.test/a", "https://top.test/private", "https://frame.test/" },
        Failure { "https://allowed.test/a", "https://top.test/", "https://other.test/" },
        Failure { "https://allowed.test/a", "https://top.test/", "https://frame.test/blocked" } }) {
        auto actions = program.evaluate(String::fromUTF8(test.request), String::fromUTF8(test.top), String::fromUTF8(test.frame), flags);
        check(!actions.contains(action) && actions.contains(unrelated) && actions.size() == 1,
            "each failed clause removes its own action while retaining an unrelated rule");
    }
    auto getFlags = static_cast<ResourceFlags>(ResourceType::Script) | static_cast<ResourceFlags>(RequestMethod::Get);
    check(program.evaluate("https://allowed.test/a"_s, "https://top.test/"_s, "https://frame.test/"_s, getFlags).isEmpty(), "request method filtering remains active for real requests");
    auto imageFlags = static_cast<ResourceFlags>(ResourceType::Image) | static_cast<ResourceFlags>(RequestMethod::Post);
    check(program.evaluate("https://allowed.test/a"_s, "https://top.test/"_s, "https://frame.test/"_s, imageFlags).isEmpty(), "resource type filtering remains active");

    Trigger alternatives;
    alternatives.urlFilter = "^https://allowed\\.test/"_s;
    alternatives.urlFilterAlternatives = { "^https://allowed\\.test/TOKEN"_s, "^https://second\\.test/TOKEN"_s };
    alternatives.urlFilterIsCaseSensitive = true;
    alternatives.urlConditionGroups = { { URLConditionType::UnlessFrameURL, { "excluded"_s }, false } };
    alternatives.flags = flags | static_cast<ResourceFlags>(ActionCondition::Conjunction) | urlConditionRequirements(alternatives.urlConditionGroups);
    Programs unionProgram;
    auto unionAction = candidate(41, alternatives.urlConditionGroups, flags);
    check(compileTriggerURLFilters(alternatives, 41, unionProgram.request.parser, unionProgram.request.universal) == URLFilterParser::Ok
        && unionProgram.add(alternatives.urlConditionGroups, 41) && unionProgram.compile(), "production trigger compiler compiles alternatives and attached exclusions");
    for (auto& test : { std::pair { "https://allowed.test/TOKEN", true }, std::pair { "https://allowed.test/anything", true },
        std::pair { "https://second.test/TOKEN", true }, std::pair { "https://second.test/token", false }, std::pair { "https://other.test/TOKEN", false } }) {
        auto actions = unionProgram.evaluate(String::fromUTF8(test.first), emptyString(), emptyString(), flags);
        check(actions.contains(unionAction) == test.second && actions.size() == (test.second ? 1 : 0), "overlapping URL alternatives return one action with shared case sensitivity");
    }
    check(unionProgram.evaluate("https://second.test/TOKEN"_s, emptyString(), "excluded"_s, flags).isEmpty(), "condition exclusion applies to every alternative");
    check(unionProgram.evaluate("https://second.test/TOKEN"_s, emptyString(), emptyString(), getFlags).isEmpty(), "method flags apply to every alternative");
    alternatives.urlFilterAlternatives.append(".*"_s);
    Program universalAlternative;
    check(compileTriggerURLFilters(alternatives, 41, universalAlternative.parser, universalAlternative.universal) == URLFilterParser::Ok
        && universalAlternative.compile(), "universal URL alternative compiles alongside specific patterns");
    auto universalMatches = universalAlternative.run("https://allowed.test/TOKEN"_s, flags);
    check(universalMatches.contains(unionAction) && universalMatches.size() == 1
        && universalAlternative.run("any"_s, getFlags).isEmpty(), "universal alternative shares the action identity and preserves method filtering");
    alternatives.urlFilterAlternatives.append("a|b"_s);
    Program invalidAlternative;
    check(compileTriggerURLFilters(alternatives, 41, invalidAlternative.parser, invalidAlternative.universal) != URLFilterParser::Ok,
        "invalid alternative fails compilation even when an earlier alternative matches everything");

    unsigned comparisons = 0;
    unsigned disagreements = 0;
    for (unsigned requirements = 1; requirements < 64; ++requirements) {
        URLConditionGroups selected;
        for (unsigned bit = 0; bit < 6; ++bit) {
            if (requirements & (1u << bit))
                selected.append({ types[bit], { "x"_s }, false });
        }
        auto base = candidate(0xFFFFFFFF, selected, flags);
        for (unsigned matches = 0; matches < 64; ++matches) {
            URLConditionActions request;
            URLConditionActions top;
            URLConditionActions frame;
            request.add(base);
            request.add(0); // Real zero-offset unconditional action.
            bool expected = true;
            for (unsigned bit = 0; bit < 6; ++bit) {
                auto& set = bit < 2 ? request : bit < 4 ? top : frame;
                if (matches & (1u << bit))
                    set.add(tag(0xFFFFFFFF, types[bit]));
                if (requirements & (1u << bit))
                    expected &= (bit % 2) ? !(matches & (1u << bit)) : !!(matches & (1u << bit));
            }
            filterActionsByURLConditions(request, top, frame);
            ++comparisons;
            if (request.contains(base) != expected || !request.contains(0) || request.size() != (expected ? 2 : 1))
                ++disagreements;
        }
    }
    printf("Condition truth table: %u comparisons, %u disagreements\n", comparisons, disagreements);
    check(!disagreements, "all 63 non-empty requirement masks match their independent six-clause truth tables");

    for (unsigned bit = 0; bit < 6; ++bit) {
        URLConditionGroups one { { types[bit], { ".*"_s }, false } };
        Programs all;
        check(all.add(one, 0) && all.compile(), "universal condition compiles with high flag bits");
        auto request = all.request.run(emptyString());
        auto top = all.top.run(emptyString());
        auto frame = all.frame.run(emptyString());
        auto base = candidate(0, one);
        request.add(base);
        filterActionsByURLConditions(request, top, frame);
        check(request.contains(base) == !(bit % 2) && request.size() == (bit % 2 ? 0 : 1), "universal positive/negative clauses retain their meaning for empty input");
    }
    for (auto mode : { ActionCondition::IfTopURL, ActionCondition::UnlessTopURL, ActionCondition::IfFrameURL, ActionCondition::UnlessFrameURL }) {
        bool excluded = mode == ActionCondition::UnlessTopURL || mode == ActionCondition::UnlessFrameURL;
        bool useTop = mode == ActionCondition::IfTopURL || mode == ActionCondition::UnlessTopURL;
        Program legacy;
        auto legacyAction = (static_cast<uint64_t>(flags | static_cast<ResourceFlags>(mode)) << 32) | 31;
        legacy.parser.addPattern("^https://match\\.test/"_s, false, legacyAction);
        check(legacy.compile(), "legacy condition bytecode compiles with method flags");
        for (bool matches : { false, true }) {
            auto matched = legacy.run(matches ? "https://match.test/"_s : "https://other.test/"_s);
            URLConditionActions empty;
            URLConditionActions request { legacyAction, 0 };
            filterActionsByURLConditions(request, useTop ? matched : empty, useTop ? empty : matched);
            check(request.contains(legacyAction) == (matches != excluded) && request.contains(0), "legacy top/frame conditions preserve method-bearing action identities");
        }
    }
    auto sensitiveGroups = parse(R"([{"type":"if-frame-url","urls":["TOKEN"],"case-sensitive":true},{"type":"unless-frame-url","urls":["BLOCK"],"case-sensitive":false}])");
    Programs sensitive;
    check(sensitive.add(*sensitiveGroups, 3) && sensitive.compile(), "case sensitivity is independent for each condition group");
    auto sensitiveAction = candidate(3, *sensitiveGroups);
    for (auto& test : { std::pair { "TOKEN", true }, std::pair { "token", false }, std::pair { "TOKENblock", false } }) {
        URLConditionActions request { sensitiveAction };
        filterActionsByURLConditions(request, { }, sensitive.frame.run(String::fromUTF8(test.first)));
        check(request.contains(sensitiveAction) == test.second, "per-group case flags reach the actual interpreter");
    }
    auto invalidRegex = parse(R"([{"type":"if-request-url","urls":["a|b"]}])");
    Programs rejected;
    check(invalidRegex && !rejected.add(*invalidRegex, 4), "unsupported regex fails condition compilation");
    auto source = JSON::Value::parseJSON(R"([{"type":"if-request-url","urls":["original"]}])"_s);
    auto copied = parseURLConditionGroups(*source);
    source->asArray()->get(0)->asObject()->getArray("urls"_s)->setString(0, "changed"_s);
    check(copied && copied->first().urls.first() == "original"_s, "parsed groups retain their data after caller mutation");
    Trigger trigger;
    trigger.urlFilter = "base"_s;
    trigger.urlFilterAlternatives = { "second"_s, "third"_s };
    trigger.urlConditionGroups = *groups;
    trigger.flags = static_cast<ResourceFlags>(ActionCondition::Conjunction) | urlConditionRequirements(*groups);
    auto copy = trigger.isolatedCopy();
    trigger.urlFilterAlternatives[0] = "changed"_s;
    trigger.urlConditionGroups[0].urls[0] = "changed"_s;
    check(copy.urlFilterAlternatives[0] == "second"_s && copy.urlConditionGroups[0].urls[0] == "^https://allowed\\.test/"_s,
        "cross-thread trigger copy preserves alternatives and all condition groups");
    printf("%u checks passed, %u failed\n", passed, failed);
    return failed ? 1 : 0;
}
