#include "ui/BrowserWindow.h"
#include "ui/Messages.h"
#include "ui/ExtensionPermissionPrompt.h"
#include "ui/ExtensionController.h"
#include "ui/ExtensionInstaller.h"
#include "ui/ExtensionManager.h"
#include "core/Address.h"
#include <Alert.h>
#include <Application.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <Path.h>
#include <Roster.h>
#if !SUMMIT_MODERN_WEBKIT
#include <WebPage.h>
#include <WebSettings.h>
#endif
#include <filesystem>
#include <atomic>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

class SummitApp : public BApplication {
public:
    explicit SummitApp(status_t& status) : BApplication("application/x-vnd.Kunanyi-Summit", &status) {}
    void ArgvReceived(int32 argc, char** argv) override
    {
        for (int32 i = 1; i < argc; ++i) {
            std::string argument = argv[i];
            if (argument == "--profile" && i + 1 < argc) { fProfile = argv[++i]; continue; }
            if (fWindow.IsValid()) {
                BMessage message(summit::kNewTab); message.AddString("url", argument.c_str());
                fWindow.SendMessage(&message);
            } else fURLs.push_back(argument);
        }
    }
    void RefsReceived(BMessage* message) override
    {
        if (fWindow.IsValid()) fWindow.SendMessage(message);
        else {
            entry_ref ref;
            for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; ++i) {
                BPath path(&ref);
                if (path.InitCheck() == B_OK) fURLs.push_back(summit::FileURL(path.Path()));
            }
        }
    }
    void ReadyToRun() override
    {
        if (fProfile.empty()) {
            BPath path;
            status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
            if (status == B_OK) status = path.Append(
#if SUMMIT_MODERN_WEBKIT
                "SummitModern"
#else
                "Summit"
#endif
            );
            if (status != B_OK) {
                StartupError("Could not locate your settings folder: " + std::string(std::strerror(status)));
                return;
            }
            fProfile = path.Path();
        }
        std::error_code error;
#if SUMMIT_MODERN_WEBKIT
        fProfile = std::filesystem::absolute(fProfile, error);
        if (error) {
            StartupError("Could not resolve the profile folder: " + error.message());
            return;
        }
#endif
        std::filesystem::create_directories(fProfile, error);
        if (error) {
            StartupError("Could not open the profile folder:\n" + fProfile.string() + "\n\n" + error.message());
            return;
        }
#if SUMMIT_MODERN_WEBKIT
        status_t initialization = BWebKitInitialize();
        if (initialization != B_OK) {
            StartupError("Could not initialize WebKit: " + std::string(std::strerror(initialization)));
            return;
        }
        fWebKitInitialized = true;
        fWebKitContext = std::make_shared<BWebKitContext>((fProfile / "WebKit").c_str());
        if (fWebKitContext->InitCheck() != B_OK) {
            StartupError("Could not open the WebKit profile: " + std::string(std::strerror(fWebKitContext->InitCheck())));
            return;
        }
        initialization = fWebKitContext->SetDownloadListener(BMessenger(this));
        if (initialization != B_OK) {
            StartupError("Could not initialize downloads: " + std::string(std::strerror(initialization)));
            return;
        }
        fPermissionPrompts = std::make_unique<summit::ExtensionPermissionPrompt>(
            [context = std::weak_ptr<BWebKitContext>(fWebKitContext)](const std::string& identifier, bool allowed) {
                if (auto liveContext = context.lock())
                    liveContext->RespondToExtensionPermissionRequest(identifier.c_str(), allowed);
            });
        AddHandler(fPermissionPrompts.get());
        initialization = fWebKitContext->SetExtensionPermissionListener(BMessenger(fPermissionPrompts.get()));
        if (initialization == B_NOT_SUPPORTED) {
            RemoveHandler(fPermissionPrompts.get());
            fPermissionPrompts.reset();
        } else if (initialization != B_OK) {
            StartupError("Could not initialize extension permissions: " + std::string(std::strerror(initialization)));
            return;
        }
#else
        setenv("CURL_COOKIE_JAR_PATH", (fProfile / "cookies.sqlite").c_str(), 1);
        BWebPage::InitializeOnce();
        fWebKitInitialized = true;
        BWebPage::SetCacheModel(B_WEBKIT_CACHE_MODEL_WEB_BROWSER);
        BWebSettings::SetPersistentStoragePath((fProfile / "WebKit").c_str());
#endif
        app_info info; GetAppInfo(&info); BPath executable(&info.ref);
        auto home = std::filesystem::path(executable.Path()).parent_path() / "resources/start.html";
        if (!std::filesystem::exists(home, error)) home = "/boot/system/data/Summit/start.html";
        if (!std::filesystem::exists(home, error)) {
            std::fprintf(stderr, "Summit: missing start page at %s\n", home.c_str());
        }
        auto* window = new summit::BrowserWindow(fProfile / "profile.json", summit::FileURL(home.string()), fURLs
#if SUMMIT_MODERN_WEBKIT
            , fWebKitContext, bool(fPermissionPrompts)
#endif
        );
        fWindow = BMessenger(window);
        window->Show();
#if SUMMIT_MODERN_WEBKIT
        if (fPermissionPrompts) {
            fExtensions = std::make_unique<summit::ExtensionController>(fWebKitContext, fProfile / "Extensions",
                [this] { RefreshExtensions(); });
            AddHandler(fExtensions.get());
            fExtensions->Start();
            fInstaller = std::make_unique<summit::ExtensionInstaller>(fWebKitContext, fProfile / "Extensions",
                [this](summit::InstalledExtension entry, std::string baseURL, std::string error, bool installed) {
                    fExtensions->AddLoaded(std::move(entry), std::move(baseURL), std::move(error), installed);
                }, [this] { RefreshExtensions(); });
            AddHandler(fInstaller.get());
            fInstaller->Start();
        }
#endif
    }
    void MessageReceived(BMessage* message) override
    {
#if SUMMIT_MODERN_WEBKIT
        if (message->what == summit::kShowExtensions) {
            if (!fWindow.IsValid() || !fExtensions || !fInstaller) return;
            if (!fExtensionWindow.IsValid()) {
                auto* manager = new summit::ExtensionManager(BMessenger(this), fExtensionWindows);
                fExtensionWindow = BMessenger(manager);
                manager->Show();
            } else fExtensionWindow.SendMessage(summit::kShowExtensions);
            RefreshExtensions();
            return;
        }
        if (message->what == summit::kExtensionSelected || message->what == summit::kExtensionApprove
            || message->what == summit::kExtensionCancel || message->what == summit::kExtensionEnable
            || message->what == summit::kExtensionRemove || message->what == summit::kExtensionManagerClosed) {
            BMessenger sender;
            if (!fInstaller || !fExtensions || message->FindMessenger("window", &sender) != B_OK || sender != fExtensionWindow) return;
            uint64 generation = 0;
            message->FindUInt64("generation", &generation);
            if (message->what == summit::kExtensionManagerClosed) {
                fExtensionWindow = {};
                // The window may close before the first import snapshot has
                // reached it. Closing the owning manager cancels its current
                // import even when that window still has an older generation.
                fInstaller->Cancel(fInstaller->Generation());
                return;
            }
            if (!fWindow.IsValid()) return;
            if (message->what == summit::kExtensionSelected && fExtensions->IsReady() && !fInstaller->IsBusy()) {
                entry_ref ref;
                if (message->FindRef("refs", &ref) == B_OK) {
                    BPath path(&ref);
                    if (path.InitCheck() == B_OK) fInstaller->Import(path.Path());
                }
            } else if (message->what == summit::kExtensionApprove) {
                bool files = false;
                if (message->FindBool("allow_files", &files) == B_OK) fInstaller->Approve(generation, files, false);
            } else if (message->what == summit::kExtensionCancel) fInstaller->Cancel(generation);
            else if (!fInstaller->IsBusy()) {
                const char* identifier = nullptr;
                if (message->FindString("extension_identifier", &identifier) == B_OK) {
                    if (message->what == summit::kExtensionRemove) fExtensions->Remove(identifier);
                    else {
                        bool enabled = false;
                        if (message->FindBool("enabled", &enabled) == B_OK) fExtensions->SetEnabled(identifier, enabled);
                    }
                }
            }
            RefreshExtensions();
            return;
        }
        if (message->what == B_WEBKIT_DOWNLOAD_STARTED || message->what == B_WEBKIT_DOWNLOAD_PROGRESS
            || message->what == B_WEBKIT_DOWNLOAD_FINISHED) {
            // The application remains the listener while closing windows and
            // draining cancellation replies on the WebKit main loop.
            if (fWindow.IsValid()) fWindow.SendMessage(message);
            return;
        }
        if (message->what == summit::kWindowReadyToClose) {
            BMessenger sender;
            if (message->FindMessenger("window", &sender) != B_OK || sender != fWindow)
                return;
            if (!fWindow.LockTarget()) return;
            BLooper* looper = nullptr;
            auto* window = dynamic_cast<summit::BrowserWindow*>(fWindow.Target(&looper));
            fWindow = BMessenger();
            if (window) window->Quit();
            else if (looper) looper->Unlock();
            PostMessage(B_QUIT_REQUESTED);
            return;
        }
#endif
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
        if (fWindow.IsValid() && (message->what == summit::kNewTab || message->what == summit::kNavigate
            || message->what == summit::kBrowserState)) fWindow.SendMessage(message);
        else BApplication::MessageReceived(message);
    }
    void AboutRequested() override { if (fWindow.IsValid()) fWindow.SendMessage(B_ABOUT_REQUESTED); }
#if SUMMIT_MODERN_WEBKIT
    void Pulse() override
    {
        if (fWaitingForNativeUI) PostMessage(B_QUIT_REQUESTED);
    }
    bool QuitRequested() override
    {
        if (fWindow.IsValid()) {
            BMessage request(summit::kRequestWindowClose);
            fWindow.SendMessage(&request);
            return false;
        }
        if (fWebKitContext) {
            if (fExtensionWindow.IsValid()) fExtensionWindow.SendMessage(summit::kCloseExtensionManager);
            if (fInstaller) fInstaller->Shutdown();
            if (fExtensions) fExtensions->Shutdown();
            if (fExtensionWindows->load() || (fInstaller && fInstaller->HasPendingWork())
                || (fExtensions && fExtensions->HasPendingWork())) {
                fWaitingForNativeUI = true;
                SetPulseRate(100000);
                return false;
            }
            if (fInstaller) {
                RemoveHandler(fInstaller.get());
                fInstaller.reset();
            }
            if (fExtensions) {
                RemoveHandler(fExtensions.get());
                fExtensions.reset();
            }
            if (fPermissionPrompts) {
                fWebKitContext->CancelExtensionPermissionRequests();
                fPermissionPrompts->Shutdown();
                if (fPermissionPrompts->HasOpenWindows()) {
                    fWaitingForNativeUI = true;
                    SetPulseRate(100000);
                    return false;
                }
                RemoveHandler(fPermissionPrompts.get());
                fPermissionPrompts.reset();
            }
            if (fWebKitContext->HasPendingDownloads()) {
                if (!fCancellingDownloads) {
                    fCancellingDownloads = true;
                    fWebKitContext->CancelAllDownloads();
                }
                fWaitingForNativeUI = true;
                SetPulseRate(100000);
                return false;
            }
            fWebKitContext.reset();
            PostMessage(B_QUIT_REQUESTED);
            return false;
        }
        if (BWebKitHasPendingNativeUI()) {
            fWaitingForNativeUI = true;
            fNativeUIExitReady = false;
            SetPulseRate(100000); // Native pulse intervals use 100 ms granularity.
            return false;
        }
        fWaitingForNativeUI = false;
        SetPulseRate(0);
        if (!fNativeUIExitReady) {
            fNativeUIExitReady = true;
            PostMessage(B_QUIT_REQUESTED);
            return false;
        }
        return true;
    }
#endif
    bool WebKitInitialized() const { return fWebKitInitialized; }
    int ExitStatus() const { return fExitStatus; }
private:
#if SUMMIT_MODERN_WEBKIT
    void RefreshExtensions()
    {
        if (!fExtensionWindow.IsValid() || !fExtensions || !fInstaller) return;
        BMessage state = fInstaller->Snapshot();
        state.what = summit::kExtensionsState;
        state.AddBool("ready", fExtensions->IsReady());
        state.AddString("catalog_error", fExtensions->CatalogError().c_str());
        for (const auto& entry : fExtensions->Entries()) {
            BMessage item;
            item.AddString("identifier", entry.installation.identifier.c_str());
            item.AddString("name", entry.installation.name.c_str());
            item.AddString("version", entry.installation.version.c_str());
            item.AddString("error", entry.error.c_str());
            item.AddBool("enabled", entry.installation.enabled);
            item.AddBool("loaded", entry.loaded);
            item.AddBool("installed", entry.installed);
            state.AddMessage("entry", &item);
        }
        fExtensionWindow.SendMessage(&state, static_cast<BHandler*>(nullptr), 0);
    }
#endif
    void StartupError(const std::string& message)
    {
        fExitStatus = 1;
        std::fprintf(stderr, "Summit: %s\n", message.c_str());
        (new BAlert("Summit", message.c_str(), "Quit", nullptr, nullptr,
            B_WIDTH_AS_USUAL, B_STOP_ALERT))->Go();
        PostMessage(B_QUIT_REQUESTED);
    }
    BMessenger fWindow;
#if SUMMIT_MODERN_WEBKIT
    std::shared_ptr<BWebKitContext> fWebKitContext;
    std::unique_ptr<summit::ExtensionPermissionPrompt> fPermissionPrompts;
    std::unique_ptr<summit::ExtensionController> fExtensions;
    std::unique_ptr<summit::ExtensionInstaller> fInstaller;
    BMessenger fExtensionWindow;
    std::shared_ptr<std::atomic<unsigned>> fExtensionWindows = std::make_shared<std::atomic<unsigned>>(0);
    bool fWaitingForNativeUI = false;
    bool fNativeUIExitReady = false;
    bool fCancellingDownloads = false;
#endif
    bool fWebKitInitialized = false;
    int fExitStatus = 0;
    std::filesystem::path fProfile;
    std::vector<std::string> fURLs;
};

static int RunApplication()
{
    umask(0077);
    status_t status = B_NO_INIT;
    SummitApp app(status);
    if (status == B_ALREADY_RUNNING)
        return 0; // The registrar delivered the launch arguments to the existing instance.
    if (status != B_OK) {
        std::fprintf(stderr, "Summit: could not initialize the native application: %s\n", std::strerror(status));
        return 1;
    }
    app.Run();
#if !SUMMIT_MODERN_WEBKIT
    if (app.WebKitInitialized()) BWebPage::ShutdownOnce();
#endif
    return app.ExitStatus();
}

int main()
{
    const int status = RunApplication();
#if SUMMIT_MODERN_WEBKIT
    // RunApplication destroys the native application after its asynchronous
    // window/context cleanup. WebKit worker TLS destructors may still be
    // finishing, so do not race them with libbe's global handler-token table
    // destruction. This matches the Haiku WebKit helper-process exit path.
    std::fflush(nullptr);
    std::_Exit(status);
#else
    return status;
#endif
}
