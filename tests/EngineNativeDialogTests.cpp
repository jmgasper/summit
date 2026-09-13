#include "JavaScriptDialogHaiku.h"
#include <Application.h>
#include <MessageRunner.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

using namespace WebKit;

static int checks = 0;
static int failures = 0;
static void Check(bool result, const char* description)
{
    ++checks;
    failures += !result;
    std::printf("%s %s\n", result ? "PASS" : "FAIL", description);
    std::fflush(stdout);
}

static void RegistryChecks()
{
    JavaScriptDialogRequestsHaiku requests;
    int calls = 0;
    bool accepted = false;
    std::string value;
    auto identifier = requests.add([&](bool result, std::string text) { ++calls; accepted = result; value = std::move(text); });
    Check(requests.contains(identifier), "pending dialog retains its completion");
    Check(requests.complete(identifier, true, std::string("a\0b", 3))
        && !requests.complete(identifier, false) && calls == 1 && accepted && value == std::string("a\0b", 3),
        "reply ownership preserves embedded NUL and completes exactly once");

    int nested = 0;
    identifier = requests.add([&](bool, std::string) {
        auto child = requests.add([&](bool, std::string) { ++nested; });
        requests.complete(child, true);
    });
    requests.complete(identifier, true);
    Check(nested == 1, "callbacks can reenter the registry without holding its mutex");

    constexpr size_t count = 64;
    std::array<std::atomic<int>, count> outcomes { };
    std::vector<uint64_t> identifiers;
    for (size_t index = 0; index < count; ++index)
        identifiers.push_back(requests.add([&, index](bool, std::string) { ++outcomes[index]; }));
    std::thread replies([&] { for (auto id : identifiers) requests.complete(id, true, "late response"); });
    std::thread closing([&] { requests.cancelAll(true); });
    replies.join();
    closing.join();
    bool once = true;
    for (auto& result : outcomes) once &= result == 1;
    Check(once, "concurrent native replies and page close complete all 64 requests exactly once");
    int rejected = 0;
    identifier = requests.add([&](bool result, std::string text) { rejected += !result && text.empty(); });
    Check(identifier == 0 && rejected == 1, "closed registry immediately cancels new requests");
}

enum : uint32 { beginCase = 'dgbe', completedCase = 'dgco' };
struct Case {
    JavaScriptDialogKindHaiku kind;
    uint32 action;
    bool accepted;
    std::string initial;
    std::string expected;
    const char* description;
    bool clear = false;
};

class DialogTests final : public BApplication {
public:
    DialogTests()
        : BApplication("application/x-vnd.Kunanyi-Summit-native-dialog-tests")
        , m_requests(std::make_shared<JavaScriptDialogRequestsHaiku>())
        , m_cases {
            { JavaScriptDialogKindHaiku::Alert, javaScriptDialogAcceptHaiku, true, "", "", "native alert acknowledges OK" },
            { JavaScriptDialogKindHaiku::Alert, B_QUIT_REQUESTED, false, "", "", "closing an alert completes the request" },
            { JavaScriptDialogKindHaiku::Confirm, javaScriptDialogCancelHaiku, false, "", "", "native confirm Cancel returns false" },
            { JavaScriptDialogKindHaiku::Confirm, javaScriptDialogAcceptHaiku, true, "", "", "native confirm OK returns true" },
            { JavaScriptDialogKindHaiku::Prompt, javaScriptDialogAcceptHaiku, true, "Héllo 世界", "Héllo 世界", "prompt OK preserves its Unicode default" },
            { JavaScriptDialogKindHaiku::Prompt, javaScriptDialogAcceptHaiku, true, "default", "", "accepted empty prompt stays distinct from cancellation", true },
            { JavaScriptDialogKindHaiku::Prompt, B_KEY_DOWN, true, "default", "default", "Return in the native prompt accepts its current value" },
            { JavaScriptDialogKindHaiku::Prompt, javaScriptDialogCancelHaiku, false, "default", "", "prompt Cancel rejects the default value" },
            { JavaScriptDialogKindHaiku::Prompt, javaScriptDialogAcceptHaiku, true, std::string("a\0b", 3), std::string("a\0b", 3), "prompt default retains embedded NUL through native input" },
            { JavaScriptDialogKindHaiku::Prompt, javaScriptDialogAcceptHaiku, true, std::string("a\0b", 3), "", "editing a prompt with embedded NUL returns its edited value", true },
            { JavaScriptDialogKindHaiku::BeforeUnload, javaScriptDialogCancelHaiku, false, "", "", "beforeunload Stay cancels navigation" },
            { JavaScriptDialogKindHaiku::BeforeUnload, javaScriptDialogAcceptHaiku, true, "", "", "beforeunload Leave allows navigation" },
            { JavaScriptDialogKindHaiku::Prompt, javaScriptDialogDismissHaiku, false, "default", "", "owner dismissal cancels a live native prompt" },
        } { }

    void ReadyToRun() override
    {
        m_parent = new BWindow(BRect(100, 100, 900, 650), "Native JavaScript dialog checks", B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS);
        m_parent->Show();
        queueChecks();
        SetPulseRate(100000);
        PostMessage(beginCase);
    }

    void MessageReceived(BMessage* message) override
    {
        if (message->what == beginCase) {
            startCase();
            return;
        }
        if (message->what == completedCase) {
            int32 index;
            bool accepted;
            const void* bytes;
            ssize_t length;
            if (message->FindInt32("index", &index) != B_OK || index != m_index
                || message->FindBool("accepted", &accepted) != B_OK
                || message->FindData("value", B_RAW_TYPE, &bytes, &length) != B_OK || length < 1) {
                Check(false, "native result belongs to the expected request");
                PostMessage(B_QUIT_REQUESTED);
                return;
            }
            const auto& test = m_cases[m_index];
            const std::string value(static_cast<const char*>(bytes), static_cast<size_t>(length - 1));
            Check(accepted == test.accepted && value == test.expected, test.description);
            Check(m_pulses > m_startedAtPulse, "application looper keeps processing pulses while the dialog is visible");
            m_next = true;
            return;
        }
        BApplication::MessageReceived(message);
    }

    void Pulse() override
    {
        ++m_pulses;
        if (m_quitting) {
            PostMessage(B_QUIT_REQUESTED);
            return;
        }
        if (m_started && system_time() - m_started > 5000000) {
            Check(false, "native dialog result arrived before its timeout");
            PostMessage(B_QUIT_REQUESTED);
        }
        if (m_next && !m_active.IsValid()) {
            Check(JavaScriptDialogHaiku::activeCount() == 0, "native activity ends after dialog window destruction");
            m_next = false;
            m_started = 0;
            ++m_index;
            if (m_index == static_cast<int32>(m_cases.size())) PostMessage(B_QUIT_REQUESTED);
            else PostMessage(beginCase);
        }
    }

    bool QuitRequested() override
    {
        m_quitting = true;
        m_requests->cancelAll(true);
        if (m_active.IsValid()) {
            BMessage dismiss(javaScriptDialogDismissHaiku);
            m_active.SendMessage(&dismiss, static_cast<BHandler*>(nullptr), 0);
            return false;
        }
        if (JavaScriptDialogHaiku::activeCount()) return false;
        if (m_parent && m_parent->Lock()) { m_parent->Quit(); m_parent = nullptr; }
        return true;
    }

private:
    void queueChecks()
    {
        auto requests = std::make_shared<JavaScriptDialogRequestsHaiku>();
        JavaScriptDialogHostHaiku host(requests);
        int cancelled = 0;
        for (int index = 0; index < 3; ++index) {
            auto identifier = requests->add([&](bool accepted, std::string) { cancelled += !accepted; });
            BMessage message(javaScriptDialogShowHaiku);
            message.AddUInt64("identifier", identifier);
            message.AddInt32("kind", static_cast<int32>(JavaScriptDialogKindHaiku::Alert));
            message.AddString("origin", "https://hidden.example");
            host.show(message, m_parent, BMessenger(), false);
        }
        Check(!cancelled && !JavaScriptDialogHaiku::activeCount(), "background tab dialogs stay queued without opening native windows");
        host.cancelAll();
        Check(cancelled == 3 && !JavaScriptDialogHaiku::activeCount(), "closing a hidden tab cancels every queued dialog exactly once");
        auto identifier = requests->add([&](bool accepted, std::string) { cancelled += !accepted; });
        BMessage malformed(javaScriptDialogShowHaiku);
        malformed.AddUInt64("identifier", identifier);
        malformed.AddInt32("kind", static_cast<int32>(JavaScriptDialogKindHaiku::Alert));
        m_parent->Lock();
        host.show(malformed, m_parent, BMessenger(), true);
        m_parent->Unlock();
        Check(cancelled == 4 && !JavaScriptDialogHaiku::activeCount(), "a malformed request without an origin is cancelled before native display");
    }

    void startCase()
    {
        const auto& test = m_cases[m_index];
        auto identifier = m_requests->add([target = BMessenger(this), index = m_index](bool accepted, std::string value) {
            BMessage result(completedCase);
            result.AddInt32("index", index);
            result.AddBool("accepted", accepted);
            result.AddData("value", B_RAW_TYPE, value.c_str(), value.size() + 1);
            target.SendMessage(&result, static_cast<BHandler*>(nullptr), 0);
        });
        auto* dialog = new JavaScriptDialogHaiku(identifier, test.kind, "https://frame.example:8443", "https://top.example",
            "Page-controlled <b>text</b>\nSecond line", test.initial, m_requests, BMessenger());
        auto* origin = dynamic_cast<BTextView*>(dialog->FindView("dialog-origin"));
        auto* top = dynamic_cast<BTextView*>(dialog->FindView("dialog-top-origin"));
        Check(origin && !origin->IsEditable() && std::string(origin->Text()) == "https://frame.example:8443"
            && top && !top->IsEditable() && std::string(top->Text()) == "https://top.example", "frame and top origins have separate native text views");
        if (test.kind == JavaScriptDialogKindHaiku::Prompt) {
            auto* input = dynamic_cast<BTextView*>(dialog->FindView("dialog-input"));
            Check(input && std::string(input->Text(), input->TextLength()) == test.initial,
                "native prompt starts with the complete supplied default value");
            if (input && test.clear) input->SetText("");
        }
        if (test.kind == JavaScriptDialogKindHaiku::BeforeUnload) {
            auto* body = dynamic_cast<BTextView*>(dialog->FindView("dialog-message"));
            Check(body && std::string(body->Text()) == "This page may contain unsaved changes. Leave this page?"
                && dialog->DefaultButton() && std::string(dialog->DefaultButton()->Label()) == "Stay",
                "beforeunload uses a native explanation and defaults to staying");
        }
        m_parent->Lock();
        Check(dialog->AddToSubset(m_parent) == B_OK, "native dialog belongs to its browser window subset");
        dialog->CenterIn(m_parent->Frame());
        m_parent->Unlock();
        m_active = BMessenger(dialog);
        dialog->Show();
        Check(JavaScriptDialogHaiku::activeCount() == 1, "native activity tracks the live dialog window");
        m_started = system_time();
        m_startedAtPulse = m_pulses;
        BMessage action(test.action);
        BMessenger actionTarget = m_active;
        if (test.action == B_KEY_DOWN) {
            action.AddString("bytes", "\n");
            actionTarget = BMessenger(dialog->FindView("dialog-input"));
        }
        m_action = std::make_unique<BMessageRunner>(actionTarget, &action, 250000, 1);
    }

    std::shared_ptr<JavaScriptDialogRequestsHaiku> m_requests;
    std::vector<Case> m_cases;
    BWindow* m_parent { nullptr };
    BMessenger m_active;
    std::unique_ptr<BMessageRunner> m_action;
    int32 m_index { 0 };
    int m_pulses { 0 }, m_startedAtPulse { 0 };
    bigtime_t m_started { 0 };
    bool m_next { false };
    bool m_quitting { false };
};

int main()
{
    RegistryChecks();
    DialogTests application;
    application.Run();
    std::printf("Native dialog checks: %d passed, %d failed\n", checks - failures, failures);
    return failures ? 1 : 0;
}
