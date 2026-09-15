#include "ExtensionController.h"
#if SUMMIT_MODERN_WEBKIT
#include <WebKit/WebKitContext.h>
#include <Message.h>
#include <Messenger.h>
#include <algorithm>
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
    Advance();
}
void ExtensionController::Advance()
{
    if (fOperation != Operation::None) {
        fOperation = Operation::None;
        fIndex = fStopping ? 0 : fEntries.size();
    } else ++fIndex;
    Changed();
    Next();
}
bool ExtensionController::IsReady() const
{
    return fStarted && !fStopping && fCatalogError.empty() && fIndex == fEntries.size() && !HasPendingWork();
}
void ExtensionController::AddLoaded(InstalledExtension entry, std::string baseURL, std::string error, bool installed)
{
    // The app serializes installer operations with this controller.
    fEntries.push_back({ std::move(entry), true, std::move(baseURL), std::move(error), installed });
    if (!fStopping) fIndex = fEntries.size();
    Changed();
    if (fStopping) Next();
}
bool ExtensionController::SetEnabled(const std::string& identifier, bool enabled)
{
    if (!IsReady()) return false;
    auto found = std::find_if(fEntries.begin(), fEntries.end(), [&](const auto& entry) { return entry.installation.identifier == identifier; });
    if (found == fEntries.end() || !found->installed) return false;
    if (enabled && found->loaded) return true;
    fIndex = found - fEntries.begin();
    fOperation = enabled ? Operation::Enable : Operation::Disable;
    found->error.clear();
    if (enabled) {
        if (!fCatalog.SetEnabled(identifier, true, found->error)) { Advance(); return false; }
        found->installation.enabled = true;
    }
    Changed();
    Next();
    return true;
}
bool ExtensionController::Remove(const std::string& identifier)
{
    if (!IsReady()) return false;
    auto found = std::find_if(fEntries.begin(), fEntries.end(), [&](const auto& entry) { return entry.installation.identifier == identifier; });
    if (found == fEntries.end()) return false;
    fIndex = found - fEntries.begin();
    fOperation = Operation::Remove;
    found->error.clear();
    Changed();
    Next();
    return true;
}
void ExtensionController::FinishRemovalOrDisable()
{
    auto& entry = fEntries[fIndex];
    if (fOperation == Operation::Remove) {
        if (entry.installed && !fCatalog.Forget(entry.installation.identifier, entry.error)) { Advance(); return; }
        fEntries.erase(fEntries.begin() + fIndex);
    } else {
        if (fCatalog.SetEnabled(entry.installation.identifier, false, entry.error)) entry.installation.enabled = false;
    }
    Advance();
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
        if (fOperation == Operation::Disable || fOperation == Operation::Remove) {
            if (!entry.loaded) { FinishRemovalOrDisable(); return; }
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
        fOperation = Operation::None;
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
    if (pending == Pending::Unload && (fOperation == Operation::Disable || fOperation == Operation::Remove))
        FinishRemovalOrDisable();
    else Advance();
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
