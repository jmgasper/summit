#include "PreferencesWindow.h"
#include "Messages.h"
#include "core/Address.h"
#include <Button.h>
#include <CheckBox.h>
#include <LayoutBuilder.h>
#include <StringView.h>
#include <TextControl.h>

namespace summit {
namespace {
constexpr uint32 homeEdited = 'phed', useStartPage = 'phsp', bookmarksBarToggled = 'pbar';
}
PreferencesWindow::PreferencesWindow(BMessenger owner, const std::string& homeURL, bool showBookmarksBar)
    : BWindow(BRect(180, 150, 700, 400), "Preferences — Summit", B_TITLED_WINDOW,
        B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE)
    , fOwner(owner)
{
    auto* general = new BStringView("general-title", "General");
    general->SetFont(be_bold_font);
    fHome = new BTextControl("home-url", "Home page:", homeURL.c_str(), nullptr);
    fHome->SetModificationMessage(new BMessage(homeEdited));
    fHome->SetExplicitMinSize(BSize(380, B_SIZE_UNSET));
    fHint = new BStringView("home-hint", "");
    fHint->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_2_TINT);
    auto* bookmarks = new BStringView("bookmarks-title", "Bookmarks");
    bookmarks->SetFont(be_bold_font);
    fBookmarksBar = new BCheckBox("show-bookmarks-bar", "Show the bookmarks bar under the toolbar",
        new BMessage(bookmarksBarToggled));
    fBookmarksBar->SetValue(showBookmarksBar ? B_CONTROL_ON : B_CONTROL_OFF);
    BLayoutBuilder::Group<>(this, B_VERTICAL, 10)
        .SetInsets(18)
        .Add(general)
        .AddGroup(B_VERTICAL, 6).SetInsets(12, 0, 0, 0)
            .Add(fHome)
            .AddGroup(B_HORIZONTAL, 8)
                .AddGlue()
                .Add(new BButton("use-current", "Use Current Page", new BMessage(kPreferencesUseCurrentPage)))
                .Add(new BButton("use-start", "Use Start Page", new BMessage(useStartPage)))
            .End()
            .Add(fHint)
        .End()
        .AddStrut(6)
        .Add(bookmarks)
        .AddGroup(B_VERTICAL, 6).SetInsets(12, 0, 0, 0)
            .Add(fBookmarksBar)
        .End();
    ShowHomeHint();
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
