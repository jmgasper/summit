#include "ExtensionController.h"
#if SUMMIT_MODERN_WEBKIT
#include <WebKit/WebKitContext.h>
#include <Message.h>
#include <Messenger.h>
#include <cstdio>
#include <cstring>
#include <utility>

namespace summit {
namespace {
std::string field(const BMessage& message, const char* name)
{
    const char* value = nullptr;
    return message.FindString(name, &value) == B_OK && value ? value : "";
}
}
ExtensionController::ExtensionController(std::shared_ptr<BWebKitContext> context,
    std::filesystem::path root, std::function<void()> changed)
    : BHandler("Summit extension controller"), fContext(std::move(context)), fCatalog(std::move(root))
    , fChanged(std::move(changed)) { }

void ExtensionController::Start()
{
    if (fStarted || fStopping) return;
    fStarted = true;
    std::vector<InstalledExtension> installed;
    if (!fCatalog.Load(installed, fCatalogError)) {
        std::fprintf(stderr, "Summit extension catalog: %s\n", fCatalogError.c_str());
        Changed();
        return;
    }
    for (auto& entry : installed) fEntries.push_back({ std::move(entry), false, { }, { } });
    Changed();
    Next();
}
void ExtensionController::Changed()
{
    if (fChanged) fChanged();
}
void ExtensionController::DiscardToken()
{
    if (fToken.empty()) return;
    fContext->DiscardPreparedExtension(fToken.c_str());
    fToken.clear();
}
void ExtensionController::Fail(const std::string& error)
{
    auto& entry = fEntries[fIndex];
    entry.error = error;
    std::fprintf(stderr, "Summit extension %s: %s\n", entry.installation.identifier.c_str(), error.c_str());
    DiscardToken();
    fPending = Pending::None;
    ++fIndex;
    Changed();
    Next();
}
void ExtensionController::Next()
{
    if (fPending != Pending::None) return;
    while (fIndex < fEntries.size()) {
        auto& entry = fEntries[fIndex];
        if (fStopping) {
            if (!entry.loaded) { ++fIndex; continue; }
            fPending = Pending::Unload;
            auto status = fContext->UnloadExtension(entry.installation.identifier.c_str(), BMessenger(this), ++fRequest);
            if (status != B_OK) Fail(std::strerror(status));
            return;
        }
        if (!entry.installation.enabled) { ++fIndex; continue; }
        fPending = Pending::Prepare;
        auto path = fCatalog.PackagePath(entry.installation);
        auto status = fContext->PrepareExtension(path.c_str(), BMessenger(this), ++fRequest);
        if (status != B_OK) Fail(std::strerror(status));
        return;
    }
    Changed();
}
void ExtensionController::MessageReceived(BMessage* message)
{
    uint32 expected = fPending == Pending::Prepare ? B_WEBKIT_EXTENSION_PACKAGE_PREPARED
        : fPending == Pending::Load ? B_WEBKIT_EXTENSION_LOADED
        : fPending == Pending::Unload ? B_WEBKIT_EXTENSION_UNLOADED : 0;
    if (!expected || message->what != expected) {
        BHandler::MessageReceived(message);
        return;
    }
    uint64 identifier = 0;
    int32 error = B_ERROR;
    if (message->FindUInt64("identifier", &identifier) != B_OK || identifier != fRequest
        || message->FindInt32("error", &error) != B_OK || fIndex >= fEntries.size())
        return;
    auto pending = std::exchange(fPending, Pending::None);
    auto& entry = fEntries[fIndex];
    if (pending == Pending::Prepare && error == B_OK)
        fToken = field(*message, "token");
    if (fStopping && pending != Pending::Unload) {
        // A submitted load can succeed before shutdown's cancellation is seen.
        // Record the real outcome so the shutdown walk unloads that runtime.
        if (pending == Pending::Load && error == B_OK) {
            entry.loaded = true;
            entry.baseURL = field(*message, "base_url");
            fToken.clear();
        }
        DiscardToken();
        fIndex = 0;
        Next();
        return;
    }
    if (error != B_OK && !(pending == Pending::Unload && error == B_NAME_NOT_FOUND)) {
        auto reason = field(*message, "description");
        Fail(reason.empty() ? std::strerror(error) : reason);
        return;
    }
    if (pending == Pending::Prepare) {
        if (fToken.empty() || field(*message, "fingerprint") != entry.installation.fingerprint) {
            Fail("The installed package has changed and requires new approval.");
            return;
        }
        BWebKitExtensionLoadOptions options;
        options.uniqueIdentifier = entry.installation.identifier;
        options.expectedFingerprint = entry.installation.fingerprint;
        options.purpose = BWebKitExtensionLoadOptions::Purpose::BrowserStartup;
        options.allowFileURLs = entry.installation.allowFileURLs;
        options.allowPrivateBrowsing = entry.installation.allowPrivateBrowsing;
        fPending = Pending::Load;
        auto status = fContext->LoadPreparedExtension(fToken.c_str(), options, BMessenger(this), ++fRequest);
        if (status != B_OK) Fail(std::strerror(status));
        return;
    }
    entry.loaded = pending == Pending::Load;
    entry.baseURL = entry.loaded ? field(*message, "base_url") : std::string();
    entry.error.clear();
    fToken.clear();
    ++fIndex;
    Changed();
    Next();
}
void ExtensionController::Shutdown()
{
    if (fStopping) return;
    fStopping = true;
    if (fPending == Pending::Prepare)
        fContext->CancelExtensionPreparation(fRequest);
    if (fPending != Pending::None) return;
    DiscardToken();
    fIndex = 0;
    Next();
}
bool ExtensionController::HasPendingWork() const
{
    return fPending != Pending::None || fContext->HasPendingExtensionPreparations();
}
}
#endif
