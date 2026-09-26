#pragma once
#include <Messenger.h>
#include <Window.h>
#include <string>

class BCheckBox;
class BRadioButton;
class BStringView;
class BTextControl;

namespace summit {
// Edit › Preferences…. One window for the application; changes apply to every
// browser window as they are made and are saved in the profile.
class PreferencesWindow final : public BWindow {
public:
    PreferencesWindow(BMessenger owner, const std::string& homeURL, bool showBookmarksBar, const std::string& interfaceStyle);
    void MessageReceived(BMessage*) override;
    bool QuitRequested() override;
private:
    void ShowHomeHint();
    BMessenger fOwner;
    BTextControl* fHome;
    BStringView* fHint;
    BCheckBox* fBookmarksBar;
    BRadioButton* fHaikuStyle;
    BRadioButton* fSafariStyle;
};
}
