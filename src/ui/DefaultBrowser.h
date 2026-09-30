#pragma once
#include <SupportDefs.h>
#include <string>

namespace summit {
// Summit's application signature (resources/Summit.rdef).
inline constexpr const char* kSummitSignature = "application/x-vnd.Kunanyi-Summit";

// Whether Summit is the system's web browser: the preferred application of
// web links (http and https) and web pages (text/html, XHTML), which is what
// `open URL`, Tracker, mail and every other application use.
struct DefaultBrowserState {
    bool isDefault = false;
    // The name of the browser web links open in now ("WebPositive"), or
    // empty when there is none.
    std::string current;
};
DefaultBrowserState QueryDefaultBrowser();

// Makes Summit the preferred application of those types and tells the
// system how to start it: through the installed launcher
// (/boot/system/apps/Summit) when there is one, which sets up the engine's
// environment, otherwise this executable. The previous choices are not kept;
// FileTypes (or another browser's own setting) changes them back.
status_t MakeDefaultBrowser(std::string& error);

// When Summit is the default browser, points the system at the executable
// MakeDefaultBrowser() would choose now, in case it moved. Cheap when nothing
// changed; call at startup.
void RefreshDefaultBrowserHint();
}
