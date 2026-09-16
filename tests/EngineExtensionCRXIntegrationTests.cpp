/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include <WebKit/WebKitContext.h>
#include <WebKit/WebKitView.h>
#include <Application.h>
#include <MessageRunner.h>
#include <OS.h>
#include <Window.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;
namespace {
constexpr uint32 heartbeat = 'crhb', afterClose = 'crcl';
unsigned checks = 0, failures = 0;
void require(bool value, const std::string& label)
{
    if (!value) throw std::runtime_error(label);
    ++checks;
    std::printf("PASS %s\n", label.c_str());
    std::fflush(stdout);
}
std::string field(const BMessage& message, const char* key)
{
    const char* value = nullptr;
    return message.FindString(key, &value) == B_OK && value ? value : "";
}
bool hasChildren()
{
    team_info team;
    int32 cookie = 0;
    while (get_next_team_info(&cookie, &team) == B_OK)
        if (team.parent == getpid()) return true;
    return false;
}
class CRXTests final : public BApplication {
public:
    CRXTests(fs::path root, fs::path fixtures)
        : BApplication("application/x-vnd.Kunanyi-Summit-CRXIntegrationTests")
        , m_root(std::move(root)), m_fixtures(std::move(fixtures))
    {
        std::ifstream input(m_fixtures / "expected.json");
        m_expected = json::parse(input);
        m_nonce = std::to_string(getpid()) + "-" + std::to_string(system_time());
    }
    void ReadyToRun() override
    {
        try {
            m_deadline = system_time() + 150000000;
            BMessage tick(heartbeat);
            m_timer = std::make_unique<BMessageRunner>(BMessenger(this), &tick, 20000);
            require(m_timer->InitCheck() == B_OK && BWebKitInitialize() == B_OK, "native SDK initializes");
            m_context = std::make_shared<BWebKitContext>((m_root / "profile").c_str());
            require(m_context->InitCheck() == B_OK, "isolated CRX context initializes");
            invalidNext();
        } catch (const std::exception& error) { fail(error.what()); }
    }
    bool QuitRequested() override
    {
        if (m_done) return true;
        fail("unexpected application quit");
        return false;
    }
    void MessageReceived(BMessage* message) override
    {
        try { received(message); }
        catch (const std::exception& error) { fail(error.what()); }
    }
private:
    using Reply = std::function<void(const BMessage&)>;
    void received(BMessage* message)
    {
        if (message->what == heartbeat) { tick(); return; }
        if (m_finishing) return;
        if (message->what == afterClose) { pageClosed(); return; }
        if (message->what == B_WEBKIT_PROCESS_EXITED) throw std::runtime_error("extension process exited during checks");
        if (message->what == B_WEBKIT_STATE_CHANGED && m_waitingPage) {
            BMessenger view;
            if (message->FindMessenger("view", &view) != B_OK || view != m_view) return;
            if (field(*message, "loadOutcome") == "failed")
                throw std::runtime_error("extension page load failed: " + field(*message, "loadErrorDescription"));
            const auto title = field(*message, "title");
            if (!title.starts_with("CRX PROBE ")) return;
            const int boot = m_round == 2 ? 2 : 1;
            require(title == "CRX PROBE " + identity() + " " + std::to_string(boot) + " " + m_nonce + " PASS",
                "signed extension executes with verified runtime ID, both namespaces, exact resource bytes and persisted boot " + std::to_string(boot));
            m_waitingPage = false;
            m_closingPage = true;
            return;
        }
        if (message->what != B_WEBKIT_EXTENSION_PACKAGE_PREPARED && message->what != B_WEBKIT_EXTENSION_PACKAGE_DISCARDED
            && message->what != B_WEBKIT_EXTENSION_LOADED && message->what != B_WEBKIT_EXTENSION_UNLOADED) {
            BApplication::MessageReceived(message); return;
        }
        uint64 request = 0;
        int32 error = B_ERROR;
        message->FindUInt64("identifier", &request);
        message->FindInt32("error", &error);
        if (!m_reply || request != m_request || message->what != m_what || error != m_error) {
            std::fprintf(stderr, "SDK reply what=%lu request=%llu error=%ld description=%s\n",
                static_cast<unsigned long>(message->what), static_cast<unsigned long long>(request),
                static_cast<long>(error), field(*message, "description").c_str());
            throw std::runtime_error(m_label);
        }
        require(true, m_label);
        auto callback = std::exchange(m_reply, nullptr);
        callback(*message);
    }
    uint64 expect(uint32 what, status_t error, std::string label, Reply callback)
    {
        m_what = what; m_error = error; m_label = std::move(label); m_reply = std::move(callback);
        return ++m_request;
    }
    void prepare(const fs::path& path, status_t error, Reply callback)
    {
        auto request = expect(B_WEBKIT_EXTENSION_PACKAGE_PREPARED, error, "preparation status for " + path.filename().string(), std::move(callback));
        require(m_context->PrepareExtension(path.c_str(), BMessenger(this), request) == B_OK, "SDK queues preparation");
    }
    void discard(const std::string& token, status_t error, Reply callback)
    {
        auto request = expect(B_WEBKIT_EXTENSION_PACKAGE_DISCARDED, error, "prepared token ownership matches load outcome", std::move(callback));
        m_context->DiscardPreparedExtension(token.c_str(), BMessenger(this), request);
    }
    void load(const std::string& token, BWebKitExtensionLoadOptions options, status_t error, const char* label, Reply callback)
    {
        auto request = expect(B_WEBKIT_EXTENSION_LOADED, error, label, std::move(callback));
        require(m_context->LoadPreparedExtension(token.c_str(), options, BMessenger(this), request) == B_OK, "SDK queues activation");
    }
    std::string identity() const { return m_expected["identities"][m_round == 3 ? "ecdsa" : "rsa"].get<std::string>(); }
    BWebKitExtensionLoadOptions options() const
    {
        BWebKitExtensionLoadOptions value;
        value.uniqueIdentifier = identity(); value.expectedFingerprint = m_fingerprint;
        value.permissions = { "storage" };
        return value;
    }
    void invalidNext()
    {
        static const char* names[] { "corrupt.crx", "appended.crx", "unsupported.crx", "unsafe-parent.crx" };
        if (m_invalid < std::size(names)) {
            prepare(m_fixtures / names[m_invalid++], B_BAD_DATA, [this](const BMessage& result) {
                require(field(result, "token").empty() && field(result, "verified_crx_id").empty(), "invalid CRX grants neither a token nor signer metadata");
                invalidNext();
            });
            return;
        }
        prepare(m_fixtures / "published-ublock.crx", B_OK, [this](const BMessage& result) {
            require(field(result, "verified_crx_id") == "fkgkibajhfbepljeaefdnfnegdcjomkh"
                && field(result, "name") == "uBlock Origin" && field(result, "version") == "1.74.0", "unmodified published uBlock prepares with its verified signer and version");
            discard(field(result, "token"), B_OK, [this](const BMessage&) {
                prepare(m_fixtures / "unsigned.zip", B_OK, [this](const BMessage& result) {
                    require(field(result, "verified_crx_id").empty(), "unsigned ZIP cannot obtain authenticated signer metadata");
                    m_fingerprint = field(result, "fingerprint");
                    discard(field(result, "token"), B_OK, [this](const BMessage&) { prepareSigned(); });
                });
            });
        });
    }
    void prepareSigned()
    {
        const auto source = m_fixtures / (m_round == 3 ? "ecdsa.crx" : "rsa.crx");
        fs::copy_file(source, m_root / "mutable.crx", fs::copy_options::overwrite_existing);
        prepare(m_root / "mutable.crx", B_OK, [this](const BMessage& result) {
            require(field(result, "verified_crx_id") == identity() && field(result, "fingerprint") == m_fingerprint,
                "signed envelope authenticates identity while preserving resource fingerprint");
            require(field(result, "manifest_json").find("conflicting-manifest@summit.invalid") != std::string::npos,
                "conflicting manifest identity remains visible without overriding the signer");
            m_token = field(result, "token");
            if (m_round == 3) { rejectWrongID(); return; }
            prepare(m_root / "mutable.crx", B_OK, [this](const BMessage& second) {
                require(field(second, "verified_crx_id") == identity(), "independent snapshot retains signer metadata");
                m_reloadToken = field(second, "token");
                std::ofstream changed(m_root / "mutable.crx", std::ios::binary | std::ios::app);
                changed << "unsigned modification"; changed.close();
                require(bool(changed), "original CRX is changed after preparation");
                prepare(m_root / "mutable.crx", B_BAD_DATA, [this](const BMessage&) {
                    require(!hasChildren(), "metadata preparation and rejection execute no extension or network process");
                    rejectWrongID();
                });
            });
        });
    }
    void rejectWrongID()
    {
        auto wrong = options(); wrong.uniqueIdentifier = "conflicting-manifest@summit.invalid";
        load(m_token, wrong, B_NOT_ALLOWED, "verified CRX rejects caller-supplied manifest identity", [this](const BMessage&) {
            auto wrong = options(); wrong.uniqueIdentifier = std::string(32, 'a');
            load(m_token, wrong, B_NOT_ALLOWED, "verified CRX rejects another syntactically valid Chrome identity", [this](const BMessage&) {
                load(m_token, options(), B_OK, "same retained token loads under the authenticated signer after rejected identities", [this](const BMessage& result) {
                    loaded(result);
                    discard(m_token, B_NAME_NOT_FOUND, [this](const BMessage&) { openPage(); });
                });
            });
        });
    }
    void loaded(const BMessage& result)
    {
        require(field(result, "extension_identifier") == identity() && field(result, "fingerprint") == m_fingerprint,
            "activation receipt identifies the verified runtime and snapshot");
        m_baseURL = field(result, "base_url");
    }
    void openPage()
    {
        status_t error = B_OK;
        require(!m_context->CreateExtensionView(BRect(0, 0, 300, 200), "wrong-id", "conflicting-manifest@summit.invalid", BMessenger(this), &error)
            && error == B_NAME_NOT_FOUND, "untrusted manifest identity has no extension runtime");
        m_window = new BWindow(BRect(160, 130, 980, 570), "Summit — signed CRX SDK tests", B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS);
        auto* view = m_context->CreateExtensionView(m_window->Bounds(), "crx-probe", identity().c_str(), BMessenger(this), &error);
        require(view && error == B_OK, "public SDK creates the verified extension page");
        m_window->AddChild(view); m_window->Show(); m_view = BMessenger(view); m_waitingPage = true;
        view->LoadURL((m_baseURL + "probe.html#nonce=" + m_nonce + "&boot=" + std::to_string(m_round == 2 ? 2 : 1)).c_str());
    }
    bool closeWindow()
    {
        if (!m_window) return true;
        if (m_window->LockWithTimeout(10000) != B_OK) return false;
        auto* window = std::exchange(m_window, nullptr); window->Quit(); return true;
    }
    void pageClosed()
    {
        auto request = expect(B_WEBKIT_EXTENSION_UNLOADED, B_OK, "verified extension unloads cleanly", [this](const BMessage&) {
            if (m_round == 1) {
                m_round = 2;
                auto startup = options(); startup.permissions.clear(); startup.purpose = BWebKitExtensionLoadOptions::Purpose::BrowserStartup;
                load(m_reloadToken, startup, B_OK, "saved approval restores the independently retained signed snapshot", [this](const BMessage& result) { loaded(result); openPage(); });
            } else if (m_round == 2) { m_round = 3; prepareSigned(); }
            else finish();
        });
        require(m_context->UnloadExtension(identity().c_str(), BMessenger(this), request) == B_OK, "SDK queues extension unload");
    }
    void fail(const std::string& message)
    {
        ++failures; std::fprintf(stderr, "FAIL %s\n", message.c_str()); finish();
    }
    void finish()
    {
        m_finishing = true; m_waitingPage = false; m_reply = nullptr;
        if (m_context) m_context->CancelExtensionPreparation(m_request);
    }
    void tick()
    {
        if (m_done) return;
        if (!m_finishing && system_time() > m_deadline) fail("CRX SDK deadline");
        if (m_finishing) {
            if (!closeWindow() || (m_context && m_context->HasPendingExtensionPreparations()) || BWebKitHasPendingNativeUI()) return;
            m_context.reset();
            if (++m_cleanupTurns < 3) return;
            if (!fs::is_empty(m_root / "temporary")) fail("context teardown leaked package snapshots");
            else require(true, "context teardown releases every signed and invalid package snapshot");
            m_done = true;
            PostMessage(B_QUIT_REQUESTED); return;
        }
        if (m_closingPage && closeWindow()) { m_closingPage = false; PostMessage(afterClose); }
    }
    fs::path m_root, m_fixtures;
    json m_expected;
    std::string m_nonce, m_token, m_reloadToken, m_fingerprint, m_baseURL, m_label;
    std::shared_ptr<BWebKitContext> m_context;
    std::unique_ptr<BMessageRunner> m_timer;
    BWindow* m_window = nullptr;
    BMessenger m_view;
    Reply m_reply;
    uint64 m_request = 0;
    uint32 m_what = 0;
    status_t m_error = B_ERROR;
    bigtime_t m_deadline = 0;
    unsigned m_invalid = 0, m_round = 1, m_cleanupTurns = 0;
    bool m_finishing = false, m_done = false, m_closingPage = false, m_waitingPage = false;
};
}
int main(int argc, char** argv)
{
    if (argc != 3 || !fs::path(argv[1]).is_absolute() || !fs::path(argv[2]).is_absolute()) return 2;
    fs::path root(argv[1]); fs::create_directories(root / "temporary");
    setenv("TMPDIR", (root / "temporary").c_str(), 1);
    try { CRXTests application(root, fs::path(argv[2])); application.Run(); }
    catch (const std::exception& error) { ++failures; std::fprintf(stderr, "FAIL %s\n", error.what()); }
    std::printf("CRX_SDK_RESULT %s checks=%u failures=%u\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
