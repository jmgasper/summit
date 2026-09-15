/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebKitContext.h"
#include "WebKitView.h"
#include <Application.h>
#include <MessageRunner.h>
#include <OS.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;
namespace {
constexpr uint32 heartbeat = 'pkhb';
unsigned passed, failed;
void check(bool value, const char* label)
{
    ++(value ? passed : failed);
    printf("%s %s\n", value ? "PASS" : "FAIL", label);
    fflush(stdout);
}
void put(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(text.data(), text.size());
    if (!output)
        std::abort();
}
std::string read(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}
std::set<fs::path> snapshots(const fs::path& directory)
{
    std::set<fs::path> result;
    for (auto& entry : fs::directory_iterator(directory))
        if (entry.path().filename().string().starts_with("WebKitPackage-"))
            result.insert(entry.path());
    return result;
}
std::string field(const BMessage& message, const char* name)
{
    const char* value = nullptr;
    return message.FindString(name, &value) == B_OK && value ? value : "";
}
std::set<std::string> fields(const BMessage& message, const char* name)
{
    std::set<std::string> result;
    const char* value;
    for (int32 index = 0; message.FindString(name, index, &value) == B_OK; ++index)
        result.insert(value);
    return result;
}
bool hasChildren()
{
    team_info team;
    int32 cookie = 0;
    while (get_next_team_info(&cookie, &team) == B_OK)
        if (team.parent == getpid())
            return true;
    return false;
}

class PreparationTests final : public BApplication {
public:
    PreparationTests(fs::path root, fs::path archive)
        : BApplication("application/x-vnd.Kunanyi-Summit-ExtensionPackagePreparationTests")
        , m_root(std::move(root)), m_archive(std::move(archive)) { }

    void ReadyToRun() final
    {
        m_deadline = system_time() + 60000000;
        // BApplication::SetPulseRate rounds down to 100 ms increments. Use a
        // messenger timer to observe the looper during short filesystem work.
        BMessage tick(heartbeat);
        m_heartbeat = std::make_unique<BMessageRunner>(BMessenger(this), &tick, 10000);
        check(m_heartbeat->InitCheck() == B_OK, "preparation heartbeat timer initializes");
        m_context = std::make_unique<BWebKitContext>((m_root / "profile").c_str());
        check(m_context->InitCheck() == B_OK, "public browser context initializes");
        if (m_context->InitCheck() != B_OK) {
            finish();
            return;
        }
        BMessenger target(this);
        check(m_context->PrepareExtension("relative", target, 99) == B_BAD_VALUE,
            "public preparation rejects a relative source path");
        check(m_context->PrepareExtension((m_root / "directory").c_str(), target, 0) == B_BAD_VALUE
            && m_context->PrepareExtension((m_root / "directory").c_str(), target, std::numeric_limits<uint64>::max()) == B_BAD_VALUE,
            "public preparation rejects reserved request identifiers");
        check(m_context->PrepareExtension((m_root / "directory").c_str(), BMessenger(), 99) == B_BAD_VALUE,
            "public preparation requires a live reply target");
        submit(m_root / "directory", 1);
        submit(m_archive, 2);
        submit(m_root / "bad.xpi", 3);
        submit(m_root / "bad-manifest", 4);
        submit(m_root / "missing", 5);
        submit(m_root / "directory", 6);
        m_context->CancelExtensionPreparation(6);
        submit(m_archive, 6);
        submit(m_root / "directory", 7);
        submit(m_root / "directory", 7);
        submit(m_root / "directory", 8);
        check(m_context->HasPendingExtensionPreparations(), "public API reports pending worker preparation");
    }

    void Pulse() final
    {
        if (m_context && m_context->HasPendingExtensionPreparations())
            ++m_pendingPulses;
        if (hasChildren())
            m_spawnedProcess = true;
        if (m_finishing) {
            if (!m_context || !m_context->HasPendingExtensionPreparations()) {
                m_other.reset();
                m_context.reset();
                check(snapshots(m_root / "temporary").empty(), "context destruction removes every retained package snapshot");
                check(fs::is_empty(m_root / "temporary"), "failed and cancelled preparation leaves no extraction directories");
                check(!m_spawnedProcess && !hasChildren(), "preparation never launches an extension or network helper");
                PostMessage(B_QUIT_REQUESTED);
            }
            return;
        }
        if (system_time() > m_deadline) {
            check(false, "public package preparation completes before its deadline");
            finish();
        }
    }

    void MessageReceived(BMessage* message) final
    {
        if (message->what == heartbeat) {
            Pulse();
            return;
        }
        if (message->what != B_WEBKIT_EXTENSION_PACKAGE_PREPARED && message->what != B_WEBKIT_EXTENSION_PACKAGE_DISCARDED) {
            BApplication::MessageReceived(message);
            return;
        }
        uint64 identifier = 0;
        int32 error = B_ERROR;
        check(message->FindUInt64("identifier", &identifier) == B_OK && message->FindInt32("error", &error) == B_OK,
            "SDK reply carries its request identifier and terminal status");
        if (!identifier) {
            finish();
            return;
        }
        if (m_finishing)
            return;
        if (message->what == B_WEBKIT_EXTENSION_PACKAGE_DISCARDED) {
            if (identifier == 100) {
                check(error == B_NAME_NOT_FOUND, "prepared tokens are scoped to their owning browser context");
                m_context->DiscardPreparedExtension(m_tokens.front().c_str(), BMessenger(this), 101);
            } else if (identifier == 101) {
                check(error == B_OK, "owner can discard a prepared package");
                check(snapshots(m_root / "temporary").size() == 4, "discard releases exactly one owned package directory");
                m_context->DiscardPreparedExtension(m_tokens.front().c_str(), BMessenger(this), 102);
            } else if (identifier == 102) {
                check(error == B_NAME_NOT_FOUND, "discarding the same package again reports a stale token");
                finish();
            } else {
                check(false, "discard reply has an expected identifier");
                finish();
            }
            return;
        }
        if (identifier == 9) {
            check(error == B_OK && field(*message, "name") == "Summit changed source"
                && field(*message, "fingerprint") != m_originalFingerprint,
                "a later preparation sees changed source bytes and a new fingerprint");
            if (error != B_OK) {
                finish();
                return;
            }
            unsigned originalDirectories = 0;
            for (auto& path : m_originalSnapshots)
                if (read(path / "manifest.json").find("Summit directory package") != std::string::npos)
                    ++originalDirectories;
            check(originalDirectories == 2, "earlier prepared snapshots retain their original manifest after source edits");
            m_tokens.push_back(field(*message, "token"));
            check(snapshots(m_root / "temporary").size() == 5, "only successful prepared packages retain directories");
            m_other = std::make_unique<BWebKitContext>(nullptr, true);
            check(m_other->InitCheck() == B_OK, "independent private browser context initializes");
            m_other->DiscardPreparedExtension(m_tokens.front().c_str(), BMessenger(this), 100);
            return;
        }
        auto key = std::make_pair(identifier, error);
        ++m_replies[key];
        if (error == B_OK) {
            bool archive = identifier == 2 || identifier == 6;
            check(field(*message, "name") == (archive ? "Summit archive package" : "Summit directory package")
                && field(*message, "version") == (archive ? "2.0" : "1.0"),
                "prepared metadata belongs to the requested package, including a reused request ID");
            double manifestVersion = 0;
            check(message->FindDouble("manifest_version", &manifestVersion) == B_OK && manifestVersion == 2
                && fields(*message, "permission") == std::set<std::string> { "cookies", "storage" }
                && fields(*message, "origin") == std::set<std::string> { "https://summit-cookie.invalid/*", "https://summit-content.invalid/*" },
                "public preparation returns the actual manifest and requested permissions");
            check(field(*message, "manifest_json").find("summitUnknownCapability") != std::string::npos,
                "complete manifest preserves requirements outside WebKit's recognized permission set");
            auto token = field(*message, "token");
            check(!token.empty() && field(*message, "fingerprint").size() == 64,
                "prepared package has an opaque token and full resource fingerprint");
            m_tokens.push_back(token);
            if (identifier == 1)
                m_originalFingerprint = field(*message, "fingerprint");
        }
        if (++m_initialReplies != 10)
            return;
        std::map<std::pair<uint64, int32>, unsigned> expected {
            { { 1, B_OK }, 1 }, { { 2, B_OK }, 1 }, { { 3, B_BAD_DATA }, 1 },
            { { 4, B_BAD_DATA }, 1 }, { { 5, B_ENTRY_NOT_FOUND }, 1 },
            { { 6, B_CANCELED }, 1 }, { { 6, B_OK }, 1 },
            { { 7, B_NAME_IN_USE }, 1 }, { { 7, B_OK }, 1 }, { { 8, B_BUSY }, 1 }
        };
        check(m_replies == expected, "each successful, rejected, cancelled and reused operation gets its own terminal result");
        if (failed) {
            finish();
            return;
        }
        check(!m_context->HasPendingExtensionPreparations(), "all accepted worker preparations drain");
        check(m_pendingPulses > 0, "application looper remains responsive while package copying and hashing are pending");
        m_originalSnapshots = snapshots(m_root / "temporary");
        check(m_originalSnapshots.size() == 4, "cancelled and invalid packages leave no retained snapshots");
        auto manifest = read(m_root / "directory/manifest.json");
        manifest.replace(manifest.find("Summit directory package"), std::string("Summit directory package").size(), "Summit changed source");
        put(m_root / "directory/manifest.json", manifest);
        submit(m_root / "directory", 9);
    }

private:
    void submit(const fs::path& path, uint64 identifier)
    {
        check(m_context->PrepareExtension(path.c_str(), BMessenger(this), identifier) == B_OK,
            "public API accepts an asynchronous preparation request");
    }
    void finish()
    {
        m_finishing = true;
        if (m_context)
            for (uint64 identifier = 1; identifier <= 9; ++identifier)
                m_context->CancelExtensionPreparation(identifier);
    }
    fs::path m_root, m_archive;
    std::unique_ptr<BWebKitContext> m_context, m_other;
    std::unique_ptr<BMessageRunner> m_heartbeat;
    std::map<std::pair<uint64, int32>, unsigned> m_replies;
    std::vector<std::string> m_tokens;
    std::set<fs::path> m_originalSnapshots;
    std::string m_originalFingerprint;
    unsigned m_initialReplies { 0 }, m_pendingPulses { 0 };
    bigtime_t m_deadline { 0 };
    bool m_finishing { false }, m_spawnedProcess { false };
};
}

int main()
{
    const char* archive = getenv("SUMMIT_EXTENSION_PREPARATION_ARCHIVE");
    if (!archive || !fs::path(archive).is_absolute())
        return 2;
    char temporary[] = "/tmp/summit-package-preparation-XXXXXX";
    if (!mkdtemp(temporary))
        return 2;
    fs::path root(temporary);
    fs::create_directory(root / "temporary");
    setenv("TMPDIR", (root / "temporary").c_str(), 1);
    put(root / "directory/manifest.json", R"JSON({"manifest_version":2,"name":"Summit directory package","version":"1.0","permissions":["storage","cookies","https://summit-cookie.invalid/*","summitUnknownCapability"],"background":{"page":"background.html","persistent":true},"content_scripts":[{"matches":["https://summit-content.invalid/*"],"js":["background.js"]}]})JSON");
    put(root / "directory/background.html", "<!doctype html><script src=background.js></script>");
    put(root / "directory/background.js", "throw new Error('Preparation must not execute extension code');");
    put(root / "directory/payload.bin", std::string(32 * 1024 * 1024, 'A'));
    put(root / "bad.xpi", "invalid archive");
    put(root / "bad-manifest/manifest.json", "{ invalid JSON");
    {
        PreparationTests application(root, fs::path(archive));
        application.Run();
    }
    std::error_code error;
    fs::remove_all(root, error);
    check(!error && !fs::exists(root), "isolated package preparation fixture is removed");
    printf("%u checks passed, %u failed\n", passed, failed);
    return failed ? 1 : 0;
}
