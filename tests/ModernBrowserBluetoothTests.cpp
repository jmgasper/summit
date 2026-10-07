// The actual Summit app: native chooser -> permission -> simulated GATT read.
#define SUMMIT_EXTENSION_MANAGER_HELPERS_ONLY
#include "ModernExtensionManagerTests.cpp"
int main(int argc,char** argv)
{
    if(argc!=4)return 2;
    status_t status;BApplication application("application/x-vnd.Summit-browser-bluetooth-tests",&status);if(status!=B_OK)return 1;
    auto team=static_cast<team_id>(std::strtol(argv[1],nullptr,10));BMessenger app;bool verified=false;
    try {
        app_info info;
        Require(Wait([&]{app=BMessenger(nullptr,team);return app.IsValid()&&be_roster->GetRunningAppInfo(team,&info)==B_OK;}),"owned browser registers");
        BPath executable(&info.ref);Require(std::filesystem::canonical(executable.Path())==std::filesystem::canonical(argv[2]),"owned executable matches Bluetooth build");verified=true;
        auto browser=Window(app,0);
        auto snapshot=[&]{auto state=State(browser);BMessage tab;if(state.FindMessage("tab",Index(state,Selected(state)),&tab)!=B_OK||tab.GetBool("loading",true))return json();auto title=String(tab,"title");return title.starts_with("BLUETOOTH ")?json::parse(title.substr(10),nullptr,false):json();};
        auto picker=[&]{return NamedWindow(app,"Connect Bluetooth Device");};
        auto open=[&]{auto report=snapshot();Require(report.is_object()&&report.contains("button"),"fixture reports its request button");auto point=report["button"];Click(Page(browser,Selected(State(browser))),BPoint(point[0].get<float>(),point[1].get<float>()));Require(Wait([&]{return picker().IsValid();}),"real browser click opens native Bluetooth picker");};
        Require(Wait([&]{return Has(snapshot(),"ready",true);}),"Bluetooth fixture loads in Summit");
        open();Button(picker(),"cancel");
        Require(Wait([&]{auto r=snapshot();return !picker().IsValid()&&r.is_object()&&r.contains("operation")&&r["operation"].is_object()&&r["operation"].value("error","")=="NotFoundError";}),"native Cancel rejects the JavaScript chooser");
        open();Button(picker(),"share");
        Require(Wait([&]{auto r=snapshot();return r.is_object()&&r.contains("operation")&&r["operation"].is_object()&&r["operation"].value("ok",false);}),"native Connect permits the page to perform its GATT read");
        auto result=snapshot()["operation"]["value"];
        Require(result["name"]=="Summit Test Peripheral"&&result["battery"]==88&&result["connected"]==true,"JavaScript receives the selected simulated device and battery value");
        open();Send(browser,summit::kNavigate,-1,"about:blank");
        Require(Wait([&]{return !picker().IsValid();}),"navigation dismisses the real native Bluetooth picker");
        Send(browser,summit::kNavigate,-1,argv[3]);Require(Wait([&]{return Has(snapshot(),"ready",true);}),"fresh Bluetooth fixture loads");
        open();Send(app,B_QUIT_REQUESTED);
        Require(Wait([&]{team_info state;return get_team_info(team,&state)==B_BAD_TEAM_ID;}),"Summit exits with a Bluetooth picker pending");
        std::printf("BROWSER_BLUETOOTH_PASS checks=%d\n",checks);return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"BROWSER_BLUETOOTH_FAIL checks=%d error=%s\n",checks,error.what());if(verified)Cleanup(app,team);return 1;}
}
