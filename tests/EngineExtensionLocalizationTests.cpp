#include "config.h"
#include "WebExtensionLocalization.h"
#include <Application.h>
#include <JavaScriptCore/JavaScript.h>
#include <cstdio>
#include <wtf/MainThread.h>
#include <wtf/text/MakeString.h>

using namespace WebKit;
static unsigned checks, failures;

static void check(bool result, const char* description)
{
    ++checks;
    failures += !result;
    std::printf("%s %s\n", result ? "PASS" : "FAIL", description);
    std::fflush(stdout);
}

static void equal(const String& actual, const String& expected, const char* description)
{
    check(actual == expected, description);
    if (actual != expected)
        std::printf("  expected [%s], received [%s]\n", expected.utf8().legacyCStringPointer(), actual.utf8().legacyCStringPointer());
}

static RefPtr<JSON::Object> json(const String& text)
{
    auto value = JSON::Value::parseJSON(text);
    RELEASE_ASSERT(value && value->asObject());
    return value->asObject();
}

static void checkLookup()
{
    auto regional = json(R"({"SHARED":{"message":"regional"},"regional":{"message":"region only"}})"_s);
    auto language = json(R"({"shared":{"message":"language"},"language":{"message":"language only"}})"_s);
    auto defaults = json(R"({"shared":{"message":"default"},"language":{"message":"default language"},"default":{"message":"default only"},"empty":{"message":""}})"_s);
    auto before = regional->toJSONString();
    auto localizer = WebExtensionLocalization::create(regional, language, defaults, "en-US"_s, "native-i18n-test"_s);
    equal(localizer->localizedStringForKey("sHaReD"_s), "regional"_s, "regional catalog takes precedence with case-insensitive keys");
    equal(localizer->localizedStringForKey("language"_s), "language only"_s, "language catalog precedes default catalog");
    equal(localizer->localizedStringForKey("default"_s), "default only"_s, "missing regional and language messages fall back to default");
    for (auto key : { ""_s, "unknown"_s, "empty"_s })
        equal(localizer->localizedStringForKey(key), emptyString(), "empty and missing messages produce empty strings");
    equal(regional->toJSONString(), before, "catalog merging leaves caller data unchanged");
    equal(localizer->localizedStringForKey("@@extension_id"_s), "native-i18n-test"_s, "predefined extension identity is available");
    equal(localizer->localizedStringForKey("@@ui_locale"_s), "en-US"_s, "predefined locale preserves the chosen catalog locale");
    equal(localizer->localizedStringForKey("@@bidi_dir"_s), "ltr"_s, "real English locale uses left-to-right direction");
    equal(localizer->localizedStringForKey("@@bidi_reversed_dir"_s), "rtl"_s, "English reverse direction is right-to-left");
    equal(localizer->localizedStringForKey("@@bidi_start_edge"_s), "left"_s, "English starts at the left edge");
    equal(localizer->localizedStringForKey("@@bidi_end_edge"_s), "right"_s, "English ends at the right edge");
    auto rtl = WebExtensionLocalization::create(nullptr, nullptr, defaults, "ar"_s, "rtl-test"_s);
    equal(rtl->localizedStringForKey("@@bidi_dir"_s), "rtl"_s, "real Arabic locale uses right-to-left direction");
    equal(rtl->localizedStringForKey("@@bidi_reversed_dir"_s), "ltr"_s, "Arabic reverse direction is left-to-right");
    equal(rtl->localizedStringForKey("@@bidi_start_edge"_s), "right"_s, "Arabic starts at the right edge");
    equal(rtl->localizedStringForKey("@@bidi_end_edge"_s), "left"_s, "Arabic ends at the left edge");
    auto unlocalized = WebExtensionLocalization::create(nullptr, nullptr, nullptr, emptyString(), "plain-test"_s);
    equal(unlocalized->localizedStringForKey("@@extension_id"_s), "plain-test"_s, "an extension without catalogs still has its predefined identity");
    equal(unlocalized->localizedStringForKey("unknown"_s), emptyString(), "an extension without catalogs returns empty for unknown messages");

    equal(localizer->localizedStringForString("prefix __MSG_shared__ / __MSG_default__ suffix"_s),
        "prefix regional / default only suffix"_s, "manifest and CSS message tokens localize within surrounding text");
    auto input = json(R"({"name":"__MSG_shared__","items":["__MSG_default__",{"nested":"__MSG_language__"},[true,17,null]],"enabled":false})"_s);
    auto original = input->toJSONString();
    auto output = localizer->localizedJSONforJSON(input);
    equal(output->getString("name"_s), "regional"_s, "manifest string property is localized");
    equal(output->getArray("items"_s)->get(0)->asString(), "default only"_s, "manifest array string is localized");
    equal(output->getArray("items"_s)->get(1)->asObject()->getString("nested"_s), "language only"_s, "nested manifest object is localized");
    equal(output->getArray("items"_s)->get(2)->toJSONString(), "[true,17,null]"_s, "nested non-string JSON values are preserved");
    equal(input->toJSONString(), original, "recursive localization leaves the original manifest unchanged");
}

static void checkSubstitutions()
{
    auto messages = json(R"({
        "named":{"message":"Hello $WHO$; $who$!","placeholders":{"Who":{"content":"$1"}}},
        "adjacent":{"message":"$first$$second$$first$","placeholders":{"first":{"content":"$1"},"second":{"content":"$2"}}},
        "ordered":{"message":"[$1] / [$2] / [$1]"},
        "one":{"message":"$1"},
        "all":{"message":"$9/$8/$7/$6/$5/$4/$3/$2/$1"},
        "dollars":{"message":"$$1 $$$2 $$$$3"},
        "mixed_dollars":{"message":"$$1 / $1"},
        "escape":{"message":"<b>$WHO$</b> < $2 & >","placeholders":{"who":{"content":"<i>$1</i>"}}},
        "unicode":{"message":"\u96ea $1 \uD83C\uDFD4"},
        "zero":{"message":"$0 / $1 / $9 / $16"},
        "literal":{"message":"$unclosed or closing$ or th$is or $$th$$at$"},
        "named_repeat":{"message":"$a$ $b$ $a$","placeholders":{"a":{"content":"[$b$]"},"b":{"content":"done"}}}
    })"_s);
    auto localizer = WebExtensionLocalization::create(nullptr, nullptr, messages, "en"_s, "substitutions"_s);
    equal(localizer->localizedStringForKey("named"_s, { "world"_s }), "Hello world; world!"_s, "named placeholders are case-insensitive and expand positional references");
    equal(localizer->localizedStringForKey("adjacent"_s, { "a"_s, "b"_s }), "aba"_s, "adjacent and repeated positional placeholders are all replaced");
    equal(localizer->localizedStringForKey("ordered"_s, { ""_s, "second"_s }), "[] / [second] / []"_s, "empty substitutions do not skip later placeholders");
    equal(localizer->localizedStringForKey("all"_s, { "1"_s, "2"_s, "3"_s, "4"_s, "5"_s, "6"_s, "7"_s, "8"_s, "9"_s }),
        "9/8/7/6/5/4/3/2/1"_s, "all nine substitution positions are available");
    equal(localizer->localizedStringForKey("ordered"_s, { "first"_s }), "[first] / [] / [first]"_s, "missing positional values become empty strings");
    equal(localizer->localizedStringForKey("ordered"_s, { "$2"_s, "second"_s }), "[$2] / [second] / [$2]"_s, "caller text is never reinterpreted as a positional placeholder");
    equal(localizer->localizedStringForKey("one"_s, { "$$caller"_s }), "$$caller"_s, "caller dollar literals survive unchanged");
    equal(localizer->localizedStringForKey("dollars"_s, { "a"_s, "b"_s, "c"_s }), "$1 $$2 $$$3"_s, "a dollar escape consumes one dollar from each consecutive run");
    equal(localizer->localizedStringForKey("mixed_dollars"_s, { "value"_s }), "$1 / value"_s, "an escaped occurrence stays literal beside an unescaped occurrence");
    equal(localizer->localizedStringForKey("escape"_s, { "<user>"_s, "<second>"_s }, true),
        "&lt;b>&lt;i><user>&lt;/i>&lt;/b> &lt; <second> & >"_s, "escapeLt escapes translation and named content without escaping caller text");
    equal(localizer->localizedStringForKey("escape"_s, { "<user>"_s, "<second>"_s }),
        "<b><i><user></i></b> < <second> & >"_s, "default lookup preserves less-than characters");
    equal(localizer->localizedStringForKey("unicode"_s, { String::fromUTF8("山") }), String::fromUTF8("雪 山 🏔"), "Unicode translation and substitution text survive together");
    equal(localizer->localizedStringForKey("zero"_s, { "one"_s }), " / one /  / one6"_s, "zero and missing positions are empty and positions consume one digit");
    equal(localizer->localizedStringForKey("literal"_s), "$unclosed or closing$ or th$is or $th$at$"_s, "existing WebKit handling of unmatched dollar text is preserved");
    equal(localizer->localizedStringForKey("named_repeat"_s), "[$b$] done [$b$]"_s, "named replacement text is not interpreted as another named placeholder");
    equal(localizer->localizedStringForKey("ordered"_s, { "$1"_s, "$2"_s }), "[$1] / [$2] / [$1]"_s, "self-referential caller text terminates without alteration");
    Vector<String> tooMany(10);
    equal(localizer->localizedStringForKey("one"_s, WTF::move(tooMany)), emptyString(), "shared localizer rejects more than nine substitutions");
    auto tokens = WebExtensionLocalization::create(nullptr, nullptr,
        json(R"({"outer":{"message":"__MSG_inner__"},"inner":{"message":"expanded"}})"_s), "en"_s, "tokens"_s);
    equal(tokens->localizedStringForString("__MSG_outer__ / __MSG_inner__ / __MSG_outer__"_s),
        "__MSG_inner__ / expanded / __MSG_inner__"_s, "manifest token replacement never reinterprets inserted translation text");
}

class LocalizationApplication final : public BApplication {
public:
    LocalizationApplication() : BApplication("application/x-vnd.Kunanyi-Summit-I18nTests") { }
    bool didRun { false };
    void ReadyToRun() override
    {
        WTF::initializeMainThread();
        auto context = JSGlobalContextCreate(nullptr);
        checkLookup();
        checkSubstitutions();
        JSGlobalContextRelease(context);
        didRun = true;
        PostMessage(B_QUIT_REQUESTED);
    }
};

int main()
{
    LocalizationApplication application;
    application.Run();
    check(application.didRun, "localization suite ran from BApplication::ReadyToRun");
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
