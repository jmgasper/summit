#include "config.h"
#include "WebExtensionDeclarativeNetRequestURLFilter.h"
#include <Application.h>
#include <JavaScriptCore/JavaScript.h>
#include <JavaScriptCore/JSRetainPtr.h>
#include <WebCore/CombinedURLFilters.h>
#include <WebCore/DFABytecodeCompiler.h>
#include <WebCore/DFABytecodeInterpreter.h>
#include <WebCore/NFAToDFA.h>
#include <WebCore/URLFilterParser.h>
#include <wtf/MainThread.h>
#include <wtf/URL.h>
#include <wtf/text/MakeString.h>
#include <cstdio>
#include <string>
#include <vector>

using namespace WebKit;
using namespace WebCore::ContentExtensions;
static unsigned passed;
static unsigned failed;
static void check(bool ok, const char* description)
{
    ++(ok ? passed : failed);
    printf("%s %s\n", ok ? "PASS" : "FAIL", description);
}

struct CompiledFilters {
    Vector<DFABytecode> bytecode;
    HashSet<uint64_t> universal;
    bool valid { true };

    explicit CompiledFilters(const Vector<WebExtensionDeclarativeNetRequestURLFilter>& filters)
    {
        CombinedURLFilters combined;
        URLFilterParser parser(combined);
        uint64_t identifier = 0;
        for (auto& filter : filters) {
            ++identifier;
            for (auto& pattern : filter.patterns) {
                auto status = parser.addPattern(pattern, filter.caseSensitive, identifier);
                if (status == URLFilterParser::MatchesEverything)
                    universal.add(identifier);
                else if (status != URLFilterParser::Ok)
                    valid = false;
            }
        }
        if (!valid)
            return;
        valid = combined.processNFAs(50000, [&](NFA&& nfa) {
            auto dfa = NFAToDFA::convert(WTF::move(nfa));
            if (!dfa)
                return false;
            dfa->minimize();
            DFABytecodeCompiler(*dfa, bytecode).compile();
            return true;
        });
    }

    bool matches(const String& address, uint64_t identifier = 1) const
    {
        if (universal.contains(identifier))
            return true;
        return !bytecode.isEmpty() && DFABytecodeInterpreter(bytecode.span()).interpret(address, 0).contains(identifier);
    }
};

static auto parse(const char* json)
{
    return WebExtensionDeclarativeNetRequestURLFilter::parse(*JSON::Value::parseJSON(String::fromUTF8(json))->asObject());
}

static void match(const char* json, const char* address, bool expected, const char* description)
{
    auto filter = parse(json);
    if (!filter) {
        printf("ERROR %s: %s\n", json, filter.error().utf8().legacyCStringPointer());
        check(false, description);
        return;
    }
    CompiledFilters compiled({ *filter });
    URL url { String::fromUTF8(address) };
    check(compiled.valid && url.isValid() && compiled.matches(url.string()) == expected, description);
}

// Independent token matcher: no regular expressions or translated text. This
// tests the truth table of wildcard/separator/end-anchor combinations against
// the production NFA -> DFA -> bytecode interpreter, including zero-width '^'.
static bool referenceMatch(const std::string& pattern, const std::string& address)
{
    size_t begin = !pattern.empty() && pattern[0] == '|' ? 1 : 0;
    size_t end = pattern.size();
    bool endAnchor = end > begin && pattern[end - 1] == '|';
    if (endAnchor)
        --end;
    bool startAnchor = begin;
    std::vector<bool> positions(address.size() + 1, !startAnchor);
    positions[0] = true;
    for (size_t index = begin; index < end; ++index) {
        std::vector<bool> next(address.size() + 1, false);
        char token = pattern[index];
        if (token == '*') {
            bool reachable = false;
            for (size_t offset = 0; offset <= address.size(); ++offset) {
                reachable = reachable || positions[offset];
                next[offset] = reachable;
            }
        } else {
            for (size_t offset = 0; offset <= address.size(); ++offset) {
                if (!positions[offset])
                    continue;
                if (offset == address.size()) {
                    if (token == '^')
                        next[offset] = true;
                    continue;
                }
                unsigned char character = address[offset];
                bool separator = !((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
                    || (character >= '0' && character <= '9') || character == '_' || character == '-' || character == '.' || character == '%');
                if ((token == '^' && separator) || (token != '^' && token == character))
                    next[offset + 1] = true;
            }
        }
        positions = WTF::move(next);
    }
    if (endAnchor)
        return positions.back();
    for (bool reachable : positions) {
        if (reachable)
            return true;
    }
    return false;
}

int main()
{
    BApplication application("application/x-vnd.Kunanyi-Summit-extension-dnr-url-filter-tests");
    WTF::initializeMainThread();
    JSRetainPtr context(Adopt, JSGlobalContextCreate(nullptr));

    match("{}", "https://example.test/a", true, "omitted filters match all URLs");
    match(R"({"urlFilter":"abc"})", "https://abcd.com", true, "plain substring matches host");
    match(R"({"urlFilter":"abc"})", "https://example.com/abcd", true, "plain substring matches path");
    match(R"({"urlFilter":"abc"})", "https://ab.com", false, "plain substring excludes different text");
    match(R"({"urlFilter":"abc*d"})", "https://abcxyzd.com", true, "wildcard matches multiple characters");
    match(R"({"urlFilter":"abc*d"})", "https://abc.com", false, "wildcard does not remove required suffix");
    match(R"({"urlFilter":"|https*"})", "https://example.com", true, "left anchor matches scheme");
    match(R"({"urlFilter":"|https*"})", "http://https.com", false, "left anchor cannot move to host");
    match(R"({"urlFilter":"example*^123|"})", "https://example.com/123", true, "wildcard separator and right anchor combine");
    match(R"({"urlFilter":"example*^123|"})", "https://example.com/1234", false, "right anchor excludes trailing characters");
    match(R"({"urlFilter":"example*^123|"})", "https://other.com/example0123", false, "digits are not separators");

    for (const char* address : { "https://a.example.com/", "https://b.a.example.com/path", "https://a.example.company/",
        "https://user:pass@a.example.com/", "https://user:pass@b.a.example.com:8443/path" })
        match(R"({"urlFilter":"||a.example.com"})", address, true, "domain anchor matches authority and subdomain boundaries");
    for (const char* address : { "https://ba.example.com/", "https://example.com/a.example.com", "https://other.test/?q=a.example.com",
        "https://a.example.com@other.test/", "https://other.test/#a.example.com", "https://other.test:8443/a.example.com" })
        match(R"({"urlFilter":"||a.example.com"})", address, false, "domain anchor excludes unrelated authority, userinfo and URL tails");
    match(R"({"urlFilter":"||example.com^"})", "https://example.company/", false, "domain separator excludes a longer suffix");
    match(R"({"urlFilter":"||example.com^"})", "https://example.com:8443/", true, "domain separator matches port boundary");
    match(R"({"urlFilter":"||example.com^"})", "https://sub.example.com/", true, "domain separator matches slash boundary");
    match(R"({"urlFilter":"||example*tail"})", "https://example.test/path@tail", true, "authority wildcard can continue into path text containing at sign");
    match(R"({"urlFilter":"||example*tail"})", "https://example-tail@other.test/", false, "authority wildcard cannot terminate inside credentials");
    match(R"({"urlFilter":"||example*tail"})", "https://user@sub.example.test/path/tail", true, "authority wildcard handles credentials and subdomain together");
    match(R"({"urlFilter":"||example^*tail"})", "https://example@other.test/path/tail", false, "separator cannot cross a userinfo delimiter as a host boundary");
    match(R"({"urlFilter":"||example^*tail"})", "https://example/path/tail", true, "separator can cross the actual authority boundary");
    match(R"({"urlFilter":"||example@other"})", "https://example@other.test/", false, "literal userinfo syntax does not become a host match");
    match(R"({"urlFilter":"||127.0.0.1^"})", "http://127.0.0.1:8080/", true, "IPv4 authority is supported");
    match(R"({"urlFilter":"||[::1]^"})", "http://[::1]:8080/", true, "IPv6 authority is supported and brackets are literal");
    match(R"({"urlFilter":"||xn--bcher-kva.test^"})", "https://bücher.test/", true, "internationalized host uses actual URL punycode serialization");
    match(R"({"urlFilter":"%C3%A9|"})", "https://example.test/é", true, "non-ASCII path uses actual percent encoding");

    match(R"({"urlFilter":"/tail^"})", "https://example.test/tail", true, "separator matches end of URL without consuming a byte");
    match(R"({"urlFilter":"/tail^"})", "https://example.test/tail/next", true, "separator also consumes a real separator");
    match(R"({"urlFilter":"/tail^"})", "https://example.test/tails", false, "separator does not consume an ordinary letter");
    match(R"({"urlFilter":"/tail^^*^|"})", "https://example.test/tail", true, "multiple separators and wildcard can finish at end of URL");
    match(R"({"urlFilter":"/tail^^*^|"})", "https://example.test/tail//suffix", true, "only the final separator needs to match end of URL");
    match(R"({"urlFilter":"/tail^x"})", "https://example.test/tail", false, "end-of-URL separator does not bypass later literal text");
    match(R"({"urlFilter":"/tail^x"})", "https://example.test/tail/x", true, "internal separator requires a consumed character");
    match(R"({"urlFilter":"/tail^|"})", "https://example.test/tail/a", false, "right anchor constrains the consuming separator branch");
    match(R"({"urlFilter":"*"})", "https://example.test/", true, "match-everything parser result is handled explicitly");
    match(R"({"urlFilter":"/A+()[].$?"})", "https://example.test/A+()[].$?", true, "regex punctuation is literal in URL patterns");
    match(R"({"urlFilter":"/A+()[]{}$.?"})", "https://example.test/A+()[]{}$.?", false, "URL serialization percent-encodes curly braces before matching");
    match(R"({"urlFilter":"/A+()[]{}$.?"})", "https://example.test/AAAA", false, "literal punctuation cannot become regex operators");
    match(R"({"urlFilter":"TOKEN"})", "https://example.test/token", true, "matching defaults to case insensitive");
    match(R"({"urlFilter":"TOKEN","isUrlFilterCaseSensitive":true})", "https://example.test/token", false, "explicit case sensitivity reaches bytecode");
    match(R"({"urlFilter":"TOKEN","isUrlFilterCaseSensitive":true})", "https://example.test/TOKEN", true, "case-sensitive literal matches exact case");
    match(R"({"regexFilter":"^https://example\\.test/a[0-9]+$"})", "https://example.test/a123", true, "supported raw regex reaches the real filter compiler");
    match(R"({"regexFilter":"^https://example\\.test/a[0-9]+$"})", "https://example.test/a123b", false, "raw regex anchors remain intact");
    match(R"({"regexFilter":".*"})", "https://example.test/", true, "match-all raw regex is accepted");
    match(R"({"regexFilter":"^$"})", "https://example.test/", false, "anchored empty regex is not an unconditional match");
    match(R"({"regexFilter":"^a*$"})", "https://example.test/", false, "anchored optional regex keeps its end constraint");
    match(R"({"regexFilter":"^.*$"})", "https://example.test/", true, "anchored wildcard still matches a complete URL");
    match(R"({"regexFilter":"^a*.*$"})", "https://example.test/", true, "nullable prefix before a complete wildcard remains unconditional");
    match(R"({"regexFilter":"\\x61+$"})", "https://example.test/aaa", true, "ASCII hex escape retains its meaning");

    for (const char* json : { R"({"urlFilter":null})", R"({"urlFilter":[]})", R"({"urlFilter":true})", R"({"urlFilter":3})",
        R"({"urlFilter":""})", R"({"regexFilter":""})", R"({"regexFilter":{}})", R"({"urlFilter":"a","regexFilter":"a"})",
        R"({"urlFilter":null,"regexFilter":"a"})", R"({"isUrlFilterCaseSensitive":1})", R"({"isUrlFilterCaseSensitive":null})",
        R"({"isUrlFilterCaseSensitive":"false"})", R"({"urlFilter":"é"})", R"({"regexFilter":"é"})",
        R"({"urlFilter":"a\u0000b"})", R"({"regexFilter":"a\u0000b"})", R"({"urlFilter":"||*example.test"})",
        R"({"regexFilter":"a|b"})", R"JSON({"regexFilter":"(?=a)"})JSON", R"({"regexFilter":"(a)\\1"})", R"({"regexFilter":"\\d"})",
        R"({"regexFilter":"["})", R"({"regexFilter":"a{3}"})", R"({"regexFilter":"a^b"})",
        R"({"regexFilter":"\\0"})", R"({"regexFilter":"\\x00"})", R"({"regexFilter":"\\u0000"})",
        R"({"regexFilter":"\\xGG"})", R"({"regexFilter":"\\xA"})", R"({"regexFilter":"\\q"})",
        R"({"regexFilter":"\\k<name>"})" }) {
        auto result = parse(json);
        if (result)
            printf("UNEXPECTED ACCEPT %s\n", json);
        check(!result && !result.error().isEmpty(), "malformed or unsupported URL condition reports an error");
    }

    auto condition = JSON::Object::create();
    condition->setString("urlFilter"_s, "abc"_s);
    auto snapshot = WebExtensionDeclarativeNetRequestURLFilter::parse(condition);
    condition->setString("urlFilter"_s, "other"_s);
    check(snapshot && CompiledFilters({ *snapshot }).matches("https://test/abc"_s), "parsed patterns survive caller JSON mutation");
    condition->setString("urlFilter"_s, "abc"_s);
    check(WebExtensionDeclarativeNetRequestURLFilter::parse(condition, { 3, 1, 3 }).has_value(), "input output and pattern limits include the exact boundary");
    check(!WebExtensionDeclarativeNetRequestURLFilter::parse(condition, { 2, 1, 3 }), "input limit rejects one extra character");
    check(!WebExtensionDeclarativeNetRequestURLFilter::parse(condition, { 3, 1, 2 }), "output limit rejects one extra character");
    check(!WebExtensionDeclarativeNetRequestURLFilter::parse(condition, { 3, 0, 3 }), "zero pattern budget rejects the translation");
    condition->setString("urlFilter"_s, "abc^"_s);
    check(!WebExtensionDeclarativeNetRequestURLFilter::parse(condition, { 4, 1, 100 }), "separator alternatives cannot exceed the pattern budget");
    check(WebExtensionDeclarativeNetRequestURLFilter::parse(condition, { 4, 2, 100 }).has_value(), "two-pattern budget retains both separator meanings");
    condition->setString("urlFilter"_s, "********************************abc"_s);
    auto coalesced = WebExtensionDeclarativeNetRequestURLFilter::parse(condition, { 100, 1, 5 });
    check(coalesced && coalesced->patterns[0] == ".*abc"_s, "adjacent wildcards do not inflate the generated expression");
    condition->setString("urlFilter"_s, String::fromUTF8(std::string(4000, '^')));
    check(!WebExtensionDeclarativeNetRequestURLFilter::parse(condition, { }), "adversarial separator expansion is bounded and rejected");

    std::vector<std::string> tokens { "*", "^", "a", "/" };
    std::vector<std::string> patterns = tokens;
    for (auto& first : tokens) {
        for (auto& second : tokens) {
            patterns.push_back(first + second);
            for (auto& third : tokens)
                patterns.push_back(first + second + third);
        }
    }
    auto unanchored = patterns;
    for (auto& pattern : unanchored) {
        patterns.push_back("|" + pattern);
        patterns.push_back(pattern + "|");
        patterns.push_back("|" + pattern + "|");
    }
    Vector<WebExtensionDeclarativeNetRequestURLFilter> parsed;
    for (auto& pattern : patterns) {
        condition->setString("urlFilter"_s, String::fromUTF8(pattern));
        auto filter = WebExtensionDeclarativeNetRequestURLFilter::parse(condition);
        if (!filter) {
            printf("Unexpected generated filter error for %s: %s\n", pattern.c_str(), filter.error().utf8().legacyCStringPointer());
            check(false, "truth-table input parses");
            return 1;
        }
        parsed.append(WTF::move(*filter));
    }
    CompiledFilters matrix(parsed);
    check(matrix.valid, "all truth-table patterns compile to actual content-extension bytecode");
    unsigned comparisons = 0;
    unsigned disagreements = 0;
    for (const char* address : { "https://t/", "https://t/a", "https://t/aa", "https://t/a/", "https://t/a//",
        "https://t/a/b", "https://t/?a", "https://t/#a", "https://t/a.a", "https://t/a-a", "https://t/a_a", "https://t/a%20" }) {
        for (size_t index = 0; index < patterns.size(); ++index) {
            ++comparisons;
            bool expected = referenceMatch(patterns[index], address);
            bool actual = matrix.matches(String::fromUTF8(address), index + 1);
            if (actual != expected) {
                ++disagreements;
                if (disagreements <= 10)
                    printf("MISMATCH %s / %s expected=%d actual=%d\n", patterns[index].c_str(), address, expected, actual);
            }
        }
    }
    printf("Token oracle: %u comparisons, %u disagreements\n", comparisons, disagreements);
    check(!disagreements, "compiled matching agrees with independent token semantics");

    // Use the parsed URL's actual host offset to choose legal start positions;
    // the token oracle then evaluates the untouched DNR pattern at each one.
    std::vector<std::string> domainPatterns { "a", "a^", "a*", "a@b", "a/b", "a?b", "a#b" };
    for (const char* first : { "*", "^", "/", "a", "@", "?", "#" }) {
        for (const char* second : { "*", "^", "/", "a", "@", "?", "#" })
            domainPatterns.push_back(std::string("a") + first + second);
    }
    auto domainUnanchored = domainPatterns;
    for (auto& pattern : domainUnanchored)
        domainPatterns.push_back(pattern + "|");
    Vector<WebExtensionDeclarativeNetRequestURLFilter> domainParsed;
    for (auto& pattern : domainPatterns) {
        condition->setString("urlFilter"_s, makeString("||"_s, String::fromUTF8(pattern)));
        auto filter = WebExtensionDeclarativeNetRequestURLFilter::parse(condition);
        if (!filter) {
            printf("Unexpected domain filter error for %s\n", pattern.c_str());
            check(false, "domain truth-table input parses");
            return 1;
        }
        domainParsed.append(WTF::move(*filter));
    }
    CompiledFilters domainMatrix(domainParsed);
    check(domainMatrix.valid, "domain truth-table patterns compile to actual bytecode");
    comparisons = 0;
    disagreements = 0;
    for (const char* address : { "https://a/", "https://b.a/", "https://ab/", "https://a.b/", "https://a.b:8443/a",
        "https://user:pass@a/", "https://user@b.a/a@b", "https://a@b/a", "https://a.b@other/a", "https://a.b@other/?a",
        "https://a/a", "https://a/a/b", "https://a/a@b", "https://a/a#b", "https://a/?a", "https://a/#a", "https://a/a/a",
        "https://a/?a@b", "https://a/#a@b", "https://a:8443/a/b", "https://b.a.b/?a", "https://a/aa", "https://a/aa@b" }) {
        URL url { String::fromUTF8(address) };
        std::string serialized(url.string().utf8().legacyCStringPointer());
        auto host = url.host();
        RELEASE_ASSERT(url.string().is8Bit() && host.is8Bit());
        // URL::host() returns a view into the serialized URL's character span.
        size_t hostOffset = host.span8().data() - url.string().span8().data();
        for (size_t index = 0; index < domainPatterns.size(); ++index) {
            bool expected = false;
            for (size_t start = 0; start < host.length(); ++start) {
                if (!start || host[start - 1] == '.')
                    expected |= referenceMatch("|" + domainPatterns[index], serialized.substr(hostOffset + start));
            }
            bool actual = domainMatrix.matches(url.string(), index + 1);
            ++comparisons;
            if (expected != actual) {
                ++disagreements;
                if (disagreements <= 10)
                    printf("DOMAIN MISMATCH ||%s / %s expected=%d actual=%d\n", domainPatterns[index].c_str(), address, expected, actual);
            }
        }
    }
    printf("Authority oracle: %u comparisons, %u disagreements\n", comparisons, disagreements);
    check(!disagreements, "domain bytecode agrees with actual host offsets and token semantics");

    struct NullableCase { const char* regex; const char* input; bool expected; };
    for (auto& test : { NullableCase { "^$", "", true }, NullableCase { "^$", "a", false },
        NullableCase { "^a*$", "", true }, NullableCase { "^a*$", "aaa", true }, NullableCase { "^a*$", "b", false },
        NullableCase { "^a?$", "", true }, NullableCase { "^a?$", "a", true }, NullableCase { "^a?$", "aa", false },
        NullableCase { "^(a*)$", "", true }, NullableCase { "^(a*)$", "aaa", true }, NullableCase { "^(a*)$", "ab", false },
        NullableCase { "^a*b*$", "aaabbb", true }, NullableCase { "^a*b*$", "ba", false },
        NullableCase { "^.*$", "", true }, NullableCase { "^.*$", "abc", true },
        NullableCase { "^a*.*$", "bcd", true }, NullableCase { "^a*", "bcd", true }, NullableCase { "a*$", "bcd", true } }) {
        auto input = JSON::Object::create();
        input->setString("regexFilter"_s, String::fromUTF8(test.regex));
        auto filter = WebExtensionDeclarativeNetRequestURLFilter::parse(input);
        bool ok = filter.has_value();
        if (ok) {
            CompiledFilters compiled({ *filter });
            ok = compiled.valid && compiled.matches(String::fromUTF8(test.input)) == test.expected;
        }
        if (!ok)
            printf("NULLABILITY MISMATCH %s / %s expected=%d\n", test.regex, test.input, test.expected);
        check(ok, "shared parser preserves anchored and unanchored nullable-expression semantics");
    }
    printf("%u checks passed, %u failed\n", passed, failed);
    return failed ? 1 : 0;
}
