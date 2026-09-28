#include "PreferencesWindow.h"
#include "Messages.h"
#include "core/Address.h"
#include <Alert.h>
#include <Button.h>
#include <CheckBox.h>
#include <Invoker.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <RadioButton.h>
#include <StringView.h>
#include <TextControl.h>
#include <cstdio>
#include <thread>

namespace summit {
namespace {
constexpr uint32 homeEdited = 'phed', useStartPage = 'phsp', bookmarksBarToggled = 'pbar', styleChosen = 'psty',
    searchChosen = 'psrc', clearHistory = 'pchi', clearHistoryConfirmed = 'pchc', clearCache = 'pcca',
    clearSiteData = 'pcsd', clearSiteDataConfirmed = 'pcsc', cacheMeasured = 'pcms';

std::string SizeLabel(uintmax_t bytes)
{
    char text[32];
    if (bytes < 1024) std::snprintf(text, sizeof(text), "%ju bytes", bytes);
    else if (bytes < 1024 * 1024) std::snprintf(text, sizeof(text), "%.0f KiB", bytes / 1024.0);
    else if (bytes < 1024ull * 1024 * 1024) std::snprintf(text, sizeof(text), "%.1f MiB", bytes / 1048576.0);
    else std::snprintf(text, sizeof(text), "%.2f GiB", bytes / 1073741824.0);
    return text;
}

BStringView* SectionTitle(const char* name, const char* text)
{
    auto* title = new BStringView(name, text);
    title->SetFont(be_bold_font);
    return title;
}
}

PreferencesWindow::PreferencesWindow(BMessenger owner, const PreferencesState& state)
    : BWindow(BRect(180, 150, 700, 400), "Preferences — Summit", B_TITLED_WINDOW,
        B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE)
    , fOwner(owner)
    , fWebKitDirectory(state.webKitDirectory)
{
    fHome = new BTextControl("home-url", "Home page:", state.homeURL.c_str(), nullptr);
    fHome->SetModificationMessage(new BMessage(homeEdited));
    fHome->SetExplicitMinSize(BSize(380, B_SIZE_UNSET));
    fHint = new BStringView("home-hint", "");
    fHint->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_2_TINT);
    auto* engines = new BPopUpMenu("search-engines");
    for (const auto& engine : SearchEngines()) {
        auto* message = new BMessage(searchChosen);
        message->AddString("search_engine", engine.id);
        auto* item = new BMenuItem(engine.name, message);
        item->SetMarked(state.searchEngine == engine.id);
        engines->AddItem(item);
    }
    fSearchEngine = new BMenuField("search-engine", "Search engine:", engines);
    auto* searchHint = new BStringView("search-hint", "Used for text typed in the address field that is not an address.");
    searchHint->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_2_TINT);
    fBookmarksBar = new BCheckBox("show-bookmarks-bar", "Show the bookmarks bar under the toolbar",
        new BMessage(bookmarksBarToggled));
    fBookmarksBar->SetValue(state.showBookmarksBar ? B_CONTROL_ON : B_CONTROL_OFF);
    auto styleMessage = [](const char* style) {
        auto* message = new BMessage(styleChosen);
        message->AddString("interface_style", style);
        return message;
    };
    fHaikuStyle = new BRadioButton("style-haiku", "Haiku: buttons and tabs like other Haiku applications", styleMessage("haiku"));
    fSafariStyle = new BRadioButton("style-safari", "Safari-like: a flat toolbar with the tabs underneath", styleMessage("safari"));
    (state.interfaceStyle == "safari" ? fSafariStyle : fHaikuStyle)->SetValue(B_CONTROL_ON);
    fHistoryInfo = new BStringView("history-info", "");
    fCacheInfo = new BStringView("cache-info", "Cache: measuring…");
    auto* dataInfo = new BStringView("site-data-info", "Cookies and site data");
    auto* dataHint = new BStringView("site-data-hint", "Clearing them signs you out of websites. Extensions keep their own data.");
    dataHint->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_2_TINT);
    fDataStatus = new BStringView("data-status", "");
    fDataStatus->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_2_TINT);
    ShowHistoryCount(state.historyCount);
    BLayoutBuilder::Group<>(this, B_VERTICAL, 10)
        .SetInsets(18)
        .Add(SectionTitle("general-title", "General"))
        .AddGroup(B_VERTICAL, 6).SetInsets(12, 0, 0, 0)
            .Add(fHome)
            .AddGroup(B_HORIZONTAL, 8)
                .AddGlue()
                .Add(new BButton("use-current", "Use Current Page", new BMessage(kPreferencesUseCurrentPage)))
                .Add(new BButton("use-start", "Use Start Page", new BMessage(useStartPage)))
            .End()
            .Add(fHint)
            .AddStrut(4)
            .AddGroup(B_HORIZONTAL, 0)
                .Add(fSearchEngine)
                .AddGlue()
            .End()
            .Add(searchHint)
        .End()
        .AddStrut(6)
        .Add(SectionTitle("bookmarks-title", "Bookmarks"))
        .AddGroup(B_VERTICAL, 6).SetInsets(12, 0, 0, 0)
            .Add(fBookmarksBar)
        .End()
        .AddStrut(6)
        .Add(SectionTitle("appearance-title", "Appearance"))
        .AddGroup(B_VERTICAL, 4).SetInsets(12, 0, 0, 0)
            .Add(fHaikuStyle)
            .Add(fSafariStyle)
        .End()
        .AddStrut(6)
        .Add(SectionTitle("data-title", "History and Data"))
        .AddGrid(10, 6).SetInsets(12, 0, 0, 0)
            .Add(fHistoryInfo, 0, 0)
            .Add(new BButton("clear-history", "Clear History…", new BMessage(clearHistory)), 1, 0)
            .Add(fCacheInfo, 0, 1)
            .Add(new BButton("clear-cache", "Clear Cache", new BMessage(clearCache)), 1, 1)
            .Add(dataInfo, 0, 2)
            .Add(new BButton("clear-site-data", "Clear Cookies and Site Data…", new BMessage(clearSiteData)), 1, 2)
            .Add(dataHint, 0, 3, 2, 1)
            .Add(fDataStatus, 0, 4, 2, 1)
        .End();
    ShowHomeHint();
    MeasureCache();
}

void PreferencesWindow::ShowHomeHint()
{
    const auto text = Trim(fHome->Text());
    if (text.empty()) {
        fHint->SetText("The Home button opens the Summit start page.");
        return;
    }
    const auto address = ResolveAddress(text);
    if (!address.error.empty()) fHint->SetText(address.error.c_str());
    else if (address.search) fHint->SetText("This is not an address, so Home will search for it.");
    else fHint->SetText(("The Home button opens " + address.url).c_str());
}

void PreferencesWindow::ShowHistoryCount(size_t count)
{
    const std::string text = count == 0 ? "History is empty"
        : "History: " + std::to_string(count) + (count == 1 ? " page" : " pages");
    fHistoryInfo->SetText(text.c_str());
}

void PreferencesWindow::MeasureCache()
{
    // Walking a few thousand cache files takes a moment; the window stays usable.
    std::thread([directory = fWebKitDirectory / "Cache", window = BMessenger(this)] {
        uintmax_t total = 0;
        std::error_code error;
        for (auto entry = std::filesystem::recursive_directory_iterator(directory,
                 std::filesystem::directory_options::skip_permission_denied, error);
            !error && entry != std::filesystem::recursive_directory_iterator(); entry.increment(error)) {
            std::error_code ignored;
            if (entry->is_regular_file(ignored)) total += entry->file_size(ignored);
        }
        BMessage measured(cacheMeasured);
        measured.AddUInt64("bytes", total);
        window.SendMessage(&measured);
    }).detach();
}

void PreferencesWindow::MessageReceived(BMessage* message)
{
    switch (message->what) {
        case homeEdited: {
            ShowHomeHint();
            const auto text = Trim(fHome->Text());
            if (!text.empty() && !ResolveAddress(text).error.empty()) break;
            BMessage changed(kPreferencesChanged);
            changed.AddString("home_url", text.c_str());
            fOwner.SendMessage(&changed);
            break;
        }
        case useStartPage:
            fHome->SetText("");
            PostMessage(homeEdited);
            break;
        case bookmarksBarToggled: {
            BMessage changed(kPreferencesChanged);
            changed.AddBool("show_bookmarks_bar", fBookmarksBar->Value() == B_CONTROL_ON);
            fOwner.SendMessage(&changed);
            break;
        }
        case styleChosen: {
            BMessage changed(kPreferencesChanged);
            changed.AddString("interface_style", message->GetString("interface_style", "haiku"));
            fOwner.SendMessage(&changed);
            break;
        }
        case searchChosen: {
            BMessage changed(kPreferencesChanged);
            changed.AddString("search_engine", message->GetString("search_engine", "duckduckgo"));
            fOwner.SendMessage(&changed);
            // The home page hint may now search elsewhere.
            ShowHomeHint();
            break;
        }
        case clearHistory: {
            auto* alert = new BAlert("Clear History", "Clear your history?\n\nEvery page you have visited is forgotten, "
                "with the icons of sites you have not bookmarked. Bookmarks and open tabs are kept.",
                "Cancel", "Clear History", nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
            alert->SetShortcut(0, B_ESCAPE);
            alert->Go(new BInvoker(new BMessage(clearHistoryConfirmed), this));
            break;
        }
        case clearHistoryConfirmed:
            if (message->GetInt32("which", 0) != 1) break;
            fDataStatus->SetText("Clearing history…");
            fOwner.SendMessage(kClearHistoryRequest);
            break;
        case clearCache:
            fDataStatus->SetText("Clearing the cache…");
            fOwner.SendMessage(kClearCacheRequest);
            break;
        case clearSiteData: {
            auto* alert = new BAlert("Clear Cookies and Site Data", "Clear cookies and site data?\n\nYou will be signed "
                "out of websites, and the data they keep on this computer is deleted. Extensions keep their own data.",
                "Cancel", "Clear", nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
            alert->SetShortcut(0, B_ESCAPE);
            alert->Go(new BInvoker(new BMessage(clearSiteDataConfirmed), this));
            break;
        }
        case clearSiteDataConfirmed:
            if (message->GetInt32("which", 0) != 1) break;
            fDataStatus->SetText("Clearing cookies and site data…");
            fOwner.SendMessage(kClearSiteDataRequest);
            break;
        case kDataCleared: {
            const char* error = nullptr;
            const uint32 kind = message->GetInt32("kind", 0);
            if (message->FindString("error", &error) == B_OK && error)
                fDataStatus->SetText((std::string("Could not clear: ") + error).c_str());
            else fDataStatus->SetText(kind == kClearHistoryRequest ? "History cleared."
                : kind == kClearCacheRequest ? "Cache cleared." : "Cookies and site data cleared.");
            if (kind == kClearCacheRequest) MeasureCache();
            break;
        }
        case cacheMeasured:
            fCacheInfo->SetText(("Cache: " + SizeLabel(message->GetUInt64("bytes", 0))).c_str());
            break;
        case kPreferencesUseCurrentPage:
            fOwner.SendMessage(kPreferencesUseCurrentPage);
            break;
        case kPreferencesState: {
            // The browser changed a setting (the current page, or View › Bookmarks Bar).
            const char* home = nullptr;
            if (message->FindString("home_url", &home) == B_OK && home && Trim(fHome->Text()) != home) {
                fHome->SetText(home);
                ShowHomeHint();
            }
            bool bar = false;
            if (message->FindBool("show_bookmarks_bar", &bar) == B_OK)
                fBookmarksBar->SetValue(bar ? B_CONTROL_ON : B_CONTROL_OFF);
            const char* style = nullptr;
            if (message->FindString("interface_style", &style) == B_OK && style)
                (std::string(style) == "safari" ? fSafariStyle : fHaikuStyle)->SetValue(B_CONTROL_ON);
            const char* engine = nullptr;
            if (message->FindString("search_engine", &engine) == B_OK && engine)
                for (int32 i = 0; auto* item = fSearchEngine->Menu()->ItemAt(i); ++i)
                    item->SetMarked(item->Message() && std::string(item->Message()->GetString("search_engine", "")) == engine);
            int32 count = 0;
            if (message->FindInt32("history_count", &count) == B_OK) ShowHistoryCount(count);
            break;
        }
        case kShowPreferences: Activate(); break;
        default: BWindow::MessageReceived(message); break;
    }
}

bool PreferencesWindow::QuitRequested()
{
    BMessage closed(kPreferencesClosed);
    closed.AddMessenger("window", BMessenger(this));
    fOwner.SendMessage(&closed);
    return true;
}
}
