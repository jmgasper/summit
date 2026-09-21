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
#include <Invoker.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <ListItem.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <MessageFilter.h>
#include <Path.h>
#include <Roster.h>
#include <ScrollView.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#if SUMMIT_MODERN_WEBKIT
#include "ExtensionInstaller.h"
#include <PopUpMenu.h>
#include <WebKit/WebKitInfo.h>
#else
#include <WebDownload.h>
#include <WebKitInfo.h>
#include <WebPage.h>
#include <WebView.h>
#endif
#include <app/AppMisc.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>

namespace summit {
#if SUMMIT_MODERN_WEBKIT
class ExtensionActionMenuItem final : public BMenuItem {
public:
    ExtensionActionMenuItem(const char* label, BMessage* message) : BMenuItem(label, message) { }
    bool Selected() const { return IsSelected(); }
    status_t Activate() { return Invoke(); }
};
class ExtensionActionsMenu final : public BPopUpMenu {
public:
    explicit ExtensionActionsMenu(std::shared_ptr<std::atomic<bool>> cancelled)
        : BPopUpMenu("Extension actions", false, false), fCancelled(std::move(cancelled))
    {
        SetAsyncAutoDestruct(true);
        SetTrackingHook([](BMenu*, void* state) {
            return static_cast<std::atomic<bool>*>(state)->load();
        }, fCancelled.get());
    }
    void AttachedToWindow() override
    {
        BPopUpMenu::AttachedToWindow();
        BMessage tick('exmt');
        fTimer = std::make_unique<BMessageRunner>(BMessenger(this), &tick, 50000);
        if (fTimer->InitCheck() != B_OK) *fCancelled = true;
    }
    void DetachedFromWindow() override
    {
        fTimer.reset();
        BPopUpMenu::DetachedFromWindow();
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what == 'exmt') {
            // Haiku's tracking hook is outside its mouse-idle wait loop.
            // Deliver Escape on the menu looper to wake that loop as well.
            if (fCancelled->load()) { const char escape = B_ESCAPE; KeyDown(&escape, 1); }
            return;
        }
        BPopUpMenu::MessageReceived(message);
    }
    void KeyDown(const char* bytes, int32 count) override
    {
        if (count && (bytes[0] == B_ENTER || bytes[0] == B_SPACE)) {
            for (int32 i = 0; i < CountItems(); ++i) {
                auto* item = dynamic_cast<ExtensionActionMenuItem*>(ItemAt(i));
                if (!item || !item->Selected() || !item->IsEnabled()) continue;
                // BPopUpMenu::Go can repeat tracking during its opening-click
                // interval and discard a quick keyboard choice. Deliver once
                // here, then cancel tracking without leaving a chosen item.
                item->Activate();
                *fCancelled = true;
                const char escape = B_ESCAPE;
                BPopUpMenu::KeyDown(&escape, 1);
                return;
            }
        }
        BPopUpMenu::KeyDown(bytes, count);
        if (!Window()) {
            // Escape and native mnemonic activation can also finish during
            // the opening-click interval. Do not restart a dismissed menu.
            *fCancelled = true;
            return;
        }
        if (count && bytes[0] == B_DOWN_ARROW) {
            // Haiku's first Down traversal skips the final item when no item
            // was selected. If all actions are disabled, reach Manage as well.
            for (int32 i = 0; i < CountItems(); ++i)
                if (auto* item = dynamic_cast<ExtensionActionMenuItem*>(ItemAt(i)); item && item->Selected()) return;
            const char up = B_UP_ARROW;
            BPopUpMenu::KeyDown(&up, 1);
        }
    }
private:
    // The native tracking thread can outlive its browser window's derived
    // members. Keep the cancellation flag alive until the menu is destroyed.
    std::shared_ptr<std::atomic<bool>> fCancelled;
    std::unique_ptr<BMessageRunner> fTimer;
};
#endif
class AddressEnterFilter final : public BMessageFilter {
public:
    explicit AddressEnterFilter(BTextControl& control)
        : BMessageFilter(B_KEY_DOWN), fControl(control) { }

    filter_result Filter(BMessage* message, BHandler**) override
    {
        const char* bytes = nullptr;
        if (message->FindString("bytes", &bytes) != B_OK || !bytes
            || bytes[0] != B_ENTER || bytes[1] != '\0') return B_DISPATCH_MESSAGE;
        if (fControl.IsEnabled() && fControl.TextView()->IsFocus()) {
            // BTextControl normally suppresses Enter when the text matches its
            // saved value. An address must remain submit-able for retry/reload.
            fControl.Invoke();
            fControl.TextView()->SelectAll();
        }
        return B_SKIP_MESSAGE;
    }

private:
    BTextControl& fControl;
};

class AddressControl final : public BTextControl {
public:
    AddressControl() : BTextControl("address", nullptr, "", new BMessage(kNavigate))
    {
        TextView()->AddFilter(new AddressEnterFilter(*this));
    }

    status_t Invoke(BMessage* message = nullptr) override
    {
        // Native BTextControl also invokes changed text when focus leaves.
        // A tab switch or beforeunload prompt must not submit an address draft.
        if (!TextView()->IsFocus()) return B_OK;
        return BTextControl::Invoke(message);
    }
};

static bool FindDownloads(BPath& path, std::string& error)
{
    status_t status = find_directory(B_USER_DIRECTORY, &path);
    if (status == B_OK) status = path.Append("Downloads");
    if (status != B_OK) {
        error = "Could not locate the Downloads folder: " + std::string(std::strerror(status));
        return false;
    }
    std::error_code filesystemError;
    std::filesystem::create_directories(path.Path(), filesystemError);
    if (filesystemError) {
        error = "Could not open the Downloads folder: " + filesystemError.message();
        return false;
    }
    return true;
}

static void AddItem(BMenu* menu, const char* label, uint32 what, char key = 0, uint32 mods = 0)
{
    menu->AddItem(new BMenuItem(label, new BMessage(what), key, mods));
}
BrowserWindow::BrowserWindow(std::filesystem::path profile, std::string homeURL,
    const std::vector<std::string>& urls
#if SUMMIT_MODERN_WEBKIT
    , std::shared_ptr<BWebKitContext> context, bool extensionsEnabled
#endif
    )
    : BrowserWindowBase(BRect(75, 65, 1195, 745), "Summit", B_TITLED_WINDOW_LOOK, B_NORMAL_WINDOW_FEEL,
          B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS),
#if SUMMIT_MODERN_WEBKIT
      fWebKitContext(std::move(context)),
#endif
      fProfilePath(std::move(profile)), fHomeURL(std::move(homeURL))
{
#if SUMMIT_MODERN_WEBKIT
    fExtensionsEnabled = extensionsEnabled;
#endif
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
#if SUMMIT_MODERN_WEBKIT
    auto* extensions = new BMenuItem("Extensions…", new BMessage(kShowExtensions));
    extensions->SetEnabled(extensionsEnabled);
    window->AddItem(extensions);
#endif
    menu->AddItem(window);

    auto* toolbar = new BGroupView(B_HORIZONTAL, 4);
    auto* sidebarButton = new ToolButton("sidebar", "Show / hide sidebar", Icon::Sidebar, kToggleSidebar);
    fBack = new ToolButton("back", "Back", Icon::Back, kBack);
    fForward = new ToolButton("forward", "Forward", Icon::Forward, kForward);
    fReload = new ToolButton("reload", "Reload / stop", Icon::Reload, kReload);
    fAddress = new AddressControl;
    fAddress->SetExplicitMinSize(BSize(240, 30));
    fAddress->SetExplicitMaxSize(BSize(660, B_SIZE_UNSET));
    fAddress->TextView()->SetAlignment(B_ALIGN_CENTER);
    fAddress->SetToolTip("Search or enter a website address");
    auto* downloadsButton = new ToolButton("downloads", "Open Downloads", Icon::Downloads, kShowDownloads);
    BLayoutBuilder::Group<>(toolbar)
        .SetInsets(8, 7, 8, 7)
        .Add(sidebarButton).AddStrut(6).Add(fBack).Add(fForward)
        .AddGlue().Add(fAddress, 3).Add(fReload).AddGlue()
        .Add(new ToolButton("bookmark", "Bookmark this page", Icon::Bookmark, kBookmark))
        .Add(downloadsButton)
#if SUMMIT_MODERN_WEBKIT
        .Add(fExtensionActions = new BGroupView("extension-actions", B_HORIZONTAL, 2))
#endif
        .Add(new ToolButton("new-tab", "New tab", Icon::Plus, kNewTab));
#if SUMMIT_MODERN_WEBKIT
    fExtensionActions->Hide();
#endif
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
#if SUMMIT_MODERN_WEBKIT
    BPath downloadsPath;
    std::string downloadsError;
    if (!FindDownloads(downloadsPath, downloadsError)) ShowError(downloadsError);
    else if (auto status = fWebKitContext->SetDownloadDirectory(downloadsPath.Path()); status != B_OK)
        ShowError("Could not configure downloads: " + std::string(std::strerror(status)));
#else
    BWebPage::SetDownloadListener(BMessenger(this));
#endif

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
BrowserWindow::~BrowserWindow()
{
#if SUMMIT_MODERN_WEBKIT
    if (fExtensionMenuCancelled) *fExtensionMenuCancelled = true;
    fWebKitContext->SetBrowserWindowTabs(BMessenger(this), { }, nullptr, false);
    SaveSession();
    fSaveTimer.reset();
    for (auto& tab : fTabs) {
        tab.view->RemoveSelf();
        delete tab.view;
    }
    fTabs.clear();
#endif
}
#if SUMMIT_MODERN_WEBKIT
BrowserWindow::Tab* BrowserWindow::FindTab(const BMessenger& view)
{
    if (!view.IsValid()) return nullptr;
    for (auto& tab : fTabs) if (tab.messenger == view) return &tab;
    return nullptr;
}
#else
BrowserWindow::Tab* BrowserWindow::FindTab(BWebView* view)
{
    for (auto& tab : fTabs) if (tab.view == view) return &tab;
    return nullptr;
}
#endif
BrowserWindow::Tab* BrowserWindow::ActiveTab()
{
    for (auto& tab : fTabs) if (tab.id == fSelected) return &tab;
    return nullptr;
}
// Posts the burst from its own thread: the window thread has to stay free to
// draw the frames whose pacing is being measured.
status_t BrowserWindow::RunScrollBurst(void* data)
{
    std::unique_ptr<ScrollBurst> burst(static_cast<ScrollBurst*>(data));
    for (int32 index = 0; index < burst->count; ++index) {
        // A full view port throttles the burst instead of dropping events.
        if (burst->view.SendMessage(&burst->event, static_cast<BHandler*>(nullptr), 2000000) != B_OK)
            return B_ERROR;
        if (burst->interval) snooze(burst->interval);
    }
    return B_OK;
}
void BrowserWindow::SimulateScroll(const BMessage& message, BMessage& reply)
{
    const char* enabled = std::getenv("SUMMIT_ENABLE_INPUT_SYNTHESIS");
    if (!enabled || std::strcmp(enabled, "1") != 0) {
        reply.AddString("error", "input synthesis is disabled");
        return;
    }
    auto* tab = ActiveTab();
    if (!tab || !tab->view) {
        reply.AddString("error", "no active tab");
        return;
    }
    const int32 count = message.GetInt32("count", 60);
    const int32 interval = message.GetInt32("interval_ms", 16);
    const float delta = message.GetFloat("delta", 3.0f);
    if (count < 1 || count > 100000 || interval < 0 || interval > 10000
        || !std::isfinite(delta) || std::fabs(delta) > 1000) {
        reply.AddString("error", "unusable burst parameters");
        return;
    }
    auto burst = std::make_unique<ScrollBurst>();
    status_t status = B_OK;
    burst->view = BMessenger(tab->view, this, &status);
    if (status != B_OK || !burst->view.IsValid()) {
        reply.AddString("error", "the page view cannot be addressed");
        return;
    }
    burst->count = count;
    burst->interval = bigtime_t(interval) * 1000;
    burst->event.what = B_MOUSE_WHEEL_CHANGED;
    burst->event.AddFloat("be:wheel_delta_x", 0.0f);
    burst->event.AddFloat("be:wheel_delta_y", delta);
    // A wheel notch applies where the pointer is. Name the middle of the page
    // instead of moving the pointer, so a burst does not take the mouse away
    // from whoever is using the machine.
    BRect bounds = tab->view->Bounds();
    BPoint centre(bounds.left + bounds.Width() / 2, bounds.top + bounds.Height() / 2);
    burst->event.AddPoint("summit:view_where", centre);
    // BWindow routes a wheel message to the view named by "_view_token", and
    // only falls back to whichever view the pointer last moved over. Without
    // the token a synthesized notch is delivered to that other view, or
    // dropped, and the page never scrolls.
    burst->event.AddInt32("_view_token", _get_object_token_(tab->view));
    thread_id thread = spawn_thread(&BrowserWindow::RunScrollBurst, "summit scroll burst",
        B_DISPLAY_PRIORITY, burst.get());
    if (thread < 0) {
        reply.AddString("error", "no thread for the burst");
        return;
    }
    burst.release();
    resume_thread(thread);
    reply.AddInt32("count", count);
    reply.AddInt32("interval_ms", interval);
    reply.AddFloat("delta", delta);
    reply.AddPoint("at", centre);
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
#if SUMMIT_MODERN_WEBKIT
void BrowserWindow::CreateTab(const std::string& input, bool select, const char* extensionIdentifier, int32 index, uint64 command, int64 replaces)
#else
void BrowserWindow::CreateTab(const std::string& input, bool select, BWebView* adopted)
#endif
{
#if SUMMIT_MODERN_WEBKIT
    // An extension learns about a failure through its own promise; only the
    // user's own requests interrupt with an alert.
    auto fail = [&](const std::string& error) {
        if (command) TabOpenedForCommand(command, nullptr, error);
        else ShowError(error);
    };
    if (fClosingWindow) { if (command) TabOpenedForCommand(command, nullptr, "The window is closing."); return; }
#else
    auto fail = [&](const std::string& error) { ShowError(error); };
#endif
#if !SUMMIT_MODERN_WEBKIT
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
#endif
    if (fTabs.size() >= 512) { fail("The session has reached its 512-tab limit."); return; }
    const auto address = ResolveAddress(input);
    if (!address.error.empty()) { fail(address.error); return; }
#if SUMMIT_MODERN_WEBKIT
    const bool extensionPage = address.url.starts_with("webkit-extension:");
    if (extensionPage && !extensionIdentifier) {
        // The application owns the live extension catalog. Resolve its current
        // origin and create the privileged view on WebKit's application thread.
        BMessage create(kCreateTabOnApp);
        create.AddString("url", address.url.c_str());
        create.AddBool("select", select);
        create.AddMessenger("window", BMessenger(this));
        create.AddInt32("index", index);
        create.AddUInt64("command", command);
        create.AddInt64("replaces", replaces);
        if (be_app->PostMessage(&create) != B_OK) fail("Could not open the extension page.");
        return;
    }
    BWebKitView* webView = nullptr;
    if (extensionPage) {
        if (!*extensionIdentifier) { fail("This extension is not available."); return; }
        status_t status;
        webView = fWebKitContext->CreateExtensionView(BRect(0, 0, 319, 199), "web-page", extensionIdentifier, BMessenger(this), &status);
        if (!webView) { fail("Could not open the extension page: " + std::string(std::strerror(status))); return; }
    } else
        webView = new BWebKitView(BRect(0, 0, 319, 199), "web-page", BMessenger(this), B_FOLLOW_ALL, fWebKitContext);
    if (webView->InitCheck() != B_OK) {
        const status_t status = webView->InitCheck();
        delete webView;
        fail("Could not create the web page: " + std::string(std::strerror(status)));
        return;
    }
#else
    auto* webView = adopted ? adopted : new BWebView("web-page");
#endif
    webView->SetExplicitMinSize(BSize(320, 200));
    webView->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
#if SUMMIT_MODERN_WEBKIT
    // The card layout and the tab list share one order; keep them in step.
    const size_t position = index < 0 ? fTabs.size() : std::min(static_cast<size_t>(index), fTabs.size());
    fCards->AddView(static_cast<int32>(position), webView);
    Tab created { };
    created.id = fNextID++;
    created.view = webView;
    created.url = address.url;
    created.title = address.url == "summit:home" ? "Start Page" : "Loading…";
    created.messenger = BMessenger(webView);
    const int64 createdID = created.id;
    fTabs.insert(fTabs.begin() + position, std::move(created));
    if (select || fSelected == 0) SelectTab(createdID);
    else if (auto* active = ActiveTab()) {
        // Inserting before the visible card shifts its index.
        for (size_t i = 0; i < fTabs.size(); ++i)
            if (fTabs[i].id == active->id) fCards->SetVisibleItem(static_cast<int32>(i));
    }
#else
    fCards->AddView(webView);
    fTabs.push_back({fNextID++, webView, address.url, address.url == "summit:home" ? "Start Page" : "Loading…"});
    if (select || fSelected == 0) SelectTab(fTabs.back().id);
#endif
#if SUMMIT_MODERN_WEBKIT
    webView->LoadURL(address.url == "summit:home" ? fHomeURL.c_str() : address.url.c_str());
#else
    if (!adopted) webView->LoadURL(address.url == "summit:home" ? fHomeURL.c_str() : address.url.c_str(), select);
#endif
    if (select && address.url == "summit:home") fAddress->MakeFocus();
#if SUMMIT_MODERN_WEBKIT
    SyncBrowserWindow();
#endif
    RefreshChrome();
#if SUMMIT_MODERN_WEBKIT
    if (replaces && replaces != createdID) CloseTab(replaces);
    // Answer after the registry submission above, so the engine can already
    // resolve the new view as a tab of this window.
    if (command) {
        auto created = std::find_if(fTabs.begin(), fTabs.end(), [&](const Tab& tab) { return tab.id == createdID; });
        TabOpenedForCommand(command, created == fTabs.end() ? nullptr : &*created, "The tab closed before it could be reported.");
    }
#endif
}
#if SUMMIT_MODERN_WEBKIT
void BrowserWindow::ReplaceTabView(const Tab& tab, const std::string& url)
{
    int32 index = -1;
    for (size_t i = 0; i < fTabs.size(); ++i) if (fTabs[i].id == tab.id) index = static_cast<int32>(i);
    // Views are created on the application thread; the old tab closes once the new one exists.
    BMessage create(kCreateTabOnApp);
    create.AddString("url", url.c_str());
    create.AddBool("select", tab.id == fSelected);
    create.AddMessenger("window", BMessenger(this));
    create.AddInt32("index", index);
    create.AddInt64("replaces", tab.id);
    be_app->PostMessage(&create);
}
#endif
void BrowserWindow::SelectTab(int64 id, bool forClose)
{
#if !SUMMIT_MODERN_WEBKIT
    (void)forClose;
#endif
    for (size_t i = 0; i < fTabs.size(); ++i) {
        if (fTabs[i].id != id) continue;
#if SUMMIT_MODERN_WEBKIT
        if (!forClose) ++fSelectionGeneration;
#endif
        fSelected = id;
        fCards->SetVisibleItem(static_cast<int32>(i));
#if !SUMMIT_MODERN_WEBKIT
        SetCurrentWebView(fTabs[i].view);
        fTabs[i].view->WebPage()->ResendNotifications();
#else
        ShowTabStatus(fTabs[i]);
#endif
        fTabs[i].view->MakeFocus();
        fAddress->SetText(fTabs[i].url == "summit:home" ? "" : fTabs[i].url.c_str());
#if SUMMIT_MODERN_WEBKIT
        SyncBrowserWindow();
#endif
        RefreshChrome();
        return;
    }
}
void BrowserWindow::CloseTab(int64 id)
{
#if SUMMIT_MODERN_WEBKIT
    if (fClosingWindow) return;
    for (auto& tab : fTabs) {
        if (tab.id != id || tab.closeRequested || tab.closeQueued || tab.closeApproved) continue;
        tab.closeQueued = true;
        ContinueTabCloses();
        return;
    }
}

void BrowserWindow::StartCloseRequest(Tab& tab)
{
    tab.closeQueued = false;
    tab.closeRequested = true;
    tab.view->RequestClose();
}

void BrowserWindow::ContinueTabCloses()
{
    if (fCloseCommitPending) return;
    if (fClosingWindow) { ContinueWindowClose(); return; }
    // One outstanding decision avoids overlapping beforeunload prompts and
    // lets a cancelled window close reset completed approvals safely.
    if (std::any_of(fTabs.begin(), fTabs.end(), [](const Tab& tab) { return tab.closeRequested; })) return;
    // A page can request window.close while another approval is committing.
    // Keep that completed approval queued instead of requesting it again.
    for (const auto& tab : fTabs) {
        if (!tab.closeApproved) continue;
        CommitTabCloses({ tab.id }, false);
        return;
    }
    for (auto& tab : fTabs) {
        if (!tab.closeQueued) continue;
        StartCloseRequest(tab);
        return;
    }
}

BrowserWindow::CloseFocusState BrowserWindow::CaptureCloseFocus() const
{
    CloseFocusState state;
    state.selected = fSelected;
    if (auto* focus = CurrentFocus()) state.focus = BMessenger(focus);
    state.address = fAddress->Text();
    fAddress->TextView()->GetSelection(&state.selectionStart, &state.selectionEnd);
    state.generation = fSelectionGeneration;
    return state;
}

void BrowserWindow::RestoreCloseFocus(const CloseFocusState& state)
{
    if (std::none_of(fTabs.begin(), fTabs.end(), [&](const Tab& tab) { return tab.id == state.selected; })) return;
    SelectTab(state.selected, true);
    fAddress->SetText(state.address.c_str());
    BLooper* looper = nullptr;
    if (auto* focus = dynamic_cast<BView*>(state.focus.Target(&looper)); focus && looper == this)
        focus->MakeFocus();
    // BTextControl selects all text when its editor regains focus.
    fAddress->TextView()->Select(state.selectionStart, state.selectionEnd);
}

void BrowserWindow::WebKitClosePrompt(const BMessage& message)
{
    BMessenger sender;
    if (message.FindMessenger("view", &sender) != B_OK) return;
    auto* tab = FindTab(sender);
    if (!tab || !tab->closeRequested) return;
    // The bridge sends this before the actual dialog message to this same
    // window looper, so making the card visible precedes native presentation.
    if (tab->id == fSelected) return;
    if (fClosingWindow) {
        if (!fClosePromptTab || !fWindowCloseFocus || fWindowCloseFocus->generation != fSelectionGeneration)
            fWindowCloseFocus = CaptureCloseFocus();
        fClosePromptTab = tab->id;
    } else if (!tab->closeFocus || tab->closeFocus->generation != fSelectionGeneration)
        tab->closeFocus = CaptureCloseFocus();
    SelectTab(tab->id, true);
}

void BrowserWindow::BeginWindowClose()
{
    if (fExtensionMenuCancelled) *fExtensionMenuCancelled = true;
    if (fClosingWindow) return;
    if (fCloseCommitPending) { fWindowCloseQueued = true; return; }
    fWindowCloseQueued = false;
    fWindowCloseInvalidated = false;
    fDownloadQuitApproved = false;
    // Approval is provisional until every tab agrees. Keep live documents,
    // undo state and the full saved session intact when any tab chooses Stay.
    SaveSession();
    fWindowCloseFocus = CaptureCloseFocus();
    fClosePromptTab = 0;
    for (const auto& tab : fTabs) {
        if (tab.closeRequested && tab.closeFocus && tab.closeFocus->generation == fSelectionGeneration && tab.id == fSelected) {
            fWindowCloseFocus = tab.closeFocus;
            fClosePromptTab = tab.id;
            break;
        }
    }
    fClosingWindow = true;
    ContinueWindowClose();
}

void BrowserWindow::ContinueWindowClose()
{
    if (!fClosingWindow || fCloseCommitPending) return;
    if (std::any_of(fTabs.begin(), fTabs.end(), [](const Tab& tab) { return tab.closeRequested; })) return;
    if (fWindowCloseInvalidated) {
        CancelWindowClose();
        fStatus->SetText("Close cancelled");
        return;
    }
    if (auto* selected = ActiveTab(); selected && !selected->closeApproved) {
        StartCloseRequest(*selected);
        return;
    }
    for (auto& tab : fTabs) {
        if (tab.closeApproved) continue;
        StartCloseRequest(tab);
        return;
    }
    if (fDownloadPromptPending) return;
    if (!fDownloads.empty() && !fDownloadQuitApproved) {
        auto* reply = new BMessage(kDownloadQuitReply);
        reply->AddUInt64("generation", ++fDownloadPromptGeneration);
        auto* prompt = new BAlert("Downloads", "Downloads are still in progress. Quit and cancel them?",
            "Keep Browsing", "Quit");
        fDownloadPrompt = BMessenger(prompt);
        fDownloadPromptPending = true;
        fClosePromptTab = fSelected;
        if (prompt->Go(new BInvoker(reply, BMessenger(this))) != B_OK) CancelWindowClose();
        return;
    }
    std::vector<int64> identifiers;
    for (const auto& tab : fTabs) identifiers.push_back(tab.id);
    CommitTabCloses(identifiers, true);
}

void BrowserWindow::InvalidateWindowClose()
{
    if (!fClosingWindow) return;
    fWindowCloseInvalidated = true;
    // Keep the pending request owned by the window-close sequence until its
    // definitive reply. Resetting now could turn a late approval into an
    // ordinary tab-close event after cancellation.
    ContinueWindowClose();
}

bool BrowserWindow::PrepareNavigation()
{
    if (fCloseCommitPending && (fCommitWholeWindow
        || std::find(fCommitTabs.begin(), fCommitTabs.end(), fSelected) != fCommitTabs.end()))
        return false;
    ++fSelectionGeneration;
    InvalidateWindowClose();
    return true;
}

void BrowserWindow::CommitTabCloses(const std::vector<int64>& identifiers, bool wholeWindow)
{
    if (fCloseCommitPending) return;
    std::vector<BWebKitView*> views;
    for (auto id : identifiers) {
        auto found = std::find_if(fTabs.begin(), fTabs.end(), [id](const Tab& tab) { return tab.id == id; });
        if (found == fTabs.end()) return;
        views.push_back(found->view);
    }
    fCloseCommitPending = true;
    fCommitWholeWindow = wholeWindow;
    fCommitTabs = identifiers;
    BWebKitView::CommitClose(views, BMessenger(this), ++fCloseCommitIdentifier);
}

void BrowserWindow::WebKitCloseCommitted(const BMessage& message)
{
    uint64 identifier;
    bool closed;
    if (!fCloseCommitPending || message.FindUInt64("identifier", &identifier) != B_OK
        || identifier != fCloseCommitIdentifier || message.FindBool("closed", &closed) != B_OK) return;
    fCloseCommitPending = false;
    auto identifiers = std::exchange(fCommitTabs, { });
    if (fCommitWholeWindow) {
        if (!closed) {
            CancelWindowClose();
            fStatus->SetText("Close cancelled");
            return;
        }
        for (auto& tab : fTabs) {
            tab.view->RemoveSelf();
            delete tab.view;
        }
        fTabs.clear();
        fSelected = 0;
        BMessage ready(kWindowReadyToClose);
        ready.AddMessenger("window", BMessenger(this));
        be_app->PostMessage(&ready);
        return;
    }
    for (auto id : identifiers) {
        auto found = std::find_if(fTabs.begin(), fTabs.end(), [id](const Tab& tab) { return tab.id == id; });
        if (found == fTabs.end()) continue;
        auto focus = found->closeFocus;
        const bool restoreFocus = focus && id == fSelected && focus->generation == fSelectionGeneration;
        if (closed)
            FinishCloseTab(id);
        else {
            found->closeApproved = found->closeRequested = found->closeQueued = false;
            found->closeFocus.reset();
            found->view->ResetCloseRequest();
            TabCloseSettled(id, false);
        }
        if (restoreFocus) RestoreCloseFocus(*focus);
    }
    if (!closed) { SaveSession(); fStatus->SetText("Close cancelled"); }
    if (fWindowCloseQueued) BeginWindowClose();
    else ContinueTabCloses();
}

void BrowserWindow::CancelWindowClose()
{
    ++fDownloadPromptGeneration;
    fDownloadPromptPending = false;
    fDownloadQuitApproved = false;
    if (fDownloadPrompt.IsValid()) fDownloadPrompt.SendMessage(B_QUIT_REQUESTED);
    fDownloadPrompt = BMessenger();
    for (auto& tab : fTabs) {
        tab.closeQueued = tab.closeRequested = tab.closeApproved = false;
        tab.closeFocus.reset();
        tab.view->ResetCloseRequest();
    }
    // Tab closes an extension asked for were absorbed by this window close.
    while (!fCloseCommands.empty()) TabCloseSettled(*fCloseCommands.front().tabs.begin(), false);
    fClosingWindow = false;
    fWindowCloseInvalidated = false;
    fWindowCloseQueued = false;
    if (fWindowCloseFocus && fClosePromptTab == fSelected && fWindowCloseFocus->generation == fSelectionGeneration)
        RestoreCloseFocus(*fWindowCloseFocus);
    fWindowCloseFocus.reset();
    fClosePromptTab = 0;
    SaveSession();
}

void BrowserWindow::WebKitCloseResult(const BMessage& message)
{
    BMessenger sender;
    if (message.FindMessenger("view", &sender) != B_OK) return;
    auto* tab = FindTab(sender);
    if (!tab) return;
    if (message.what == B_WEBKIT_CLOSE_CANCELLED) {
        if (!tab->closeRequested) return;
        tab->closeRequested = false;
        if (!fClosingWindow) TabCloseSettled(tab->id, false);
        if (fClosingWindow)
            CancelWindowClose();
        else {
            auto focus = std::exchange(tab->closeFocus, std::nullopt);
            if (focus && tab->id == fSelected && focus->generation == fSelectionGeneration)
                RestoreCloseFocus(*focus);
            SaveSession();
            ContinueTabCloses();
        }
        fStatus->SetText("Close cancelled");
        return;
    }
    tab->closeRequested = tab->closeQueued = false;
    if (fClosingWindow) {
        tab->closeApproved = true;
        ContinueWindowClose();
        return;
    }
    tab->closeApproved = true;
    ContinueTabCloses();
}

void BrowserWindow::FinishCloseTab(int64 id)
{
#endif
    for (size_t i = 0; i < fTabs.size(); ++i) {
        if (fTabs[i].id != id) continue;
        const bool selected = id == fSelected;
        auto* webView = fTabs[i].view;
        fClosedTabs.push_back({fTabs[i].url, fTabs[i].title});
        if (fClosedTabs.size() > 25) fClosedTabs.erase(fClosedTabs.begin());
#if !SUMMIT_MODERN_WEBKIT
        if (selected) SetCurrentWebView(nullptr);
#endif
        webView->RemoveSelf();
#if SUMMIT_MODERN_WEBKIT
        delete webView;
#else
        webView->Shutdown();
#endif
        fTabs.erase(fTabs.begin() + i);
        if (fTabs.empty()) {
            fSelected = 0;
#if SUMMIT_MODERN_WEBKIT
            if (!fClosingWindow)
#endif
                CreateTab("summit:home");
        }
        else if (selected) SelectTab(fTabs[std::min(i, fTabs.size() - 1)].id);
#if SUMMIT_MODERN_WEBKIT
        SyncBrowserWindow();
#endif
        RefreshChrome();
#if SUMMIT_MODERN_WEBKIT
        TabCloseSettled(id, true);
#endif
        return;
    }
}
void BrowserWindow::Navigate(const std::string& text)
{
    auto address = ResolveAddress(text);
    if (!address.error.empty()) { ShowError(address.error); return; }
#if SUMMIT_MODERN_WEBKIT
    if (!PrepareNavigation()) return;
#endif
    if (auto* tab = ActiveTab()) {
        fAddress->SetText(address.url == "summit:home" ? "" : address.url.c_str());
        tab->view->LoadURL(address.url == "summit:home" ? fHomeURL.c_str() : address.url.c_str());
    }
}
#if SUMMIT_MODERN_WEBKIT
void BrowserWindow::SyncBrowserWindow()
{
    std::vector<BWebKitView*> views;
    views.reserve(fTabs.size());
    for (auto& tab : fTabs)
        views.push_back(tab.view);
    auto* active = ActiveTab();
    fWebKitContext->SetBrowserWindowTabs(BMessenger(this), views, active ? active->view : nullptr, IsActive());
    // Registration is dispatched to the application looper. A queued window
    // invalidation also works during construction on the application thread.
    if (fExtensionsEnabled) PostMessage(B_WEBKIT_EXTENSION_ACTIONS_CHANGED);
}

void BrowserWindow::RespondToCommand(uint64 identifier, status_t status, const std::vector<BWebKitView*>& views, const char* error)
{
    fWebKitContext->RespondToBrowserCommand(identifier, status, views, error);
}

bool BrowserWindow::PrepareTabNavigation(const Tab& tab)
{
    // A tab whose close is being committed no longer accepts navigation.
    if (fCloseCommitPending && (fCommitWholeWindow
        || std::find(fCommitTabs.begin(), fCommitTabs.end(), tab.id) != fCommitTabs.end()))
        return false;
    if (tab.id == fSelected) ++fSelectionGeneration;
    InvalidateWindowClose();
    return true;
}

void BrowserWindow::TabOpenedForCommand(uint64 command, Tab* tab, const std::string& error)
{
    auto found = std::find_if(fOpenCommands.begin(), fOpenCommands.end(),
        [command](const OpenCommand& pending) { return pending.identifier == command; });
    if (found == fOpenCommands.end()) return; // The engine stopped waiting.
    if (tab) {
        found->views.push_back(tab->view);
        if (found->window) fExtensionWindowTabs[found->extension].push_back(tab->id);
    } else if (found->error.empty()) found->error = error;
    if (found->pending && --found->pending) return;
    auto finished = std::move(*found);
    fOpenCommands.erase(found);
    if (finished.views.empty()) RespondToCommand(finished.identifier, B_ERROR, { }, finished.error.c_str());
    else RespondToCommand(finished.identifier, B_OK, finished.views);
}

void BrowserWindow::TabCloseSettled(int64 id, bool closed)
{
    for (auto& entry : fExtensionWindowTabs)
        if (closed) std::erase(entry.second, id);
    for (auto command = fCloseCommands.begin(); command != fCloseCommands.end();) {
        if (!command->tabs.contains(id)) { ++command; continue; }
        if (!closed) {
            // One kept tab fails the whole request, like a rejected tabs.remove().
            const auto identifier = command->identifier;
            command = fCloseCommands.erase(command);
            RespondToCommand(identifier, B_CANCELED, { }, "The tab was kept open.");
            continue;
        }
        command->tabs.erase(id);
        if (!command->tabs.empty()) { ++command; continue; }
        const auto identifier = command->identifier;
        command = fCloseCommands.erase(command);
        RespondToCommand(identifier, B_OK);
    }
}

void BrowserWindow::CloseTabsForCommand(uint64 identifier, const std::vector<int64>& tabs)
{
    if (fClosingWindow) { RespondToCommand(identifier, B_BUSY, { }, "The window is closing."); return; }
    CloseCommand command;
    command.identifier = identifier;
    for (auto id : tabs)
        if (std::any_of(fTabs.begin(), fTabs.end(), [id](const Tab& tab) { return tab.id == id; })) command.tabs.insert(id);
    // Tabs that are already gone count as closed.
    if (command.tabs.empty()) { RespondToCommand(identifier, B_OK); return; }
    const auto pending = command.tabs;
    fCloseCommands.push_back(std::move(command));
    // The usual asynchronous handshake runs beforeunload one tab at a time and
    // never blocks this looper; its outcome arrives through TabCloseSettled.
    for (auto id : pending) CloseTab(id);
}

void BrowserWindow::OpenTabsForCommand(const BMessage& message, uint64 identifier, bool window)
{
    OpenCommand command;
    command.identifier = identifier;
    command.window = window;
    command.extension = message.GetString("extension_identifier", "");
    std::vector<std::string> urls;
    const char* url = nullptr;
    for (int32 i = 0; message.FindString("url", i, &url) == B_OK; ++i) urls.push_back(url);
    bool select = message.GetBool(window ? "focused" : "active", true);
    if (window) {
        // This browser has one window: windows.create() opens its pages as
        // tabs here. Existing tabs named by the request already live here.
        BMessenger view;
        for (int32 i = 0; message.FindMessenger("view", i, &view) == B_OK; ++i) {
            auto* tab = FindTab(view);
            if (!tab) continue;
            command.views.push_back(tab->view);
            fExtensionWindowTabs[command.extension].push_back(tab->id);
            if (select) { SelectTab(tab->id); select = false; }
        }
        if (urls.empty() && command.views.empty()) urls.push_back("summit:home");
        if (urls.empty()) { RespondToCommand(identifier, B_OK, command.views); return; }
    } else if (urls.empty()) urls.push_back("summit:home");
    const int32 index = window ? -1 : message.GetInt32("index", -1);
    command.pending = urls.size();
    fOpenCommands.push_back(std::move(command));
    for (const auto& address : urls) {
        // Only the first page of a new "window" takes the selection.
        CreateTab(address, select, nullptr, index, identifier);
        select = false;
    }
}

void BrowserWindow::BrowserCommand(const BMessage& message)
{
    uint64 identifier = 0;
    uint32 command = 0;
    if (message.FindUInt64("identifier", &identifier) != B_OK || !identifier) return;
    if (message.what == B_WEBKIT_BROWSER_COMMAND_CANCELLED) {
        std::erase_if(fOpenCommands, [identifier](const OpenCommand& pending) { return pending.identifier == identifier; });
        std::erase_if(fCloseCommands, [identifier](const CloseCommand& pending) { return pending.identifier == identifier; });
        return;
    }
    if (message.FindUInt32("command", &command) != B_OK) { RespondToCommand(identifier, B_BAD_VALUE); return; }
    BMessenger view;
    auto* tab = message.FindMessenger("view", &view) == B_OK ? FindTab(view) : nullptr;
    switch (command) {
        case B_WEBKIT_BROWSER_OPEN_TAB: OpenTabsForCommand(message, identifier, false); return;
        case B_WEBKIT_BROWSER_OPEN_WINDOW: OpenTabsForCommand(message, identifier, true); return;
        case B_WEBKIT_BROWSER_CLOSE_TABS: {
            std::vector<int64> tabs;
            for (int32 i = 0; message.FindMessenger("view", i, &view) == B_OK; ++i)
                if (auto* closing = FindTab(view)) tabs.push_back(closing->id);
            CloseTabsForCommand(identifier, tabs);
            return;
        }
        case B_WEBKIT_BROWSER_CLOSE_WINDOW: {
            // Never let an extension close the only browser window. It may
            // close the tabs this window opened for its windows.create() calls.
            const auto tabs = fExtensionWindowTabs[message.GetString("extension_identifier", "")];
            if (tabs.empty()) RespondToCommand(identifier, B_NOT_ALLOWED, { }, "Summit has a single window, which extensions cannot close.");
            else CloseTabsForCommand(identifier, tabs);
            return;
        }
        case B_WEBKIT_BROWSER_FOCUS_WINDOW:
            if (IsMinimized()) Minimize(false);
            Activate(true);
            RespondToCommand(identifier, B_OK);
            return;
        case B_WEBKIT_BROWSER_SET_WINDOW_STATE: {
            const std::string state = message.GetString("state", "");
            if (state == "minimized") Minimize(true);
            else if (state == "normal") { if (IsMinimized()) Minimize(false); }
            else { RespondToCommand(identifier, B_NOT_SUPPORTED); return; }
            RespondToCommand(identifier, B_OK);
            return;
        }
        default: break;
    }
    if (!tab) { RespondToCommand(identifier, B_ENTRY_NOT_FOUND, { }, "The tab is not open in this window."); return; }
    switch (command) {
        case B_WEBKIT_BROWSER_ACTIVATE_TAB:
            if (fClosingWindow) { RespondToCommand(identifier, B_BUSY, { }, "The window is closing."); return; }
            SelectTab(tab->id);
            RespondToCommand(identifier, B_OK);
            return;
        case B_WEBKIT_BROWSER_NAVIGATE_TAB: {
            const auto address = ResolveAddress(message.GetString("url", ""));
            if (!address.error.empty()) { RespondToCommand(identifier, B_BAD_VALUE, { }, address.error.c_str()); return; }
            // Extension pages need the privileged view that CreateTab makes;
            // an ordinary web view cannot become one.
            const bool extensionPage = address.url.starts_with("webkit-extension:");
            const bool extensionTab = tab->url.starts_with("webkit-extension:");
            if (extensionPage != extensionTab || (extensionPage
                && address.url.substr(0, address.url.find('/', 19)) != tab->url.substr(0, tab->url.find('/', 19)))) {
                // The page needs the other kind of view: replace the tab's view in place.
                ReplaceTabView(*tab, address.url);
                RespondToCommand(identifier, B_OK);
                return;
            }
            if (!PrepareTabNavigation(*tab)) { RespondToCommand(identifier, B_BUSY, { }, "The tab is closing."); return; }
            if (tab->id == fSelected) fAddress->SetText(address.url == "summit:home" ? "" : address.url.c_str());
            tab->view->LoadURL(address.url == "summit:home" ? fHomeURL.c_str() : address.url.c_str());
            RespondToCommand(identifier, B_OK);
            return;
        }
        case B_WEBKIT_BROWSER_RELOAD_TAB:
            // The view has no cache-bypassing reload yet; bypass_cache reloads normally.
            if (!PrepareTabNavigation(*tab)) { RespondToCommand(identifier, B_BUSY, { }, "The tab is closing."); return; }
            tab->view->Reload();
            RespondToCommand(identifier, B_OK);
            return;
        case B_WEBKIT_BROWSER_GO_BACK: case B_WEBKIT_BROWSER_GO_FORWARD: {
            const bool back = command == B_WEBKIT_BROWSER_GO_BACK;
            if (!(back ? tab->back : tab->forward)) {
                RespondToCommand(identifier, B_ERROR, { }, back ? "There is no page to go back to." : "There is no page to go forward to.");
                return;
            }
            if (!PrepareTabNavigation(*tab)) { RespondToCommand(identifier, B_BUSY, { }, "The tab is closing."); return; }
            if (back) tab->view->GoBack(); else tab->view->GoForward();
            RespondToCommand(identifier, B_OK);
            return;
        }
        default:
            RespondToCommand(identifier, B_NOT_SUPPORTED);
    }
}

void BrowserWindow::RefreshExtensionActions()
{
    if (!fExtensionsEnabled) return;
    fExtensionActionSnapshot = 0;
    for (auto* button : fExtensionActionButtons) button->SetEnabled(false);
    if (fExtensionActionsOverflow) fExtensionActionsOverflow->SetEnabled(false);
    const auto status = fWebKitContext->GetExtensionActions(BMessenger(this), BMessenger(this), ++fExtensionActionRequest);
    if (status != B_OK) {
        BMessage reply(B_WEBKIT_EXTENSION_ACTIONS);
        reply.AddUInt64("identifier", fExtensionActionRequest);
        reply.AddInt32("error", status);
        ExtensionActionsReceived(reply);
    }
}

void BrowserWindow::ExtensionActionsReceived(const BMessage& message)
{
    uint64 request = 0;
    if (!fExtensionsEnabled || message.FindUInt64("identifier", &request) != B_OK || request != fExtensionActionRequest) return;
    std::vector<BMessage> actions;
    if (message.GetInt32("error", B_ERROR) == B_OK) {
        BMessage action;
        for (int32 index = 0; message.FindMessage("action", index, &action) == B_OK; ++index) {
            const char* identity = nullptr;
            if (action.FindString("extension_identifier", &identity) != B_OK || !*identity
                || !action.GetUInt64("load_identifier", 0) || !action.GetUInt64("page_identifier", 0)) continue;
            actions.push_back(action);
        }
    }
    const auto visible = std::min(size_t(4), actions.size());
    bool sameState = actions.size() == fExtensionActionState.size();
    for (size_t i = 0; sameState && i < actions.size(); ++i)
        sameState = actions[i].HasSameData(fExtensionActionState[i]);
    if (!sameState || !fExtensionActionRevision) {
        ++fExtensionActionRevision;
        if (fExtensionMenuCancelled) *fExtensionMenuCancelled = true;
    }
    bool sameButtons = visible == fExtensionActionButtons.size() && bool(fExtensionActionsOverflow) == (actions.size() > visible);
    for (size_t i = 0; sameButtons && i < visible; ++i) {
        const char* identity = "";
        actions[i].FindString("extension_identifier", &identity);
        sameButtons = std::string(fExtensionActionButtons[i]->Name()) == std::string("extension-action-") + identity;
    }
    if (!sameButtons) {
        while (auto* child = fExtensionActions->ChildAt(0)) { child->RemoveSelf(); delete child; }
        fExtensionActionButtons.clear();
        fExtensionActionsOverflow = nullptr;
        for (size_t i = 0; i < visible; ++i) {
            const char* identity = "";
            actions[i].FindString("extension_identifier", &identity);
            auto* button = new ExtensionActionButton(identity);
            BLayoutBuilder::Group<>(fExtensionActions).Add(button);
            button->SetTarget(this);
            fExtensionActionButtons.push_back(button);
        }
        if (actions.size() > visible) {
            fExtensionActionsOverflow = new ToolButton("extension-actions-more", "More extension actions", Icon::More, kShowExtensionActions);
            BLayoutBuilder::Group<>(fExtensionActions).Add(fExtensionActionsOverflow);
            fExtensionActionsOverflow->SetTarget(this);
        }
    }
    fExtensionActionState = std::move(actions);
    fExtensionActionSnapshot = fExtensionActionRevision;
    for (size_t i = 0; i < visible; ++i) fExtensionActionButtons[i]->SetAction(fExtensionActionState[i], fExtensionActionSnapshot);
    if (fExtensionActionsOverflow) fExtensionActionsOverflow->SetEnabled(true);
    if (fExtensionActionState.empty()) {
        if (!fExtensionActions->IsHidden()) fExtensionActions->Hide();
    } else if (fExtensionActions->IsHidden()) fExtensionActions->Show();
}

void BrowserWindow::ActivateExtensionAction(const BMessage& message)
{
    if (!fExtensionsEnabled || !fExtensionActionSnapshot || message.GetUInt64("snapshot", 0) != fExtensionActionSnapshot
        || fClosingWindow || fCloseCommitPending || !IsActive()) return;
    const char* identity = nullptr;
    if (message.FindString("extension_identifier", &identity) != B_OK) return;
    const auto load = message.GetUInt64("load_identifier", 0);
    const auto page = message.GetUInt64("page_identifier", 0);
    const auto status = fWebKitContext->ActivateExtensionAction(identity, BMessenger(this), load, page,
        BMessenger(this), ++fExtensionActionInvocation);
    if (status != B_OK) {
        fStatus->SetText(("Could not activate extension: " + std::string(std::strerror(status))).c_str());
        RefreshExtensionActions();
    }
}

void BrowserWindow::ShowExtensionActions()
{
    if (!fExtensionActionSnapshot || !fExtensionActionsOverflow) return;
    if (fExtensionMenuCancelled) *fExtensionMenuCancelled = true;
    fExtensionMenuCancelled = std::make_shared<std::atomic<bool>>(false);
    auto* menu = new ExtensionActionsMenu(fExtensionMenuCancelled);
    for (size_t i = fExtensionActionButtons.size(); i < fExtensionActionState.size(); ++i) {
        const auto& action = fExtensionActionState[i];
        const char* title = "";
        action.FindString("title", &title);
        if (!*title) action.FindString("name", &title);
        auto label = ExtensionDisplayText(title);
        const char* badge = "";
        action.FindString("badge", &badge);
        if (*badge) label += " (" + ExtensionDisplayText(badge) + ")";
        BString menuLabel(label.c_str());
        menu->TruncateString(&menuLabel, B_TRUNCATE_END, 360);
        auto* invocation = new BMessage(action);
        invocation->what = kActivateExtensionAction;
        invocation->AddUInt64("snapshot", fExtensionActionSnapshot);
        auto* item = new ExtensionActionMenuItem(menuLabel.String(), invocation);
        item->SetTarget(BMessenger(this));
        item->SetEnabled(action.GetBool("enabled", false));
        menu->AddItem(item);
    }
    menu->AddSeparatorItem();
    auto* manage = new ExtensionActionMenuItem("Manage extensions…", new BMessage(kShowExtensions));
    manage->SetTarget(BMessenger(this));
    menu->AddItem(manage);
    menu->Go(fExtensionActionsOverflow->ConvertToScreen(fExtensionActionsOverflow->Bounds().LeftBottom()), true, true, true);
}

void BrowserWindow::WindowActivated(bool active)
{
    BrowserWindowBase::WindowActivated(active);
    SyncBrowserWindow();
}
#endif

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
#if SUMMIT_MODERN_WEBKIT
    if (fClosingWindow) return;
#endif
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
#if SUMMIT_MODERN_WEBKIT
    // The application closes this window and drains queued WebKit destruction
    // before it stops the application looper.
    be_app->PostMessage(B_QUIT_REQUESTED);
    return false;
#else
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
#endif
}
void BrowserWindow::MessageReceived(BMessage* message)
{
    auto* tab = ActiveTab();
    switch (message->what) {
#if SUMMIT_MODERN_WEBKIT
        case B_WEBKIT_EXTENSION_ACTIONS_CHANGED: RefreshExtensionActions(); break;
        case B_WEBKIT_BROWSER_COMMAND: case B_WEBKIT_BROWSER_COMMAND_CANCELLED: BrowserCommand(*message); break;
        case B_WEBKIT_EXTENSION_ACTIONS: ExtensionActionsReceived(*message); break;
        case kActivateExtensionAction: ActivateExtensionAction(*message); break;
        case kShowExtensionActions: ShowExtensionActions(); break;
        case B_WEBKIT_EXTENSION_ACTION_ACTIVATED:
            if (message->GetUInt64("identifier", 0) == fExtensionActionInvocation) {
                fExtensionActionResultIdentifier = fExtensionActionInvocation;
                fExtensionActionResultError = message->GetInt32("error", B_ERROR);
                if (fExtensionActionResultError != B_OK) {
                    fStatus->SetText("The extension action changed before it could open. Please try again.");
                    RefreshExtensionActions();
                }
            }
            break;
        case kDownloadQuitReply: {
            uint64 generation = 0;
            int32 which = 0;
            if (!fClosingWindow || !fDownloadPromptPending
                || message->FindUInt64("generation", &generation) != B_OK
                || generation != fDownloadPromptGeneration) break;
            message->FindInt32("which", &which);
            fDownloadPromptPending = false;
            fDownloadPrompt = BMessenger();
            if (which == 1) {
                fDownloadQuitApproved = true;
                ContinueWindowClose();
            } else CancelWindowClose();
            break;
        }
        case kRequestWindowClose: BeginWindowClose(); break;
        case B_WEBKIT_CLOSE_PROMPT: WebKitClosePrompt(*message); break;
        case B_WEBKIT_CLOSE_COMMITTED: WebKitCloseCommitted(*message); break;
        case B_WEBKIT_CLOSE_REQUESTED: case B_WEBKIT_CLOSE_CANCELLED:
            WebKitCloseResult(*message); break;
        case B_WEBKIT_EXTERNAL_NAVIGATION_REQUESTED: {
            // An extension page sent its tab to a web page (for example a sign-in page).
            BMessenger view;
            const char* url = nullptr;
            if (message->FindMessenger("view", &view) != B_OK || message->FindString("url", &url) != B_OK) break;
            if (auto* tab = FindTab(view); tab && !tab->closeRequested && !tab->closeQueued)
                ReplaceTabView(*tab, url);
            break;
        }
#endif
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
        case kBack:
#if SUMMIT_MODERN_WEBKIT
            if (!tab || !tab->back || !PrepareNavigation()) break;
#endif
            if (tab) tab->view->GoBack();
            break;
        case kForward:
#if SUMMIT_MODERN_WEBKIT
            if (!tab || !tab->forward || !PrepareNavigation()) break;
#endif
            if (tab) tab->view->GoForward();
            break;
        case kReload: if (tab) {
#if SUMMIT_MODERN_WEBKIT
            if (!tab->loading && !PrepareNavigation()) break;
#endif
            if (tab->loading) {
#if SUMMIT_MODERN_WEBKIT
                tab->view->Stop();
#else
                tab->view->StopLoading();
#endif
            } else tab->view->Reload();
        } break;
        case kHome: Navigate("summit:home"); break;
        case kFocusAddress: fAddress->MakeFocus(); fAddress->TextView()->SelectAll(); break;
        case kToggleSidebar: if (fSidebar->IsHidden()) fSidebar->Show(); else fSidebar->Hide(); break;
        case kShowBookmarks: case kShowHistory:
            if (fSidebar->IsHidden()) fSidebar->Show();
            RefreshSidebar(message->what == kShowHistory); break;
        case kShowExtensions: be_app->PostMessage(kShowExtensions); break;
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
        case kCloseFind:
            if (!fFindBar->IsHidden()) fFindBar->Hide();
            if (tab) {
#if SUMMIT_MODERN_WEBKIT
                tab->view->HideFindUI();
                fStatus->SetText(tab->loading ? "Loading…" : "Ready");
#endif
                tab->view->MakeFocus();
            }
            break;
        case kFindNext: case kFindPrevious:
            if (tab) {
                tab->view->FindString(fFindText->Text(), message->what == kFindNext);
#if SUMMIT_MODERN_WEBKIT
                fStatus->SetText(*fFindText->Text() ? "Searching…" : "Ready");
#endif
            }
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
                BPath path(&ref);
                if (path.InitCheck() == B_OK) CreateTab(FileURL(path.Path()));
            } break;
        }
        case kShowDownloads: {
            BPath path;
            std::string error;
            if (!FindDownloads(path, error)) { ShowError(error); break; }
            entry_ref ref;
            status_t status = get_ref_for_path(path.Path(), &ref);
            if (status == B_OK) status = be_roster->Launch(&ref);
            if (status != B_OK) ShowError("Could not open Downloads: " + std::string(std::strerror(status)));
            break;
        }
#if !SUMMIT_MODERN_WEBKIT
        case B_DOWNLOAD_ADDED: {
            BWebDownload* download = nullptr;
            if (message->FindPointer("download", reinterpret_cast<void**>(&download)) != B_OK || !download) break;
            BPath path;
            std::string error;
            if (!FindDownloads(path, error)) {
                download->Cancel();
                ShowError(error);
                break;
            }
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
            int32 status = -1;
            if (message->FindInt32("status", &status) != B_OK) status = -1;
            switch (status) {
                case B_DOWNLOAD_FINISHED: fStatus->SetText("Download complete — open Downloads to view the file"); break;
                case B_DOWNLOAD_FAILED: fStatus->SetText("Download failed"); break;
                case B_DOWNLOAD_BLOCKED: fStatus->SetText("Download blocked"); break;
                case B_DOWNLOAD_CANNOT_SHOW_URL: fStatus->SetText("This address could not be downloaded"); break;
                default: fStatus->SetText("Download ended — open Downloads to view the file"); break;
            }
            message->SendReply(B_REPLY); break;
        }
#else
        case B_WEBKIT_DOWNLOAD_STARTED: {
            uint64 identifier = 0;
            if (message->FindUInt64("identifier", &identifier) != B_OK || !identifier) break;
            fDownloads.insert(identifier);
            fStatus->SetText("Downloading to your Downloads folder…");
            break;
        }
        case B_WEBKIT_DOWNLOAD_PROGRESS: {
            uint64 current = 0;
            message->FindUInt64("current_size", &current);
            std::string status = "Downloaded " + std::to_string(current / 1024) + " KiB";
            fStatus->SetText(status.c_str());
            break;
        }
        case B_WEBKIT_DOWNLOAD_FINISHED: {
            uint64 identifier = 0;
            uint32 result = B_WEBKIT_DOWNLOAD_FAILED;
            if (message->FindUInt64("identifier", &identifier) != B_OK || !identifier) break;
            fDownloads.erase(identifier);
            message->FindUInt32("result", &result);
            if (result == B_WEBKIT_DOWNLOAD_SUCCEEDED)
                fStatus->SetText("Download complete — open Downloads to view the file");
            else if (result == B_WEBKIT_DOWNLOAD_CANCELLED)
                fStatus->SetText("Download cancelled");
            else {
                const char* description = nullptr;
                message->FindString("error_description", &description);
                std::string status = "Download failed";
                if (description && *description) status += ": " + std::string(description);
                fStatus->SetText(status.c_str());
            }
            break;
        }
        case B_WEBKIT_STATE_CHANGED: case B_WEBKIT_PROCESS_EXITED:
            WebKitStateChanged(*message);
            break;
        case B_WEBKIT_FIND_RESULT:
            WebKitFindResult(*message);
            break;
#endif
        case B_ABOUT_REQUESTED: {
#if SUMMIT_MODERN_WEBKIT
            std::string info = "Summit — a browser for KunanyiOS\nModern WebKit development build\n\nWebKit "
                + std::string(BWebKitVersion()) + "\nHaiku port " + BWebKitPortVersion();
#else
            std::string info = "Summit — a browser for KunanyiOS\nDevelopment build\n\nWebKit "
                + std::string(WebKitInfo::WebKitVersion().String()) + "\nHaiku port "
                + WebKitInfo::HaikuWebKitVersion().String();
#endif
            (new BAlert("About Summit", info.c_str(), "OK"))->Go(nullptr); break;
        }
        case B_CUT: case B_COPY: case B_PASTE: case B_SELECT_ALL: case B_UNDO: case B_REDO:
#if SUMMIT_MODERN_WEBKIT
            if (tab && CurrentFocus() == tab->view) {
                tab->view->ExecuteEditCommand(message->what);
                break;
            }
#endif
            if (CurrentFocus()) PostMessage(message, CurrentFocus());
            break;
        case kSimulateScroll: {
            BMessage reply(B_REPLY);
            SimulateScroll(*message, reply);
            message->SendReply(&reply);
            break;
        }
        case kBrowserState: {
            BMessage reply(B_REPLY);
            reply.AddInt32("count", fTabs.size()); reply.AddInt64("selected", fSelected);
            reply.AddString("address", fAddress->Text());
            reply.AddString("status", fStatus->Text());
#if SUMMIT_MODERN_WEBKIT
            reply.AddString("backend", "modern");
            reply.AddInt32("download_count", fDownloads.size());
            reply.AddUInt64("extension_action_snapshot", fExtensionActionSnapshot);
            reply.AddUInt64("extension_action_result_identifier", fExtensionActionResultIdentifier);
            reply.AddInt32("extension_action_result_error", fExtensionActionResultError);
            if (fExtensionActionSnapshot)
                for (const auto& action : fExtensionActionState) reply.AddMessage("extension_action", &action);
            reply.AddBool("closing", fClosingWindow || fWindowCloseQueued || fCloseCommitPending
                || std::any_of(fTabs.begin(), fTabs.end(), [](const Tab& tab) {
                    return tab.closeRequested || tab.closeQueued || tab.closeApproved;
                }));
            reply.AddBool("engine_version_available", true);
            reply.AddString("webkit", BWebKitVersion());
            reply.AddString("haiku_webkit", BWebKitPortVersion());
            reply.AddString("webkit_revision", BWebKitSourceRevision());
#else
            reply.AddString("backend", "legacy");
            reply.AddString("webkit", WebKitInfo::WebKitVersion());
            reply.AddString("haiku_webkit", WebKitInfo::HaikuWebKitVersion());
#endif
            for (const auto& page : fTabs) {
                BMessage item; item.AddInt64("id", page.id); item.AddString("url", page.url.c_str());
                item.AddString("title", page.title.c_str()); item.AddBool("loading", page.loading);
#if SUMMIT_MODERN_WEBKIT
                item.AddDouble("pageZoom", page.pageZoom);
                item.AddDouble("textZoom", page.textZoom);
                item.AddBool("loadError", !page.loadError.empty());
                item.AddString("loadErrorText", page.loadError.c_str());
                item.AddString("loadErrorDescription", page.loadErrorDescription.c_str());
                item.AddString("loadErrorDomain", page.loadErrorDomain.c_str());
                item.AddString("loadErrorURL", page.loadErrorURL.c_str());
                item.AddInt32("loadErrorCode", page.loadErrorCode);
                item.AddBool("loadErrorProvisional", page.loadErrorProvisional);
                item.AddUInt64("loadGeneration", page.loadGeneration);
                item.AddString("loadOutcome", page.loadOutcome.c_str());
                item.AddUInt64("loadSuccessSequence", page.loadSuccessSequence);
#endif
                reply.AddMessage("tab", &item);
            }
            message->SendReply(&reply); break;
        }
        default: BrowserWindowBase::MessageReceived(message); break;
    }
}
#if SUMMIT_MODERN_WEBKIT
void BrowserWindow::WebKitFindResult(const BMessage& message)
{
    BMessenger sender;
    if (message.FindMessenger("view", &sender) != B_OK) return;
    auto* tab = FindTab(sender);
    const char* query = nullptr;
    if (!tab || tab->id != fSelected || message.FindString("query", &query) != B_OK
        || !query || std::strcmp(query, fFindText->Text())) return;
    int32 error;
    if (message.FindInt32("error", &error) != B_OK) return;
    if (error != B_OK) {
        fStatus->SetText("Could not search this page.");
        return;
    }
    bool found;
    if (message.FindBool("found", &found) == B_OK)
        fStatus->SetText(found ? "Match found" : "No matches");
}

void BrowserWindow::WebKitStateChanged(const BMessage& message)
{
    BMessenger sender;
    if (message.FindMessenger("view", &sender) != B_OK) return;
    auto* tab = FindTab(sender);
    if (!tab) return;
    if (message.what == B_WEBKIT_PROCESS_EXITED) {
        tab->processExited = true;
        tab->loading = false;
        tab->progress = 0;
        int32 error;
        tab->processError = message.FindInt32("error", &error) == B_OK && error != B_OK
            ? "Could not initialize the web page: " + std::string(std::strerror(error))
            : "The page process exited. Reload to try again.";
        if (tab->id == fSelected) fStatus->SetText(tab->processError.c_str());
        RefreshChrome();
        return;
    }
    bool closeInvalidated = false;
    if (message.FindBool("closeApprovalInvalidated", &closeInvalidated) == B_OK && closeInvalidated)
        InvalidateWindowClose();
    const char* value = nullptr;
    uint64 generation;
    // Error fields are a copied snapshot, never a native pointer into WebKit.
    // Older generations may still carry ordinary progress/close state, but
    // cannot restore a previous navigation's load error.
    if (message.FindUInt64("loadGeneration", &generation) == B_OK && generation >= tab->loadGeneration) {
        tab->loadGeneration = generation;
        if (message.FindString("loadOutcome", &value) == B_OK && value) tab->loadOutcome = value;
        bool failed = false;
        if (message.FindBool("loadError", &failed) == B_OK) {
            tab->loadError.clear();
            tab->loadErrorDescription.clear();
            tab->loadErrorDomain.clear();
            tab->loadErrorURL.clear();
            tab->loadErrorCode = 0;
            tab->loadErrorProvisional = false;
            if (failed) {
                if (message.FindString("loadErrorDescription", &value) == B_OK && value) tab->loadErrorDescription = value;
                if (message.FindString("loadErrorDomain", &value) == B_OK && value) tab->loadErrorDomain = value;
                if (message.FindString("loadErrorURL", &value) == B_OK && value) tab->loadErrorURL = value;
                message.FindInt32("loadErrorCode", &tab->loadErrorCode);
                message.FindBool("loadErrorProvisional", &tab->loadErrorProvisional);
                tab->loadError = tab->loadErrorProvisional ? "Could not load " : "Loading was interrupted for ";
                tab->loadError += tab->loadErrorURL.empty() ? "this page" : tab->loadErrorURL;
                tab->loadError += ": " + (tab->loadErrorDescription.empty()
                    ? std::string("The request failed.") : tab->loadErrorDescription);
                tab->loadError += " Enter the address again to retry.";
                // Keep native status text on one line; retain the original
                // diagnostic strings separately in the state probe.
                for (auto& character : tab->loadError)
                    if (static_cast<unsigned char>(character) < 0x20 || character == 0x7f) character = ' ';
            }
        }
    }
    if (message.FindString("url", &value) == B_OK && value && *value) tab->url = StoredURL(value);
    if (message.FindString("title", &value) == B_OK && value)
        tab->title = *value ? value : tab->url == "summit:home" ? "Start Page" : tab->url;
    message.FindBool("loading", &tab->loading);
    message.FindBool("canGoBack", &tab->back);
    message.FindBool("canGoForward", &tab->forward);
    double progress;
    if (message.FindDouble("progress", &progress) == B_OK && std::isfinite(progress))
        tab->progress = static_cast<float>(std::clamp(progress, 0.0, 1.0));
    double zoom;
    if (message.FindDouble("pageZoom", &zoom) == B_OK && std::isfinite(zoom)) tab->pageZoom = zoom;
    if (message.FindDouble("textZoom", &zoom) == B_OK && std::isfinite(zoom)) tab->textZoom = zoom;
    if (tab->loading) {
        tab->processExited = false;
        tab->processError.clear();
    }
    uint64 successSequence;
    if (message.FindUInt64("loadSuccessSequence", &successSequence) == B_OK
        && successSequence > tab->loadSuccessSequence) {
        tab->loadSuccessSequence = successSequence;
        const char* successURL = nullptr;
        const char* successTitle = nullptr;
        if (message.FindString("loadSuccessURL", &successURL) == B_OK && successURL && *successURL
            && message.FindString("loadSuccessTitle", &successTitle) == B_OK && successTitle)
            fProfile.Visit({StoredURL(successURL), successTitle});
    }
    const char* successfulURL = nullptr;
    if (tab->loadOutcome == "succeeded"
        && message.FindString("loadSuccessURL", &successfulURL) == B_OK && successfulURL
        && StoredURL(successfulURL) == tab->url) {
        for (auto& page : fProfile.history) if (page.url == tab->url) page.title = tab->title;
    }
    if (fSidebarHistory) RefreshSidebar(true);
    if (tab->id == fSelected) {
        if (!fAddress->TextView()->IsFocus()) fAddress->SetText(tab->url == "summit:home" ? "" : tab->url.c_str());
        ShowTabStatus(*tab);
    }
    RefreshChrome();
}

void BrowserWindow::ShowTabStatus(const Tab& tab)
{
    const char* text = tab.processExited ? tab.processError.c_str()
        : !tab.loadError.empty() ? tab.loadError.c_str()
        : tab.loading ? "Loading…" : "Ready";
    fStatus->SetText(text);
    fStatus->SetToolTip(tab.loadError.empty() && !tab.processExited ? nullptr : text);
}
#else
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
#endif
}
