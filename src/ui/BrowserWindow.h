#pragma once
#include "core/Profile.h"
#if SUMMIT_MODERN_WEBKIT
#include <WebKit/WebKitView.h>
#include <Window.h>
#else
#include <WebWindow.h>
#endif
#include <Messenger.h>
#include <Message.h>
#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

class BCardLayout;
class BFilePanel;
class BGroupLayout;
class BStatusBar;
class BGroupView;
class BMenu;
class BMenuItem;
class BMessageRunner;
class BStringView;
class BTextControl;
#if !SUMMIT_MODERN_WEBKIT
class BWebDownload;
#endif

namespace summit {
class ToolButton;
class ExtensionActionButton;
class TabStrip;
class ProgressLine;
class BookmarksBar;
class FaviconCache;
class SharedProfile;
// How a new window starts.
struct BrowserWindowOptions {
    // The window's place in the saved session (windows are saved in key order).
    uint64 key = 0;
    // Pages to open; when empty, the saved tabs of session are restored.
    std::vector<std::string> urls;
    WindowSession session;
    // A page another page opened (B_WEBKIT_NEW_PAGE_REQUESTED), shown as the only tab.
    uint64 newPage = 0;
    std::string newPageURL;
    // Invalid: placed by the window itself.
    BRect frame;
    // Start without tabs; an extension's windows.create() fills the window.
    bool empty = false;
};
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
    BrowserWindow(std::shared_ptr<SharedProfile> profile, std::string startURL, const BrowserWindowOptions& options,
        std::shared_ptr<BWebKitContext> context, bool extensionsEnabled = false);
#else
    BrowserWindow(std::shared_ptr<SharedProfile> profile, std::string startURL, const BrowserWindowOptions& options);
#endif
    uint64 Key() const { return fKey; }
    // Browser windows that are open and not closing, across the application.
    static int32 CountOpenWindows();
    ~BrowserWindow() override;
    void MessageReceived(BMessage* message) override;
    bool QuitRequested() override;
    void MenusBeginning() override;
#if SUMMIT_MODERN_WEBKIT
    // index < 0 appends. A nonzero command is an extension's browser command
    // that is answered once this tab exists or could not be created.
    // A nonzero replaces closes that tab once the new one exists: extension pages and web
    // pages need different kinds of view, so moving a tab between them swaps its view.
    // A nonzero newPage shows the page of a B_WEBKIT_NEW_PAGE_REQUESTED
    // notification (url is then only what the tab shows until it loads).
    void CreateTab(const std::string& url, bool select = true, const char* extensionIdentifier = nullptr,
        int32 index = -1, uint64 command = 0, int64 replaces = 0, uint64 newPage = 0);
    void WindowActivated(bool active) override;
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
        // For summit:history and summit:bookmarks, the page revision shown.
        uint64 pageRevision = 0;
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
#else
    Tab* FindTab(BWebView* view);
#endif
    Tab* ActiveTab();
    Tab* FindTabByID(int64 id);
    void ProfileChanged(const BMessage&);
    void ApplyInterfaceStyle();
    void RequestNewWindow(const std::vector<std::string>& urls);
    void ShowTabMenu(const BMessage&);
    void UpdateWindowList();
#if SUMMIT_MODERN_WEBKIT
    void ShowPageContextMenu(const BMessage&);
    void NewPageRequested(const BMessage&);
    void LinkOpenRequested(const BMessage&);
    void LinkHovered(const BMessage&);
    void SaveLinkAs(const std::string& url, const std::string& filename);
    void DownloadFinishedForSave(const BMessage&);
    // Save panels waiting for a chosen place, and downloads to move there.
    std::map<std::string, std::string> fSaveAsTargets;
    std::map<uint64, std::string> fSaveAsDownloads;
    std::unique_ptr<BFilePanel> fSavePanel;
    std::string fSavePanelURL;
    std::string fHoveredLink;
    // Background tabs opened from the current tab go after it, in order.
    int32 fBackgroundInsert = -1;
    int32 BackgroundTabIndex();
    bool fQuittingApp = false;
    uint64 fCloseWindowCommand = 0;
#endif
    // Benchmark input synthesis: posts a paced burst of mouse wheel or key
    // events to the active page, so scrolling can be measured the way a person
    // produces it. Refused unless SUMMIT_ENABLE_INPUT_SYNTHESIS=1 (see
    // docs/performance.md); nothing in the browser sends this by itself.
    void SimulateScroll(const BMessage&, BMessage& reply);
    struct ScrollBurst {
        BMessenger view;
        BMessenger window;
        BMessage event;
        int32 count = 0;
        bigtime_t interval = 16666;
    };
    bool fScrollBurstActive = false;
    int32 fScrollBurstRequested = 0;
    int32 fScrollBurstSent = 0;
    bigtime_t fScrollBurstDuration = 0;
    status_t fScrollBurstStatus = B_OK;
#if SUMMIT_MODERN_WEBKIT
    BMessage fScrollBurstCompletionFrameStats;
    bool fScrollBurstCompletionFrameStatsAvailable = false;
#endif
    static status_t RunScrollBurst(void* burst);
    void SelectTab(int64 id, bool forClose = false);
    void CloseTab(int64 id);
#if SUMMIT_MODERN_WEBKIT
    void ReplaceTabView(const Tab& tab, const std::string& url);
#endif
    void Navigate(const std::string& text);
    void RefreshChrome();
    void AnnouncePointer();
#if SUMMIT_MODERN_WEBKIT
    void SyncBrowserWindow();
    // Extension requests to change tabs and windows (B_WEBKIT_BROWSER_COMMAND).
    // Every accepted command is answered exactly once through the context.
    struct OpenCommand {
        uint64 identifier = 0;
        size_t pending = 0;
        std::vector<BWebKitView*> views;
        std::string error;
        bool window = false;
        std::string extension;
    };
    struct CloseCommand {
        uint64 identifier = 0;
        std::set<int64> tabs;
    };
    void BrowserCommand(const BMessage&);
    void OpenTabsForCommand(const BMessage&, uint64 identifier, bool window);
    void CloseTabsForCommand(uint64 identifier, const std::vector<int64>&);
    void TabOpenedForCommand(uint64 command, Tab*, const std::string& error);
    void TabCloseSettled(int64 id, bool closed);
    void RespondToCommand(uint64 identifier, status_t, const std::vector<BWebKitView*>& = { }, const char* error = nullptr);
    bool PrepareTabNavigation(const Tab&);
    std::vector<OpenCommand> fOpenCommands;
    std::vector<CloseCommand> fCloseCommands;
    // Tabs opened for windows.create(), per extension. This single-window
    // browser closes those, never itself, when that extension removes "its" window.
    std::map<std::string, std::vector<int64>> fExtensionWindowTabs;
    void RefreshExtensionActions();
    void ExtensionActionsReceived(const BMessage&);
    void ActivateExtensionAction(const BMessage&);
    void ShowExtensionActions();
    bool fExtensionsEnabled = false;
    BGroupView* fExtensionActions = nullptr;
    ToolButton* fExtensionActionsOverflow = nullptr;
    std::vector<ExtensionActionButton*> fExtensionActionButtons;
    std::vector<BMessage> fExtensionActionState;
    uint64 fExtensionActionRequest = 0;
    uint64 fExtensionActionSnapshot = 0;
    uint64 fExtensionActionRevision = 0;
    uint64 fExtensionActionInvocation = 0;
    uint64 fExtensionActionResultIdentifier = 0;
    status_t fExtensionActionResultError = B_OK;
    std::shared_ptr<std::atomic<bool>> fExtensionMenuCancelled;
#endif
    // Built-in pages (summit:home, summit:history, summit:bookmarks) are
    // files; this gives the address to load for any address.
    std::string LoadableURL(const std::string& url);
    std::filesystem::path InternalPagePath(const std::string& url) const;
    bool WriteInternalPage(const std::string& url);
    void ShowInternalPage(const std::string& url);
    void RefreshInternalPage(Tab& tab, bool force = false);
    void PagesChanged();
    void BookmarksChanged();
    void RefreshBookmarks();
    void RebuildDynamicMenus();
    void ShowBookmarkMenu();
    void IconLoaded(const BMessage& message);
    void SetBookmarksBarVisible(bool visible);
    std::string HomeAddress() const;
    std::string DisplayURL(const std::string& url) const;
    void SaveSession();
    void ShowError(const std::string& error);
    std::string StoredURL(const BString& url) const;
    std::shared_ptr<SharedProfile> fShared;
    uint64 fKey = 0;
    std::string fStartURL;
    bool fBookmarksBarVisible = true;
    std::string fInterfaceStyle;
    BGroupLayout* fLayout = nullptr;
    BGroupView* fToolbar = nullptr;
    ToolButton* fGo = nullptr;
    BStatusBar* fStatusProgress = nullptr;
    BMenu* fWindowMenu = nullptr;
    int32 fWindowMenuFixed = 0;
    std::vector<Tab> fTabs;
    std::vector<PageRecord> fClosedTabs;
    int64 fNextID = 1;
    int64 fSelected = 0;
    TabStrip* fTabStrip;
    BookmarksBar* fBookmarksBar;
    BCardLayout* fCards;
    BView* fPages;
    BMenu* fHistoryMenu;
    BMenu* fBookmarksMenu;
    int32 fHistoryMenuFixed = 0, fBookmarksMenuFixed = 0;
    BMenuItem* fBookmarksBarItem;
    BMenuItem* fRemoveBookmarkItem;
    ToolButton* fBookmarkButton;
    std::unique_ptr<FaviconCache> fFavicons;
    // Bumped whenever history, bookmarks or icons change, so open built-in
    // pages can be brought up to date when they are shown again.
    uint64 fPagesRevision = 1;
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
