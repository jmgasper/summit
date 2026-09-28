#pragma once
#include <Messenger.h>
#include <Window.h>
#include <filesystem>
#include <string>

class BCheckBox;
class BMenuField;
class BRadioButton;
class BStringView;
class BTextControl;

namespace summit {
// What the Preferences window starts with.
struct PreferencesState {
    std::string homeURL;
    bool showBookmarksBar = true;
    std::string interfaceStyle;
    std::string searchEngine;
    size_t historyCount = 0;
    // The profile's WebKit folder, whose Cache folder's size is shown.
    std::filesystem::path webKitDirectory;
};
// Edit › Preferences…. One window for the application; changes apply to every
// browser window as they are made and are saved in the profile.
class PreferencesWindow final : public BWindow {
public:
    PreferencesWindow(BMessenger owner, const PreferencesState& state);
    void MessageReceived(BMessage*) override;
    bool QuitRequested() override;
private:
    void ShowHomeHint();
    void ShowHistoryCount(size_t count);
    // Measures the cache on another thread; kCacheMeasured brings the result.
    void MeasureCache();
    BMessenger fOwner;
    BTextControl* fHome;
    BStringView* fHint;
    BMenuField* fSearchEngine;
    BCheckBox* fBookmarksBar;
    BRadioButton* fHaikuStyle;
    BRadioButton* fSafariStyle;
    BStringView* fHistoryInfo;
    BStringView* fCacheInfo;
    BStringView* fDataStatus;
    std::filesystem::path fWebKitDirectory;
};
}
