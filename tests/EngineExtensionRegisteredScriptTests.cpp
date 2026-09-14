#include "config.h"
#include "WebExtensionRegisteredScriptParser.h"
#include <cstdio>

using namespace WebKit;
using Mode = WebExtensionRegisteredScriptParseMode;

static unsigned checks;
static unsigned failures;

static void check(bool value, const char* message)
{
    ++checks;
    if (!value) {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}

static auto parse(const char* json, Mode mode = Mode::Registration)
{
    auto value = JSON::Value::parseJSON(String::fromUTF8(json));
    RELEASE_ASSERT(value);
    RefPtr array = value->asArray();
    RELEASE_ASSERT(array);
    Vector<Ref<JSON::Object>> scripts;
    for (auto& item : *array) {
        RefPtr object = item->asObject();
        RELEASE_ASSERT(object);
        scripts.append(object.releaseNonNull());
    }
    return parseWebExtensionRegisteredScripts(scripts, mode);
}

int main()
{
    auto empty = parse("[]");
    check(empty && empty->isEmpty(), "empty saved database is valid");
    auto minimal = parse(R"([{"id":"script","matches":["https://example.org/*"],"js":["first.js","second.js"]}])");
    check(minimal && minimal->size() == 1, "minimal registration loads");
    if (minimal) {
        auto& p = minimal->at(0);
        check(p.identifier == "script"_s, "identifier preserved");
        check(p.js && p.js->size() == 2 && p.js->at(0) == "first.js"_s && p.js->at(1) == "second.js"_s, "script order preserved");
        check(p.persistent == true, "registration defaults to persistent");
        check(!p.css && !p.excludeMatchPatterns && !p.allFrames && !p.matchParentFrame, "missing optional fields stay absent");
        check(!p.injectionTime && !p.world && !p.styleLevel, "default execution choices remain deferred to injection");
    }
    auto full = parse(R"([{"id":"full","matches":["<all_urls>"],"excludeMatches":["https://private.example/*"],"css":["first.css"],"js":[],"runAt":"document_start","cssOrigin":"USER","world":"MAIN","allFrames":false,"matchOriginAsFallback":false,"persistAcrossSessions":false}])");
    check(full.has_value(), "all supported fields load");
    if (full) {
        auto& p = full->at(0);
        check(p.allFrames == false && p.persistent == false, "explicit false values survive");
        check(p.matchParentFrame == WebCore::UserContentMatchParentFrame::Never, "explicit false fallback stays present");
        check(p.world == WebExtensionContentWorldType::Main && p.styleLevel == WebCore::UserStyleLevel::User, "world and CSS origin accept ASCII case variants");
        check(p.injectionTime == WebExtension::InjectionTime::DocumentStart, "document start preserved");
        check(p.css && p.css->at(0) == "first.css"_s && p.js && p.js->isEmpty(), "CSS-only registration retains empty JavaScript array");
        check(p.excludeMatchPatterns && p.excludeMatchPatterns->at(0) == "https://private.example/*"_s, "exclusions preserved");
    }
    auto options = parse(R"([{"id":"options","matches":["<all_urls>"],"js":["x.js"],"runAt":"document_end","world":"isolated","cssOrigin":"author","allFrames":true,"matchOriginAsFallback":true}])");
    check(options && options->at(0).injectionTime == WebExtension::InjectionTime::DocumentEnd, "document end preserved");
    check(options && options->at(0).world == WebExtensionContentWorldType::ContentScript && options->at(0).styleLevel == WebCore::UserStyleLevel::Author, "isolated world and author origin preserved");
    check(options && options->at(0).allFrames == true && options->at(0).matchParentFrame == WebCore::UserContentMatchParentFrame::ForOpaqueOrigins, "true frame choices preserved");
    auto idle = parse(R"([{"id":"idle","matches":["<all_urls>"],"js":["x.js"],"runAt":"document_idle"}])");
    check(idle && idle->at(0).injectionTime == WebExtension::InjectionTime::DocumentIdle, "document idle preserved");
    auto update = parse(R"([{"id":"script"}])", Mode::Update);
    check(update && !update->at(0).persistent && !update->at(0).js && !update->at(0).matchPatterns, "partial update does not supply registration defaults");
    auto clear = parse(R"([{"id":"script","css":[],"js":[],"excludeMatches":[],"persistAcrossSessions":false}])", Mode::Update);
    check(clear && clear->at(0).css && clear->at(0).css->isEmpty() && clear->at(0).persistent == false, "update can explicitly clear file lists and disable persistence");

    for (auto json : {
        R"([{}])",
        R"([{"id":""}])",
        R"([{"id":"_reserved"}])",
        R"([{"id":12}])",
        R"([{"id":"x","matches":null}])",
        R"([{"id":"x","matches":[]}])",
        R"([{"id":"x","matches":["<all_urls>"]}])",
        R"([{"id":"x","matches":["<all_urls>"],"js":[],"css":[]}])",
        R"([{"id":"x","matches":[null],"js":["x.js"]}])",
        R"([{"id":"x","matches":["<all_urls>"],"js":"x.js"}])",
        R"([{"id":"x","matches":["<all_urls>"],"js":["x.js",9]}])",
        R"([{"id":"x","matches":["<all_urls>"],"js":["x.js"],"excludeMatches":[false]}])",
        R"([{"id":"x","matches":["<all_urls>"],"css":[null]}])",
        R"([{"id":"x","matches":["<all_urls>"],"js":["x.js"],"allFrames":1}])",
        R"([{"id":"x","matches":["<all_urls>"],"js":["x.js"],"matchOriginAsFallback":"true"}])",
        R"([{"id":"x","matches":["<all_urls>"],"js":["x.js"],"persistAcrossSessions":null}])",
        R"([{"id":"x","matches":["<all_urls>"],"js":["x.js"],"runAt":"DOCUMENT_START"}])",
        R"([{"id":"x","matches":["<all_urls>"],"js":["x.js"],"runAt":false}])",
        R"([{"id":"x","matches":["<all_urls>"],"js":["x.js"],"world":"privileged"}])",
        R"([{"id":"x","matches":["<all_urls>"],"js":["x.js"],"world":null}])",
        R"([{"id":"x","matches":["<all_urls>"],"js":["x.js"],"cssOrigin":"agent"}])",
        R"([{"id":"x","matches":["<all_urls>"],"js":["x.js"],"cssOrigin":1}])"
    }) {
        auto invalid = parse(json);
        check(!invalid && !invalid.error().isEmpty(), "malformed registration returns an error");
    }
    check(!parse(R"([{"id":"script","matches":[]}])", Mode::Update), "update still rejects explicitly empty matches");
    check(!parse(R"([{"id":"ok","matches":["<all_urls>"],"js":["x.js"]},{"id":"bad"}])"), "invalid later script rejects entire parsed batch");
    auto unicode = parse(R"([{"id":"snow-\u96ea","matches":["https://example.org/*"],"js":["file\u0000name.js"],"unknownFutureField":42}])");
    check(unicode && unicode->at(0).identifier == String::fromUTF8("snow-雪"), "Unicode identifiers survive persisted JSON");
    check(unicode && unicode->at(0).js->at(0).length() == 12 && unicode->at(0).js->at(0)[4] == 0, "embedded NUL retained for resource validation");
    std::printf("%u native registered script parsing checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
