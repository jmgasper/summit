#pragma once

#if SUMMIT_MODERN_WEBKIT

#include <Handler.h>
#include <Messenger.h>
#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace summit {

// Attach to the application looper. Decisions run on that looper and may
// submit an asynchronous reply to BWebKitContext. Windows own no engine state.
class ExtensionPermissionPrompt final : public BHandler {
public:
    using Respond = std::function<void(const std::string&, bool)>;
    explicit ExtensionPermissionPrompt(Respond);
    ~ExtensionPermissionPrompt() override;
    void MessageReceived(BMessage*) override;
    void Shutdown();
    bool HasOpenWindows() const;

private:
    void Dismiss();
    Respond fRespond;
    BMessenger fWindow;
    std::string fIdentifier;
    bigtime_t fDeadline = 0;
    std::shared_ptr<std::atomic<unsigned>> fOpenWindows;
    bool fShutdown = false;
};

} // namespace summit

#endif
