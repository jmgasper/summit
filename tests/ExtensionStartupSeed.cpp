/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "core/ExtensionCatalog.h"
#include <WebKit/WebKitContext.h>
#include <WebKit/WebKitView.h>
#include <Application.h>
#include <MessageRunner.h>
#include <Path.h>
#include <Roster.h>
#include <Window.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>

namespace fs = std::filesystem;
namespace {
constexpr uint32 tickMessage = 'estk';
constexpr const char* identity = "summit-startup-fixture";
std::string field(const BMessage& message, const char* name)
{
    const char* value = nullptr;
    return message.FindString(name, &value) == B_OK && value ? value : "";
}
class Seed final : public BApplication {
public:
    Seed(fs::path profile, fs::path package, std::string nonce)
        : BApplication("application/x-vnd.Kunanyi-Summit-ExtensionStartupSeed")
        , fProfile(std::move(profile)), fPackage(std::move(package)), fNonce(std::move(nonce))
        , fCatalog(fProfile / "Extensions") { }
    int Result() const { return fFailed ? 1 : 0; }
    void ReadyToRun() override
    {
        fDeadline = system_time() + 45000000;
        BMessage tick(tickMessage);
        fTimer = std::make_unique<BMessageRunner>(BMessenger(this), &tick, 20000);
        if (fTimer->InitCheck() != B_OK || BWebKitInitialize() != B_OK) { fail("native initialization"); return; }
        fContext = std::make_shared<BWebKitContext>((fProfile / "WebKit").c_str());
        if (fContext->InitCheck() != B_OK) { fail("profile initialization"); return; }
        std::string error;
        fStage = fCatalog.Stage(fPackage, error);
        if (!fStage) { fail(error); return; }
        fExpected = B_WEBKIT_EXTENSION_PACKAGE_PREPARED;
        submit(fContext->PrepareExtension(fStage->Path().c_str(), BMessenger(this), ++fRequest));
    }
    bool QuitRequested() override
    {
        if (fDone) return true;
        fail("unexpected application quit");
        return false;
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what == tickMessage) { tick(); return; }
        if (message->what == B_WEBKIT_PROCESS_EXITED && !fFinishing) { fail("content process exited"); return; }
        if (message->what == B_WEBKIT_STATE_CHANGED && fWaitingPage) {
            BMessenger view;
            if (message->FindMessenger("view", &view) != B_OK || view != fView) return;
            if (field(*message, "loadOutcome") == "failed") { fail(field(*message, "loadErrorDescription")); return; }
            const auto title = field(*message, "title");
            const auto prefix = "SUMMIT STARTUP SEED " + fNonce + " ";
            if (!title.starts_with(prefix)) return;
            if (title != prefix + "PASS 1") { fail(title); return; }
            std::printf("PASS extension background executed with approved grants and persisted its first boot\n");
            fWaitingPage = false;
            fFinishing = true;
            return;
        }
        if (!fExpected || message->what != fExpected) { BApplication::MessageReceived(message); return; }
        uint64 request = 0;
        int32 error = B_ERROR;
        if (message->FindUInt64("identifier", &request) != B_OK || request != fRequest
            || message->FindInt32("error", &error) != B_OK || error != B_OK) {
            fail("SDK receipt: " + field(*message, "description"));
            return;
        }
        const auto operation = std::exchange(fExpected, 0);
        if (operation == B_WEBKIT_EXTENSION_PACKAGE_PREPARED) {
            fToken = field(*message, "token");
            fFingerprint = field(*message, "fingerprint");
            if (fToken.empty() || fFingerprint.size() != 64) { fail("incomplete preparation receipt"); return; }
            std::printf("Prepared startup package fingerprint: %s\n", fFingerprint.c_str());
            std::string error;
            auto disabled = fCatalog.Stage(fPackage, error);
            auto changed = fCatalog.Stage(fPackage, error);
            if (!disabled || !changed) { fail(error); return; }
            auto entry = [&](const char* id, bool enabled) {
                return summit::InstalledExtension { id, "Startup fixture", "1.0", fFingerprint, {}, enabled, false, false };
            };
            // Invalid/disabled entries precede the valid package so startup
            // must continue after skipping and rejecting them.
            if (!fCatalog.Install(*disabled, entry("summit-startup-disabled", false), error)
                || !fCatalog.Install(*changed, entry("summit-startup-changed", true), error)
                || !fCatalog.Install(*fStage, entry(identity, true), error)) { fail(error); return; }
            std::ofstream modified(changed->Path() / "background.js", std::ios::app);
            modified << "\n// Package changed after catalog approval.\n";
            modified.close();
            if (!modified) { fail("changing the unapproved package"); return; }
            BWebKitExtensionLoadOptions options;
            options.uniqueIdentifier = identity;
            options.expectedFingerprint = fFingerprint;
            options.permissions = { "storage" };
            options.origins = { "http://10.0.2.2/*" };
            fExpected = B_WEBKIT_EXTENSION_LOADED;
            submit(fContext->LoadPreparedExtension(fToken.c_str(), options, BMessenger(this), ++fRequest));
            return;
        }
        if (operation == B_WEBKIT_EXTENSION_LOADED) {
            fLoaded = true;
            fToken.clear();
            if (field(*message, "extension_identifier") != identity || field(*message, "fingerprint") != fFingerprint) {
                fail("activation receipt identity/fingerprint"); return;
            }
            fWindow = new BWindow(BRect(180, 160, 900, 600), "Summit — extension startup setup",
                B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS);
            status_t status = B_ERROR;
            auto* view = fContext->CreateExtensionView(fWindow->Bounds(), "startup-probe", identity, BMessenger(this), &status);
            if (!view || status != B_OK) { fail("extension view creation"); return; }
            fWindow->AddChild(view);
            fWindow->Show();
            fView = BMessenger(view);
            fWaitingPage = true;
            view->LoadURL((field(*message, "base_url") + "probe.html#" + fNonce).c_str());
            return;
        }
        if (operation == B_WEBKIT_EXTENSION_UNLOADED) {
            fLoaded = false;
            std::printf("PASS seeded extension unloaded before the setup process exits\n");
        }
    }
private:
    void submit(status_t status) { if (status != B_OK) fail(std::strerror(status)); }
    void fail(const std::string& message)
    {
        std::fprintf(stderr, "FAIL %s\n", message.c_str());
        fFailed = fFinishing = true;
        fWaitingPage = false;
        fExpected = 0;
        if (fContext) fContext->CancelExtensionPreparation(fRequest);
    }
    void tick()
    {
        if (!fFinishing && system_time() > fDeadline) fail("startup setup deadline");
        if (!fFinishing || fDone) return;
        if (fWindow) {
            if (fWindow->LockWithTimeout(10000) != B_OK) return;
            auto* window = std::exchange(fWindow, nullptr);
            window->Quit();
            return; // Let the posted engine page teardown run before unload.
        }
        if (fExpected) return;
        if (fLoaded && !fUnloadAttempted) {
            fUnloadAttempted = true;
            fExpected = B_WEBKIT_EXTENSION_UNLOADED;
            submit(fContext->UnloadExtension(identity, BMessenger(this), ++fRequest));
            return;
        }
        if (fContext && fContext->HasPendingExtensionPreparations()) return;
        if (BWebKitHasPendingNativeUI()) return;
        if (fContext) {
            if (!fToken.empty()) fContext->DiscardPreparedExtension(fToken.c_str());
            fContext.reset();
            return;
        }
        if (++fCleanupTurns < 3) return;
        fDone = true;
        PostMessage(B_QUIT_REQUESTED);
    }
    fs::path fProfile, fPackage;
    std::string fNonce, fToken, fFingerprint;
    summit::ExtensionCatalog fCatalog;
    std::unique_ptr<summit::StagedExtensionPackage> fStage;
    std::shared_ptr<BWebKitContext> fContext;
    std::unique_ptr<BMessageRunner> fTimer;
    BWindow* fWindow = nullptr;
    BMessenger fView;
    uint32 fExpected = 0;
    uint64 fRequest = 0;
    bigtime_t fDeadline = 0;
    int fCleanupTurns = 0;
    bool fLoaded = false, fWaitingPage = false, fFinishing = false, fDone = false, fFailed = false, fUnloadAttempted = false;
};

int run(int argc, char** argv)
{
    if (argc == 4 && !std::strcmp(argv[1], "--close-browser")) {
        const team_id team = std::strtol(argv[2], nullptr, 10);
        app_info info;
        if (be_roster->GetRunningAppInfo(team, &info) != B_OK) return 2;
        BPath path(&info.ref);
        if (path.InitCheck() != B_OK || fs::canonical(path.Path()) != fs::canonical(argv[3])) return 3;
        BMessage close(B_QUIT_REQUESTED);
        return BMessenger(nullptr, team).SendMessage(&close, static_cast<BHandler*>(nullptr), 1000000) == B_OK ? 0 : 4;
    }
    if (argc != 4) return 2;
    Seed application(argv[1], argv[2], argv[3]);
    application.Run();
    return application.Result();
}
}
int main(int argc, char** argv)
{
    int status = run(argc, argv);
    std::fflush(nullptr);
    std::_Exit(status);
}
