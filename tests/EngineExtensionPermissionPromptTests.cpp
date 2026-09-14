#include "config.h"
#include "ExtensionPermissionPromptHaiku.h"
#include "ExtensionPermissionPrompt.h"
#include "WebKitExtensionPermission.h"
#include <Application.h>
#include <Button.h>
#include <MessageRunner.h>
#include <TextView.h>
#include <Window.h>
#include <cstdio>
#include <wtf/MainThread.h>

using WebKit::ExtensionPermissionPromptHaiku;
using Result = std::expected<bool, status_t>;
static unsigned checks = 0;
static unsigned failures = 0;
static void check(bool passed, const char* description)
{
    ++checks;
    failures += !passed;
    std::printf("%s %s\n", passed ? "PASS" : "FAIL", description);
    std::fflush(stdout);
}

static ExtensionPermissionPromptHaiku::Details details()
{
    return { "native-permission-test@example.test"_s, String::fromUTF8("Reader\n\xe2\x80\xae" "Consent check"),
        { "tabs"_s, "clipboardRead"_s }, { "https://example.test/*"_s }, false };
}

constexpr uint32 nextCase = 'eptn';
constexpr uint32 interact = 'epti';

class Sink final : public BHandler {
public:
    Sink() : BHandler("Permission test sink") { }
    void MessageReceived(BMessage*) override { }
};

class PromptTests final : public BApplication {
public:
    PromptTests()
        : BApplication("application/x-vnd.Kunanyi-Summit-permission-prompt-tests")
        , m_ui([this](const std::string& identifier, bool allowed) {
            ++m_uiReplies;
            m_broker->respond(String::fromUTF8(identifier), allowed);
        })
    {
        AddHandler(&m_sink);
        AddHandler(&m_ui);
    }

    ~PromptTests() override
    {
        m_ui.Shutdown();
        RemoveHandler(&m_ui);
        RemoveHandler(&m_sink);
    }

    void ReadyToRun() override
    {
        // Match Summit's ReadyToRun WebKit initialization. The frozen helper
        // library predates the separately verified pre-Run loop attachment fix.
        WTF::initializeMainThread();
        m_started = system_time();
        SetPulseRate(100000);
        brokerChecks();
        PostMessage(nextCase);
    }

    void Pulse() override
    {
        if (system_time() - m_started > 20000000) {
            check(false, "all native consent cases complete before watchdog");
            PostMessage(B_QUIT_REQUESTED);
        }
    }

    bool QuitRequested() override
    {
        if (m_broker)
            m_broker->shutdown();
        m_ui.Shutdown();
        check(!m_ui.HasOpenWindows(), "permission windows are destroyed before application shutdown");
        return true;
    }

    void MessageReceived(BMessage* message) override
    {
        if (message->what == nextCase) {
            startCase();
            return;
        }
        if (message->what == B_WEBKIT_EXTENSION_PERMISSION_REQUESTED) {
            const char* identifier = nullptr;
            check(message->FindString("identifier", &identifier) == B_OK, "native request carries opaque identifier");
            check(!message->HasUInt64("owner") && !message->HasUInt64("privileged_identifier"), "load capability is not exposed to native consent messages");
            ++m_displayed;
            m_ui.MessageReceived(message);
            if (!m_case) {
                if (auto* ready = std::fopen("permission-dialog.ready", "w"))
                    std::fclose(ready);
            }
            if (m_case != 4) {
                BMessage action(interact);
                action.AddString("identifier", identifier);
                // Leave the first real consent window visible for its screenshot.
                BMessageRunner::StartSending(BMessenger(this), &action, m_case ? 100000 : 1000000, 1);
            }
            return;
        }
        if (message->what == B_WEBKIT_EXTENSION_PERMISSION_CANCELLED) {
            m_ui.MessageReceived(message);
            return;
        }
        if (message->what == interact) {
            const char* identifier = nullptr;
            if (message->FindString("identifier", &identifier) == B_OK)
                interactWithWindow(String::fromUTF8(identifier));
            return;
        }
        BApplication::MessageReceived(message);
    }

private:
    void brokerChecks()
    {
        auto broker = ExtensionPermissionPromptHaiku::create();
        auto called = std::make_shared<bool>(false);
        broker->request(1, details(), [called](Result result) {
            *called = true;
            check(!result && result.error() == B_NO_INIT, "missing host is an error, not consent");
        });
        check(!*called, "validation failures complete asynchronously");
        // Keep local result state alive until queued callbacks have run.
        WTF::RunLoop::mainSingleton().dispatch([broker] { });
        check(broker->setListener(BMessenger(&m_sink)) == B_OK, "local consent listener configured");
        auto invalid = details();
        invalid.extensionName = String::fromUTF8(std::span("a\0b", 3));
        broker->request(1, WTF::move(invalid), [](Result result) {
            check(!result && result.error() == B_BAD_VALUE, "embedded NUL in consent payload is rejected");
        });
        broker->request(0, details(), [](Result result) {
            check(!result && result.error() == B_BAD_VALUE, "zero load owner is rejected");
        });
        invalid = details();
        invalid.permissions.clear();
        invalid.origins.clear();
        broker->request(1, WTF::move(invalid), [](Result result) {
            check(!result && result.error() == B_BAD_VALUE, "empty consent request is rejected");
        });
        invalid = details();
        for (unsigned index = 0; index < 257; ++index)
            invalid.origins.append("https://example.test/*"_s);
        broker->request(1, WTF::move(invalid), [](Result result) {
            check(!result && result.error() == B_BAD_VALUE, "oversized capability list is rejected");
        });
        auto replies = std::make_shared<unsigned>(0);
        for (unsigned index = 0; index < 32; ++index) {
            broker->request(10, details(), [replies](Result result) {
                check(!result && result.error() == B_CANCELED, "cancelled queued request cannot grant access");
                ++*replies;
            });
        }
        broker->request(10, details(), [](Result result) {
            check(!result && result.error() == B_BUSY, "bounded consent queue reports overflow");
        });
        check(broker->setListener({ }) == B_BUSY, "pending requests prevent listener replacement");
        broker->cancelAll();
        check(!broker->hasPendingRequests(), "cancellation detaches all requests before callbacks");
        broker->shutdown();
        broker->request(10, details(), [](Result result) {
            check(!result && result.error() == B_CANCELED, "shutdown rejects new consent requests");
        });
        WTF::RunLoop::mainSingleton().dispatch([broker, replies] {
            check(*replies == 32, "all queued requests complete exactly once");
        });
    }

    void startCase()
    {
        if (m_case >= 0)
            check(m_completed == (m_case >= 5 ? 2u : 1u), "duplicate late replies never complete a request twice");
        if (++m_case == 8) {
            check(m_uiReplies >= 6, "real UI decisions reached broker on application looper");
            PostMessage(B_QUIT_REQUESTED);
            return;
        }
        m_broker = ExtensionPermissionPromptHaiku::create(m_case == 4 ? 250000 : 5000000);
        check(m_broker->setListener(BMessenger(this)) == B_OK, "application consent listener accepted");
        m_displayed = 0;
        m_completed = 0;
        auto requested = details();
        requested.privateBrowsing = m_case == 7;
        m_identifier = m_broker->request(100, WTF::move(requested), [this](Result result) {
            ++m_completed;
            switch (m_case) {
            case 0: check(result && *result, "native Allow button returns consent"); break;
            case 1: check(result && !*result, "native Deny button refuses consent"); break;
            case 2: check(result && !*result, "closing the native window refuses consent"); break;
            case 3: check(!result && result.error() == B_CANCELED, "unload cancellation wins over a late Allow"); break;
            case 4: check(!result && result.error() == B_TIMED_OUT, "unanswered request expires without consent"); break;
            case 5: check(result && *result, "first queued prompt accepts its own decision"); break;
            case 6: check(!result && result.error() == B_CANCELED, "shutdown cancels visible prompt"); break;
            case 7:
                check(result && *result, "completion callback can enqueue another request");
                enqueueSecond();
                return;
            }
            finishCase();
        });
        if (m_case == 5 || m_case == 6)
            enqueueSecond();
        m_broker->respond("not-a-live-request"_s, true);
        check(m_broker->hasPendingRequests(), "unknown reply leaves the real request pending");
    }

    void enqueueSecond()
    {
        m_second = m_broker->request(200, details(), [this](Result result) {
            ++m_completed;
            if (m_case == 6)
                check(!result && result.error() == B_CANCELED, "shutdown cancels queued prompt");
            else
                check(result && !*result, "next queued prompt gets its separate Deny decision");
            finishCase();
        });
        if (m_case != 7) {
            m_broker->respond(m_second, true);
            check(m_completed == 0, "queued request cannot be approved before presentation");
        }
    }

    void finishCase()
    {
        unsigned expected = m_case >= 5 ? 2 : 1;
        if (m_completed != expected)
            return;
        check(!m_broker->hasPendingRequests(), "completed consent case leaves no broker requests");
        check(m_displayed == ((m_case == 5 || m_case == 7) ? 2u : 1u), "only the expected prompts were presented");
        m_broker->respond(m_identifier, true);
        m_broker->respond(m_second, true);
        // Let cancellation/window messages drain before advancing the test.
        BMessage next(nextCase);
        BMessageRunner::StartSending(BMessenger(this), &next, 100000, 1);
    }

    void interactWithWindow(const String& identifier)
    {
        BWindow* window = nullptr;
        for (int32 index = 0; (window = WindowAt(index)); ++index) {
            if (!std::strcmp(window->Title(), "Extension permission request"))
                break;
        }
        bool locked = window && window->Lock();
        check(locked, "native consent window exists and accepts interaction");
        if (!locked)
            return;
        auto* deny = dynamic_cast<BButton*>(window->FindView("deny"));
        auto* allow = dynamic_cast<BButton*>(window->FindView("allow"));
        auto* body = dynamic_cast<BTextView*>(window->FindView("permission-details"));
        check(deny && allow && window->DefaultButton() == deny, "Deny is the default native button");
        check(body && !body->IsEditable() && std::strstr(body->Text(), "https://example.test/*")
            && std::strstr(body->Text(), "clipboardRead"), "scrollable consent text includes every requested capability");
        check(body && !std::strstr(body->Text(), "\xe2\x80\xae")
            && std::strstr(body->Text(), "Reader  Consent check"), "untrusted extension name cannot inject line breaks or bidi controls");
        check(body && !!std::strstr(body->Text(), "Private browsing profile") == (m_case == 7 && identifier == m_identifier),
            "consent identifies the private profile when requested");
        if (!deny || !allow) {
            window->Unlock();
            PostMessage(B_QUIT_REQUESTED);
            return;
        }
        if (m_case == 3 || m_case == 6) {
            window->Unlock();
            if (m_case == 3) {
                m_broker->cancelOwner(999);
                check(m_broker->hasPendingRequests(), "other extension load cannot cancel this prompt");
                m_broker->cancelOwner(100);
            } else
                m_broker->shutdown();
            m_broker->respond(identifier, true);
            return;
        }
        if (m_case == 2)
            window->PostMessage(B_QUIT_REQUESTED);
        else if (m_case == 1 || identifier == m_second)
            deny->Invoke();
        else
            allow->Invoke();
        window->Unlock();
    }

    Sink m_sink;
    summit::ExtensionPermissionPrompt m_ui;
    std::shared_ptr<ExtensionPermissionPromptHaiku> m_broker;
    String m_identifier;
    String m_second;
    int m_case = -1;
    unsigned m_displayed = 0;
    unsigned m_completed = 0;
    unsigned m_uiReplies = 0;
    bigtime_t m_started = 0;
};

int main()
{
    PromptTests application;
    application.Run();
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
