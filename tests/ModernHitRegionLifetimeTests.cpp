// Check native canvas destruction and recording through the public inspector API.
#define SUMMIT_PDF_HELPERS_ONLY
#include "ModernPDFTests.cpp"

class CanvasInspector {
public:
    explicit CanvasInspector(std::shared_ptr<BWebKitInspectorSession> session) : m_session(std::move(session)) { }
    void Drain()
    {
        std::vector<std::string> messages;
        m_session->Take(messages);
        for (const auto& text : messages) {
            auto message = JSON::parse(text);
            auto method = message.value("method", "");
            auto parameters = message.value("params", JSON::object());
            if (method == "Target.targetCreated") {
                auto info = parameters.at("targetInfo");
                if (info.value("type", "") != "page") continue;
                auto target = info.at("targetId").get<std::string>();
                if (!info.value("isProvisional", false)) m_target = target;
                if (info.value("isPaused", false)) Outer("Target.resume", {{"targetId", target}});
            } else if (method == "Target.didCommitProvisionalTarget")
                m_target = parameters.at("newTargetId").get<std::string>();
            else if (method == "Target.dispatchMessageFromTarget") {
                auto inner = JSON::parse(parameters.at("message").get<std::string>());
                if (inner.contains("id")) m_answers[inner.at("id").get<unsigned>()] = inner;
                else {
                    auto name = inner.value("method", "");
                    auto params = inner.value("params", JSON::object());
                    if (name == "Canvas.canvasAdded") canvases.insert(params.at("canvas").at("canvasId").get<std::string>());
                    else if (name == "Canvas.canvasRemoved") canvases.erase(params.at("canvasId").get<std::string>());
                    else if (name == "Canvas.recordingFinished") recording = params.value("recording", JSON());
                }
            }
        }
    }
    JSON Command(const char* method, const JSON& params = JSON::object())
    {
        auto deadline = system_time() + 10000000;
        while (m_target.empty() && system_time() < deadline) { Drain(); snooze(10000); }
        if (m_target.empty()) throw std::runtime_error("Canvas inspector target timeout");
        unsigned id = ++m_inner;
        Outer("Target.sendMessageToTarget", {{"targetId", m_target},
            {"message", JSON{{"id", id}, {"method", method}, {"params", params}}.dump()}});
        while (system_time() < deadline) {
            Drain();
            if (auto it = m_answers.find(id); it != m_answers.end()) {
                auto response = std::move(it->second); m_answers.erase(it);
                if (response.contains("error")) throw std::runtime_error(std::string(method) + ": " + response.dump());
                return response.value("result", JSON::object());
            }
            snooze(10000);
        }
        throw std::runtime_error(std::string(method) + " timed out");
    }
    JSON Evaluate(const std::string& expression)
    {
        auto result = Command("Runtime.evaluate", {{"expression", expression}, {"returnByValue", true},
            {"doNotPauseOnExceptionsAndMuteConsole", true}});
        if (result.value("wasThrown", false)) throw std::runtime_error("Canvas fixture threw: " + result.dump());
        return result.value("result", JSON::object()).value("value", JSON());
    }
    std::set<std::string> canvases;
    JSON recording;
private:
    void Outer(const char* method, const JSON& params)
    {
        auto text = JSON{{"id", ++m_outer}, {"method", method}, {"params", params}}.dump();
        m_session->Send(text.c_str());
    }
    std::shared_ptr<BWebKitInspectorSession> m_session;
    std::string m_target;
    std::map<unsigned, JSON> m_answers;
    unsigned m_outer {0}, m_inner {0};
};

class HitRegionLifetimeTests final : public BApplication {
public:
    HitRegionLifetimeTests(const char* bundle, status_t& status)
        : BApplication("application/x-vnd.Summit-HitRegionLifetimeTests", &status), m_bundle(bundle) { }
    int Result() const { return m_failed ? 1 : 0; }
    void ReadyToRun() override
    {
        try {
            Require(BWebKitInitialize() == B_OK, "initialize native hit-region lifetime WebKit");
            m_context = std::make_shared<BWebKitContext>(nullptr, true);
            Require(m_context->InitCheck() == B_OK, "create isolated private context");
            m_window = new BWindow(BRect(60, 60, 699, 539), "Summit canvas lifetime tests", B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS);
            m_view = new BWebKitView(m_window->Bounds(), "canvas-page", BMessenger(this), B_FOLLOW_ALL, m_context);
            m_window->AddChild(m_view); Require(m_view->InitCheck() == B_OK, "create native canvas view");
            m_window->Show(); SetPulseRate(100000);
            m_worker = std::thread([this] {
                try { RunTests(); }
                catch (const std::exception& error) { std::fprintf(stderr, "HitRegionLifetime_FAILURE %s\n", error.what()); m_failed = true; }
                m_done = true;
            });
        } catch (const std::exception& error) { std::fprintf(stderr, "HitRegionLifetime_FAILURE %s\n", error.what()); m_failed = true; m_done = true; SetPulseRate(100000); }
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
            std::printf("HitRegionLifetime_RESULT %s checks=%u\n", m_failed ? "FAIL" : "PASS", checks);
            m_finished = true; PostMessage(B_QUIT_REQUESTED);
        } else if (system_time() - m_closing > 30000000) { std::fputs("HitRegionLifetime_FAILURE helper teardown deadline\n", stderr); _Exit(1); }
    }
    bool QuitRequested() override { return m_finished; }
private:
    void Wait(const std::function<bool()>& predicate, const char* description)
    {
        auto deadline = system_time() + 15000000;
        while (system_time() < deadline) { if (predicate()) { Require(true, description); return; } snooze(50000); }
        Require(false, description);
    }
    void RunTests()
    {
        Require(m_window->Lock(), "lock lifetime view for navigation"); m_view->LoadURL("about:blank"); m_window->Unlock();
        Wait([&] { std::lock_guard lock(m_mutex); return std::string(m_state.GetString("url", "")) == "about:blank" && !m_state.GetBool("loading", true); }, "blank lifetime page loaded");
        Require(m_window->Lock(), "lock lifetime view for inspector");
        m_session = BWebKitInspectorSession::Create(*m_view, BMessenger(this)); m_window->Unlock();
        CanvasInspector inspector(m_session);
        inspector.Command("Runtime.enable"); inspector.Command("Heap.enable"); inspector.Command("Canvas.enable");
        Require(inspector.Evaluate(R"JS((() => {
            window.lifetimeCanvases = [];
            const make = () => { const canvas = document.createElement('canvas'); canvas.width = canvas.height = 20; lifetimeCanvases.push(canvas); return canvas; };
            const region = (canvas, control) => { const context = canvas.getContext('2d'); context.rect(0, 0, 20, 20); context.addHitRegion({ id: 'lifetime', control }); };
            for (let i = 0; i < 8; ++i) {
                const self = make(); region(self, self);
                const ancestor = document.createElement('div'), child = make(); ancestor.append(child); region(child, ancestor);
                const parent = make(), button = document.createElement('button'); parent.append(button); region(parent, button);
                const first = make(), second = make(); region(first, second); region(second, first);
            }
            return lifetimeCanvases.length;
        })())JS") == 40, "create forty canvases with self, ancestor, descendant and mutual controls");
        Wait([&] { inspector.Drain(); return inspector.canvases.size() == 40; }, "inspector observes forty native canvas contexts");
        Require(inspector.Evaluate("window.lifetimeCanvases = null; true") == true, "release all fixture canvas references");
        inspector.Command("Heap.gc");
        Wait([&] { inspector.Drain(); return inspector.canvases.empty(); }, "all forty native canvas contexts are destroyed after collection");

        Require(inspector.Evaluate("window.recordCanvas = document.createElement('canvas'); recordCanvas.getContext('2d'); true") == true, "create recording canvas");
        Wait([&] { inspector.Drain(); return inspector.canvases.size() == 1; }, "inspector observes recording canvas");
        const auto canvasId = *inspector.canvases.begin();
        inspector.Command("Canvas.startRecording", {{"canvasId", canvasId}});
        Require(inspector.Evaluate(R"JS((() => {
            const context = recordCanvas.getContext('2d'), path = new Path2D(); path.rect(0, 0, 10, 10);
            context.addHitRegion({ id: 'recorded', control: recordCanvas, path });
            context.fillStyle = 'green'; context.fillRect(0, 0, 10, 10);
            context.removeHitRegion('recorded'); context.clearHitRegions();
            return true;
        })())JS") == true, "hit-region methods work while canvas recording is active");
        inspector.Command("Canvas.stopRecording", {{"canvasId", canvasId}});
        Wait([&] { inspector.Drain(); return !inspector.recording.is_null(); }, "canvas recording finishes");
        const auto recording = inspector.recording.dump();
        Require(recording.find("\"fillRect\"") != std::string::npos, "recording retains drawing commands");
        for (const char* name : {"addHitRegion", "removeHitRegion", "clearHitRegions"})
            Require(recording.find(std::string("\"") + name + "\"") == std::string::npos, "recording omits non-drawing hit-region commands");
        Require(inspector.Evaluate("window.recordCanvas = null; true") == true, "release recorded canvas");
        inspector.Command("Heap.gc");
        Wait([&] { inspector.Drain(); return inspector.canvases.empty(); }, "recording canvas is destroyed after collection");
    }
    std::string m_bundle;
    std::shared_ptr<BWebKitContext> m_context; std::shared_ptr<BWebKitInspectorSession> m_session;
    BWindow* m_window {nullptr}; BWebKitView* m_view {nullptr};
    std::thread m_worker; std::mutex m_mutex; BMessage m_state;
    std::atomic<bool> m_done {false}, m_failed {false}; bigtime_t m_closing {0}; bool m_finished {false};
};

int main(int argc, char** argv)
{
    if (argc != 2) { std::fprintf(stderr, "Usage: %s BUNDLE\n", argv[0]); return 2; }
    status_t status; HitRegionLifetimeTests app(argv[1], status); if (status != B_OK) return 1;
    app.Run(); return app.Result();
}
