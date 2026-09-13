#include "BrowserWindow.h"
#include "Chrome.h"
#include "Messages.h"
#include "core/Address.h"
#include <Alert.h>
#include <Application.h>
#include <CardLayout.h>
#include <Entry.h>
#include <FilePanel.h>
#include <FindDirectory.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <ListItem.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <Path.h>
#include <Roster.h>
#include <ScrollView.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#include <WebDownload.h>
#include <WebKitInfo.h>
#include <WebPage.h>
#include <WebView.h>
#include <algorithm>
#include <cstdio>

namespace summit {
static void AddItem(BMenu* menu, const char* label, uint32 what, char key = 0, uint32 mods = 0)
{
    menu->AddItem(new BMenuItem(label, new BMessage(what), key, mods));
}
BrowserWindow::BrowserWindow(std::filesystem::path profile, std::string homeURL,
    const std::vector<std::string>& urls)
    : BWebWindow(BRect(75, 65, 1195, 745), "Summit", B_TITLED_WINDOW_LOOK, B_NORMAL_WINDOW_FEEL,
          B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS),
      fProfilePath(std::move(profile)), fHomeURL(std::move(homeURL))
{
    std::string error;
    fProfile = Profile::Load(fProfilePath, error);
    fProfileWritable = error.empty();
    auto* menu = new BMenuBar("menu");
    auto* file = new BMenu("File");
    AddItem(file, "New Tab", kNewTab, 'T');
    AddItem(file, "Open File…", kOpenFile, 'O');
    AddItem(file, "Close Tab", kCloseTab, 'W');
    AddItem(file, "Reopen Closed Tab", kReopenTab, 'T', B_SHIFT_KEY);
    file->AddSeparatorItem();
    AddItem(file, "About Summit", B_ABOUT_REQUESTED);
    AddItem(file, "Quit", B_QUIT_REQUESTED, 'Q');
    menu->AddItem(file);
    auto* edit = new BMenu("Edit");
    AddItem(edit, "Undo", B_UNDO, 'Z');
    AddItem(edit, "Redo", B_REDO, 'Z', B_SHIFT_KEY);
    edit->AddSeparatorItem();
    AddItem(edit, "Cut", B_CUT, 'X');
    AddItem(edit, "Copy", B_COPY, 'C');
    AddItem(edit, "Paste", B_PASTE, 'V');
    AddItem(edit, "Select All", B_SELECT_ALL, 'A');
    edit->AddSeparatorItem();
    AddItem(edit, "Find in Page…", kFind, 'F');
    AddItem(edit, "Find Next", kFindNext, 'G');
    menu->AddItem(edit);
    auto* view = new BMenu("View");
    AddItem(view, "Show / Hide Sidebar", kToggleSidebar, 'S', B_SHIFT_KEY);
    AddItem(view, "Focus Address", kFocusAddress, 'L');
    AddItem(view, "Reload", kReload, 'R');
    AddItem(view, "Start Page", kHome);
    view->AddSeparatorItem();
    AddItem(view, "Zoom In", kZoomIn, '+');
    AddItem(view, "Zoom Out", kZoomOut, '-');
    AddItem(view, "Actual Size", kZoomReset, '0');
    menu->AddItem(view);
    auto* history = new BMenu("History");
    AddItem(history, "Back", kBack, '[');
    AddItem(history, "Forward", kForward, ']');
    AddItem(history, "Show History", kShowHistory, 'Y');
    menu->AddItem(history);
    auto* bookmarks = new BMenu("Bookmarks");
    AddItem(bookmarks, "Bookmark This Page", kBookmark, 'D');
    AddItem(bookmarks, "Show Bookmarks", kShowBookmarks);
    menu->AddItem(bookmarks);
    auto* window = new BMenu("Window");
    AddItem(window, "Next Tab", kNextTab);
    AddItem(window, "Previous Tab", kPreviousTab);
    AddItem(window, "Downloads", kShowDownloads, 'J');
    menu->AddItem(window);

    auto* toolbar = new BGroupView(B_HORIZONTAL, 4);
    auto* sidebarButton = new ToolButton("sidebar", "Show / hide sidebar", Icon::Sidebar, kToggleSidebar);
    fBack = new ToolButton("back", "Back", Icon::Back, kBack);
    fForward = new ToolButton("forward", "Forward", Icon::Forward, kForward);
    fReload = new ToolButton("reload", "Reload / stop", Icon::Reload, kReload);
    fAddress = new BTextControl("address", nullptr, "", new BMessage(kNavigate));
    fAddress->SetExplicitMinSize(BSize(240, 30));
    fAddress->SetExplicitMaxSize(BSize(660, B_SIZE_UNSET));
    fAddress->TextView()->SetAlignment(B_ALIGN_CENTER);
    fAddress->SetToolTip("Search or enter a website address");
    BLayoutBuilder::Group<>(toolbar)
        .SetInsets(8, 7, 8, 7)
        .Add(sidebarButton).AddStrut(6).Add(fBack).Add(fForward)
        .AddGlue().Add(fAddress, 3).Add(fReload).AddGlue()
        .Add(new ToolButton("bookmark", "Bookmark this page", Icon::Bookmark, kBookmark))
        .Add(new ToolButton("downloads", "Open Downloads", Icon::Downloads, kShowDownloads))
        .Add(new ToolButton("new-tab", "New tab", Icon::Plus, kNewTab));
    fTabStrip = new TabStrip();
    fProgress = new ProgressLine();
    fSidebar = new BGroupView(B_VERTICAL, 8);
    fSidebar->SetViewColor(240, 244, 241);
    fSidebar->SetExplicitMinSize(BSize(205, 0));
    fSidebar->SetExplicitMaxSize(BSize(205, B_SIZE_UNLIMITED));
    auto* brand = new BStringView("brand", "SUMMIT");
    BFont brandFont(be_bold_font); brandFont.SetSize(19); brand->SetFont(&brandFont);
    fSidebarTitle = new BStringView("sidebar-title", "Bookmarks");
    fSidebarTitle->SetFont(be_bold_font);
    fSavedList = new BListView("saved-pages");
    fSavedList->SetViewColor(240, 244, 241);
    fSavedList->SetInvocationMessage(new BMessage(kOpenSaved));
    fSavedList->SetTarget(this);
    auto* savedScroll = new BScrollView("saved-scroll", fSavedList, 0, false, true, B_NO_BORDER);
    BLayoutBuilder::Group<>(fSidebar)
        .SetInsets(14, 20, 10, 10).Add(brand)
        .Add(new BStringView("platform", "A browser for KunanyiOS"))
        .AddStrut(16)
        .Add(new BButton("start", "Start Page", new BMessage(kHome)))
        .Add(new BButton("bookmarks", "Bookmarks", new BMessage(kShowBookmarks)))
        .Add(new BButton("history", "History", new BMessage(kShowHistory)))
        .AddStrut(12).Add(fSidebarTitle).Add(savedScroll, 1);
    fPages = new BView("pages", 0);
    fCards = new BCardLayout();
    fPages->SetLayout(fCards);
    fPages->SetExplicitMinSize(BSize(320, 200));
    fFindBar = new BGroupView(B_HORIZONTAL, 6);
    fFindText = new BTextControl("find", "Find:", "", new BMessage(kFindNext));
    BLayoutBuilder::Group<>(fFindBar).SetInsets(8, 5, 8, 5)
        .Add(fFindText).Add(new BButton("previous", "Previous", new BMessage(kFindPrevious)))
        .Add(new BButton("next", "Next", new BMessage(kFindNext)))
        .Add(new BButton("done", "Done", new BMessage(kCloseFind)));
    fFindBar->Hide();
    fStatus = new BStringView("status", "Ready");
    fStatus->SetExplicitMinSize(BSize(150, 21));
    fStatus->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, 21));
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
        .Add(menu).Add(toolbar).Add(fTabStrip).Add(fProgress)
        .AddGroup(B_HORIZONTAL, 0, 1).Add(fSidebar).Add(fPages, 1).End()
        .Add(fFindBar).Add(fStatus);
    AddShortcut(B_TAB, B_CONTROL_KEY, new BMessage(kNextTab));
    AddShortcut(B_TAB, B_CONTROL_KEY | B_SHIFT_KEY, new BMessage(kPreviousTab));
    SetSizeLimits(760, 10000, 450, 10000);
    BWebPage::SetDownloadListener(BMessenger(this));

    const auto restored = fProfile.tabs;
    const auto selected = fProfile.selected;
    if (!urls.empty()) for (const auto& url : urls) CreateTab(url);
    else if (!restored.empty()) {
        for (const auto& page : restored) CreateTab(page.url, false);
        if (!fTabs.empty()) SelectTab(fTabs[std::min(selected, fTabs.size() - 1)].id);
    } else CreateTab("summit:home");
    // Invalid command-line or saved URLs may all have been rejected.
    if (fTabs.empty()) CreateTab("summit:home");
    RefreshSidebar(false);
    BMessage save(kSaveSession);
    fSaveTimer = std::make_unique<BMessageRunner>(BMessenger(this), &save, 5000000);
    if (!error.empty()) ShowError("The saved profile could not be read. It has been preserved. " + error);
}
BrowserWindow::~BrowserWindow() = default;
BrowserWindow::Tab* BrowserWindow::FindTab(BWebView* view)
{
    for (auto& tab : fTabs) if (tab.view == view) return &tab;
    return nullptr;
}
BrowserWindow::Tab* BrowserWindow::ActiveTab()
{
    for (auto& tab : fTabs) if (tab.id == fSelected) return &tab;
    return nullptr;
}
std::string BrowserWindow::StoredURL(const BString& url) const
{
    // Haiku's URL notification can use file:/path before WebKit commits
    // the canonical file:///path form. Both refer to our internal start page.
    const std::string value = url.String();
    if (value == fHomeURL || (fHomeURL.rfind("file:///", 0) == 0 && value == "file:" + fHomeURL.substr(7)))
        return "summit:home";
    return value;
}
void BrowserWindow::CreateTab(const std::string& input, bool select, BWebView* adopted)
{
    // BWebPage is owned by the application looper. Its constructor accesses
    // WebCore and takes the application lock; never call it from a window thread.
    if (!adopted && find_thread(nullptr) != be_app->Thread()) {
        BMessage create(kCreateTabOnApp);
        create.AddString("url", input.c_str());
        create.AddBool("select", select);
        create.AddMessenger("window", BMessenger(this));
        be_app->PostMessage(&create);
        return;
    }
    if (fTabs.size() >= 512) { ShowError("The session has reached its 512-tab limit."); return; }
    const auto address = ResolveAddress(input);
    if (!address.error.empty()) { ShowError(address.error); return; }
    auto* webView = adopted ? adopted : new BWebView("web-page");
    webView->SetExplicitMinSize(BSize(320, 200));
    webView->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
    fCards->AddView(webView);
    fTabs.push_back({fNextID++, webView, address.url, address.url == "summit:home" ? "Start Page" : "Loading…"});
    if (select || fSelected == 0) SelectTab(fTabs.back().id);
    if (!adopted) webView->LoadURL(address.url == "summit:home" ? fHomeURL.c_str() : address.url.c_str(), select);
    if (select && address.url == "summit:home") fAddress->MakeFocus();
    RefreshChrome();
}
void BrowserWindow::SelectTab(int64 id)
{
    for (size_t i = 0; i < fTabs.size(); ++i) {
        if (fTabs[i].id != id) continue;
        fSelected = id;
        fCards->SetVisibleItem(static_cast<int32>(i));
        SetCurrentWebView(fTabs[i].view);
        fTabs[i].view->WebPage()->ResendNotifications();
        fTabs[i].view->MakeFocus();
        fAddress->SetText(fTabs[i].url == "summit:home" ? "" : fTabs[i].url.c_str());
        RefreshChrome();
        return;
    }
}
void BrowserWindow::CloseTab(int64 id)
{
    for (size_t i = 0; i < fTabs.size(); ++i) {
        if (fTabs[i].id != id) continue;
        const bool selected = id == fSelected;
        auto* webView = fTabs[i].view;
        fClosedTabs.push_back({fTabs[i].url, fTabs[i].title});
        if (fClosedTabs.size() > 25) fClosedTabs.erase(fClosedTabs.begin());
        if (selected) SetCurrentWebView(nullptr);
        webView->RemoveSelf();
        webView->Shutdown();
        fTabs.erase(fTabs.begin() + i);
        if (fTabs.empty()) { fSelected = 0; CreateTab("summit:home"); }
        else if (selected) SelectTab(fTabs[std::min(i, fTabs.size() - 1)].id);
        RefreshChrome();
        return;
    }
}
void BrowserWindow::Navigate(const std::string& text)
{
    auto address = ResolveAddress(text);
    if (!address.error.empty()) { ShowError(address.error); return; }
    if (auto* tab = ActiveTab()) {
        fAddress->SetText(address.url == "summit:home" ? "" : address.url.c_str());
        tab->view->LoadURL(address.url == "summit:home" ? fHomeURL.c_str() : address.url.c_str());
    }
}
void BrowserWindow::RefreshChrome()
{
    std::vector<TabLabel> labels;
    for (const auto& tab : fTabs) labels.push_back({tab.id, tab.title, tab.loading});
    fTabStrip->SetTabs(std::move(labels), fSelected);
    if (auto* tab = ActiveTab()) {
        SetTitle((tab->title + " — Summit").c_str());
        fBack->SetEnabled(tab->back); fForward->SetEnabled(tab->forward);
        fReload->SetIcon(tab->loading ? Icon::Stop : Icon::Reload);
        fProgress->SetProgress(tab->loading ? std::max(0.03f, tab->progress) : 0);
    }
}
void BrowserWindow::RefreshSidebar(bool history)
{
    fSidebarHistory = history;
    fSidebarTitle->SetText(history ? "History" : "Bookmarks");
    fSidebarPages = history ? fProfile.history : fProfile.bookmarks;
    while (auto* item = fSavedList->RemoveItem(int32(0))) delete item;
    for (const auto& page : fSidebarPages)
        fSavedList->AddItem(new BStringItem((page.title.empty() ? page.url : page.title).c_str()));
}
void BrowserWindow::SaveSession()
{
    if (!fProfileWritable) return;
    fProfile.tabs.clear();
    for (size_t i = 0; i < fTabs.size(); ++i) {
        fProfile.tabs.push_back({fTabs[i].url, fTabs[i].title});
        if (fTabs[i].id == fSelected) fProfile.selected = i;
    }
    std::string error;
    if (!fProfile.Save(fProfilePath, error)) fStatus->SetText(("Could not save session: " + error).c_str());
}
void BrowserWindow::ShowError(const std::string& error)
{
    (new BAlert("Summit", error.c_str(), "OK", nullptr, nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT))->Go(nullptr);
}
bool BrowserWindow::QuitRequested()
{
    if (!fDownloads.empty()) {
        auto* prompt = new BAlert("Downloads", "Downloads are still in progress. Quit and cancel them?", "Keep Browsing", "Quit");
        if (prompt->Go() == 0) return false;
        for (auto* download : fDownloads) download->Cancel();
    }
    SaveSession();
    fSaveTimer.reset();
    SetCurrentWebView(nullptr);
    for (auto& tab : fTabs) { tab.view->RemoveSelf(); tab.view->Shutdown(); }
    fTabs.clear();
    be_app->PostMessage(B_QUIT_REQUESTED);
    return true;
}
void BrowserWindow::MessageReceived(BMessage* message)
{
    auto* tab = ActiveTab();
    switch (message->what) {
        case kNavigate: {
            const char* url = nullptr;
            Navigate(message->FindString("url", &url) == B_OK ? url : fAddress->Text()); break;
        }
        case kNewTab: {
            const char* url = nullptr;
            CreateTab(message->FindString("url", &url) == B_OK ? url : "summit:home");
            fAddress->MakeFocus(); break;
        }
        case kSelectTab: case kCloseTab: {
            int64 id;
            if (message->FindInt64("id", &id) != B_OK) id = fSelected;
            if (message->what == kSelectTab) SelectTab(id); else CloseTab(id); break;
        }
        case kBack: if (tab) tab->view->GoBack(); break;
        case kForward: if (tab) tab->view->GoForward(); break;
        case kReload: if (tab) { if (tab->loading) tab->view->StopLoading(); else tab->view->Reload(); } break;
        case kHome: Navigate("summit:home"); break;
        case kFocusAddress: fAddress->MakeFocus(); fAddress->TextView()->SelectAll(); break;
        case kToggleSidebar: if (fSidebar->IsHidden()) fSidebar->Show(); else fSidebar->Hide(); break;
        case kShowBookmarks: case kShowHistory:
            if (fSidebar->IsHidden()) fSidebar->Show();
            RefreshSidebar(message->what == kShowHistory); break;
        case kOpenSaved: {
            int32 index = fSavedList->CurrentSelection();
            if (index >= 0 && size_t(index) < fSidebarPages.size()) Navigate(fSidebarPages[index].url);
            break;
        }
        case kBookmark:
            if (tab && tab->url != "summit:home" && !tab->url.empty()) {
                auto found = std::find_if(fProfile.bookmarks.begin(), fProfile.bookmarks.end(), [&](const auto& b) { return b.url == tab->url; });
                if (found == fProfile.bookmarks.end()) fProfile.bookmarks.push_back({tab->url, tab->title});
                RefreshSidebar(false); SaveSession(); fStatus->SetText("Bookmark saved");
            } break;
        case kFind: if (fFindBar->IsHidden()) fFindBar->Show(); fFindText->MakeFocus(); fFindText->TextView()->SelectAll(); break;
        case kCloseFind: if (!fFindBar->IsHidden()) fFindBar->Hide(); if (tab) tab->view->MakeFocus(); break;
        case kFindNext: case kFindPrevious:
            if (tab) tab->view->FindString(fFindText->Text(), message->what == kFindNext);
            break;
        case kZoomIn: if (tab) tab->view->IncreaseZoomFactor(false); break;
        case kZoomOut: if (tab) tab->view->DecreaseZoomFactor(false); break;
        case kZoomReset: if (tab) tab->view->ResetZoomFactor(); break;
        case kSaveSession: SaveSession(); break;
        case kNextTab: case kPreviousTab:
            for (size_t i = 0; i < fTabs.size(); ++i) if (fTabs[i].id == fSelected) {
                SelectTab(fTabs[(i + (message->what == kNextTab ? 1 : fTabs.size() - 1)) % fTabs.size()].id); break;
            } break;
        case kReopenTab:
            if (!fClosedTabs.empty()) { auto page = fClosedTabs.back(); fClosedTabs.pop_back(); CreateTab(page.url); } break;
        case kOpenFile:
            if (!fOpenPanel) {
                BMessenger target(this);
                fOpenPanel = std::make_unique<BFilePanel>(B_OPEN_PANEL, &target);
            }
            fOpenPanel->Show(); break;
        case B_REFS_RECEIVED: {
            entry_ref ref;
            for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; ++i) {
                BPath path(&ref); CreateTab("file://" + std::string(path.Path()));
            } break;
        }
        case kShowDownloads: {
            BPath path; find_directory(B_USER_DIRECTORY, &path); path.Append("Downloads");
            std::filesystem::create_directories(path.Path());
            entry_ref ref; if (get_ref_for_path(path.Path(), &ref) == B_OK) be_roster->Launch(&ref); break;
        }
        case B_DOWNLOAD_ADDED: {
            BWebDownload* download = nullptr;
            if (message->FindPointer("download", reinterpret_cast<void**>(&download)) != B_OK || !download) break;
            BPath path; find_directory(B_USER_DIRECTORY, &path); path.Append("Downloads");
            std::filesystem::create_directories(path.Path());
            fDownloads.push_back(download);
            download->SetProgressListener(BMessenger(this));
            download->Start(path);
            fStatus->SetText("Downloading to your Downloads folder…"); break;
        }
        case B_DOWNLOAD_PROGRESS: {
            int64 current = 0; message->FindInt64("current size", &current);
            std::string status = "Downloaded " + std::to_string(current / 1024) + " KiB";
            fStatus->SetText(status.c_str()); break;
        }
        case B_DOWNLOAD_REMOVED: {
            BWebDownload* download = nullptr;
            message->FindPointer("download", reinterpret_cast<void**>(&download));
            fDownloads.erase(std::remove(fDownloads.begin(), fDownloads.end(), download), fDownloads.end());
            fStatus->SetText("Download ended — open Downloads to view the file");
            message->SendReply(B_REPLY); break;
        }
        case B_ABOUT_REQUESTED: {
            std::string info = "Summit — a browser for KunanyiOS\nDevelopment build\n\nWebKit "
                + std::string(WebKitInfo::WebKitVersion().String()) + "\nHaiku port "
                + WebKitInfo::HaikuWebKitVersion().String();
            (new BAlert("About Summit", info.c_str(), "OK"))->Go(nullptr); break;
        }
        case B_CUT: case B_COPY: case B_PASTE: case B_SELECT_ALL: case B_UNDO: case B_REDO:
            if (CurrentFocus()) PostMessage(message, CurrentFocus());
            break;
        case kBrowserState: {
            BMessage reply(B_REPLY);
            reply.AddInt32("count", fTabs.size()); reply.AddInt64("selected", fSelected);
            reply.AddString("address", fAddress->Text());
            reply.AddString("webkit", WebKitInfo::WebKitVersion());
            reply.AddString("haiku_webkit", WebKitInfo::HaikuWebKitVersion());
            for (const auto& page : fTabs) {
                BMessage item; item.AddInt64("id", page.id); item.AddString("url", page.url.c_str());
                item.AddString("title", page.title.c_str()); item.AddBool("loading", page.loading);
                reply.AddMessage("tab", &item);
            }
            message->SendReply(&reply); break;
        }
        default: BWebWindow::MessageReceived(message); break;
    }
}
void BrowserWindow::NavigationRequested(const BString& url, BWebView* view)
{
    // This is a notification for a navigation WebKit has already started.
    // Starting another load here would continually restart the same request.
    if (view == CurrentWebView() && !fAddress->TextView()->IsFocus())
        fAddress->SetText(StoredURL(url) == "summit:home" ? "" : url.String());
}
void BrowserWindow::NewWindowRequested(const BString& url, bool primary) { CreateTab(url.String(), primary); }
void BrowserWindow::NewPageCreated(BWebView* view, BRect, bool, bool, bool activate) { CreateTab("about:blank", activate, view); }
void BrowserWindow::CloseWindowRequested(BWebView* view) { if (auto* tab = FindTab(view)) CloseTab(tab->id); }
void BrowserWindow::LoadNegotiating(const BString& url, BWebView* view)
{
    if (auto* tab = FindTab(view)) { tab->loading = true; tab->progress = 0.03f; }
    (void)url;
    if (view == CurrentWebView()) fStatus->SetText("Connecting…");
    RefreshChrome();
}
void BrowserWindow::LoadCommitted(const BString& url, BWebView* view)
{
    if (auto* tab = FindTab(view)) tab->url = StoredURL(url);
    if (view == CurrentWebView() && !fAddress->TextView()->IsFocus())
        fAddress->SetText(StoredURL(url) == "summit:home" ? "" : url.String());
}
void BrowserWindow::LoadProgress(float progress, BWebView* view)
{
    if (auto* tab = FindTab(view)) {
        tab->progress = std::clamp(progress / 100.0f, 0.0f, 1.0f);
        if (tab->progress >= 1.0f) tab->loading = false;
    }
    if (progress >= 100 && view == CurrentWebView()) fStatus->SetText("Ready");
    RefreshChrome();
}
void BrowserWindow::LoadFailed(const BString&, BWebView* view)
{
    if (auto* tab = FindTab(view)) tab->loading = false;
    if (view == CurrentWebView()) fStatus->SetText("The page could not be loaded");
    RefreshChrome();
}
void BrowserWindow::LoadFinished(const BString& url, BWebView* view)
{
    if (auto* tab = FindTab(view)) {
        // BWebWindow calls this at DOM readiness, before images and other
        // resources finish. Completion arrives through LoadProgress(100).
        tab->url = StoredURL(url);
        fProfile.Visit({tab->url, tab->title});
    }
    if (fSidebarHistory) RefreshSidebar(true);
    RefreshChrome();
}
void BrowserWindow::MainDocumentError(const BString&, const BString& error, BWebView* view)
{
    if (view == CurrentWebView()) fStatus->SetText(error.String());
}
void BrowserWindow::TitleChanged(const BString& title, BWebView* view)
{
    if (auto* tab = FindTab(view)) {
        tab->title = title.String();
        for (auto& page : fProfile.history)
            if (page.url == tab->url) page.title = tab->title;
    }
    RefreshChrome();
}
void BrowserWindow::StatusChanged(const BString& status, BWebView* view)
{
    if (view == CurrentWebView()) fStatus->SetText(status.String());
}
void BrowserWindow::NavigationCapabilitiesChanged(bool back, bool forward, bool, BWebView* view)
{
    // The loader can still report "can stop" while dispatching its final
    // progress notification. It is not a new loading transition.
    if (auto* tab = FindTab(view)) { tab->back = back; tab->forward = forward; }
    RefreshChrome();
}
}
