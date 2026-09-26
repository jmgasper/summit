#pragma once
#include "core/Profile.h"
#include <Messenger.h>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace summit {
// The profile all browser windows share: bookmarks, history, settings and the
// saved session. Every window runs on its own thread, so all access goes
// through this object, and changes are announced to the windows with a
// kProfileChanged message ("changes": the bits below, "sender": the window
// that made the change, "icon_url" for kIconChanged).
class SharedProfile {
public:
    enum : uint32 {
        kHistoryChanged = 1 << 0,
        kHistoryCleared = 1 << 1,
        kBookmarksChanged = 1 << 2,
        kSettingsChanged = 1 << 3,
        kIconChanged = 1 << 4,
    };
    explicit SharedProfile(std::filesystem::path path);
    const std::filesystem::path& Path() const { return fPath; }
    // Why the saved profile could not be read (it is then left untouched).
    const std::string& LoadError() const { return fLoadError; }
    // The windows saved when Summit last quit.
    std::vector<WindowSession> SavedWindows() const;

    void AddListener(const BMessenger&);
    void RemoveListener(const BMessenger&);

    // Runs function(const Profile&) under the lock and returns its result.
    template<typename Function> auto Read(Function&& function) const
    {
        std::lock_guard lock(fMutex);
        return function(static_cast<const Profile&>(fProfile));
    }
    // Runs function(Profile&) under the lock. It returns the change bits (0
    // when nothing changed); a change is announced to every window and saved,
    // at once or (saveNow false, for visits) with the next session save.
    void Change(const std::function<uint32(Profile&)>& function, const BMessenger& sender = BMessenger(),
        bool saveNow = true);
    void IconChanged(const std::string& pageURL, const BMessenger& sender);

    // Each window publishes its tabs under its own key; windows are saved in
    // key order. A window closed while Summit keeps running is removed.
    void SetWindowSession(uint64 key, const WindowSession& session);
    void RemoveWindowSession(uint64 key);
    // The page a new tab opens when an enabled extension overrides it
    // (chrome_url_overrides.newtab); empty for the home page. Not saved.
    void SetNewTabOverride(std::string url)
    {
        std::lock_guard lock(fMutex);
        fNewTabOverride = std::move(url);
    }
    std::string NewTabOverride() const
    {
        std::lock_guard lock(fMutex);
        return fNewTabOverride;
    }
    // Writes the profile if anything changed since the last save.
    bool Save(std::string& error);

private:
    void Announce(uint32 changes, const BMessenger& sender, const std::string& iconURL = std::string());
    mutable std::mutex fMutex;
    std::mutex fSaveMutex;
    const std::filesystem::path fPath;
    Profile fProfile;
    std::vector<WindowSession> fSavedWindows;
    std::string fLoadError;
    std::string fNewTabOverride;
    bool fWritable = true;
    std::map<uint64, WindowSession> fSessions;
    std::vector<BMessenger> fListeners;
    uint64 fRevision = 1, fSavedRevision = 0;
};
}
