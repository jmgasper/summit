#include "SharedProfile.h"
#include "Messages.h"
#include <Message.h>
#include <algorithm>

namespace summit {
static bool SameSession(const WindowSession& a, const WindowSession& b)
{
    if (a.selected != b.selected || a.tabs.size() != b.tabs.size()
        || !std::equal(std::begin(a.frame), std::end(a.frame), std::begin(b.frame))) return false;
    for (size_t i = 0; i < a.tabs.size(); ++i)
        if (a.tabs[i].url != b.tabs[i].url || a.tabs[i].title != b.tabs[i].title) return false;
    return true;
}

SharedProfile::SharedProfile(std::filesystem::path path) : fPath(std::move(path))
{
    fProfile = Profile::Load(fPath, fLoadError);
    // An unreadable profile is preserved: nothing is written over it.
    fWritable = fLoadError.empty();
    fSavedWindows = std::move(fProfile.windows);
    fProfile.windows.clear();
}

std::vector<WindowSession> SharedProfile::SavedWindows() const
{
    std::lock_guard lock(fMutex);
    return fSavedWindows;
}

void SharedProfile::AddListener(const BMessenger& listener)
{
    std::lock_guard lock(fMutex);
    fListeners.push_back(listener);
}

void SharedProfile::RemoveListener(const BMessenger& listener)
{
    std::lock_guard lock(fMutex);
    std::erase(fListeners, listener);
}

void SharedProfile::Change(const std::function<uint32(Profile&)>& function, const BMessenger& sender, bool saveNow)
{
    uint32 changes;
    {
        std::lock_guard lock(fMutex);
        changes = function(fProfile);
        if (changes) ++fRevision;
    }
    if (!changes) return;
    std::string error;
    if (saveNow) Save(error);
    Announce(changes, sender);
}

void SharedProfile::IconChanged(const std::string& pageURL, const BMessenger& sender)
{
    Announce(kIconChanged, sender, pageURL);
}

void SharedProfile::Announce(uint32 changes, const BMessenger& sender, const std::string& iconURL)
{
    std::vector<BMessenger> listeners;
    {
        std::lock_guard lock(fMutex);
        std::erase_if(fListeners, [](const BMessenger& listener) { return !listener.IsValid(); });
        listeners = fListeners;
    }
    BMessage message(kProfileChanged);
    message.AddUInt32("changes", changes);
    message.AddMessenger("sender", sender);
    if (!iconURL.empty()) message.AddString("icon_url", iconURL.c_str());
    for (const auto& listener : listeners) listener.SendMessage(&message, static_cast<BHandler*>(nullptr), 0);
}

void SharedProfile::SetWindowSession(uint64 key, const WindowSession& session)
{
    std::lock_guard lock(fMutex);
    auto found = fSessions.find(key);
    if (found != fSessions.end() && SameSession(found->second, session)) return;
    fSessions[key] = session;
    ++fRevision;
}

void SharedProfile::RemoveWindowSession(uint64 key)
{
    std::lock_guard lock(fMutex);
    if (fSessions.erase(key)) ++fRevision;
}

bool SharedProfile::Save(std::string& error)
{
    error.clear();
    std::lock_guard saving(fSaveMutex);
    Profile snapshot;
    uint64 revision;
    {
        std::lock_guard lock(fMutex);
        if (!fWritable || fRevision == fSavedRevision) return true;
        snapshot = fProfile;
        for (const auto& [key, session] : fSessions)
            if (!session.tabs.empty()) snapshot.windows.push_back(session);
        revision = fRevision;
    }
    if (!snapshot.Save(fPath, error)) return false;
    std::lock_guard lock(fMutex);
    fSavedRevision = std::max(fSavedRevision, revision);
    return true;
}
}
