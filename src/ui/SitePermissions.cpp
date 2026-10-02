#include "SitePermissions.h"
#include "FaviconCache.h"
#include "Location.h"
#include "Messages.h"
#include <WebKit/WebKitContext.h>
#include <Alert.h>
#include <AppFileInfo.h>
#include <Application.h>
#include <Bitmap.h>
#include <Invoker.h>
#include <MessageRunner.h>
#include <Notification.h>
#include <Roster.h>
#include <Window.h>
#include <cstdio>
#include <cstring>

namespace summit {
namespace {
constexpr uint32 kPromptAnswered = 'sppa';
constexpr uint32 kLocationFound = 'splf';
constexpr uint32 kLocationRefresh = 'splr';
// How long a position is reused, and how often watching pages get a new one.
constexpr bigtime_t kPositionLifetime = 60 * 1000000LL;
constexpr bigtime_t kRefreshInterval = 5 * 60 * 1000000LL;

std::string HostOf(const std::string& origin)
{
    const size_t start = origin.find("://");
    return start == std::string::npos ? origin : origin.substr(start + 3);
}

const char* Question(const std::string& permission)
{
    return permission == "notifications" ? "wants to show you notifications."
        : permission == "geolocation" ? "wants to know your location." : "wants a permission.";
}
}

SitePermissionService::SitePermissionService(std::shared_ptr<BWebKitContext> context, bool privateBrowsing,
    std::filesystem::path favicons, DecisionSaver save)
    : BHandler(privateBrowsing ? "private site permissions" : "site permissions")
    , fContext(context)
    , fPrivate(privateBrowsing)
    , fFavicons(std::make_unique<FaviconCache>(std::move(favicons), false))
    , fSave(std::move(save))
{
}

SitePermissionService::~SitePermissionService() = default;

void SitePermissionService::MessageReceived(BMessage* message)
{
    switch (message->what) {
    case B_WEBKIT_PERMISSION_REQUESTED: Ask(*message); break;
    case kPromptAnswered: Answered(*message); break;
    case B_WEBKIT_NOTIFICATION_SHOW: ShowNotification(*message); break;
    case B_WEBKIT_NOTIFICATION_CLOSE:
        // Haiku can not take a notification back; it times out by itself.
        fNotifications.erase(message->GetUInt64("identifier", 0));
        break;
    case B_WEBKIT_GEOLOCATION_START: StartLocation(message->GetBool("high_accuracy", false)); break;
    case B_WEBKIT_GEOLOCATION_STOP: StopLocation(); break;
    case kLocationFound: LocationResult(*message); break;
    case kLocationRefresh:
        if (fWatching && !fLookingUp) {
            fLookingUp = true;
            LookUpLocation(BMessenger(this), kLocationFound);
        }
        break;
    default: BHandler::MessageReceived(message); break;
    }
}

// The question goes to the front of the asking page's window, and does not
// stop the user from doing something else first: it floats, it is not modal.
void SitePermissionService::Ask(const BMessage& request)
{
    const uint64 identifier = request.GetUInt64("identifier", 0);
    const std::string permission = request.GetString("permission", "");
    const std::string origin = request.GetString("origin", "");
    auto context = fContext.lock();
    if (!context || !identifier) return;
    if (permission.empty() || origin.empty()) {
        context->RespondToPermissionRequest(identifier, false);
        return;
    }
    auto& waiting = fPrompts[{permission, origin}];
    waiting.push_back(identifier);
    if (waiting.size() > 1) return;
    std::string text = HostOf(origin) + " " + Question(permission);
    if (permission == "geolocation")
        text += "\n\nSummit asks BeaconDB (beacondb.net) where you are, from the Wi-Fi networks nearby and your "
            "network address.";
    if (fPrivate) text += "\n\nIn a private window, the answer is forgotten when the window closes.";
    auto* alert = new BAlert("Summit", text.c_str(), "Block", "Not Now", "Allow", B_WIDTH_FROM_LABEL, B_IDEA_ALERT);
    alert->SetShortcut(1, B_ESCAPE);
    alert->SetFeel(B_FLOATING_APP_WINDOW_FEEL);
    BMessenger view;
    if (request.FindMessenger("view", &view) == B_OK) {
        BLooper* looper = nullptr;
        view.Target(&looper);
        if (auto* window = dynamic_cast<BWindow*>(looper); window && window->LockWithTimeout(100000) == B_OK) {
            const BRect frame = window->Frame();
            window->Unlock();
            alert->Lock();
            const BRect size = alert->Frame();
            alert->MoveTo(frame.left + (frame.Width() - size.Width()) / 2, frame.top + 90);
            alert->Unlock();
        }
    }
    auto* answer = new BMessage(kPromptAnswered);
    answer->AddString("permission", permission.c_str());
    answer->AddString("origin", origin.c_str());
    alert->Go(new BInvoker(answer, this));
}

void SitePermissionService::Answered(const BMessage& message)
{
    const std::string permission = message.GetString("permission", ""), origin = message.GetString("origin", "");
    const int32 which = message.GetInt32("which", 1);
    auto found = fPrompts.find({permission, origin});
    if (found == fPrompts.end()) return;
    const auto identifiers = std::move(found->second);
    fPrompts.erase(found);
    const bool allowed = which == 2;
    // "Not Now" answers this request only; Block and Allow are remembered.
    if (which != 1) fSave(permission, origin, allowed);
    if (auto context = fContext.lock()) {
        if (which != 1 && fPrivate) context->SetSitePermission(permission.c_str(), origin.c_str(), allowed ? 1 : 0);
        for (const uint64 identifier : identifiers)
            context->RespondToPermissionRequest(identifier, allowed);
    }
}

void SitePermissionService::ShowNotification(const BMessage& message)
{
    const uint64 identifier = message.GetUInt64("identifier", 0);
    const std::string origin = message.GetString("origin", ""), tag = message.GetString("tag", "");
    std::string title = message.GetString("title", "");
    std::string body = message.GetString("body", "");
    if (!identifier) return;
    if (title.size() > 200) title.resize(200);
    if (body.size() > 1000) body.resize(1000);
    BNotification notification(B_INFORMATION_NOTIFICATION);
    notification.SetGroup("Summit");
    notification.SetTitle(title.empty() ? HostOf(origin).c_str() : title.c_str());
    notification.SetContent((body.empty() ? HostOf(origin) : body + "\n" + HostOf(origin)).c_str());
    // A notification with the tag of an earlier one replaces it.
    notification.SetMessageID((tag.empty() ? "summit:" + std::to_string(identifier) : "summit:" + origin + "#" + tag).c_str());
    if (const BBitmap* icon = fFavicons->Icon(origin + "/")) notification.SetIcon(icon);
    // A click starts this executable with the identifier; as Summit is
    // running, the system hands the arguments to it (ArgvReceived).
    app_info info;
    if (be_app->GetAppInfo(&info) == B_OK) {
        notification.SetOnClickFile(&info.ref);
        notification.AddOnClickArg(kNotificationClickArgument);
        notification.AddOnClickArg(fPrivate ? "private" : "normal");
        notification.AddOnClickArg(std::to_string(identifier).c_str());
    }
    BMessenger view;
    message.FindMessenger("view", &view);
    fNotifications[identifier] = {view};
    if (fNotifications.size() > 200) fNotifications.erase(fNotifications.begin());
    notification.Send();
}

void SitePermissionService::NotificationClicked(uint64 identifier)
{
    auto found = fNotifications.find(identifier);
    if (found == fNotifications.end()) return;
    // The page's tab comes forward, then the page hears the click (its
    // handler usually focuses itself or navigates).
    if (found->second.view.IsValid()) {
        BMessage show(kShowTabOfView);
        show.AddMessenger("view", found->second.view);
        BLooper* looper = nullptr;
        found->second.view.Target(&looper);
        if (looper) BMessenger(looper).SendMessage(&show);
    }
    if (auto context = fContext.lock()) context->NotificationClicked(identifier);
}

void SitePermissionService::StartLocation(bool)
{
    fWatching = true;
    auto context = fContext.lock();
    if (fLast && system_time() - fLast->at < kPositionLifetime && context)
        context->SetGeolocationPosition(fLast->latitude, fLast->longitude, fLast->accuracy, fLast->timestamp);
    else if (!fLookingUp) {
        fLookingUp = true;
        LookUpLocation(BMessenger(this), kLocationFound);
    }
    if (!fRefresh) {
        BMessage refresh(kLocationRefresh);
        fRefresh = std::make_unique<BMessageRunner>(BMessenger(this), &refresh, kRefreshInterval);
    }
}

void SitePermissionService::StopLocation()
{
    fWatching = false;
    fRefresh.reset();
}

void SitePermissionService::LocationResult(const BMessage& message)
{
    fLookingUp = false;
    auto context = fContext.lock();
    if (!context) return;
    const char* error = nullptr;
    if (message.FindString("error", &error) == B_OK) {
        std::fprintf(stderr, "Summit location: %s\n", error);
        if (fWatching) context->GeolocationUnavailable(error);
        return;
    }
    Position position;
    position.latitude = message.GetDouble("latitude", 0);
    position.longitude = message.GetDouble("longitude", 0);
    position.accuracy = message.GetDouble("accuracy", 25000);
    position.timestamp = message.GetDouble("timestamp", 0);
    position.at = system_time();
    fLast = position;
    if (fWatching) context->SetGeolocationPosition(position.latitude, position.longitude, position.accuracy, position.timestamp);
}
}
