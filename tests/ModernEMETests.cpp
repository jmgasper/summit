// Native Clear Key EME negotiation, encrypted playback, and key lifecycle checks.
#define SUMMIT_PDF_HELPERS_ONLY
#include "ModernPDFTests.cpp"
#include <cmath>

class EMETests final : public BApplication {
public:
    EMETests(char** argv, status_t& status)
        : BApplication("application/x-vnd.Summit-EMETests", &status), m_base(argv[1]), m_bundle(argv[2]), m_capture(argv[3]) { }
    int Result() const { return m_failed ? 1 : 0; }
    void ReadyToRun() override
    {
        try {
            Require(BWebKitInitialize() == B_OK, "initialize native EME WebKit");
            m_context = std::make_shared<BWebKitContext>(nullptr, true);
            Require(m_context->InitCheck() == B_OK, "create isolated private EME context");
            m_window = new BWindow(BRect(60, 60, 1139, 909), "Summit EME tests", B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS);
            m_view = new BWebKitView(m_window->Bounds(), "eme-page", BMessenger(this), B_FOLLOW_ALL, m_context);
            m_window->AddChild(m_view); Require(m_view->InitCheck() == B_OK, "create native EME view");
            m_window->Show(); SetPulseRate(100000);
            m_worker = std::thread([this] {
                try { RunTests(); }
                catch (const std::exception& error) { std::fprintf(stderr, "EME_FAILURE %s\n", error.what()); m_failed = true; }
                m_done = true;
            });
        } catch (const std::exception& error) { std::fprintf(stderr, "EME_FAILURE %s\n", error.what()); m_failed = true; m_done = true; SetPulseRate(100000); }
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
            std::printf("EME_RESULT %s checks=%u\n", m_failed ? "FAIL" : "PASS", checks);
            m_finished = true; PostMessage(B_QUIT_REQUESTED);
        } else if (system_time() - m_closing > 30000000) { std::fputs("EME_FAILURE helper teardown deadline\n", stderr); _Exit(1); }
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
        Require(m_window->Lock(), "lock EME view for navigation"); m_view->LoadURL(url.c_str()); m_window->Unlock();
        Wait([&] { std::lock_guard lock(m_mutex); return std::string(m_state.GetString("url", "")) == url && !m_state.GetBool("loading", true); }, "EME fixture page finishes navigation");
    }
    void WaitScript(Inspector& inspector, const std::string& expression, const char* description, bigtime_t duration = 45000000)
    {
        try { Wait([&] { return inspector.Evaluate(expression) == true; }, description, duration); }
        catch (...) { std::fprintf(stderr, "EME_STATE %s\n", inspector.Evaluate("emeTest.state()").dump().c_str()); throw; }
    }
    JSON Operation(Inspector& inspector, const std::string& method, const JSON& arguments = JSON::array())
    {
        std::string parameters;
        for (const auto& argument : arguments) parameters += "," + argument.dump();
        Require(inspector.Evaluate("emeTest.run(" + JSON(method).dump() + parameters + ")") == true, "start asynchronous EME operation");
        WaitScript(inspector, "emeTest.operation()!==null", "EME operation settles", 90000000);
        auto result = inspector.Evaluate("emeTest.operation()");
        if (!result.value("ok", false)) std::fprintf(stderr, "EME_OPERATION %s\n", result.dump().c_str());
        Require(result.value("ok", false), method.c_str());
        return result["value"];
    }
    void CheckResults(const JSON& results)
    {
        Require(results.is_array() && !results.empty(), "EME JavaScript checks return results");
        for (const auto& result : results) {
            if (!result.value("pass", false)) std::fprintf(stderr, "EME_CHECK %s\n", result.dump().c_str());
            Require(result.value("pass", false), result["label"].get<std::string>().c_str());
        }
    }
    void Ready(Inspector& inspector)
    {
        WaitScript(inspector, "video.readyState>=2 && video.videoWidth>0 && !video.error", "EME decodes a first frame while paused");
        Require(inspector.Evaluate("emeTest.pixels()>500") == true, "EME canvas contains decoded colorful video pixels");
    }
    void Play(Inspector& inspector)
    {
        inspector.Evaluate("(()=>{window.playResult=null;emeTest.play().then(r=>window.playResult=r);return true})()");
        WaitScript(inspector, "window.playResult===true", "EME play promise resolves");
    }
    void Click(Inspector& inspector, const char* id)
    {
        auto point = inspector.Evaluate("(()=>{const r=document.getElementById(" + JSON(id).dump() + ").getBoundingClientRect();return [r.x+r.width/2,r.y+r.height/2]})()");
        Require(point.is_array(), "locate the native audio playback control");
        BPoint where(point[0].get<float>(), point[1].get<float>()); BMessenger view(m_view);
        for (uint32 what : {B_MOUSE_DOWN, B_MOUSE_UP}) {
            BMessage message(what); message.AddPoint("where", where); message.AddPoint("be:view_where", where);
            message.AddInt32("buttons", what == B_MOUSE_DOWN ? B_PRIMARY_MOUSE_BUTTON : 0);
            message.AddInt32("clicks", 1); message.AddInt32("modifiers", 0); message.AddInt64("when", system_time());
            Require(view.SendMessage(&message) == B_OK, "send native EME play pointer event");
        }
    }
    void Seek(Inspector& inspector, double position)
    {
        auto expression = "(()=>{window.seekDone=false;video.addEventListener('seeked',()=>window.seekDone=true,{once:true});video.currentTime=" + std::to_string(position) + ";return true})()";
        inspector.Evaluate(expression);
        WaitScript(inspector, "window.seekDone && !video.seeking && Math.abs(video.currentTime-" + std::to_string(position) + ")<0.2 && video.readyState>=2", "EME seek resolves with a decoded frame at the requested position");
    }
    void RunTests()
    {
        auto page = m_base + "/test.html?run=native-" + std::to_string(getpid()); Navigate(page);
        Require(m_window->Lock(), "lock EME view for Inspector attachment");
        m_session = BWebKitInspectorSession::Create(*m_view, BMessenger(this)); m_window->Unlock(); Inspector inspector(m_session);
        WaitScript(inspector, "typeof emeTest==='object'", "connect Inspector to EME fixture");
        Require(inspector.Evaluate("isSecureContext && typeof navigator.requestMediaKeySystemAccess==='function' && typeof video.setMediaKeys==='function'") == true,
            "trusted localhost exposes Encrypted Media Extensions");
        CheckResults(Operation(inspector, "capabilities"));
        CheckResults(Operation(inspector, "sessionChecks"));
        CheckResults(Operation(inspector, "securityChecks"));
        for (const auto& policy : {"deny", "self", "all", "origins"}) {
            Navigate(page + "&policy=" + policy);
            WaitScript(inspector, "typeof emeTest==='object'", "policy fixture is ready");
            auto probe = Operation(inspector, "securityProbe");
            Require(probe.value("allowed", false) == (std::string(policy) != "deny"), "top-level response policy governs EME access");
            auto child = Operation(inspector, "probeFrame", {true, "encrypted-media"});
            bool delegated = std::string(policy) == "all" || std::string(policy) == "origins";
            Require(child.value("allowed", false) == delegated, "parent response policy limits cross-origin iframe delegation");
        }
        if (const char* insecureOrigin = std::getenv("SUMMIT_EME_INSECURE_ORIGIN")) {
            Navigate(std::string(insecureOrigin) + "/test.html");
            WaitScript(inspector, "typeof emeTest==='object'", "insecure fixture is ready");
            auto probe = Operation(inspector, "securityProbe");
            Require(!probe.value("secure", true) && probe["api"] == "undefined" && probe["mediaKeys"] == "undefined" && probe["constructor"] == "undefined",
                "ordinary HTTP exposes neither EME entry points nor interfaces");
        }
        Navigate(page);
        WaitScript(inspector, "typeof emeTest==='object'", "return to trusted EME playback fixture");

        Operation(inspector, "prepare", {"mse", "hold"});
        inspector.Evaluate("video.play().catch(()=>{})");
        WaitScript(inspector, "emeTest.state().requests.length>0 && emeTest.state().events.some(e=>e.name==='waitingforkey')", "encrypted MSE requests keys and signals waitingforkey");
        snooze(1200000);
        Require(inspector.Evaluate("video.getVideoPlaybackQuality().totalVideoFrames===0 && video.currentTime<0.2 && !video.error") == true,
            "missing keys keep ciphertext out of decoders and stall the playback clock");
        Require(Operation(inspector, "release").get<int>() > 0, "deliver delayed Clear Key license");
        Ready(inspector);
        WaitScript(inspector, "video.currentTime>1 && video.getVideoPlaybackQuality().totalVideoFrames>15", "late keys resume actual encrypted MSE video playback");
        Require(inspector.Evaluate("emeTest.state().requests.every(r=>r.messageType==='license-request' && r.request.type==='temporary') && emeTest.state().sessions.some(s=>s.statuses.length===2 && s.statuses.every(k=>k==='usable'))") == true,
            "CENC license requests expose temporary sessions and two usable keys");
        Click(inspector, "play");
        WaitScript(inspector, "video.currentTime>2 && !video.muted", "native user gesture enables decrypted AAC audio");
        snooze(700000); Capture(m_capture);
        inspector.Evaluate("video.pause()"); auto time = inspector.Evaluate("video.currentTime").get<double>(); snooze(500000);
        Require(std::abs(inspector.Evaluate("video.currentTime").get<double>() - time) < 0.05, "encrypted playback pauses without clock drift");
        Seek(inspector, 11.4);
        Require(inspector.Evaluate("emeTest.pixels()>500") == true, "encrypted seek renders a decrypted frame");

        Operation(inspector, "duplicateSession");
        Operation(inspector, "closeFirst");
        Seek(inspector, 7.2);
        Require(inspector.Evaluate("emeTest.pixels()>500 && !video.error") == true, "closing one session retains a key still licensed by another session");
        Operation(inspector, "closeSessions");
        inspector.Evaluate("video.currentTime=5;video.play().catch(()=>{});true");
        WaitScript(inspector, "emeTest.state().events.filter(e=>e.name==='waitingforkey').length>=2", "closed sessions revoke keys for newly decoded samples");
        auto heldFrames = inspector.Evaluate("video.getVideoPlaybackQuality().totalVideoFrames").get<unsigned>(); snooze(700000);
        Require(inspector.Evaluate("video.getVideoPlaybackQuality().totalVideoFrames").get<unsigned>() == heldFrames, "closed-session ciphertext is never decoded using stale keys");
        Operation(inspector, "reopen");
        WaitScript(inspector, "video.currentTime>5.5 && !video.seeking", "a fresh session restores playback after key revocation");
        inspector.Evaluate("video.pause()"); Seek(inspector, 16.5); Play(inspector);
        WaitScript(inspector, "video.ended && !video.error", "encrypted MSE reaches a clean end");

        Operation(inspector, "prepare", {"mse", "late"});
        inspector.Evaluate("video.play().catch(()=>{})");
        WaitScript(inspector, "emeTest.state().requests.length>0 && emeTest.state().events.some(e=>e.name==='waitingforkey')", "encrypted samples wait when MediaKeys are not attached");
        Operation(inspector, "release"); snooze(400000);
        Require(inspector.Evaluate("!video.mediaKeys && video.getVideoPlaybackQuality().totalVideoFrames===0") == true,
            "a key from an unattached MediaKeys object cannot decrypt the media element");
        Operation(inspector, "attach"); Ready(inspector);
        WaitScript(inspector, "video.currentTime>1", "attaching the correct MediaKeys wakes pending encrypted samples");

        Operation(inspector, "prepare", {"dash", "automatic"}); Ready(inspector); Play(inspector);
        WaitScript(inspector, "video.currentTime>1 && emeTest.state().requests.length>0", "direct DASH also plays CENC through Clear Key EME");
        inspector.Evaluate("video.pause()"); Seek(inspector, 12.4);
        Require(inspector.Evaluate("emeTest.pixels()>500 && !video.error") == true, "direct encrypted DASH seeks and decrypts both tracks");

        auto crossBase = m_base;
        auto host = crossBase.find("127.0.0.1");
        Require(host != std::string::npos, "CORS fixture uses a second localhost origin");
        crossBase.replace(host, 9, "localhost");
        Operation(inspector, "prepare", {"dash", "automatic", crossBase + "/manifest.mpd", false});
        WaitScript(inspector, "emeTest.state().events.some(e=>e.name==='encrypted')", "cross-origin DASH encounters encryption metadata");
        Require(inspector.Evaluate("emeTest.state().events.filter(e=>e.name==='encrypted').every(e=>e.type==='' && e.size===0) && emeTest.state().requests.length===0") == true,
            "DASH without CORS permission hides cross-origin encryption metadata");
        Operation(inspector, "prepare", {"dash", "automatic", crossBase + "/cors/manifest.mpd", true});
        Ready(inspector); Play(inspector);
        WaitScript(inspector, "video.currentTime>1 && emeTest.state().requests.length>0", "CORS-authorized cross-origin DASH decrypts and plays");
        Require(inspector.Evaluate("emeTest.pixels()>500") == true, "CORS-authorized DASH permits canvas access");

        Operation(inspector, "prepare", {"dash-clear", "automatic", m_base + "/cross-clear.mpd", false});
        WaitScript(inspector, "video.readyState>=2 && video.videoWidth>0 && !video.error", "same-origin manifest plays cross-origin clear segments");
        Require(inspector.Evaluate("(()=>{try{emeTest.pixels();return false}catch(error){return error.name==='SecurityError'}})()") == true,
            "cross-origin DASH segments taint canvas even with a same-origin manifest");

        Operation(inspector, "prepare", {"clear", "automatic"}); Ready(inspector); Play(inspector);
        WaitScript(inspector, "video.currentTime>1 && !video.mediaKeys && !video.error", "clear MSE regression plays without a CDM");
        Operation(inspector, "prepare", {"mse", "hold"});
        inspector.Evaluate("video.play().catch(()=>{})");
        WaitScript(inspector, "emeTest.state().events.some(e=>e.name==='waitingforkey')", "close-during-key-wait fixture is waiting");
        // Destruction must wake decoder queues even when no license will arrive.
    }
    std::string m_base, m_bundle, m_capture;
    std::shared_ptr<BWebKitContext> m_context; std::shared_ptr<BWebKitInspectorSession> m_session;
    BWindow* m_window {nullptr}; BWebKitView* m_view {nullptr};
    std::thread m_worker; std::mutex m_mutex; BMessage m_state;
    std::atomic<bool> m_done {false}, m_failed {false}; bigtime_t m_closing {0}; bool m_finished {false};
};

int main(int argc, char** argv)
{
    if (argc != 4) { std::fprintf(stderr, "Usage: %s BASE_URL BUNDLE SCREENSHOT_PPM\n", argv[0]); return 2; }
    status_t status; EMETests app(argv, status); if (status != B_OK) return 1;
    app.Run(); return app.Result();
}
