#include "config.h"
#include "WebExtensionCommandSettings.h"
#include "WebExtensionCommandShortcut.h"
#include <Application.h>
#include <cstdio>
#include <wtf/MainThread.h>
#include <wtf/text/MakeString.h>

using namespace WebKit;
using Modifier = WebExtensionCommandModifier;
using Source = WebExtensionCommandShortcutSource;
static unsigned checks;
static unsigned failures;
static void expect(bool pass, const char* message)
{
    ++checks;
    if (!pass) {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}
static auto update(ASCIILiteral json)
{
    auto value = JSON::Value::parseJSON(json);
    return parseWebExtensionCommandUpdate(value.get());
}
int main()
{
    BApplication application("application/x-vnd.Kunanyi-Summit-extension-command-settings-tests");
    WTF::initializeMainThread();
    for (auto name : { "_execute_action"_s, "_execute_browser_action"_s, "_execute_page_action"_s, "_execute_sidebar_action"_s })
        expect(isWebExtensionActionCommandIdentifier(name), "reserved action command never becomes an ordinary command event");
    expect(!isWebExtensionActionCommandIdentifier("toggle-feature"_s), "ordinary extension command remains dispatchable");
    struct Case { ASCIILiteral input; ASCIILiteral normalized; };
    for (auto test : {
        Case { ""_s, ""_s }, { "Ctrl+A"_s, "Ctrl+A"_s }, { "Ctrl+a"_s, "Ctrl+A"_s },
        { "Alt+9"_s, "Alt+9"_s }, { "Ctrl+Shift+Z"_s, "Ctrl+Shift+Z"_s },
        { "Alt+Ctrl+Comma"_s, "Ctrl+Alt+Comma"_s }, { "Alt+Shift+Period"_s, "Alt+Shift+Period"_s },
        { "Ctrl+Space"_s, "Ctrl+Space"_s }, { "F1"_s, "F1"_s }, { "Shift+F12"_s, "Shift+F12"_s },
        { "Ctrl+Alt+F9"_s, "Ctrl+Alt+F9"_s }, { "MediaPlayPause"_s, "MediaPlayPause"_s },
        { "MediaNextTrack"_s, "MediaNextTrack"_s }, { "MediaPrevTrack"_s, "MediaPrevTrack"_s }, { "MediaStop"_s, "MediaStop"_s }
    }) {
        auto parsed = parseWebExtensionCommandShortcut(test.input);
        expect(parsed && webExtensionCommandShortcutString(parsed->activationKey, parsed->modifiers) == test.normalized, "valid shortcut parses and formats");
    }
    auto control = parseWebExtensionCommandShortcut("Ctrl+Y"_s);
    expect(control && control->modifiers == OptionSet { Modifier::Control } && control->activationKey == "y"_s, "Ctrl is native Control, with a lowercase activation key");
    auto empty = parseWebExtensionCommandShortcut(nullString());
    expect(empty && empty->activationKey.isEmpty() && empty->modifiers.isEmpty(), "null shortcut is unassigned");
    for (auto invalid : { "A"_s, "Shift+A"_s, "Command+A"_s, "MacCtrl+A"_s, "Ctrl"_s, "Ctrl+"_s, "+A"_s,
        "Ctrl++A"_s, "Ctrl+A+"_s, "Ctrl+Ctrl+A"_s, "Alt+Alt+9"_s, "Ctrl+Shift+Alt+A"_s,
        "Ctrl+Shift+Shift"_s, "Ctrl+Alt"_s, "Ctrl+MediaPlayPause"_s, "Ctrl+!"_s,
        "Control+A"_s, "ctrl+A"_s, "Ctrl+CommaX"_s, "Ctrl+Enter"_s, "Ctrl+Tab"_s,
        "Ctrl+Escape"_s, "Shift+Ctrl+A"_s, "Shift+Alt+A"_s, "F0"_s, "F01"_s, "F13"_s, "F20"_s, "F4294967297"_s,
        "Ctrl+F+1"_s, "Ctrl +A"_s, "Ctrl+ A"_s, "Ctrl+é"_s })
        expect(!parseWebExtensionCommandShortcut(invalid), "malformed or unsupported manifest shortcut is rejected");
    for (unsigned number = 1; number <= 20; ++number) {
        auto name = makeString('F', number);
        auto manifest = parseWebExtensionCommandShortcut(name);
        auto runtime = parseWebExtensionCommandShortcut(name, Source::Update);
        expect(manifest.has_value() == (number <= 12), "manifest function key range");
        expect(runtime.has_value() == (number <= 19), "updated function key range");
        if (runtime)
            expect(runtime->activationKey.length() == 1 && runtime->activationKey[0] == 0xF703 + number, "function key native code");
    }
    struct Named { ASCIILiteral name; char16_t key; };
    for (auto named : { Named { "Comma"_s, ',' }, { "Period"_s, '.' }, { "Space"_s, ' ' },
        { "Insert"_s, 0xF727 }, { "Delete"_s, 0xF728 }, { "Home"_s, 0xF729 },
        { "End"_s, 0xF72B }, { "PageUp"_s, 0xF72C }, { "PageDown"_s, 0xF72D },
        { "Up"_s, 0xF700 }, { "Down"_s, 0xF701 }, { "Left"_s, 0xF702 }, { "Right"_s, 0xF703 } }) {
        auto parsed = parseWebExtensionCommandShortcut(makeString("Ctrl+"_s, named.name));
        expect(parsed && parsed->activationKey.length() == 1 && parsed->activationKey[0] == named.key, "named key native code");
    }
    const char16_t nulKey[] { 'C', 't', 'r', 'l', '+', 'A', 0 };
    expect(!parseWebExtensionCommandShortcut(String(std::span(nulKey))), "embedded NUL is not truncated");
    const char16_t surrogate[] { 0xd800 };
    expect(!isWebExtensionCommandActivationKey(String(std::span(surrogate))), "unpaired surrogate is not an activation key");
    expect(webExtensionCommandShortcutString("a"_s, { Modifier::Shift }).isEmpty(), "incomplete native setter state has no assigned shortcut");
    expect(webExtensionCommandShortcutString("a"_s, { Modifier::Command }).isEmpty(), "Mac modifier cannot acquire a native shortcut");
    expect(webExtensionCommandShortcutString("a"_s, OptionSet<Modifier>::fromRaw((1u << 18) | 1u)).isEmpty(), "unknown modifier bits are rejected");
    expect(webExtensionCommandShortcutString("MediaStop"_s, { Modifier::Control }).isEmpty(), "media key cannot gain a modifier");
    for (auto bad : { "null"_s, "[]"_s, "{}"_s, "true"_s, "{\"name\":1}"_s,
        "{\"name\":\"a\",\"shortcut\":null}"_s, "{\"name\":\"a\",\"description\":false}"_s,
        "{\"name\":\"a\",\"shortcut\":\"Shift+A\"}"_s, "{\"name\":\"a\",\"extra\":\"x\"}"_s })
        expect(!update(bad), "invalid update arguments are rejected");
    auto description = update("{\"name\":\"a\",\"description\":\"new\"}"_s);
    expect(description && description->description == "new"_s && !description->shortcut, "omitted shortcut is distinct from clearing it");
    auto cleared = update("{\"name\":\"a\",\"description\":\"\",\"shortcut\":\"\"}"_s);
    expect(cleared && cleared->description && cleared->description->isEmpty() && cleared->shortcut && cleared->shortcut->isEmpty(), "explicit empty description and shortcut survive parsing");
    auto function = update("{\"name\":\"a\",\"shortcut\":\"F19\"}"_s);
    expect(function && function->shortcut == "F19"_s, "update allows F19");
    auto normalized = update("{\"name\":\"a\",\"shortcut\":\"Alt+Ctrl+z\"}"_s);
    expect(normalized && normalized->shortcut == "Ctrl+Alt+Z"_s, "update canonicalizes modifiers and letter case");

    auto originalValue = JSON::Value::parseJSON("{\"unrelated\":{\"enabled\":true},\"CommandOverrides\":{\"a\":{\"shortcut\":\"Ctrl+Z\"},\"b\":{\"description\":\"keep\"}}}"_s);
    auto original = originalValue->asObject();
    auto originalJSON = original->toJSONString();
    auto edited = webExtensionCommandStateWithUpdate(original.get(), "a"_s, *description);
    auto saved = webExtensionCommandOverride(edited.get(), "a"_s);
    expect(saved && saved->getString("description"_s) == "new"_s && saved->getString("shortcut"_s) == "Ctrl+Z"_s, "partial update preserves earlier shortcut override");
    expect(edited->getObject("unrelated"_s)->getBoolean("enabled"_s).value_or(false), "unrelated settings survive update");
    expect(original->toJSONString() == originalJSON, "preparing a state update does not mutate the committed state");
    auto removed = webExtensionCommandStateWithUpdate(edited.get(), "a"_s, std::nullopt);
    expect(!webExtensionCommandOverride(removed.get(), "a"_s), "reset removes all overrides for the command");
    expect(webExtensionCommandOverride(removed.get(), "b"_s)->getString("description"_s) == "keep"_s, "reset preserves other commands");
    expect(!!webExtensionCommandOverride(edited.get(), "a"_s), "reset does not mutate the earlier state");
    auto emptyState = webExtensionCommandStateWithUpdate(nullptr, "a"_s, *cleared);
    saved = webExtensionCommandOverride(emptyState.get(), "a"_s);
    expect(saved && saved->getValue("shortcut"_s) && saved->getString("shortcut"_s).isEmpty(), "explicit clear remains persisted");
    auto reloadedValue = JSON::Value::parseJSON(emptyState->toJSONString());
    expect(!!webExtensionCommandOverride(reloadedValue->asObject().get(), "a"_s)->getValue("shortcut"_s), "cleared shortcut survives JSON reload");
    auto migratedValue = JSON::Value::parseJSON("{\"CommandOverrides\":{\"_execute_browser_action\":{\"shortcut\":\"Ctrl+Y\",\"description\":\"old\"}}}"_s);
    auto migrated = migratedValue->asObject();
    expect(webExtensionCommandOverride(migrated.get(), "_execute_action"_s)->getString("shortcut"_s) == "Ctrl+Y"_s, "MV3 inherits the older browser-action override");
    auto newAction = webExtensionCommandStateWithUpdate(migrated.get(), "_execute_action"_s, WebExtensionCommandUpdate { "_execute_action"_s, "new"_s, std::nullopt });
    expect(webExtensionCommandOverride(newAction.get(), "_execute_action"_s)->getString("shortcut"_s) == "Ctrl+Y"_s, "MV3 partial update preserves migrated shortcut");
    expect(!webExtensionCommandOverride(newAction.get(), "_execute_browser_action"_s), "MV3 update retires old action key");
    auto resetAction = webExtensionCommandStateWithUpdate(migrated.get(), "_execute_action"_s, std::nullopt);
    expect(!webExtensionCommandOverride(resetAction.get(), "_execute_action"_s), "MV3 reset cannot resurrect the older shortcut");
    auto bothValue = JSON::Value::parseJSON("{\"CommandOverrides\":{\"_execute_browser_action\":{\"shortcut\":\"Ctrl+Y\"},\"_execute_action\":{\"shortcut\":\"\"}}}"_s);
    expect(webExtensionCommandOverride(bothValue->asObject().get(), "_execute_action"_s)->getString("shortcut"_s).isEmpty(), "explicit MV3 clear takes precedence over old action");
    auto corruptValue = JSON::Value::parseJSON("{\"CommandOverrides\":{\"a\":{\"shortcut\":\"bad\",\"description\":7,\"unknown\":true}}}"_s);
    auto repaired = webExtensionCommandStateWithUpdate(corruptValue->asObject().get(), "a"_s, *description);
    saved = webExtensionCommandOverride(repaired.get(), "a"_s);
    expect(saved && saved->size() == 1 && saved->getString("description"_s) == "new"_s, "partial update drops malformed saved fields");
    expect(!webExtensionCommandOverride(original.get(), "absent"_s), "missing command has no invented override");
    std::printf("Command settings checks: %u, failures: %u\n", checks, failures);
    return failures ? 1 : 0;
}
