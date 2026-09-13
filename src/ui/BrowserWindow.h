#pragma once
#include "core/Profile.h"
#include <WebWindow.h>
#include <memory>
#include <vector>

class BCardLayout;
class BFilePanel;
class BGroupView;
class BListView;
class BMessageRunner;
class BStringView;
class BTextControl;
class BWebDownload;

namespace summit {
class ToolButton;
class TabStrip;
class ProgressLine;
class BrowserWindow : public BWebWindow {
public:
    BrowserWindow(std::filesystem::path profile, std::string homeURL, const std::vector<std::string>& urls);
    ~BrowserWindow() override;
    void MessageReceived(BMessage* message) override;
    bool QuitRequested() override;
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
private:
    struct Tab {
        int64 id;
        BWebView* view;
        std::string url, title;
        bool loading = false, back = false, forward = false;
        float progress = 0;
    };
    Tab* FindTab(BWebView* view);
    Tab* ActiveTab();
    void SelectTab(int64 id);
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
    std::vector<BWebDownload*> fDownloads;
};
}
