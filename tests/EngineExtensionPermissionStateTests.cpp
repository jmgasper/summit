#include "config.h"
#include "WebExtensionPermissionState.h"
#include "WebExtensionStateHaiku.h"

#include <Application.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <wtf/MainThread.h>
#include <wtf/text/MakeString.h>

using namespace WebKit;
static unsigned checks, failures;

static void check(bool result, const char* description)
{
    ++checks;
    failures += !result;
    std::printf("%s %s\n", result ? "PASS" : "FAIL", description);
}

static auto details(const String& text)
{
    auto value = JSON::Value::parseJSON(text);
    return parseWebExtensionPermissionDetails(value.get());
}

static auto saved(const String& text, double now = 100)
{
    auto value = JSON::Value::parseJSON(text);
    RefPtr object = value ? value->asObject() : nullptr;
    return readWebExtensionPermissionState(object.get(), WallTime::fromRawSeconds(now));
}

static void checkDetails()
{
    check(!parseWebExtensionPermissionDetails(nullptr), "missing details are rejected");
    for (auto invalid : { "null"_s, "[]"_s, "true"_s, "1"_s, "\"tabs\""_s })
        check(!details(invalid), "details must be an object");
    for (auto empty : { "{}"_s, R"({"permissions":[],"origins":[]})"_s }) {
        auto value = details(empty);
        check(value && value->permissions.isEmpty() && value->origins.isEmpty(), "empty permission sets are valid");
    }
    for (auto key : { "permissions"_s, "origins"_s }) {
        for (auto invalid : { "null"_s, "true"_s, "5"_s, "{}"_s, "\"tabs\""_s, "[1]"_s, "[false]"_s,
                "[null]"_s, "[{}]"_s, "[[]]"_s, "[\"\"]"_s, "[\"tabs\\u0000ignored\"]"_s })
            check(!details(makeString("{\""_s, key, "\":"_s, invalid, '}')), "invalid member types and truncated identifiers are rejected");
    }
    check(!details(R"({"permissions":["tabs"],"data_collection":[]})"_s), "unsupported fields cannot silently report a grant");
    auto value = details(R"({"permissions":["tabs","tabs","storage"],"origins":["https://example.org/*","https://example.org/*"]})"_s);
    check(value && value->permissions == HashSet<String> { "tabs"_s, "storage"_s }
        && value->origins == HashSet<String> { "https://example.org/*"_s }, "both categories are deduplicated without losing entries");
    auto source = JSON::Value::parseJSON(R"({"permissions":["__proto__","unrecognized"],"origins":["invalid-pattern"]})"_s);
    auto before = source->toJSONString();
    value = parseWebExtensionPermissionDetails(source.get());
    check(value && value->permissions.contains("unrecognized"_s) && value->origins.contains("invalid-pattern"_s),
        "structural parsing preserves names for the context's manifest validation");
    check(source->toJSONString() == before, "details parsing does not mutate the caller's data");
}

static void checkCodec()
{
    auto missing = readWebExtensionPermissionState(nullptr, WallTime::fromRawSeconds(100));
    check(missing && missing->permissions.isEmpty() && missing->origins.isEmpty(), "a fresh context has no saved grants");
    check(saved("{}"_s)->permissions.isEmpty(), "unrelated state grants no permissions");
    for (auto invalid : { "null"_s, "true"_s, "[]"_s, "{}"_s,
            R"({"version":0,"permissions":{},"origins":{}})"_s,
            R"({"version":2,"permissions":{},"origins":{}})"_s,
            R"({"version":1.5,"permissions":{},"origins":{}})"_s,
            R"({"version":"1","permissions":{},"origins":{}})"_s,
            R"({"version":true,"permissions":{},"origins":{}})"_s,
            R"({"version":1,"permissions":{}})"_s,
            R"({"version":1,"permissions":[],"origins":{}})"_s,
            R"({"version":1,"permissions":{},"origins":null})"_s })
        check(!saved(makeString("{\"PermissionGrants\":"_s, invalid, '}')), "malformed or unknown state versions fail closed");
    for (auto key : { "permissions"_s, "origins"_s }) {
        for (auto invalid : { "true"_s, "\"never\""_s, "[]"_s, "{}"_s }) {
            auto object = JSON::Object::create();
            auto grants = JSON::Object::create();
            grants->setInteger("version"_s, 1);
            grants->setObject("permissions"_s, JSON::Object::create());
            grants->setObject("origins"_s, JSON::Object::create());
            auto entries = JSON::Object::create();
            entries->setValue("tabs"_s, JSON::Value::parseJSON(invalid).releaseNonNull());
            grants->setObject(key, WTF::move(entries));
            object->setObject("PermissionGrants"_s, WTF::move(grants));
            check(!readWebExtensionPermissionState(object.ptr(), WallTime::fromRawSeconds(100)), "non-numeric expiration values are rejected");
        }
    }
    check(!saved(R"({"PermissionGrants":{"version":1,"permissions":{"":null},"origins":{}}})"_s), "empty saved permission names are rejected");
    check(!saved(R"({"PermissionGrants":{"version":1,"permissions":{},"origins":{"x\u0000y":null}}})"_s), "embedded NUL in saved origins is rejected");
    auto values = saved(R"({"PermissionGrants":{"version":1,"permissions":{"permanent":null,"expired":99,"boundary":100,"temporary":100.5,"ancient":-50},"origins":{"https://example.org/*":101,"https://old.org/*":90}}})"_s);
    check(values && values->permissions.size() == 2 && values->origins.size() == 1, "expired grants are pruned from both categories");
    check(values && values->permissions.get("permanent"_s) == WallTime::infinity(), "null preserves a permanent grant");
    check(values && values->permissions.get("temporary"_s) == WallTime::fromRawSeconds(100.5), "fractional finite expiration is preserved");
    check(values && !values->permissions.contains("boundary"_s), "a grant expires at its exact deadline");

    auto original = JSON::Value::parseJSON(R"({"StorageAccessLevels":{"session":"TRUSTED_CONTEXTS"},"Commands":{"open":{"description":"Keep me"}},"PermissionGrants":{"version":9}})"_s)->asObject();
    auto before = original->toJSONString();
    WebExtensionPermissionState grants;
    grants.permissions.set("tabs"_s, WallTime::infinity());
    grants.permissions.set("storage"_s, WallTime::fromRawSeconds(150.25));
    grants.origins.set("https://example.org/*"_s, WallTime::infinity());
    auto state = stateWithWebExtensionPermissions(original.get(), grants);
    check(state.has_value(), "a current snapshot replaces obsolete saved permission data");
    if (!state)
        return;
    auto restored = readWebExtensionPermissionState(state->ptr(), WallTime::fromRawSeconds(100));
    check(restored && restored->permissions == grants.permissions && restored->origins == grants.origins, "finite and permanent grants round-trip exactly");
    check(original->toJSONString() == before, "preparing a transaction leaves the old state intact");
    check(state->get().getObject("StorageAccessLevels"_s)->getString("session"_s) == "TRUSTED_CONTEXTS"_s,
        "permission changes retain unrelated storage settings");
    state->get().getObject("Commands"_s)->setString("new"_s, "changed"_s);
    check(original->toJSONString() == before, "nested state is copied instead of aliasing the live state");
    auto later = readWebExtensionPermissionState(state->ptr(), WallTime::fromRawSeconds(151));
    check(later && later->permissions.size() == 1 && later->permissions.contains("tabs"_s), "restoring later never extends a temporary grant");
    auto empty = stateWithWebExtensionPermissions(state->ptr(), { });
    auto removed = empty ? readWebExtensionPermissionState(empty->ptr(), WallTime::fromRawSeconds(100)) : restored;
    check(empty && removed && removed->permissions.isEmpty() && removed->origins.isEmpty(), "an empty snapshot persists revocation of every saved grant");
    for (auto expiration : { WallTime::nan(), -WallTime::infinity() }) {
        grants.permissions.set("tabs"_s, expiration);
        check(!stateWithWebExtensionPermissions(original.get(), grants), "invalid expiration cannot be serialized as a permanent grant");
        check(original->toJSONString() == before, "failed serialization preserves the old state");
    }
    grants.permissions.clear();
    for (auto invalid : { emptyString(), makeString("tabs"_s, static_cast<UChar>(0), "ignored"_s) }) {
        grants.origins.clear();
        grants.origins.set(invalid, WallTime::infinity());
        check(!stateWithWebExtensionPermissions(nullptr, grants), "invalid identifiers cannot be written into saved grants");
    }
}

static void checkDisk()
{
    char temporary[] = "/tmp/summit-permission-state-XXXXXX";
    if (!mkdtemp(temporary)) {
        check(false, "native temporary directory is created");
        return;
    }
    auto root = std::filesystem::path(temporary);
    auto path = root / "Permission State %25.json";
    auto nativePath = [](const std::filesystem::path& path) { return String::fromUTF8(path.c_str()); };
    auto original = JSON::Object::create();
    original->setString("ExtensionName"_s, String::fromUTF8("雪 extension"));
    WebExtensionPermissionState grants;
    grants.permissions.set("tabs"_s, WallTime::infinity());
    grants.origins.set("https://example.org/*"_s, WallTime::fromRawSeconds(200));
    auto first = stateWithWebExtensionPermissions(original.ptr(), grants);
    check(first && writeWebExtensionStateHaiku(nativePath(path), first->get()).has_value(), "initial grant snapshot is written through the native atomic writer");
    auto disk = readWebExtensionStateHaiku(nativePath(path));
    check(disk.has_value(), "saved permission state is read from the native filesystem");
    if (disk) {
        auto restored = readWebExtensionPermissionState(disk->ptr(), WallTime::fromRawSeconds(100));
        check(restored && restored->permissions == grants.permissions && restored->origins == grants.origins, "disk reload restores exactly the accepted grants");
        check(disk->get().getString("ExtensionName"_s) == String::fromUTF8("雪 extension"), "disk grant changes retain unrelated Unicode metadata");
    }
    struct stat info;
    check(!stat(path.c_str(), &info) && (info.st_mode & 0777) == 0600, "saved grants are private to the owning user");
    auto revoked = stateWithWebExtensionPermissions(first->ptr(), { });
    check(revoked && writeWebExtensionStateHaiku(nativePath(path), revoked->get()).has_value(), "revocation replaces the saved snapshot");
    disk = readWebExtensionStateHaiku(nativePath(path));
    if (disk) {
        auto restored = readWebExtensionPermissionState(disk->ptr(), WallTime::fromRawSeconds(100));
        check(restored && restored->permissions.isEmpty() && restored->origins.isEmpty(), "revoked grants stay revoked after disk reload");
    } else
        check(false, "revoked snapshot is readable");
    auto absent = writeWebExtensionStateHaiku(nativePath(root / "absent/State.json"), first->get());
    check(!absent && absent.error() == WebExtensionStateErrorHaiku::TemporaryFileFailed, "missing parent reports a write failure");
    auto blocked = root / "blocked";
    std::filesystem::create_directory(blocked);
    { std::ofstream stream(blocked / "keep"); stream << "original"; }
    auto failed = writeWebExtensionStateHaiku(nativePath(blocked), first->get());
    check(!failed && failed.error() == WebExtensionStateErrorHaiku::ReplaceFailed, "failed atomic replacement reports failure");
    std::string keep;
    { std::ifstream stream(blocked / "keep"); stream >> keep; }
    check(keep == "original", "failed replacement leaves the existing destination intact");
    unsigned leftover = 0;
    for (auto& entry : std::filesystem::directory_iterator(root))
        leftover += entry.path().filename().string().starts_with("State-");
    check(!leftover, "successful and failed transactions leave no temporary state files");
    auto unchanged = readWebExtensionStateHaiku(nativePath(path));
    check(unchanged && disk && unchanged->get().toJSONString() == disk->get().toJSONString(), "failed writes do not modify another saved snapshot");
    { std::ofstream stream(path); stream << "{\"PermissionGrants\":"; }
    auto corrupt = readWebExtensionStateHaiku(nativePath(path));
    check(!corrupt && corrupt.error() == WebExtensionStateErrorHaiku::InvalidJSON, "a truncated grant file reports a read error");
    std::filesystem::remove_all(root);
}

int main()
{
    BApplication application("application/x-vnd.Kunanyi-Summit-permission-state-tests");
    WTF::initializeMainThread();
    checkDetails();
    checkCodec();
    checkDisk();
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
