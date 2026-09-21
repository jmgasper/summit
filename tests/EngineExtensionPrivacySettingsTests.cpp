/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionPrivacySettingsHaiku.h"
#include "WebExtensionStateHaiku.h"
#include <Application.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <wtf/MainThread.h>
#include <wtf/text/MakeString.h>

using namespace WebKit;
using Setting = WebExtensionPrivacySettingHaiku;
using Scope = WebExtensionPrivacyScopeHaiku;
unsigned checks, failures;
static void check(bool pass, const char* label)
{
    ++checks;
    if (!pass) {
        ++failures;
        printf("FAIL %s\n", label);
    }
}
static Ref<JSON::Object> object(const String& input)
{
    auto value = JSON::Value::parseJSON(input);
    auto result = value ? value->asObject() : nullptr;
    RELEASE_ASSERT(result);
    return result.releaseNonNull();
}
static bool matches(const std::optional<WebExtensionPrivacyValueHaiku>& value, bool expected, bool specific = false)
{
    return value && value->value == expected && value->incognitoSpecific == specific;
}
int main()
{
    BApplication application("application/x-vnd.Kunanyi-Summit-PrivacySettingsTests");
    WTF::initializeMainThread();
    check(parseWebExtensionPrivacySettingHaiku("network.networkPredictionEnabled"_s) == Setting::NetworkPrediction, "network prediction name is recognized");
    check(parseWebExtensionPrivacySettingHaiku("websites.hyperlinkAuditingEnabled"_s) == Setting::HyperlinkAuditing, "hyperlink auditing name is recognized");
    for (auto name : { ""_s, "network.webRTCIPHandlingPolicy"_s, "networkPredictionEnabled"_s, "network.unknown"_s })
        check(!parseWebExtensionPrivacySettingHaiku(name), "unsupported setting cannot enter the boolean backend");
    for (auto name : { ""_s, "REGULAR"_s, "private"_s, "incognito"_s })
        check(!parseWebExtensionPrivacyScopeHaiku(name), "invalid scope is rejected");

    for (auto setting : { Setting::NetworkPrediction, Setting::HyperlinkAuditing }) {
        WebExtensionPrivacySettingsHaiku settings;
        WebExtensionPrivacyValuesHaiku session;
        check(!settings.effective(setting, false) && !settings.effective(setting, true), "no contribution is distinct from false");
        check(settings.set(setting, Scope::Regular, false), "regular override is accepted");
        check(matches(settings.effective(setting, false), false) && matches(settings.effective(setting, true), false), "regular false applies to both profiles");
        settings.set(setting, Scope::RegularOnly, true);
        check(matches(settings.effective(setting, false), true) && matches(settings.effective(setting, true), false), "regular-only overrides regular without changing private inheritance");
        settings.set(setting, Scope::IncognitoPersistent, true);
        check(matches(settings.effective(setting, true), true, true) && matches(settings.effective(setting, false), true), "private persisted value takes precedence only for private pages");
        session[static_cast<size_t>(setting)] = false;
        check(matches(settings.effective(setting, true, &session), false, true), "private session false overrides persistent true");
        check(matches(settings.effective(setting, false, &session), true), "regular queries ignore private session values");
        check(!settings.set(setting, Scope::IncognitoSessionOnly, true), "session values cannot enter the persisted model");
        session[static_cast<size_t>(setting)].reset();
        check(matches(settings.effective(setting, true, &session), true, true), "ending a session restores the persisted private value");
        settings.set(setting, Scope::IncognitoPersistent, std::nullopt);
        check(matches(settings.effective(setting, true), false), "clearing private persistence restores regular inheritance");
        settings.set(setting, Scope::RegularOnly, std::nullopt);
        check(matches(settings.effective(setting, false), false), "clearing regular-only restores regular");
        settings.set(setting, Scope::Regular, std::nullopt);
        check(!settings.effective(setting, false) && !settings.effective(setting, true), "clearing every scope removes the contribution");
    }

    WebExtensionPrivacySettingsHaiku original;
    original.set(Setting::NetworkPrediction, Scope::Regular, false);
    original.set(Setting::NetworkPrediction, Scope::IncognitoPersistent, true);
    original.set(Setting::HyperlinkAuditing, Scope::RegularOnly, false);
    auto state = object("{\"unrelated\":{\"keep\":true}}"_s);
    state->setObject("PrivacySettings"_s, serializeWebExtensionPrivacySettingsHaiku(original, "install-a"_s));
    auto encoded = state->toJSONString();
    auto decoded = object(encoded);
    auto restored = parseWebExtensionPrivacySettingsHaiku(decoded.get(), "install-a"_s);
    check(restored && restored->values == original.values, "all persisted true, false and absent values survive JSON encoding");
    check(decoded->toJSONString() == encoded, "reading overrides leaves unrelated state intact");
    auto reinstall = parseWebExtensionPrivacySettingsHaiku(decoded.get(), "install-b"_s);
    check(reinstall && !reinstall->effective(Setting::NetworkPrediction, false) && !reinstall->effective(Setting::HyperlinkAuditing, false), "a reinstall cannot inherit overrides from an earlier installation");
    auto legacy = parseWebExtensionPrivacySettingsHaiku(decoded.get(), ""_s);
    check(legacy && !legacy->effective(Setting::NetworkPrediction, false), "an unranked legacy context cannot activate persisted overrides");
    check(!encoded.contains("incognito_session_only"_s), "serialized state contains no private session scope");

    for (auto json : { "{}"_s, "{\"unrelated\":42}"_s }) {
        auto absent = parseWebExtensionPrivacySettingsHaiku(object(json).get(), "install-a"_s);
        check(absent && !absent->effective(Setting::HyperlinkAuditing, false), "older state without privacy settings remains valid");
    }
    for (auto json : { "{\"PrivacySettings\":null}"_s, "{\"PrivacySettings\":[]}"_s,
        "{\"PrivacySettings\":{}}"_s, "{\"PrivacySettings\":{\"installation_identifier\":\"\"}}"_s,
        "{\"PrivacySettings\":{\"installation_identifier\":\"install-a\",\"version\":1}}"_s })
        check(!parseWebExtensionPrivacySettingsHaiku(object(json).get(), "install-a"_s), "malformed privacy metadata is rejected");
    for (auto version : { "0"_s, "2"_s, "1.5"_s, "true"_s, "\"1\""_s, "null"_s }) {
        auto json = makeString("{\"PrivacySettings\":{\"installation_identifier\":\"install-a\",\"version\":"_s, version, ",\"values\":{}}}"_s);
        check(!parseWebExtensionPrivacySettingsHaiku(object(json).get(), "install-a"_s), "invalid state version is rejected without truncating fractions");
    }
    for (auto invalid : { "null"_s, "true"_s, "[]"_s, "{\"incognito_session_only\":{}}"_s,
        "{\"unknown_scope\":{}}"_s, "{\"regular\":null}"_s, "{\"regular\":[]}"_s,
        "{\"regular\":{\"unknown.setting\":false}}"_s,
        "{\"regular\":{\"network.networkPredictionEnabled\":0}}"_s,
        "{\"regular\":{\"network.networkPredictionEnabled\":\"false\"}}"_s,
        "{\"regular\":{\"network.networkPredictionEnabled\":null}}"_s }) {
        auto json = makeString("{\"PrivacySettings\":{\"installation_identifier\":\"install-a\",\"version\":1,\"values\":"_s, invalid, "}}"_s);
        check(!parseWebExtensionPrivacySettingsHaiku(object(json).get(), "install-a"_s), "invalid saved scope or value cannot silently change privacy behavior");
    }

    namespace fs = std::filesystem;
    auto temporary = (fs::temp_directory_path() / "summit-privacy-settings-XXXXXX").string();
    RELEASE_ASSERT(mkdtemp(temporary.data()));
    fs::path root(temporary);
    auto file = String::fromUTF8((root / "State.json").c_str());
    check(writeWebExtensionStateHaiku(file, state.get()).has_value(), "actual native atomic writer commits the privacy payload");
    auto read = readWebExtensionStateHaiku(file);
    std::expected<WebExtensionPrivacySettingsHaiku, String> disk = makeUnexpected("read failed"_s);
    if (read)
        disk = parseWebExtensionPrivacySettingsHaiku(read->get(), "install-a"_s);
    check(disk && disk->values == original.values, "native disk reload preserves every privacy override");
    std::ofstream(root / "blocked-parent") << "ordinary file";
    auto blocked = String::fromUTF8((root / "blocked-parent/State.json").c_str());
    check(!writeWebExtensionStateHaiku(blocked, state.get()), "native writer reports an unusable parent instead of success");
    read = readWebExtensionStateHaiku(file);
    check(read && (*read)->toJSONString() == encoded, "failed write leaves the committed privacy payload unchanged");
    fs::create_directory(root / "occupied");
    std::ofstream(root / "occupied/marker") << "keep";
    auto occupied = String::fromUTF8((root / "occupied").c_str());
    auto replace = writeWebExtensionStateHaiku(occupied, state.get());
    check(!replace && replace.error() == WebExtensionStateErrorHaiku::ReplaceFailed, "failed rename is reported after writing the complete temporary payload");
    check(fs::is_regular_file(root / "occupied/marker"), "failed rename preserves the destination contents");
    bool leakedTemporary = false;
    for (const auto& entry : fs::directory_iterator(root))
        leakedTemporary |= entry.path().filename().string().starts_with("State-");
    check(!leakedTemporary, "failed atomic replacement removes its temporary payload");
    std::error_code cleanupError;
    fs::remove_all(root, cleanupError);
    check(!cleanupError && !fs::exists(root), "owned test files are removed");
    printf("PRIVACY_SETTINGS_RESULT %s checks=%u failures=%u\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
