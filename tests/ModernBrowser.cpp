#include <WebKit/WebKitView.h>
#include <Application.h>
#include <Bitmap.h>
#include <Screen.h>
#include <String.h>
#include <Window.h>
#include <cstdio>
#include <cstring>
#include <memory>

class PreviewWindow final : public BWindow {
public:
    PreviewWindow()
        : BWindow(BRect(80, 80, 1120, 820), "Summit — Modern WebKit", B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS) { }
    bool QuitRequested() override
    {
        be_app->PostMessage(B_QUIT_REQUESTED);
        return false;
    }
};

class ModernBrowser final : public BApplication {
public:
    ModernBrowser(const char* url, bool smoke)
        : BApplication("application/x-vnd.Kunanyi-Summit-ModernPreview")
        , m_url(url), m_smoke(smoke) { }

    void ReadyToRun() override
    {
        if (BWebKitInitialize() != B_OK) {
            std::fputs("FAIL initialize modern WebKit on the application thread\n", stderr);
            PostMessage(B_QUIT_REQUESTED);
            return;
        }
        m_window = new PreviewWindow;
        m_view = new BWebKitView(m_window->Bounds(), "modern-web-view", BMessenger(this));
        m_window->AddChild(m_view);
        if (m_view->InitCheck() != B_OK) {
            std::fputs("FAIL create native modern web view\n", stderr);
            PostMessage(B_QUIT_REQUESTED);
            return;
        }
        m_view->LoadURL(m_url.String());
        m_window->Show();
        m_started = system_time();
        if (!m_smoke)
            m_result = 0;
        SetPulseRate(100000);
    }

    void MessageReceived(BMessage* message) override
    {
        if (message->what == B_WEBKIT_STATE_CHANGED) {
            const char* title = message->FindString("title");
            bool loading = true;
            message->FindBool("loading", &loading);
            m_fixturePassed = title && !std::strcmp(title, "Summit fixture PASS") && !loading;
            std::printf("STATE title=%s loading=%s url=%s\n", title ? title : "", loading ? "true" : "false",
                message->FindString("url") ? message->FindString("url") : "");
            std::fflush(stdout);
            if (m_window && m_window->Lock()) {
                m_window->SetTitle(title && *title ? title : "Summit — Modern WebKit");
                m_window->Unlock();
            }
            return;
        }
        if (message->what == B_WEBKIT_PROCESS_EXITED) {
            std::fputs("FAIL modern content process exited\n", stderr);
            m_result = 1;
            if (m_smoke)
                PostMessage(B_QUIT_REQUESTED);
            return;
        }
        BApplication::MessageReceived(message);
    }

    void Pulse() override
    {
        if (!m_smoke || !m_window || !m_view)
            return;
        if (m_fixturePassed && FixturePixelsVisible()) {
            std::puts("PASS modern HTTP/DOM/JavaScript/storage/cookie fixture and native frame presentation");
            m_result = 0;
            PostMessage(B_QUIT_REQUESTED);
        } else if (system_time() - m_started > 60000000) {
            std::fputs("FAIL modern browser fixture did not finish and render within 60 seconds\n", stderr);
            PostMessage(B_QUIT_REQUESTED);
        }
    }

    bool QuitRequested() override
    {
        if (m_window) {
            auto* window = m_window;
            if (!window->Lock())
                return false;
            m_window = nullptr;
            m_view = nullptr;
            window->Quit();
            // Native view destruction queues WebKit teardown. Let that queued
            // application-thread work run before stopping the application loop.
            PostMessage(B_QUIT_REQUESTED);
            return false;
        }
        return true;
    }

    int Result() const { return m_result; }

private:
    bool FixturePixelsVisible()
    {
        if (m_window->LockWithTimeout(100000) != B_OK)
            return false;
        BPoint point = m_view->ConvertToScreen(BPoint(5, 5));
        m_window->Unlock();
        BRect region(point.x, point.y, point.x + 1, point.y + 1);
        BBitmap* captured = nullptr;
        status_t captureResult = BScreen().GetBitmap(&captured, false, &region);
        std::unique_ptr<BBitmap> bitmap(captured);
        if (captureResult != B_OK || !bitmap || !bitmap->IsValid() || bitmap->BitsLength() < 4
            || (bitmap->ColorSpace() != B_RGB32 && bitmap->ColorSpace() != B_RGBA32))
            return false;
        const auto* pixels = static_cast<const unsigned char*>(bitmap->Bits());
        // The served CSS paints #f7faf6; a blank native view is white.
        return pixels[0] == 246 && pixels[1] == 250 && pixels[2] == 247;
    }

    BString m_url;
    bool m_smoke;
    bool m_fixturePassed { false };
    bigtime_t m_started { 0 };
    PreviewWindow* m_window { nullptr };
    BWebKitView* m_view { nullptr };
    int m_result { 1 };
};

int main(int argc, char** argv)
{
    const char* url = "http://10.0.2.2:8765/basic";
    bool smoke = false;
    for (int index = 1; index < argc; ++index) {
        if (!std::strcmp(argv[index], "--smoke"))
            smoke = true;
        else
            url = argv[index];
    }
    ModernBrowser browser(url, smoke);
    if (browser.InitCheck() != B_OK) {
        std::fputs("FAIL initialize the native preview application\n", stderr);
        return 1;
    }
    browser.Run();
    return browser.Result();
}
