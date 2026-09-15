#include "ExtensionInstaller.h"
#if SUMMIT_MODERN_WEBKIT
#include <WebKit/WebKitContext.h>
#include <MessageRunner.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <set>
#include <stdexcept>
#include <utility>

namespace summit {
namespace {
constexpr uint32 pollImport = 'exwk';
std::string field(const BMessage& message, const char* name)
{
    const char* value = nullptr;
    return message.FindString(name, &value) == B_OK && value ? value : "";
}
std::vector<std::string> values(const BMessage& message, const char* name)
{
    std::vector<std::string> result;
    const char* value = nullptr;
    for (int32 i = 0; message.FindString(name, i, &value) == B_OK; ++i) {
        if (i >= 256 || !value || !*value || std::strlen(value) > 4096)
            throw std::runtime_error("The package has too many or invalid permission requirements.");
        result.emplace_back(value);
    }
    return result;
}
bool validIdentity(const std::string& value)
{
    return !value.empty() && value.size() <= 255 && value != "." && value != ".."
        && std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                || c == '.' || c == '_' || c == '-' || c == '@' || c == '{' || c == '}';
        });
}
}
std::string ExtensionDisplayText(std::string text)
{
    // Keep untrusted extension text from adding headings or directional controls.
    std::string result;
    for (size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x20 || c == 0x7f) { result += ' '; ++i; }
        else if (i + 1 < text.size() && ((c == 0xc2 && static_cast<unsigned char>(text[i + 1]) >= 0x80
            && static_cast<unsigned char>(text[i + 1]) <= 0x9f) || (c == 0xd8 && text[i + 1] == '\x9c'))) {
            result += ' '; i += 2;
        } else if (i + 2 < text.size() && c == 0xe2 && ((text[i + 1] == '\x80'
            && ((static_cast<unsigned char>(text[i + 2]) >= 0x8b && static_cast<unsigned char>(text[i + 2]) <= 0x8f)
                || (static_cast<unsigned char>(text[i + 2]) >= 0xa8 && static_cast<unsigned char>(text[i + 2]) <= 0xae)))
            || (text[i + 1] == '\x81' && static_cast<unsigned char>(text[i + 2]) >= 0xa6
                && static_cast<unsigned char>(text[i + 2]) <= 0xa9))) {
            result += ' '; i += 3;
        } else result += text[i++];
    }
    return result;
}
struct ExtensionInstaller::ImportWork {
    std::atomic<bool> finished { false }, cancelled { false };
    std::unique_ptr<StagedExtensionPackage> package;
    std::string error;
};
struct ExtensionInstaller::Draft {
    std::unique_ptr<StagedExtensionPackage> package;
    InstalledExtension entry;
    std::string token, body, baseURL, rollbackError;
    std::vector<std::string> permissions, origins;
    bool ready = false, loaded = false;
};
ExtensionInstaller::ExtensionInstaller(std::shared_ptr<BWebKitContext> context, std::filesystem::path root,
    Loaded loaded, std::function<void()> changed)
    : BHandler("Summit extension installer"), fContext(std::move(context)), fCatalog(std::move(root))
    , fLoaded(std::move(loaded)), fChanged(std::move(changed)) { }
ExtensionInstaller::~ExtensionInstaller()
{
    if (fThread.joinable()) fThread.join();
}
void ExtensionInstaller::Start()
{
    BMessage tick(pollImport);
    fTimer = std::make_unique<BMessageRunner>(BMessenger(this), &tick, 100000);
}
void ExtensionInstaller::Changed() { if (fChanged) fChanged(); }
bool ExtensionInstaller::IsBusy() const { return fWork || fDraft || fPending; }
bool ExtensionInstaller::HasPendingWork() const { return IsBusy() || fContext->HasPendingExtensionPreparations(); }
BMessage ExtensionInstaller::Snapshot() const
{
    BMessage state;
    state.AddBool("import_busy", IsBusy());
    state.AddString("import_status", fStatus.c_str());
    state.AddUInt64("generation", fGeneration);
    state.AddBool("approval_ready", fDraft && fDraft->ready);
    if (fDraft) {
        state.AddString("draft_name", ExtensionDisplayText(fDraft->entry.name).c_str());
        state.AddString("draft_body", fDraft->body.c_str());
    }
    return state;
}
bool ExtensionInstaller::Import(const std::filesystem::path& source)
{
    if (fStopping || IsBusy() || !fTimer || fTimer->InitCheck() != B_OK) return false;
    ++fGeneration;
    fCancelled = false;
    fStatus = "Reading extension package…";
    fWork = std::make_shared<ImportWork>();
    try {
        fThread = std::thread([work = fWork, catalog = fCatalog, source, target = BMessenger(this)] {
            work->package = catalog.Stage(source, work->error);
            if (work->cancelled.load()) work->package.reset();
            work->finished.store(true, std::memory_order_release);
            BMessage ready(pollImport);
            target.SendMessage(&ready, static_cast<BHandler*>(nullptr), 0);
        });
    } catch (const std::exception& error) {
        fWork.reset();
        Finish(error.what());
        return false;
    }
    Changed();
    return true;
}
void ExtensionInstaller::Poll()
{
    if (fWork && fWork->finished.load(std::memory_order_acquire)) {
        fThread.join();
        auto work = std::exchange(fWork, {});
        if (fStopping || fCancelled) { Finish("Installation cancelled."); return; }
        if (!work->package) { Finish(work->error); return; }
        fDraft = std::make_unique<Draft>();
        fDraft->package = std::move(work->package);
        fStatus = "Checking extension requirements…";
        fPending = B_WEBKIT_EXTENSION_PACKAGE_PREPARED;
        auto status = fContext->PrepareExtension(fDraft->package->Path().c_str(), BMessenger(this), ++fRequest);
        if (status != B_OK) Finish(std::strerror(status));
        else Changed();
    }
    bool pending = HasPendingWork();
    if (pending != fHadWork) { fHadWork = pending; Changed(); }
}
void ExtensionInstaller::Finish(std::string status)
{
    if (fDraft && !fDraft->token.empty()) fContext->DiscardPreparedExtension(fDraft->token.c_str());
    fDraft.reset();
    fPending = 0;
    fStatus = std::move(status);
    Changed();
}
void ExtensionInstaller::Cancel(uint64 generation)
{
    if (generation != fGeneration || !IsBusy()) return;
    fCancelled = true;
    fStatus = "Cancelling installation…";
    if (fDraft) fDraft->ready = false;
    if (fWork) fWork->cancelled.store(true);
    if (fPending == B_WEBKIT_EXTENSION_PACKAGE_PREPARED) fContext->CancelExtensionPreparation(fRequest);
    if (!fWork && !fPending) Finish("Installation cancelled.");
    else Changed();
}
void ExtensionInstaller::Shutdown()
{
    fStopping = true;
    Cancel(fGeneration);
}
void ExtensionInstaller::Approve(uint64 generation, bool allowFiles, bool allowPrivate)
{
    if (fStopping || fCancelled || generation != fGeneration || !fDraft || !fDraft->ready || fPending) return;
    fDraft->ready = false;
    fDraft->entry.allowFileURLs = allowFiles;
    fDraft->entry.allowPrivateBrowsing = allowPrivate;
    BWebKitExtensionLoadOptions options;
    options.uniqueIdentifier = fDraft->entry.identifier;
    options.expectedFingerprint = fDraft->entry.fingerprint;
    options.permissions = fDraft->permissions;
    options.origins = fDraft->origins;
    options.allowFileURLs = allowFiles;
    options.allowPrivateBrowsing = allowPrivate;
    fPending = B_WEBKIT_EXTENSION_LOADED;
    fStatus = "Installing extension…";
    auto status = fContext->LoadPreparedExtension(fDraft->token.c_str(), options, BMessenger(this), ++fRequest);
    if (status != B_OK) Finish(std::strerror(status));
    else Changed();
}
void ExtensionInstaller::RetainUnsavedRuntime(std::string reason)
{
    // An unload failure must remain visible and owned by normal app shutdown.
    if (fLoaded) fLoaded(fDraft->entry, fDraft->baseURL, reason, false);
    Finish(std::move(reason));
}
void ExtensionInstaller::Rollback(std::string reason)
{
    fDraft->rollbackError = std::move(reason);
    fPending = B_WEBKIT_EXTENSION_UNLOADED;
    auto status = fContext->UnloadExtension(fDraft->entry.identifier.c_str(), BMessenger(this), ++fRequest);
    if (status != B_OK) RetainUnsavedRuntime(fDraft->rollbackError + " Could not stop the temporary runtime: " + std::strerror(status));
}
void ExtensionInstaller::MessageReceived(BMessage* message)
{
    if (message->what == pollImport) { Poll(); return; }
    uint64 request = 0;
    int32 error = B_ERROR;
    if (!fPending || message->what != fPending || !fDraft
        || message->FindUInt64("identifier", &request) != B_OK || request != fRequest
        || message->FindInt32("error", &error) != B_OK) { BHandler::MessageReceived(message); return; }
    const auto operation = std::exchange(fPending, 0);
    if (operation == B_WEBKIT_EXTENSION_UNLOADED) {
        if (error != B_OK && error != B_NAME_NOT_FOUND)
            RetainUnsavedRuntime(fDraft->rollbackError + " The temporary runtime could not be stopped.");
        else Finish(fDraft->rollbackError);
        return;
    }
    if (operation == B_WEBKIT_EXTENSION_PACKAGE_PREPARED && error == B_OK) fDraft->token = field(*message, "token");
    if (error != B_OK) { Finish(fCancelled ? "Installation cancelled." : field(*message, "description").empty()
        ? std::strerror(error) : field(*message, "description")); return; }
    if (operation == B_WEBKIT_EXTENSION_LOADED) {
        fDraft->loaded = true;
        fDraft->token.clear();
        fDraft->baseURL = field(*message, "base_url");
        if (field(*message, "extension_identifier") != fDraft->entry.identifier
            || field(*message, "fingerprint") != fDraft->entry.fingerprint
            || !fDraft->baseURL.starts_with("webkit-extension://")) {
            Rollback("The loaded extension did not match the approved package.");
            return;
        }
        if (fStopping || fCancelled) { Rollback("Installation cancelled."); return; }
        std::string error;
        if (!fCatalog.Install(*fDraft->package, fDraft->entry, error)) { Rollback("Could not save the installation: " + error); return; }
        if (fLoaded) fLoaded(fDraft->entry, fDraft->baseURL, {}, true);
        Finish("Extension installed.");
        return;
    }
    if (fStopping || fCancelled) { Finish("Installation cancelled."); return; }
    try {
        auto& draft = *fDraft;
        draft.entry.name = field(*message, "name");
        draft.entry.version = field(*message, "version");
        draft.entry.fingerprint = field(*message, "fingerprint");
        draft.entry.package = draft.package->Path().parent_path().filename().string();
        draft.entry.identifier = "summit-" + draft.entry.package;
        const auto manifestText = field(*message, "manifest_json");
        if (draft.token.empty() || draft.entry.name.empty() || draft.entry.name.size() > 4096
            || draft.entry.version.empty() || draft.entry.version.size() > 4096 || draft.entry.fingerprint.size() != 64
            || manifestText.empty() || manifestText.size() > 1024 * 1024) throw std::runtime_error("Incomplete or oversized extension metadata.");
        auto manifest = nlohmann::json::parse(manifestText);
        for (const char* key : { "applications", "browser_specific_settings" }) {
            if (manifest.contains(key) && manifest[key].is_object() && manifest[key].contains("gecko")
                && manifest[key]["gecko"].is_object() && manifest[key]["gecko"].contains("id"))
                draft.entry.identifier = manifest[key]["gecko"]["id"].get<std::string>();
        }
        if (!validIdentity(draft.entry.identifier)) throw std::runtime_error("The extension declares an invalid identity.");
        std::vector<InstalledExtension> entries;
        std::string error;
        if (!fCatalog.Load(entries, error)) throw std::runtime_error(error);
        if (std::any_of(entries.begin(), entries.end(), [&](const auto& entry) { return entry.identifier == draft.entry.identifier; }))
            throw std::runtime_error("An extension with this identity is already installed. Remove it before installing a replacement.");
        draft.permissions = values(*message, "permission");
        draft.origins = values(*message, "origin");
        std::set<std::string> requested, optional;
        auto collect = [&](const char* name, std::set<std::string>& target) {
            if (!manifest.contains(name)) return;
            if (!manifest[name].is_array()) throw std::runtime_error("Invalid extension permission list.");
            for (const auto& value : manifest[name]) {
                auto text = value.get<std::string>();
                if (text.empty() || text.size() > 4096 || target.size() >= 256) throw std::runtime_error("Too many or invalid extension requirements.");
                target.insert(std::move(text));
            }
        };
        collect("permissions", requested);
        collect("host_permissions", requested);
        collect("optional_permissions", optional);
        collect("optional_host_permissions", optional);
        requested.insert(draft.permissions.begin(), draft.permissions.end());
        requested.insert(draft.origins.begin(), draft.origins.end());
        draft.body = ExtensionDisplayText(draft.entry.name) + "\nVersion " + ExtensionDisplayText(draft.entry.version)
            + "\n\nThis package has not been signature-verified. Install only packages you trust."
              "\nCompatibility with this extension has not been verified.\n\nRequested access:\n";
        if (requested.empty()) draft.body += "  No additional permissions requested.\n";
        for (const auto& value : requested) draft.body += "  • " + ExtensionDisplayText(value) + "\n";
        if (!draft.origins.empty()) draft.body += "\nWebsite permissions allow access to data on the listed sites.\n";
        if (!optional.empty()) {
            draft.body += "\nOptional access (not granted by this installation):\n";
            for (const auto& value : optional) draft.body += "  • " + ExtensionDisplayText(value) + "\n";
        }
        draft.body += "\nSome requested APIs may not be available in this Summit build.";
        if (draft.body.size() > 65536) throw std::runtime_error("The permission summary is too large to display.");
        draft.ready = true;
        fStatus = "Review the requested access before installing.";
        Changed();
    } catch (const std::exception& error) { Finish(error.what()); }
}
}
#endif
