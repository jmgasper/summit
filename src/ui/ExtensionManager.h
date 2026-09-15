#pragma once
#if SUMMIT_MODERN_WEBKIT
#include <Window.h>
#include <Messenger.h>
#include <atomic>
#include <memory>
#include <string>

class BButton;
class BCheckBox;
class BFilePanel;
class BGroupView;
class BListView;
class BStringView;
class BTextView;
namespace summit {
class ExtensionManager final : public BWindow {
public:
    ExtensionManager(BMessenger owner, std::shared_ptr<std::atomic<unsigned>> windows);
    ~ExtensionManager() override;
    bool QuitRequested() override;
    void MessageReceived(BMessage*) override;
private:
    void Render();
    void Send(BMessage&);
    void Apply(const BMessage&);
    BMessenger fOwner;
    std::shared_ptr<std::atomic<unsigned>> fWindows;
    std::unique_ptr<BFilePanel> fPanel;
    BListView* fList;
    BStringView* fHeading;
    BTextView* fDetails;
    BStringView* fStatus;
    BButton *fAdd, *fToggle, *fRemove, *fInstall, *fCancel;
    BCheckBox* fFiles;
    BGroupView* fApproval;
    std::string fDraftName, fDraftBody, fStatusText;
    uint64 fGeneration = 0;
    bool fBusy = false, fReady = false, fApprovalReady = false;
};
}
#endif
