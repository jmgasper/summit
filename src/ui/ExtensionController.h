#pragma once
#if SUMMIT_MODERN_WEBKIT
#include "core/ExtensionCatalog.h"
#include <Handler.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class BWebKitContext;
namespace summit {
// Attach to BApplication. The controller serializes extension operations and
// retains its context until shutdown replies and preparation workers drain.
class ExtensionController final : public BHandler {
public:
    struct Entry {
        InstalledExtension installation;
        bool loaded = false;
        std::string baseURL;
        std::string error;
        bool installed = true;
    };
    ExtensionController(std::shared_ptr<BWebKitContext>, std::filesystem::path catalogRoot,
        std::function<void()> changed = { });
    void Start();
    void Shutdown();
    bool HasPendingWork() const;
    void MessageReceived(BMessage*) override;
    const std::vector<Entry>& Entries() const { return fEntries; }
    const std::string& CatalogError() const { return fCatalogError; }
    bool IsReady() const;
    bool SetEnabled(const std::string& identifier, bool enabled);
    bool Remove(const std::string& identifier);
    void AddLoaded(InstalledExtension, std::string baseURL, std::string error = {}, bool installed = true);
private:
    enum class Pending { None, Prepare, Load, Unload };
    enum class Operation { None, Enable, Disable, Remove };
    void Next();
    void Fail(const std::string&);
    void Changed();
    void DiscardToken();
    void Advance();
    void FinishRemovalOrDisable();
    std::shared_ptr<BWebKitContext> fContext;
    ExtensionCatalog fCatalog;
    std::function<void()> fChanged;
    std::vector<Entry> fEntries;
    std::string fCatalogError, fToken;
    size_t fIndex = 0;
    uint64 fRequest = 0;
    Pending fPending = Pending::None;
    Operation fOperation = Operation::None;
    bool fStarted = false, fStopping = false;
};
}
#endif
