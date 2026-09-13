#include "ui/BrowserWindow.h"
#include "ui/Messages.h"
#include <Application.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <Path.h>
#include <Roster.h>
#include <WebPage.h>
#include <WebSettings.h>
#include <filesystem>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

class SummitApp : public BApplication {
public:
    SummitApp() : BApplication("application/x-vnd.Kunanyi-Summit") {}
    void ArgvReceived(int32 argc, char** argv) override
    {
        for (int32 i = 1; i < argc; ++i) {
            std::string argument = argv[i];
            if (argument == "--profile" && i + 1 < argc) { fProfile = argv[++i]; continue; }
            if (fWindow) {
                BMessage message(summit::kNewTab); message.AddString("url", argument.c_str());
                fWindow->PostMessage(&message);
            } else fURLs.push_back(argument);
        }
    }
    void RefsReceived(BMessage* message) override
    {
        if (fWindow) fWindow->PostMessage(message);
        else {
            entry_ref ref;
            for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; ++i) {
                BPath path(&ref); fURLs.push_back("file://" + std::string(path.Path()));
            }
        }
    }
    void ReadyToRun() override
    {
        if (fProfile.empty()) {
            BPath path; find_directory(B_USER_SETTINGS_DIRECTORY, &path); path.Append("Summit");
            fProfile = path.Path();
        }
        std::filesystem::create_directories(fProfile);
        setenv("CURL_COOKIE_JAR_PATH", (fProfile / "cookies.sqlite").c_str(), 1);
        BWebPage::InitializeOnce();
        BWebPage::SetCacheModel(B_WEBKIT_CACHE_MODEL_WEB_BROWSER);
        BWebSettings::SetPersistentStoragePath((fProfile / "WebKit").c_str());
        app_info info; GetAppInfo(&info); BPath executable(&info.ref);
        auto home = std::filesystem::path(executable.Path()).parent_path() / "resources/start.html";
        if (!std::filesystem::exists(home)) home = "/boot/system/data/Summit/start.html";
        if (!std::filesystem::exists(home)) {
            std::fprintf(stderr, "Summit: missing start page at %s\n", home.c_str());
        }
        fWindow = new summit::BrowserWindow(fProfile / "profile.json", "file://" + home.string(), fURLs);
        fWindow->Show();
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what == summit::kCreateTabOnApp) {
            BMessenger target;
            const char* url = nullptr;
            bool select = true;
            message->FindBool("select", &select);
            if (message->FindMessenger("window", &target) != B_OK
                || message->FindString("url", &url) != B_OK || !target.LockTarget()) return;
            BLooper* looper = nullptr;
            auto* window = dynamic_cast<summit::BrowserWindow*>(target.Target(&looper));
            if (window) window->CreateTab(url, select);
            if (looper) looper->Unlock();
            return;
        }
        if (fWindow && (message->what == summit::kNewTab || message->what == summit::kNavigate
            || message->what == summit::kBrowserState)) fWindow->PostMessage(message);
        else BApplication::MessageReceived(message);
    }
    void AboutRequested() override { if (fWindow) fWindow->PostMessage(B_ABOUT_REQUESTED); }
private:
    summit::BrowserWindow* fWindow = nullptr;
    std::filesystem::path fProfile;
    std::vector<std::string> fURLs;
};

int main()
{
    umask(0077);
    SummitApp app;
    app.Run();
    BWebPage::ShutdownOnce();
    return 0;
}
