// Native paste and keyboard regression tests for caret lookup and typing latency.
#define SUMMIT_PDF_HELPERS_ONLY
#include "ModernPDFTests.cpp"
#include <Clipboard.h>

class TypingTests final : public BApplication {
public:
    TypingTests(char** argv, status_t& status)
        : BApplication("application/x-vnd.Summit-TypingTests", &status), m_base(argv[1]), m_bundle(argv[2]) { }
    int Result() const { return m_failed ? 1 : 0; }
    void ReadyToRun() override
    {
        try {
            Require(BWebKitInitialize() == B_OK, "initialize native typing WebKit");
            m_context = std::make_shared<BWebKitContext>(nullptr, true);
            Require(m_context->InitCheck() == B_OK, "create isolated private typing context");
            m_window = new BWindow(BRect(60, 60, 1139, 909), "Summit typing tests", B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS);
            m_view = new BWebKitView(m_window->Bounds(), "typing-page", BMessenger(this), B_FOLLOW_ALL, m_context);
            m_window->AddChild(m_view); Require(m_view->InitCheck() == B_OK, "create native typing view");
            m_window->Show(); SetPulseRate(100000);
            m_worker = std::thread([this] {
                try { RunTests(); }
                catch (const std::exception& error) { std::fprintf(stderr, "Typing_FAILURE %s\n", error.what()); m_failed = true; }
                m_done = true;
            });
        } catch (const std::exception& error) { std::fprintf(stderr, "Typing_FAILURE %s\n", error.what()); m_failed = true; m_done = true; SetPulseRate(100000); }
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what == B_WEBKIT_INSPECTOR_MESSAGES) return;
        if (message->what == B_WEBKIT_STATE_CHANGED) { std::lock_guard lock(m_mutex); m_state = *message; return; }
        BApplication::MessageReceived(message);
    }
    void Pulse() override
    {
        if (!m_done) return;
        if (!m_closing) {
            m_closing = system_time(); if (m_worker.joinable()) m_worker.join(); m_session.reset();
            if (m_window && m_window->Lock()) { m_window->Quit(); m_window = nullptr; m_view = nullptr; }
            m_context.reset();
        }
        if (!HasHelpers(m_bundle) && !BWebKitHasPendingNativeUI()) {
            std::printf("Typing_RESULT %s checks=%u\n", m_failed ? "FAIL" : "PASS", checks);
            m_finished = true; PostMessage(B_QUIT_REQUESTED);
        } else if (system_time() - m_closing > 30000000) { std::fputs("Typing_FAILURE helper teardown deadline\n", stderr); _Exit(1); }
    }
    bool QuitRequested() override { return m_finished; }
private:
    void Wait(const std::function<bool()>& predicate, const char* description, bigtime_t duration = 45000000)
    {
        auto deadline = system_time() + duration;
        while (system_time() < deadline) { if (predicate()) { Require(true, description); return; } snooze(100000); }
        Require(false, description);
    }
    void Navigate(const std::string& url)
    {
        Require(m_window->Lock(), "lock typing view for navigation"); m_view->LoadURL(url.c_str()); m_window->Unlock();
        Wait([&] { std::lock_guard lock(m_mutex); return std::string(m_state.GetString("url", "")) == url && !m_state.GetBool("loading", true); }, "Typing fixture page finishes navigation");
    }
    void Key(const char* text, int32 key = 0x3c, int32 modifiers = 0)
    {
        BMessenger view(m_view);
        for (uint32 what : {B_KEY_DOWN, B_KEY_UP}) {
            BMessage event(what); event.AddString("bytes", text); event.AddInt32("key", key);
            event.AddInt32("modifiers", modifiers); event.AddInt64("when", system_time());
            Require(view.SendMessage(&event, static_cast<BHandler*>(nullptr), 500000) == B_OK, "send native typing key");
        }
    }
    void SpecialKey(char byte, int32 key, int32 modifiers = 0)
    {
        char text[] = {byte, 0}; Key(text, key, modifiers);
    }
    void Edit(uint32 command)
    {
        BMessage message(command);
        Require(BMessenger(m_view).SendMessage(&message) == B_OK, "send native editing command");
    }
    void Expect(Inspector& inspector, const std::string& value, size_t start, size_t end)
    {
        auto expression = "typingTest.value()===" + JSON(value).dump() + " && JSON.stringify(typingTest.selection())==="
            + JSON(JSON::array({start, end}).dump()).dump();
        Wait([&] { return inspector.Evaluate(expression) == true; }, "native edit preserves exact text and selection");
    }
    void RunTests()
    {
        struct ClipboardRestore {
            BMessage original;
            ClipboardRestore() { Require(be_clipboard->Lock(), "lock clipboard backup"); original = *be_clipboard->Data(); be_clipboard->Unlock(); }
            ~ClipboardRestore() { if (be_clipboard->Lock()) { be_clipboard->Clear(); *be_clipboard->Data() = original; be_clipboard->Commit(); be_clipboard->Unlock(); } }
            void Set(const std::string& text) {
                Require(be_clipboard->Lock(), "lock test clipboard"); be_clipboard->Clear();
                be_clipboard->Data()->AddData("text/plain", B_MIME_TYPE, text.data(), text.size());
                be_clipboard->Commit(); be_clipboard->Unlock();
            }
        } clipboard;
        Navigate(m_base);
        Require(m_window->Lock(), "lock typing view");
        m_session = BWebKitInspectorSession::Create(*m_view, BMessenger(this));
        m_view->MakeFocus(); m_window->Activate(); m_window->Unlock();
        Inspector inspector(m_session);
        Wait([&] { return inspector.Evaluate("typeof typingTest==='object'") == true; }, "typing fixture ready");
        // Paste through the native clipboard, then type faster than normal prose.
        // The second phase distinguishes a paste backlog from a persistent cost.
        for (size_t size : {1000, 100000, 1000000}) {
            Require(inspector.Evaluate("typingTest.prepare('textarea','')") == true, "prepare paste field");
            std::string pasted;
            while (pasted.size() < size) pasted += "Pasted text, measured typing follows.\n";
            pasted.resize(size); clipboard.Set(pasted); Edit(B_PASTE);
            for (int phase = 1; phase <= 2; ++phase) {
                const auto statsExpression = "typingTest.stats(" + std::to_string((phase - 1) * 60) + ")";
                for (int i = 0; i < 60; ++i) { Key("a"); snooze(30000); }
                Wait([&] { return inspector.Evaluate(statsExpression + ".keys===60") == true; }, "all typing events arrive");
                Expect(inspector, pasted + std::string(phase * 60, 'a'), size + phase * 60, size + phase * 60);
                Wait([&] { return inspector.Evaluate(statsExpression + ".frames===60") == true; }, "every key reaches an animation frame");
                auto stats = inspector.Evaluate(statsExpression);
                std::printf("TYPING_SAMPLE bytes=%zu phase=%d %s\n", size, phase, stats.dump().c_str()); std::fflush(stdout);
                Require(stats.at("visible") == "visible" && stats.at("focus") == true, "typing page stays visible and focused");
                Require(stats.at("scrollHeight").get<double>() - stats.at("clientHeight").get<double>()
                    - stats.at("scrollTop").get<double>() < 48, "caret reveal keeps the typed line in view");
                // One megabyte is deliberately beyond typical comment sizes.
                const double budget = size == 1000000 && phase == 1 ? 750 : 250;
                Require(stats.at("delay95").get<double>() < budget, "typing avoids a sustained input backlog");
                Require(stats.at("frame95").get<double>() < budget, "typing renders without a sustained visual backlog");
                snooze(1000000);
            }
        }
        // Exercise both ends of sparse-index intervals, middle/end positions,
        // wrapping changes, DOM edits, newlines, and the bidi fallback path.
        for (const char* kind : {"textarea", "editable"}) {
            for (const char* line : {"0123456789 abcdefghijklmnopqrstuvwxyz\n", "L\xc3\xa9tters e\xcc\x81 and \xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb text\n", "English \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d English\n"}) {
                std::string text;
                for (int i = 0; i < 500; ++i) text += line;
                text += "END";
                Require(inspector.Evaluate("typingTest.prepare(" + JSON(kind).dump() + "," + JSON(text).dump() + ")") == true, "prepare editing regression field");
                // Let JavaScript count UTF-16 offsets rather than UTF-8 bytes.
                auto length = inspector.Evaluate("typingTest.value().length").get<size_t>();
                auto lineLength = inspector.Evaluate(JSON(line).dump() + ".length").get<size_t>();
                for (size_t offset : {size_t(0), lineLength * 31, lineLength * 32, lineLength * 33, lineLength * 250, length}) {
                    auto select = "typingTest.select(" + std::to_string(offset) + ")";
                    inspector.Evaluate(select);
                    auto expected = inspector.Evaluate("typingTest.value().slice(0," + std::to_string(offset) + ")+ 'a' +typingTest.value().slice(" + std::to_string(offset) + ")").get<std::string>();
                    Key("a"); Expect(inspector, expected, offset + 1, offset + 1);
                    SpecialKey(B_LEFT_ARROW, 0x61); Expect(inspector, expected, offset, offset);
                    SpecialKey(B_RIGHT_ARROW, 0x63, B_SHIFT_KEY); Expect(inspector, expected, offset, offset + 1);
                    SpecialKey(B_BACKSPACE, 0x1e); Expect(inspector, text, offset, offset);
                    auto width = offset % 2 ? 340 : 700;
                    Require(inspector.Evaluate("typingTest.resize(" + std::to_string(width) + ")") == width, "invalidate lookup after reflow");
                }
            }
        }
    }
    std::string m_base, m_bundle;
    std::shared_ptr<BWebKitContext> m_context; std::shared_ptr<BWebKitInspectorSession> m_session;
    BWindow* m_window {nullptr}; BWebKitView* m_view {nullptr};
    std::thread m_worker; std::mutex m_mutex; BMessage m_state;
    std::atomic<bool> m_done {false}, m_failed {false}; bigtime_t m_closing {0}; bool m_finished {false};
};

int main(int argc, char** argv)
{
    if (argc != 3) { std::fprintf(stderr, "Usage: %s FIXTURE_URL BUNDLE\n", argv[0]); return 2; }
    status_t status; TypingTests app(argv, status); if (status != B_OK) return 1;
    app.Run(); return app.Result();
}
