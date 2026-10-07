// Real native picker cancellation and stale-answer regression; no device access.
#define SUMMIT_PDF_HELPERS_ONLY
#include "ModernPDFTests.cpp"
#include "ui/SitePermissions.h"
#include <Button.h>
#include <ListView.h>
#include <Roster.h>
#include <StringView.h>
class PickerTests final : public BApplication {
public:
    explicit PickerTests(const char* capture) : BApplication("application/x-vnd.Summit-BluetoothPickerTests"), m_capture(capture) { }
    int Result() const { return m_failed ? 1 : 0; }
    void ReadyToRun() override
    {
        try {
            Require(BWebKitInitialize() == B_OK,"initialize picker test engine");
            m_context=std::make_shared<BWebKitContext>(nullptr,true);
            Require(m_context->InitCheck()==B_OK,"create private picker context");
            m_service=std::make_unique<summit::SitePermissionService>(m_context,true,"/tmp/Summit-BluetoothPickerTests",
                [this](auto&,auto&,auto){m_saved=true;});
            AddHandler(m_service.get());
            m_window=new BWindow(BRect(1500,160,2300,660),"Summit Bluetooth picker tests",B_TITLED_WINDOW,B_ASYNCHRONOUS_CONTROLS);
            m_view=new BStringView(BRect(20,20,750,55),"fixture","Native Bluetooth permission and cancellation tests");
            m_window->AddChild(m_view);m_window->Show();be_roster->ActivateApp(Team());
            Ask(1); SetPulseRate(300000);
        } catch(const std::exception& error) { Fail(error.what()); }
    }
    BWindow* Picker()
    {
        for(int32 i=0;i<CountWindows();++i) if(auto* window=WindowAt(i);window && std::string(window->Title())=="Connect Bluetooth Device") return window;
        return nullptr;
    }
    void Ask(uint64 identifier)
    {
        BMessage request(B_WEBKIT_PERMISSION_REQUESTED); request.AddUInt64("identifier",identifier);
        request.AddMessenger("view",BMessenger(m_view));
        request.AddString("permission","bluetooth"); request.AddString("origin","https://bluetooth-test.invalid");
        request.AddString("device_id","virtual-first"); request.AddString("device_label","Summit Test Peripheral");
        request.AddString("device_id","virtual-second"); request.AddString("device_label","Summit Test Busy Peripheral");
        m_service->MessageReceived(&request);
    }
    void Cancel(uint64 identifier)
    {
        BMessage request(B_WEBKIT_PERMISSION_CANCELLED);request.AddUInt64("identifier",identifier);m_service->MessageReceived(&request);
    }
    void Pulse() override
    {
        try {
            if(++m_ticks>35) throw std::runtime_error("native picker timeout");
            auto* window=Picker();
            switch(m_stage) {
            case 0:
                Require(window && window->Lock(),"Bluetooth picker opens");
                { auto* list=dynamic_cast<BListView*>(window->FindView("choices")); auto* button=dynamic_cast<BButton*>(window->FindView("share"));
                  Require(list && list->CountItems()==2,"picker lists both offered devices");
                  Require(list->Bounds().Height()>=list->ItemAt(0)->Height()*2,"device rows fit in the visible list");
                  Require(button && std::string(button->Label())=="Connect","picker uses Connect action"); }
                window->Unlock(); Capture(m_capture); Cancel(999); Ask(2); ++m_stage; break;
            case 1: Require(window,"unrelated cancellation and duplicate request preserve existing picker");Cancel(1);++m_stage;break;
            case 2: if(window)return;Require(true,"exact cancellation closes native picker");Ask(3);++m_stage;break;
            case 3:
                Require(window,"replacement picker opens");
                { BMessage answer('sppa');answer.AddString("permission","bluetooth");answer.AddString("origin","https://bluetooth-test.invalid");
                  answer.AddUInt64("identifier",1);answer.AddInt32("which",2);answer.AddString("device_id","virtual-first");m_service->MessageReceived(&answer); }
                ++m_stage;break;
            case 4:
                Require(window && window->Lock(),"stale response cannot answer replacement prompt");
                { auto* button=dynamic_cast<BButton*>(window->FindView("share"));Require(button && button->Invoke()==B_OK,"invoke native Connect action"); }
                window->Unlock();++m_stage;break;
            case 5:
                if(window)return;
                Require(!m_service->AnswerOpenPrompt(2,0),"selected prompt is removed");
                Require(!m_saved,"Bluetooth approval is not saved as generic site permission");
                Ask(4);++m_stage;break;
            case 6: Require(window,"another request can open after selection");window->PostMessage(B_QUIT_REQUESTED);++m_stage;break;
            case 7:
                if(window)return;
                Require(!m_service->AnswerOpenPrompt(2,0),"closing picker settles its request");
                RemoveHandler(m_service.get());m_service.reset();m_context.reset();
                if(m_window->Lock())m_window->Quit();m_window=nullptr;m_view=nullptr;
                std::printf("BLUETOOTH_PICKER_PASS checks=%u\n",checks);PostMessage(B_QUIT_REQUESTED);break;
            }
        } catch(const std::exception& error) { Fail(error.what()); }
    }
private:
    void Fail(const char* error) { std::fprintf(stderr,"BLUETOOTH_PICKER_FAIL %s\n",error);m_failed=true;PostMessage(B_QUIT_REQUESTED); }
    std::shared_ptr<BWebKitContext> m_context;std::unique_ptr<summit::SitePermissionService> m_service;
    BWindow* m_window=nullptr;BView* m_view=nullptr;
    std::string m_capture;unsigned m_stage=0,m_ticks=0;bool m_failed=false,m_saved=false;
};
int main(int argc,char** argv) { if(argc!=2)return 2;PickerTests app(argv[1]);app.Run();return app.Result(); }
