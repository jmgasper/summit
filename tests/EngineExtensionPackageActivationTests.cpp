/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebKitContext.h"
#include "WebKitView.h"
#include <Application.h>
#include <MessageRunner.h>
#include <OS.h>
#include <Window.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
namespace {
constexpr uint32 heartbeat = 'akhb', afterViewClose = 'akvc';
constexpr const char* extensionID = "summit-activation-fixture";
unsigned passed, failed;
void check(bool condition, const char* label)
{
    ++(condition ? passed : failed);
    printf("%s %s\n", condition ? "PASS" : "FAIL", label);
    fflush(stdout);
}
std::string field(const BMessage& message, const char* name)
{
    const char* value = nullptr;
    return message.FindString(name, &value) == B_OK && value ? value : "";
}
void put(const fs::path& path, const std::string& value)
{
    std::ofstream output(path, std::ios::binary);
    output << value;
    if (!output)
        std::abort();
}
class TestWindow final : public BWindow {
public:
    TestWindow()
        : BWindow(BRect(180, 140, 940, 640), "Summit — extension activation tests",
            B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS) { }
    bool QuitRequested() final
    {
        be_app->PostMessage(B_QUIT_REQUESTED);
        return false;
    }
};

class ActivationTests final : public BApplication {
public:
    ActivationTests(fs::path root, fs::path fixture)
        : BApplication("application/x-vnd.Kunanyi-Summit-ExtensionPackageActivationTests")
        , m_root(std::move(root)), m_fixture(std::move(fixture))
        , m_nonce(std::to_string(getpid()) + "-" + std::to_string(system_time())) { }

    void ReadyToRun() final
    {
        m_deadline = system_time() + 150000000;
        BMessage tick(heartbeat);
        m_heartbeat = std::make_unique<BMessageRunner>(BMessenger(this), &tick, 20000);
        m_context = std::make_shared<BWebKitContext>((m_root / "profile").c_str());
        check(m_context->InitCheck() == B_OK && m_heartbeat->InitCheck() == B_OK,
            "public activation context and heartbeat initialize");
        if (m_context->InitCheck() != B_OK || m_heartbeat->InitCheck() != B_OK) {
            finish();
            return;
        }
        BWebKitExtensionLoadOptions empty;
        check(m_context->LoadPreparedExtension(nullptr, empty, BMessenger(this), 1) == B_BAD_VALUE
            && m_context->LoadPreparedExtension("token", empty, BMessenger(), 1) == B_BAD_VALUE
            && m_context->LoadPreparedExtension("token", empty, BMessenger(this), 0) == B_BAD_VALUE,
            "public activation rejects missing token, reply target, and reserved request ID");
        check(m_context->UnloadExtension(nullptr, BMessenger(this), 1) == B_BAD_VALUE,
            "public unload rejects a missing identity");
        status_t viewError = B_OK;
        check(!m_context->CreateExtensionView(BRect(0, 0, 300, 200), "missing-extension", extensionID,
            BMessenger(this), &viewError) && viewError == B_NAME_NOT_FOUND,
            "an unloaded extension cannot obtain a configured native view");
        prepare([this](const BMessage& result) {
            m_token = field(result, "token");
            m_fingerprint = field(result, "fingerprint");
            prepare([this](const BMessage& second) {
                m_reloadToken = field(second, "token");
                check(field(second, "fingerprint") == m_fingerprint,
                    "independent snapshots of the same package have matching fingerprints");
                // Loading must use the retained snapshot even after the original
                // package and its executable resources have been changed.
                put(m_root / "package/manifest.json", R"JSON({"manifest_version":2,"name":"Changed source","version":"2.0","permissions":["storage","cookies","https://summit-activation.invalid/*"]})JSON");
                put(m_root / "package/probe.js", "document.title = 'FAIL changed source executed';");
                validateNext();
            });
        });
    }

    void MessageReceived(BMessage* message) final
    {
        if (message->what == heartbeat) {
            tick();
            return;
        }
        if (message->what == afterViewClose) {
            if (!m_finishing)
                unloadAfterPage();
            return;
        }
        if (message->what == B_WEBKIT_PROCESS_EXITED && !m_finishing) {
            check(false, "extension content process remains alive during verification");
            finish();
            return;
        }
        if (message->what == B_WEBKIT_STATE_CHANGED && m_waitingPage) {
            BMessenger view;
            if (message->FindMessenger("view", &view) != B_OK || view != m_viewMessenger)
                return;
            if (m_testingNavigation) {
                if (field(*message, "loadOutcome") == "failed") {
                    check(field(*message, "loadErrorDomain") == "WebExtensionNavigationError",
                        "dedicated extension view rejects top-level navigation outside its extension");
                    m_testingNavigation = false;
                    m_waitingPage = false;
                    m_closingPage = true;
                }
                return;
            }
            if (field(*message, "loadOutcome") == "failed") {
                printf("EXTENSION_PAGE_LOAD_ERROR domain=%s description=%s url=%s\n",
                    field(*message, "loadErrorDomain").c_str(), field(*message, "loadErrorDescription").c_str(),
                    field(*message, "loadErrorURL").c_str());
                check(false, "extension document loads in its native view");
                finish();
                return;
            }
            auto title = field(*message, "title");
            auto prefix = "SUMMIT ACTIVATION " + m_nonce + " " + std::to_string(m_round) + " ";
            if (!title.starts_with(prefix))
                return;
            static constexpr unsigned counts[] { 0, 7, 6, 4 };
            auto expected = prefix + "PASS " + std::to_string(counts[m_round]);
            printf("EXTENSION_PAGE %s\n", title.c_str());
            check(title == expected, "real extension page completes its permission, storage, cookie and snapshot checks");
            if (title != expected) {
                finish();
                return;
            }
            if (m_round == 3) {
                m_testingNavigation = true;
                m_view->LoadURL("https://outside-extension.invalid/");
                return;
            }
            m_waitingPage = false;
            m_closingPage = true;
            return;
        }
        if (message->what == B_WEBKIT_EXTENSION_PACKAGE_PREPARED
            || message->what == B_WEBKIT_EXTENSION_PACKAGE_DISCARDED
            || message->what == B_WEBKIT_EXTENSION_LOADED || message->what == B_WEBKIT_EXTENSION_UNLOADED) {
            if (m_finishing)
                return;
            uint64 identifier = 0;
            int32 error = B_ERROR;
            bool matches = m_pending && message->what == m_expectedWhat
                && message->FindUInt64("identifier", &identifier) == B_OK && identifier == m_request
                && message->FindInt32("error", &error) == B_OK && error == m_expectedError;
            check(matches, m_label.c_str());
            if (!matches) {
                printf("UNEXPECTED_REPLY what=%lu id=%llu error=%ld description=%s\n",
                    static_cast<unsigned long>(message->what), static_cast<unsigned long long>(identifier),
                    static_cast<long>(error), field(*message, "description").c_str());
                finish();
                return;
            }
            auto callback = std::move(m_pending);
            m_pending = nullptr;
            callback(*message);
            return;
        }
        BApplication::MessageReceived(message);
    }

    bool QuitRequested() final
    {
        if (m_done)
            return true;
        check(false, "fixture completes before application quit");
        finish();
        return false;
    }

private:
    using Reply = std::function<void(const BMessage&)>;
    uint64 expect(uint32 what, status_t error, const char* label, Reply callback)
    {
        m_expectedWhat = what;
        m_expectedError = error;
        m_label = label;
        m_pending = std::move(callback);
        return ++m_request;
    }
    void submit(status_t status)
    {
        if (status != B_OK) {
            check(false, "public SDK accepts the asynchronous operation");
            finish();
        }
    }
    void prepare(Reply callback)
    {
        auto id = expect(B_WEBKIT_EXTENSION_PACKAGE_PREPARED, B_OK,
            "public API prepares the activation package", std::move(callback));
        submit(m_context->PrepareExtension((m_root / "package").c_str(), BMessenger(this), id));
    }
    BWebKitExtensionLoadOptions approved() const
    {
        BWebKitExtensionLoadOptions options;
        options.uniqueIdentifier = extensionID;
        options.expectedFingerprint = m_fingerprint;
        options.permissions = { "storage", "cookies" };
        options.origins = { "https://summit-activation.invalid/*" };
        return options;
    }
    void load(const std::string& token, const BWebKitExtensionLoadOptions& options,
        status_t error, const char* label, Reply callback)
    {
        auto id = expect(B_WEBKIT_EXTENSION_LOADED, error, label, std::move(callback));
        submit(m_context->LoadPreparedExtension(token.c_str(), options, BMessenger(this), id));
    }
    void discard(const std::string& token, status_t error, Reply callback)
    {
        auto id = expect(B_WEBKIT_EXTENSION_PACKAGE_DISCARDED, error,
            "prepared-token ownership matches the activation outcome", std::move(callback));
        m_context->DiscardPreparedExtension(token.c_str(), BMessenger(this), id);
    }
    void unload(status_t error, Reply callback)
    {
        auto id = expect(B_WEBKIT_EXTENSION_UNLOADED, error,
            "public unload reports the actual loaded identity", std::move(callback));
        submit(m_context->UnloadExtension(extensionID, BMessenger(this), id));
    }
    void validateNext()
    {
        auto options = approved();
        status_t error = B_NOT_ALLOWED;
        const char* label;
        switch (m_validation++) {
        case 0: options.expectedFingerprint = std::string(64, '0'); label = "activation rejects an unapproved fingerprint"; break;
        case 1: options.uniqueIdentifier = "../unsafe"; error = B_BAD_VALUE; label = "activation rejects a storage path as its identity"; break;
        case 2: options.permissions.push_back("history"); label = "activation rejects an unrequested API permission"; break;
        case 3: options.origins.push_back("https://unrequested.invalid/*"); label = "activation rejects an unrequested origin grant"; break;
        case 4:
            options.permissions.clear(); options.origins.clear();
            options.purpose = BWebKitExtensionLoadOptions::Purpose::BrowserStartup;
            label = "startup cannot restore approval from a missing installation state";
            break;
        default:
            load(m_token, options, B_OK, "approved retained snapshot loads through the public SDK", [this](const BMessage& result) {
                loaded(result);
                discard(m_token, B_NAME_NOT_FOUND, [this](const BMessage&) {
                    load(m_reloadToken, approved(), B_NAME_IN_USE, "duplicate identity cannot replace a loaded extension", [this](const BMessage&) { openPage(); });
                });
            });
            return;
        }
        load(m_token, options, error, label, [this](const BMessage&) { validateNext(); });
    }
    void loaded(const BMessage& message)
    {
        m_baseURL = field(message, "base_url");
        check(field(message, "extension_identifier") == extensionID
            && field(message, "fingerprint") == m_fingerprint && m_baseURL.starts_with("webkit-extension://"),
            "activation receipt identifies the approved runtime and its resource URL");
    }
    void openPage()
    {
        {
            BWebKitContext other(nullptr, true);
            status_t error = B_OK;
            check(other.InitCheck() == B_OK && !other.CreateExtensionView(BRect(0, 0, 300, 200),
                "foreign-extension", extensionID, BMessenger(this), &error) && error == B_NAME_NOT_FOUND,
                "dedicated extension views are scoped to their owning browser context");
        }
        m_window = new TestWindow;
        status_t error = B_ERROR;
        m_view = m_context->CreateExtensionView(m_window->Bounds(), "extension-activation",
            extensionID, BMessenger(this), &error);
        check(m_view && error == B_OK, "public SDK creates a view configured for the loaded extension");
        if (!m_view) {
            finish();
            return;
        }
        m_window->AddChild(m_view);
        check(m_view->InitCheck() == B_OK, "public browser view initializes with the extension's context");
        m_window->Show();
        m_viewMessenger = BMessenger(m_view);
        m_waitingPage = true;
        auto url = m_baseURL + "probe.html#nonce=" + m_nonce + "&round=" + std::to_string(m_round);
        m_view->LoadURL(url.c_str());
    }
    bool closeWindow()
    {
        if (!m_window)
            return true;
        if (m_window->LockWithTimeout(10000) != B_OK)
            return false;
        auto* window = m_window;
        m_window = nullptr;
        m_view = nullptr;
        window->Quit();
        return true;
    }
    void unloadAfterPage()
    {
        unload(B_OK, [this](const BMessage&) {
            if (m_round == 1) {
                unload(B_NAME_NOT_FOUND, [this](const BMessage&) {
                    auto options = approved();
                    options.permissions.clear(); options.origins.clear();
                    options.purpose = BWebKitExtensionLoadOptions::Purpose::BrowserStartup;
                    load(m_reloadToken, options, B_OK, "startup restores saved grants only for the same snapshot", [this](const BMessage& result) {
                        loaded(result); m_round = 2; openPage();
                    });
                });
            } else if (m_round == 2) {
                prepare([this](const BMessage& result) {
                    auto token = field(result, "token");
                    auto options = approved();
                    options.expectedFingerprint = field(result, "fingerprint");
                    check(options.expectedFingerprint != m_fingerprint, "changed source produces a different activation fingerprint");
                    options.permissions.clear(); options.origins.clear();
                    options.purpose = BWebKitExtensionLoadOptions::Purpose::BrowserStartup;
                    load(token, options, B_NOT_ALLOWED, "changed resources cannot inherit saved startup approval", [this, token](const BMessage&) {
                        discard(token, B_OK, [this](const BMessage&) {
                            fs::remove_all(m_root / "package");
                            fs::copy(m_fixture, m_root / "package", fs::copy_options::recursive);
                            prepare([this](const BMessage& result) {
                                check(field(result, "fingerprint") == m_fingerprint, "original package restores its original fingerprint");
                                auto options = approved();
                                options.permissions.clear(); options.origins.clear();
                                load(field(result, "token"), options, B_OK, "fresh empty approval replaces saved grants", [this](const BMessage& loadedResult) {
                                    loaded(loadedResult); m_round = 3; openPage();
                                });
                            });
                        });
                    });
                });
            } else {
                check(m_round == 3, "all three public activation runtime rounds complete");
                status_t error = B_OK;
                check(!m_context->CreateExtensionView(BRect(0, 0, 300, 200), "unloaded-extension",
                    extensionID, BMessenger(this), &error) && error == B_NAME_NOT_FOUND,
                    "unloading removes the ability to create another configured view");
                finish();
            }
        });
    }
    void finish()
    {
        m_finishing = true;
        m_waitingPage = false;
        m_pending = nullptr;
        if (m_context)
            m_context->CancelExtensionPreparation(m_request);
    }
    void tick()
    {
        if (m_done)
            return;
        if (!m_finishing && system_time() > m_deadline) {
            check(false, "public activation runtime completes before its deadline");
            finish();
        }
        if (m_finishing) {
            if (!closeWindow() || (m_context && m_context->HasPendingExtensionPreparations()) || BWebKitHasPendingNativeUI())
                return;
            m_context.reset();
            // View destruction posts its WebKit teardown to this looper.
            if (++m_cleanupTurns < 3)
                return;
            check(fs::is_empty(m_root / "temporary"), "activation teardown releases every prepared and loaded package");
            m_done = true;
            PostMessage(B_QUIT_REQUESTED);
            return;
        }
        if (m_closingPage && closeWindow()) {
            m_closingPage = false;
            PostMessage(afterViewClose);
        }
    }
    fs::path m_root, m_fixture;
    std::string m_nonce, m_token, m_reloadToken, m_fingerprint, m_baseURL, m_label;
    std::shared_ptr<BWebKitContext> m_context;
    std::unique_ptr<BMessageRunner> m_heartbeat;
    TestWindow* m_window { nullptr };
    BWebKitView* m_view { nullptr };
    BMessenger m_viewMessenger;
    Reply m_pending;
    uint64 m_request { 0 };
    uint32 m_expectedWhat { 0 };
    status_t m_expectedError { B_ERROR };
    bigtime_t m_deadline { 0 };
    unsigned m_validation { 0 }, m_round { 1 }, m_cleanupTurns { 0 };
    bool m_waitingPage { false }, m_closingPage { false }, m_testingNavigation { false }, m_finishing { false }, m_done { false };
};
}

int main()
{
    const char* fixture = getenv("SUMMIT_EXTENSION_ACTIVATION_PACKAGE");
    if (!fixture || !fs::path(fixture).is_absolute())
        return 2;
    char temporary[] = "/tmp/summit-package-activation-XXXXXX";
    if (!mkdtemp(temporary))
        return 2;
    fs::path root(temporary);
    fs::create_directory(root / "temporary");
    fs::copy(fs::path(fixture), root / "package", fs::copy_options::recursive);
    setenv("TMPDIR", (root / "temporary").c_str(), 1);
    {
        ActivationTests application(root, fs::path(fixture));
        application.Run();
    }
    std::error_code error;
    fs::remove_all(root, error);
    check(!error && !fs::exists(root), "isolated public activation fixture is removed");
    printf("%u checks passed, %u failed\n", passed, failed);
    return failed ? 1 : 0;
}
