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
// system which executable to start: the installed build's (through the link
// SUMMIT_SYSTEM_LAUNCHER names, which follows each install) or this one.
// Started that way, Summit takes its launcher's environment from launch.env
// (ApplyLaunchEnvironment()). The previous choices are not kept; FileTypes
// (or another browser's own setting) changes them back.
status_t MakeDefaultBrowser(std::string& error);

// In the installed build (SUMMIT_SYSTEM_LAUNCHER set), when Summit is the
// default browser: points the system at the link to the installed build if
// it names something else. Cheap when nothing changed; call at startup.
void RefreshDefaultBrowserHint();

// Before anything else in main(): a Summit the system started directly (for a
// web link or an HTML file) rather than its launcher script reads the
// launcher's environment from launch.env beside the executable: NAME=VALUE
// sets a variable that is not set, NAME^=VALUE puts VALUE in front of a
// colon-separated list (LIBRARY_PATH) that does not hold it yet. The engine's
// processes inherit it.
void ApplyLaunchEnvironment();
}
