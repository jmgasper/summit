#pragma once
#include "core/Profile.h"
#if SUMMIT_MODERN_WEBKIT
#include <WebKit/WebKitView.h>
#include <Window.h>
#else
#include <WebWindow.h>
#endif
#include <Messenger.h>
#include <memory>
#include <optional>
#include <set>
#include <vector>

class BCardLayout;
class BFilePanel;
class BGroupView;
class BListView;
class BMessageRunner;
class BStringView;
class BTextControl;
#if !SUMMIT_MODERN_WEBKIT
class BWebDownload;
#endif

namespace summit {
class ToolButton;
class TabStrip;
class ProgressLine;
#if SUMMIT_MODERN_WEBKIT
using BrowserWindowBase = BWindow;
using BrowserWebView = BWebKitView;
#else
using BrowserWindowBase = BWebWindow;
using BrowserWebView = BWebView;
#endif
class BrowserWindow : public BrowserWindowBase {
public:
#if SUMMIT_MODERN_WEBKIT
    BrowserWindow(std::filesystem::path profile, std::string homeURL, const std::vector<std::string>& urls,
        std::shared_ptr<BWebKitContext> context);
#else
    BrowserWindow(std::filesystem::path profile, std::string homeURL, const std::vector<std::string>& urls);
#endif
    ~BrowserWindow() override;
    void MessageReceived(BMessage* message) override;
    bool QuitRequested() override;
#if SUMMIT_MODERN_WEBKIT
    void CreateTab(const std::string& url, bool select = true);
#else
    void NavigationRequested(const BString& url, BWebView* view) override;
    void NewWindowRequested(const BString& url, bool primary) override;
    void NewPageCreated(BWebView* view, BRect frame, bool modal, bool resizable, bool activate) override;
    void CloseWindowRequested(BWebView* view) override;
    void LoadNegotiating(const BString& url, BWebView* view) override;
    void LoadCommitted(const BString& url, BWebView* view) override;
    void LoadProgress(float progress, BWebView* view) override;
    void LoadFailed(const BString& url, BWebView* view) override;
    void LoadFinished(const BString& url, BWebView* view) override;
    void MainDocumentError(const BString& url, const BString& error, BWebView* view) override;
    void TitleChanged(const BString& title, BWebView* view) override;
    void StatusChanged(const BString& status, BWebView* view) override;
    void NavigationCapabilitiesChanged(bool back, bool forward, bool stop, BWebView* view) override;
    void CreateTab(const std::string& url, bool select = true, BWebView* adopted = nullptr);
#endif
private:
#if SUMMIT_MODERN_WEBKIT
    struct CloseFocusState {
        int64 selected = 0;
        BMessenger focus;
        std::string address;
        int32 selectionStart = 0, selectionEnd = 0;
        uint64 generation = 0;
    };
#endif
    struct Tab {
        int64 id;
        BrowserWebView* view;
        std::string url, title;
        bool loading = false, back = false, forward = false;
        float progress = 0;
#if SUMMIT_MODERN_WEBKIT
        BMessenger messenger { };
        bool processExited = false;
        std::string processError;
        std::string loadError { }, loadErrorDescription { }, loadErrorDomain { }, loadErrorURL { };
        std::string loadOutcome { "idle" };
        int32 loadErrorCode = 0;
        bool loadErrorProvisional = false;
        uint64 loadGeneration = 0, loadSuccessSequence = 0;
        bool closeQueued = false;
        bool closeRequested = false;
        bool closeApproved = false;
        std::optional<CloseFocusState> closeFocus { };
        double pageZoom = 1, textZoom = 1;
#endif
    };
#if SUMMIT_MODERN_WEBKIT
    Tab* FindTab(const BMessenger& view);
    void WebKitStateChanged(const BMessage& message);
    void ShowTabStatus(const Tab&);
    void WebKitFindResult(const BMessage& message);
    void WebKitCloseResult(const BMessage& message);
    void WebKitClosePrompt(const BMessage& message);
    void WebKitCloseCommitted(const BMessage& message);
    void CommitTabCloses(const std::vector<int64>&, bool wholeWindow);
    bool PrepareNavigation();
    void InvalidateWindowClose();
    void FinishCloseTab(int64 id);
    void StartCloseRequest(Tab&);
    void ContinueTabCloses();
    void BeginWindowClose();
    void ContinueWindowClose();
    void CancelWindowClose();
    CloseFocusState CaptureCloseFocus() const;
    void RestoreCloseFocus(const CloseFocusState&);
    std::shared_ptr<BWebKitContext> fWebKitContext;
    bool fClosingWindow = false;
    bool fWindowCloseInvalidated = false;
    bool fWindowCloseQueued = false;
    bool fCloseCommitPending = false;
    bool fCommitWholeWindow = false;
    uint64 fCloseCommitIdentifier = 0;
    std::vector<int64> fCommitTabs;
    uint64 fSelectionGeneration = 0;
    int64 fClosePromptTab = 0;
    std::optional<CloseFocusState> fWindowCloseFocus;
    std::set<uint64> fDownloads;
    BMessenger fDownloadPrompt;
    uint64 fDownloadPromptGeneration = 0;
    bool fDownloadQuitApproved = false;
    bool fDownloadPromptPending = false;
#else
    Tab* FindTab(BWebView* view);
#endif
    Tab* ActiveTab();
    void SelectTab(int64 id, bool forClose = false);
    void CloseTab(int64 id);
    void Navigate(const std::string& text);
    void RefreshChrome();
    void RefreshSidebar(bool history);
    void SaveSession();
    void ShowError(const std::string& error);
    std::string StoredURL(const BString& url) const;
    std::filesystem::path fProfilePath;
    std::string fHomeURL;
    Profile fProfile;
    bool fProfileWritable = true;
    std::vector<Tab> fTabs;
    std::vector<PageRecord> fClosedTabs;
    std::vector<PageRecord> fSidebarPages;
    int64 fNextID = 1;
    int64 fSelected = 0;
    bool fSidebarHistory = false;
    TabStrip* fTabStrip;
    BCardLayout* fCards;
    BView* fPages;
    BGroupView* fSidebar;
    BListView* fSavedList;
    BStringView* fSidebarTitle;
    BStringView* fStatus;
    BTextControl* fAddress;
    BTextControl* fFindText;
    BGroupView* fFindBar;
    ToolButton* fBack;
    ToolButton* fForward;
    ToolButton* fReload;
    ProgressLine* fProgress;
    std::unique_ptr<BFilePanel> fOpenPanel;
    std::unique_ptr<BMessageRunner> fSaveTimer;
#if !SUMMIT_MODERN_WEBKIT
    std::vector<BWebDownload*> fDownloads;
#endif
};
}
