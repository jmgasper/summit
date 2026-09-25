#pragma once
#include <Messenger.h>
#include <Window.h>
#include <string>

class BCheckBox;
class BStringView;
class BTextControl;

namespace summit {
// Edit › Preferences…. Changes apply as they are made; the browser window
// owns the settings and saves them in the profile.
class PreferencesWindow final : public BWindow {
public:
    PreferencesWindow(BMessenger owner, const std::string& homeURL, bool showBookmarksBar);
    void MessageReceived(BMessage*) override;
    bool QuitRequested() override;
private:
    void ShowHomeHint();
    BMessenger fOwner;
    BTextControl* fHome;
    BStringView* fHint;
    BCheckBox* fBookmarksBar;
};
}
