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
#include <OS.h>
#include <cstdio>
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

// Whether the system accepts path as Summit: the roster forgets an app hint
// whose file does not carry the application's signature.
bool CarriesSignature(const char* path, entry_ref& ref)
{
    BEntry entry(path, true);
    BFile file(&entry, B_READ_ONLY);
    BAppFileInfo info(&file);
    char signature[B_MIME_TYPE_LENGTH] = "";
    return entry.IsFile() && info.InitCheck() == B_OK && info.GetSignature(signature) == B_OK && IsSummit(signature)
        && entry.GetRef(&ref) == B_OK;
}

// The program the system should start for Summit: its executable, which
// takes pages and files as messages (a launcher script cannot) and sets up
// the engine's environment from launch.env beside it. The installed build's
// launcher names a path that follows each install (SUMMIT_SYSTEM_LAUNCHER,
// a link to the current build); otherwise this executable.
status_t LaunchTarget(entry_ref& ref)
{
    if (const char* launcher = std::getenv("SUMMIT_SYSTEM_LAUNCHER"); launcher && *launcher && CarriesSignature(launcher, ref)) {
        // The link itself, not the build it points to now.
        BEntry entry(launcher, false);
        return entry.GetRef(&ref);
    }
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
    // The name of the program the system knows for the signature. Not
    // BRoster::FindApp(), which has the registrar create MIME entries as it
    // resolves; this only reads.
    BMimeType app(current.c_str());
    entry_ref ref;
    char description[B_MIME_TYPE_LENGTH] = "";
    if (app.GetAppHint(&ref) == B_OK && ref.name) state.current = ref.name;
    else if (app.GetShortDescription(description) == B_OK && description[0]) state.current = description;
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

void ApplyLaunchEnvironment()
{
    image_info image;
    int32 cookie = 0;
    while (get_next_image_info(B_CURRENT_TEAM, &cookie, &image) == B_OK) {
        if (image.type != B_APP_IMAGE) continue;
        BPath path(image.name);
        if (path.GetParent(&path) != B_OK || path.Append("launch.env") != B_OK) return;
        FILE* file = std::fopen(path.Path(), "r");
        if (!file) return;
        char line[4096];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text(line);
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
            if (text.empty() || text[0] == '#') continue;
            const auto equals = text.find('=');
            if (equals == std::string::npos || !equals) continue;
            const bool prepend = text[equals - 1] == '^';
            const std::string name = text.substr(0, prepend ? equals - 1 : equals), value = text.substr(equals + 1);
            if (name.empty()) continue;
            const char* current = std::getenv(name.c_str());
            if (!prepend) {
                if (!current) setenv(name.c_str(), value.c_str(), 0);
                continue;
            }
            // An unset LIBRARY_PATH means the system's default search path,
            // which the engine's processes still need after the addition.
            const std::string list = current ? current
                : name == "LIBRARY_PATH" ? "%A/lib:/boot/home/config/non-packaged/lib:/boot/home/config/lib:"
                    "/boot/system/non-packaged/lib:/boot/system/lib" : "";
            if ((":" + list + ":").find(":" + value + ":") != std::string::npos) continue;
            setenv(name.c_str(), list.empty() ? value.c_str() : (value + ":" + list).c_str(), 1);
        }
        std::fclose(file);
        return;
    }
}

void RefreshDefaultBrowserHint()
{
    // Only the installed build (whose launcher names the link to it) keeps
    // the system pointed at itself; test copies leave it alone.
    const char* installed = std::getenv("SUMMIT_SYSTEM_LAUNCHER");
    if (!installed || !*installed || !QueryDefaultBrowser().isDefault) return;
    BMimeType app(kSummitSignature);
    entry_ref target, hint;
    if (LaunchTarget(target) != B_OK || (app.GetAppHint(&hint) == B_OK && hint == target)) return;
    app.SetAppHint(&target);
}
}
