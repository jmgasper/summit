#include "DefaultBrowser.h"
#include <AppFileInfo.h>
#include <Application.h>
#include <Entry.h>
#include <File.h>
#include <Message.h>
#include <Mime.h>
#include <Path.h>
#include <Roster.h>
#include <strings.h>
#include <cstdlib>

namespace summit {
namespace {
// Links first: they decide what `open https://…` starts.
constexpr const char* kWebTypes[] = {
    "application/x-vnd.Be.URL.http",
    "application/x-vnd.Be.URL.https",
    "text/html",
    "application/xhtml+xml",
};

bool IsSummit(const char* signature)
{
    return signature && !strcasecmp(signature, kSummitSignature);
}

std::string PreferredApp(const char* type)
{
    BMimeType mime(type);
    char signature[B_MIME_TYPE_LENGTH] = "";
    if (mime.InitCheck() != B_OK || mime.GetPreferredApp(signature, B_OPEN) != B_OK) return { };
    return signature;
}

// The program the system should start for Summit. The launcher sets up the
// engine's environment (its GPU libraries and switches); a bare bundle's
// executable starts without them.
status_t LaunchTarget(entry_ref& ref)
{
    if (const char* launcher = std::getenv("SUMMIT_SYSTEM_LAUNCHER"); launcher && *launcher) {
        if (BEntry entry(launcher, true); entry.IsFile()) return entry.GetRef(&ref);
    }
    if (BEntry entry("/boot/system/apps/Summit", true); entry.IsFile()) return entry.GetRef(&ref);
    app_info info;
    if (!be_app) return B_NO_INIT;
    const status_t status = be_app->GetAppInfo(&info);
    if (status == B_OK) ref = info.ref;
    return status;
}
}

DefaultBrowserState QueryDefaultBrowser()
{
    DefaultBrowserState state;
    const auto http = PreferredApp(kWebTypes[0]);
    const auto https = PreferredApp(kWebTypes[1]);
    state.isDefault = IsSummit(http.c_str()) && IsSummit(https.c_str());
    const auto& current = !http.empty() ? http : https;
    if (current.empty()) return state;
    entry_ref ref;
    if (be_roster->FindApp(current.c_str(), &ref) == B_OK && ref.name) state.current = ref.name;
    else state.current = current;
    return state;
}

status_t MakeDefaultBrowser(std::string& error)
{
    BMimeType app(kSummitSignature);
    status_t status = app.InitCheck();
    if (status == B_OK && !app.IsInstalled()) status = app.Install();
    if (status != B_OK) {
        error = "Summit's signature is not known to the system.";
        return status;
    }
    entry_ref target;
    if ((status = LaunchTarget(target)) != B_OK || (status = app.SetAppHint(&target)) != B_OK) {
        error = "The system could not be told where Summit is.";
        return status;
    }
    for (const char* type : kWebTypes) {
        BMimeType mime(type);
        if ((status = mime.InitCheck()) == B_OK && !mime.IsInstalled()) status = mime.Install();
        if (status == B_OK) status = mime.SetPreferredApp(kSummitSignature, B_OPEN);
        if (status != B_OK) {
            error = std::string("Could not set the application for ") + type + ".";
            return status;
        }
    }
    return QueryDefaultBrowser().isDefault ? B_OK : B_ERROR;
}

void RefreshDefaultBrowserHint()
{
    if (!QueryDefaultBrowser().isDefault) return;
    BMimeType app(kSummitSignature);
    entry_ref target, hint;
    if (LaunchTarget(target) != B_OK) return;
    if (app.GetAppHint(&hint) == B_OK && hint == target) return;
    app.SetAppHint(&target);
}
}
