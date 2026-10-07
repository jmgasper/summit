// Native Web Bluetooth bindings, permissions, GATT, and teardown checks.
// Requires SUMMIT_BLUETOOTH_TEST_BACKEND=1 and SUMMIT_ENABLE_INPUT_SYNTHESIS=1.
#define SUMMIT_PDF_HELPERS_ONLY
#include "ModernPDFTests.cpp"
#include <cmath>

class BluetoothTests final : public BApplication {
public:
    BluetoothTests(char** argv, status_t& status)
        : BApplication("application/x-vnd.Summit-BluetoothTests", &status), m_base(argv[1]), m_bundle(argv[2]), m_capture(argv[3]) { }
    int Result() const { return m_failed ? 1 : 0; }
    void ReadyToRun() override
    {
        try {
            Require(BWebKitInitialize() == B_OK, "initialize native Bluetooth WebKit");
            m_context = std::make_shared<BWebKitContext>(nullptr, true);
            Require(m_context->InitCheck() == B_OK, "create isolated private Bluetooth context");
            m_context->SetPermissionListener(BMessenger(this));
            m_window = new BWindow(BRect(60, 60, 1139, 909), "Summit Bluetooth tests", B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS);
            m_view = new BWebKitView(m_window->Bounds(), "bt-page", BMessenger(this), B_FOLLOW_ALL, m_context);
            m_window->AddChild(m_view); Require(m_view->InitCheck() == B_OK, "create native Bluetooth view");
            m_window->Show(); SetPulseRate(100000);
            m_worker = std::thread([this] {
                try { RunTests(); }
                catch (const std::exception& error) { std::fprintf(stderr, "BLUETOOTH_FAILURE %s\n", error.what()); m_failed = true; }
                m_done = true;
            });
        } catch (const std::exception& error) { std::fprintf(stderr, "BLUETOOTH_FAILURE %s\n", error.what()); m_failed = true; m_done = true; SetPulseRate(100000); }
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what == B_WEBKIT_PERMISSION_REQUESTED) {
            if (!m_context) return;
            auto identifier=message->GetUInt64("identifier",0); m_lastRequest=identifier; ++m_requests;
            if (m_holdPrompt) return;
            const char* wanted=m_selectBusy ? "Summit Test Busy Peripheral" : "Summit Test Peripheral";
            const char* label; const char* token;
            for (int32 i=0;message->FindString("device_label",i,&label)==B_OK;++i) {
                if (std::string(label)!=wanted || message->FindString("device_id",i,&token)!=B_OK) continue;
                m_context->RespondToMediaPermissionRequest(identifier,true,token); return;
            }
            m_context->RespondToMediaPermissionRequest(identifier,false,""); return;
        }
        if (message->what == B_WEBKIT_PERMISSION_CANCELLED) { ++m_cancelled; return; }
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
            std::printf("BLUETOOTH_RESULT %s checks=%u\n", m_failed ? "FAIL" : "PASS", checks);
            m_finished = true; PostMessage(B_QUIT_REQUESTED);
        } else if (system_time() - m_closing > 30000000) { std::fputs("BLUETOOTH_FAILURE helper teardown deadline\n", stderr); _Exit(1); }
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
        Require(m_window->Lock(), "lock Bluetooth view for navigation"); m_view->LoadURL(url.c_str()); m_window->Unlock();
        Wait([&] { std::lock_guard lock(m_mutex); return std::string(m_state.GetString("url", "")) == url && !m_state.GetBool("loading", true); }, "Bluetooth fixture page finishes navigation");
    }
    void WaitScript(Inspector& inspector, const std::string& expression, const char* description, bigtime_t duration = 45000000)
    {
        try { Wait([&] { return inspector.Evaluate(expression) == true; }, description, duration); }
        catch (...) { std::fprintf(stderr, "BLUETOOTH_STATE %s\n", inspector.Evaluate("btTest.operation()").dump().c_str()); throw; }
    }
    JSON Operation(Inspector& inspector, const std::string& method, const JSON& arguments = JSON::array())
    {
        std::string parameters;
        for (const auto& argument : arguments) parameters += "," + argument.dump();
        Require(inspector.Evaluate("btTest.run(" + JSON(method).dump() + parameters + ")") == true, "start asynchronous Bluetooth operation");
        WaitScript(inspector, "btTest.operation()!==null", "Bluetooth operation settles", 90000000);
        auto result = inspector.Evaluate("btTest.operation()");
        if (!result.value("ok", false)) std::fprintf(stderr, "BLUETOOTH_OPERATION %s\n", result.dump().c_str());
        Require(result.value("ok", false), method.c_str());
        return result["value"];
    }
    void CheckResults(const JSON& results)
    {
        Require(results.is_array() && !results.empty(), "Bluetooth JavaScript checks return results");
        for (const auto& result : results) {
            if (!result.value("pass", false)) std::fprintf(stderr, "BLUETOOTH_CHECK %s\n", result.dump().c_str());
            Require(result.value("pass", false), result["label"].get<std::string>().c_str());
        }
    }
    void Click(Inspector& inspector, const char* id)
    {
        auto point = inspector.Evaluate("(()=>{const r=document.getElementById(" + JSON(id).dump() + ").getBoundingClientRect();return [r.x+r.width/2,r.y+r.height/2]})()");
        Require(point.is_array(), "locate the Bluetooth request control");
        BPoint where(point[0].get<float>(), point[1].get<float>()); BMessenger view(m_view);
        for (uint32 what : {B_MOUSE_DOWN, B_MOUSE_UP}) {
            BMessage message(what); message.AddPoint("where", where); message.AddPoint("be:view_where", where);
            message.AddInt32("buttons", what == B_MOUSE_DOWN ? B_PRIMARY_MOUSE_BUTTON : 0);
            message.AddInt32("clicks", 1); message.AddInt32("modifiers", 0); message.AddInt64("when", system_time());
            Require(view.SendMessage(&message) == B_OK, "send native Bluetooth request pointer event");
        }
    }
    void RunTests()
    {
        auto page = m_base + "/test.html?run=native-" + std::to_string(getpid()); Navigate(page);
        Require(m_window->Lock(), "lock Bluetooth view for Inspector attachment");
        m_session = BWebKitInspectorSession::Create(*m_view, BMessenger(this)); m_window->Unlock(); Inspector inspector(m_session);
        WaitScript(inspector, "typeof btTest==='object'", "Bluetooth fixture loaded");
        CheckResults(Operation(inspector, "capabilities"));
        inspector.Evaluate("window.nextMethod='validation';true"); Click(inspector, "request");
        WaitScript(inspector, "btTest.operation()!==null", "gesture filter checks settle");
        auto validation = inspector.Evaluate("btTest.operation()");
        Require(validation.value("ok", false), validation.dump().c_str()); CheckResults(validation["value"]);
        inspector.Evaluate("window.nextMethod='request';true");
        auto choose = [&] {
            inspector.Evaluate("btTest.run('devices');true");
            WaitScript(inspector,"btTest.operation()!==null","pre-chooser check finishes");
            Click(inspector,"request");
            WaitScript(inspector,"btTest.operation()!==null && btTest.operation().value && typeof btTest.operation().value.id==='string'","native chooser grants selected device");
            return inspector.Evaluate("btTest.operation().value");
        };
        auto chosen = choose();
        Require(chosen["name"] == "Summit Test Peripheral" && !chosen.value("connected",true), "permission grants only the chosen device without connecting");
        CheckResults(Operation(inspector,"grantChecks"));
        auto same = Operation(inspector,"frame",{false,nullptr});
        Require(same["devices"] == 1 && same["available"] == true,"same-origin child inherits Bluetooth permission");
        auto denied = Operation(inspector,"frame",{false,"bluetooth 'none'"});
        Require(denied["error"] == "SecurityError" && denied["available"] == false,"iframe allow restriction denies Bluetooth");
        auto cross = Operation(inspector,"frame",{true,nullptr});
        Require(cross["error"] == "SecurityError" && cross["available"] == false,"cross-origin child defaults to denied");
        CheckResults(Operation(inspector,"gattChecks"));
        auto forgotten=Operation(inspector,"forget"); Require(forgotten["count"] == 0 && forgotten["connected"] == false,"forget revokes the device grant");
        inspector.Evaluate("window.nextOptions={filters:[{name:'Summit Test Busy Peripheral'}],optionalServices:['battery_service']};true");
        m_selectBusy = true; choose(); CheckResults(Operation(inspector,"busy")); Operation(inspector,"forget"); m_selectBusy = false;
        for (const auto& policy : {"deny", "self", "all"}) {
            Navigate(page + "&policy=" + policy); WaitScript(inspector,"typeof btTest==='object'","policy page is ready");
            auto probe=Operation(inspector,"probe");
            Require((probe["available"] == true) == (std::string(policy) != "deny"),"response policy controls Bluetooth availability");
            Require((probe["error"] == "SecurityError") == (std::string(policy) == "deny"),"response policy controls granted-device access");
        }
        if (const char* insecure=std::getenv("SUMMIT_BLUETOOTH_INSECURE_ORIGIN")) {
            Navigate(std::string(insecure)+"/test.html"); WaitScript(inspector,"typeof btTest==='object'","insecure page is ready");
            Require(inspector.Evaluate("!isSecureContext && typeof navigator.bluetooth==='undefined' && typeof BluetoothDevice==='undefined'") == true,"ordinary HTTP hides Bluetooth entry points");
        }
        Navigate(page); WaitScript(inspector,"typeof btTest==='object'","return to secure page");
        m_holdPrompt = true; auto asked = m_requests.load(); auto cancelled = m_cancelled.load();
        Click(inspector,"request"); Wait([&] {return m_requests > asked;},"chooser reaches native permission listener");
        auto stale = m_lastRequest.load();
        Navigate(page+"&cancel=1"); Wait([&] {return m_cancelled > cancelled;},"navigation cancels the exact pending chooser");
        m_context->RespondToMediaPermissionRequest(stale,true,"stale-selection");
        m_holdPrompt = false;
        WaitScript(inspector,"typeof btTest==='object'","page after cancellation is ready");
        Require(Operation(inspector,"devices").empty(),"cancelled chooser and stale answer grant no device");
        choose();
        auto alternate=m_base; auto host=alternate.find("127.0.0.1"); Require(host!=std::string::npos,"fixture uses two secure localhost origins"); alternate.replace(host,9,"localhost");
        Navigate(alternate+"/test.html"); WaitScript(inspector,"typeof btTest==='object'","second origin ready");
        Require(Operation(inspector,"devices").empty(),"another origin cannot see device grants");
        Navigate(page); WaitScript(inspector,"typeof btTest==='object'","original origin ready");
        auto devices=Operation(inspector,"devices"); Require(devices.size()==1,"grant survives navigation within the browsing session");
        Capture(m_capture);
        // Leave an open chooser to exercise context teardown cancellation too.
        m_holdPrompt=true; asked=m_requests.load(); Click(inspector,"request");
        Wait([&] {return m_requests > asked;},"pending chooser exists during view teardown");
    }
    std::atomic<unsigned> m_requests {0}, m_cancelled {0};
    std::atomic<uint64> m_lastRequest {0};
    std::atomic<bool> m_holdPrompt {false}, m_selectBusy {false};
    std::string m_base, m_bundle, m_capture;
    std::shared_ptr<BWebKitContext> m_context; std::shared_ptr<BWebKitInspectorSession> m_session;
    BWindow* m_window {nullptr}; BWebKitView* m_view {nullptr};
    std::thread m_worker; std::mutex m_mutex; BMessage m_state;
    std::atomic<bool> m_done {false}, m_failed {false}; bigtime_t m_closing {0}; bool m_finished {false};
};

int main(int argc, char** argv)
{
    if (argc != 4) { std::fprintf(stderr, "Usage: %s BASE_URL BUNDLE SCREENSHOT_PPM\n", argv[0]); return 2; }
    status_t status; BluetoothTests app(argv, status); if (status != B_OK) return 1;
    app.Run(); return app.Result();
}
