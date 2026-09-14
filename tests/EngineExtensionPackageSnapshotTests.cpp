#include "config.h"
#include "WebExtensionPackageSnapshotHaiku.h"
#include "WebExtensionInstallStateHaiku.h"
#include <Application.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <wtf/FileSystem.h>
#include <wtf/MainThread.h>

using namespace WebKit;
namespace fs = std::filesystem;
using Error = WebExtensionPackageErrorHaiku;
static unsigned checks;
static unsigned failures;
static void check(bool value, const char* label)
{
    ++checks;
    if (!value) { ++failures; std::printf("FAIL: %s\n", label); }
}
static void put(const fs::path& path, const std::string& bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary); output.write(bytes.data(), bytes.size());
    if (!output) std::abort();
}
static std::string get(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}
static String string(const fs::path& path) { return String::fromUTF8(path.string()); }
static auto snapshot(const fs::path& source, const fs::path& parent, const WebExtensionArchiveLimitsHaiku& limits = { })
{
    return snapshotWebExtensionPackageHaiku(string(source), limits, string(parent));
}
static size_t children(const fs::path& path) { return std::distance(fs::directory_iterator(path), fs::directory_iterator()); }

int main()
{
    BApplication app("application/x-vnd.Kunanyi-Summit-package-snapshot-tests");
    WTF::initializeMainThread();
    std::string pattern = (fs::temp_directory_path() / "SummitPackageTests-XXXXXX").string();
    if (!mkdtemp(pattern.data())) return 2;
    fs::path root(pattern), packages = root / "packages", first = root / "first", second = root / "second";
    fs::create_directories(packages);
    put(first / "manifest.json", "{\"manifest_version\":3,\"name\":\"test\",\"version\":\"1.0\"}");
    put(first / "sub" / "a.js", std::string("a\0b", 3));
    put(first / "empty", "");
    put(first / "résource #%?.txt", "unicode path");
    put(second / "résource #%?.txt", "unicode path");
    put(second / "empty", "");
    put(second / "sub" / "a.js", std::string("a\0b", 3));
    put(second / "manifest.json", get(first / "manifest.json"));
    put(root / "golden" / "x", "abc");
    {
        auto golden = snapshot(root / "golden", packages);
        check(golden && golden->fingerprint().convertToASCIILowercase() == "09213d514d4352fd593ba1113bee1a448bcd4d270cb1f7fd8389256dae345b49"_s, "package format matches independently computed SHA-256 vector");
    }
    String oldFingerprint;
    fs::path ownedPath;
    {
        auto a = snapshot(first, packages);
        check(!!a, "directory with nested, empty and Unicode resources snapshots");
        if (!a) { fs::remove_all(root); return 1; }
        oldFingerprint = a->fingerprint(); ownedPath = fs::path(a->path().utf8().legacyCStringPointer());
        check(oldFingerprint.length() == 64, "fingerprint contains a full SHA-256 digest");
        check(get(ownedPath / "sub" / "a.js") == std::string("a\0b", 3), "binary resource bytes including NUL are preserved");
        check(get(ownedPath / "résource #%?.txt") == "unicode path", "native filename bytes survive snapshot");
        check(fs::file_size(ownedPath / "empty") == 0, "zero-byte resources are preserved");
        auto b = snapshot(second, packages);
        check(b && b->fingerprint() == oldFingerprint, "identity is independent of source directory and creation order");
        chmod((second / "empty").c_str(), 0644);
        auto modeChanged = snapshot(second, packages);
        check(modeChanged && modeChanged->fingerprint() == oldFingerprint, "metadata-only changes do not change content identity");
        put(first / "sub" / "a.js", "new code");
        check(get(ownedPath / "sub" / "a.js") == std::string("a\0b", 3), "edits to source do not change retained snapshot resources");
        auto changed = snapshot(first, packages);
        check(changed && changed->fingerprint() != oldFingerprint, "code change with identical manifest version changes fingerprint");
        fs::rename(first / "empty", first / "renamed");
        auto renamed = snapshot(first, packages);
        check(changed && renamed && changed->fingerprint() != renamed->fingerprint(), "resource rename changes fingerprint");
        fs::create_directory(first / "empty-directory");
        auto emptyDirectory = snapshot(first, packages);
        check(renamed && emptyDirectory && renamed->fingerprint() != emptyDirectory->fingerprint(), "empty directory presence participates in identity");
    }
    check(!fs::exists(ownedPath), "snapshot destruction removes its owned directory");
    check(children(packages) == 0, "all unconsumed snapshots clean up their resources");
    {
        auto a = snapshot(second, packages);
        check(!!a, "snapshot can be created again");
        if (a) {
            auto moved = WTF::move(*a);
            auto released = moved.release();
            check(!released.isEmpty() && moved.path().isEmpty(), "ownership release detaches the directory");
            ownedPath = fs::path(released.utf8().legacyCStringPointer());
        }
    }
    check(fs::exists(ownedPath), "released snapshot remains for the extension owner");
    fs::remove_all(ownedPath);

    fs::path linked = root / "linked";
    put(linked / "data" / "x", "inside");
    fs::create_symlink("data/x", linked / "alias");
    fs::create_directory_symlink("data", linked / "directory-alias");
    {
        auto a = snapshot(linked, packages);
        check(!!a, "contained file and directory symlinks are supported");
        if (a) {
            fs::path path(a->path().utf8().legacyCStringPointer());
            check(get(path / "alias") == "inside" && !fs::is_symlink(path / "alias"), "contained file alias is materialized");
            check(get(path / "directory-alias" / "x") == "inside" && !fs::is_symlink(path / "directory-alias"), "contained directory alias is materialized");
        }
    }
    fs::create_symlink("../second/manifest.json", linked / "escape");
    auto escaped = snapshot(linked, packages);
    check(!escaped && escaped.error() == Error::InvalidPath, "symlinks escaping the package are rejected");
    check(children(packages) == 0, "failed snapshot removes partial output");
    fs::remove(linked / "escape");
    fs::create_directory_symlink(".", linked / "cycle");
    check(!snapshot(linked, packages), "directory symlink cycles are rejected");
    fs::remove(linked / "cycle");
    fs::create_symlink("missing", linked / "dangling");
    check(!snapshot(linked, packages), "dangling aliases are rejected");
    fs::remove(linked / "dangling");
    mkfifo((linked / "pipe").c_str(), 0600);
    check(!snapshot(linked, packages), "FIFO resources are rejected without blocking");
    fs::remove(linked / "pipe");

    WebExtensionArchiveLimitsHaiku limits;
    limits.entries = 0;
    check(!snapshot(second, packages, limits), "entry limit is enforced");
    limits = { }; limits.fileBytes = 2;
    check(!snapshot(second, packages, limits), "individual file size limit is enforced");
    limits = { }; limits.expandedBytes = 3;
    check(!snapshot(second, packages, limits), "total resource byte limit is enforced");
    limits = { }; limits.pathDepth = 0;
    check(!snapshot(second, packages, limits), "directory depth limit is enforced");
    check(!snapshot(root / "golden", packages, limits), "zero path components also rejects a root-level file");
    limits.pathDepth = 1;
    {
        auto oneLevel = snapshot(root / "golden", packages, limits);
        check(!!oneLevel, "one path component permits a root-level file");
    }
    check(!snapshot(second, packages, limits), "one path component rejects a nested file");
    limits = { }; limits.pathBytes = 3;
    check(!snapshot(second, packages, limits), "resource path length limit is enforced");
    check(!snapshot(second, second), "snapshot output cannot be inside the source package");
    check(!snapshot(second / "manifest.json", packages), "source must be a directory");
    check(!snapshot(root / "missing", packages), "missing package is rejected");
    check(!snapshotWebExtensionPackageHaiku("relative"_s), "relative source path is rejected");
    check(children(packages) == 0, "all failed limit checks clean up partial output");

    auto bytes = [](const char* data, size_t length) { return std::span { reinterpret_cast<const uint8_t*>(data), length }; };
    auto memory = [&](Vector<WebExtensionMemoryResourceHaiku>&& data, Vector<WebExtensionMemoryResourceHaiku>&& strings = { }) {
        return fingerprintWebExtensionMemoryResourcesHaiku("{}"_s, WTF::move(data), WTF::move(strings));
    };
    auto ab = memory({ { "ab"_s, bytes("c", 1) } });
    check(ab.convertToASCIILowercase() == "80dc371a2b1dd825f84435424384bd8a4ab2c2ef9911fadfe29eb8b85f06bea8"_s, "memory format matches independently computed SHA-256 vector");
    check(ab != memory({ { "a"_s, bytes("bc", 2) } }), "path and content lengths prevent concatenation collisions");
    check(ab == memory({ { "ab"_s, bytes("c", 1) } }), "memory fingerprint is deterministic");
    check(ab != memory({ }, { { "ab"_s, bytes("c", 1) } }), "data and string resource precedence are represented");
    check(memory({ { "x"_s, bytes("a\0b", 3) } }) != memory({ { "x"_s, bytes("a", 1) } }), "binary data length participates in identity");
    auto nulName = String::fromUTF8(std::string("x\0y", 3));
    check(nulName.length() == 3, "NUL-name fixture preserves all three characters");
    check(memory({ { nulName, bytes("a", 1) } }) != memory({ { "x"_s, bytes("a", 1) } }), "NUL in memory resource name is not truncated");
    check(memory({ { "z"_s, bytes("1", 1) }, { "a"_s, bytes("2", 1) } }) == memory({ { "a"_s, bytes("2", 1) }, { "z"_s, bytes("1", 1) } }), "memory entry order does not change identity");
    check(memory({ }) != fingerprintWebExtensionMemoryResourcesHaiku("{\"version\":\"2\"}"_s, { }, { }), "manifest participates in memory identity");

    using Purpose = WebExtensionLoadPurposeHaiku;
    using Reason = WebExtensionInstallReasonHaiku;
    auto fingerprint = memory({ });
    auto different = memory({ { "x"_s, bytes("y", 1) } });
    Ref state = JSON::Object::create();
    auto install = [&](const String& version, const String& hash, Purpose purpose) {
        return updateWebExtensionInstallStateHaiku(state.get(), version, hash, purpose);
    };
    auto decision = install("1"_s, fingerprint, Purpose::UserInitiated);
    check(decision.reason == Reason::Install && !decision.fireStartup, "first installation gets install, not startup");
    check(decision.previousVersion.isEmpty() && decision.clearCachedState, "first install has no invented previous version and clears unknown state");
    decision = install("1"_s, fingerprint, Purpose::UserInitiated);
    check(decision.reason == Reason::None && !decision.fireStartup && !decision.clearCachedState, "reenabling unchanged package emits no install/update/startup");
    decision = install("1"_s, fingerprint, Purpose::BrowserStartup);
    check(decision.reason == Reason::None && decision.fireStartup && !decision.clearCachedState, "explicit profile startup fires startup for existing installation");
    decision = install("1"_s, different, Purpose::UserInitiated);
    check(decision.reason == Reason::Update && decision.previousVersion == "1"_s && decision.clearCachedState, "changed resources trigger update at the same version");
    decision = install("2"_s, different, Purpose::BrowserStartup);
    check(decision.reason == Reason::Update && decision.previousVersion == "1"_s && decision.fireStartup, "updated version reports actual previous version at startup");
    decision = install("2"_s, different, Purpose::BrowserUpdate);
    check(decision.reason == Reason::BrowserUpdate && decision.fireStartup && !decision.clearCachedState, "browser update preserves unchanged extension caches");
    decision = install("3"_s, fingerprint, Purpose::BrowserUpdate);
    check(decision.reason == Reason::Update, "extension update takes precedence over simultaneous browser update");
    decision = install("4"_s, different, Purpose::PrivateBrowsing);
    check(decision.reason == Reason::None && !decision.fireStartup && decision.clearCachedState, "private context refreshes state without install/startup events");
    Ref privateState = JSON::Object::create();
    decision = updateWebExtensionInstallStateHaiku(privateState.get(), "1"_s, fingerprint, Purpose::PrivateBrowsing);
    check(decision.reason == Reason::None && !decision.fireStartup, "new private context does not fabricate an installation");
    Ref firstStartup = JSON::Object::create();
    decision = updateWebExtensionInstallStateHaiku(firstStartup.get(), "1"_s, fingerprint, Purpose::BrowserStartup);
    check(decision.reason == Reason::Install && !decision.fireStartup, "first installation remains install during profile startup");
    Ref legacy = JSON::Object::create();
    legacy->setString("LastSeenVersion"_s, "1"_s);
    decision = updateWebExtensionInstallStateHaiku(legacy.get(), "1"_s, fingerprint, Purpose::UserInitiated);
    check(decision.reason == Reason::None && decision.clearCachedState, "legacy state establishes fingerprint without inventing a content update");
    check(legacy->getString("LastSeenResourceFingerprint"_s) == fingerprint && legacy->getInteger("ResourceFingerprintFormat"_s) == 1, "current fingerprint and format are persisted");
    legacy->setString("LastSeenResourceFingerprint"_s, "corrupt"_s);
    decision = updateWebExtensionInstallStateHaiku(legacy.get(), "1"_s, fingerprint, Purpose::UserInitiated);
    check(decision.reason == Reason::None && decision.clearCachedState, "malformed fingerprint invalidates caches without an unproven update event");
    legacy->setDouble("ResourceFingerprintFormat"_s, 1.5);
    decision = updateWebExtensionInstallStateHaiku(legacy.get(), "1"_s, fingerprint, Purpose::UserInitiated);
    check(decision.reason == Reason::None && decision.clearCachedState, "fractional format marker is not accepted as format one");
    legacy->setInteger("ResourceFingerprintFormat"_s, 999);
    decision = updateWebExtensionInstallStateHaiku(legacy.get(), "2"_s, different, Purpose::UserInitiated);
    check(decision.reason == Reason::Update && decision.previousVersion == "1"_s, "version change remains detectable across fingerprint format migration");
    legacy->remove("LastSeenVersion"_s);
    decision = updateWebExtensionInstallStateHaiku(legacy.get(), "3"_s, fingerprint, Purpose::UserInitiated);
    check(decision.reason == Reason::Update && decision.previousVersion.isEmpty(), "known package change does not fabricate missing previous version");

    fs::remove_all(root);
    std::printf("%u package snapshot checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
