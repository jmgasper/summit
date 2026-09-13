#include <WebKit/WebKitView.h>
#include <Application.h>
#include <Entry.h>
#include <OS.h>
#include <String.h>
#include <Window.h>
#include <image.h>
#include <unistd.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>

namespace {
constexpr uint32 continueTests = 'ctnx';
constexpr bigtime_t stepTimeout = 60000000;
constexpr bigtime_t cleanupTimeout = 30000000;

struct Step {
    const char* name;
    size_t context;
    size_t tab;
    const char* expected;
    const char* write;
    size_t networkProcesses;
};

constexpr std::array steps {
    Step { "a_seed", 0, 0, "", "A", 1 },
    Step { "a_share", 0, 1, "A", "", 1 },
    Step { "b_seed", 1, 2, "", "B", 2 },
    Step { "a_after_b", 0, 0, "A", "", 2 },
    Step { "p_seed", 2, 3, "", "P", 3 },
    Step { "p_share", 2, 4, "P", "", 3 },
    Step { "q_seed", 3, 5, "", "Q", 4 },
    Step { "p_after_q", 2, 3, "P", "", 4 },
    Step { "a_reopen", 0, 0, "A", "", 1 },
    Step { "b_reopen", 1, 2, "B", "", 2 },
    Step { "p_reopen", 2, 3, "", "P2", 3 },
    Step { "p_reopen_share", 2, 4, "P2", "", 3 },
};

class TestWindow final : public BWindow {
public:
    explicit TestWindow(size_t index)
        : BWindow(BRect(70 + index * 22, 70 + index * 22, 590 + index * 22, 360 + index * 22),
            "Summit context tests", B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS) { }
    bool QuitRequested() override
    {
        be_app->PostMessage(B_QUIT_REQUESTED);
        return false;
    }
};

struct Helpers {
    std::set<team_id> network;
    std::set<team_id> web;
};

Helpers ChildHelpers()
{
    Helpers result;
    team_info team;
    int32 cookie = 0;
    while (get_next_team_info(&cookie, &team) == B_OK) {
        if (team.parent != getpid())
            continue;
        int32 imageCookie = 0;
        image_info image;
        while (get_next_image_info(team.team, &imageCookie, &image) == B_OK) {
            if (image.type != B_APP_IMAGE)
                continue;
            const char* leaf = std::strrchr(image.name, '/');
            leaf = leaf ? leaf + 1 : image.name;
            if (!std::strcmp(leaf, "NetworkProcess"))
                result.network.insert(team.team);
            else if (!std::strcmp(leaf, "WebProcess"))
                result.web.insert(team.team);
            break;
        }
    }
    return result;
}
} // namespace

class ModernContextTests final : public BApplication {
public:
    ModernContextTests(const char* baseURL, const char* run, const char* profileRoot, status_t& status)
        : BApplication("application/x-vnd.Kunanyi-Summit-ContextTests", &status)
        , m_baseURL(baseURL), m_run(run), m_profileRoot(profileRoot)
    {
        m_privateHint = m_profileRoot;
        m_privateHint << "/private-must-not-exist";
        m_contextPIDs.fill(-1);
    }

    void ReadyToRun() override
    {
        SetPulseRate(100000);
        if (BWebKitInitialize() != B_OK || m_profileRoot.IsEmpty() || m_profileRoot.ByteAt(0) != '/') {
            Fail("native initialization or absolute profile root is invalid");
            return;
        }
        if (BEntry(m_privateHint.String()).Exists()) {
            Fail("private path hint already exists before the test");
            return;
        }
        PostMessage(continueTests);
    }

    void MessageReceived(BMessage* message) override
    {
        if (message->what == continueTests) {
            if (!m_closing && !m_finished)
                StartStep();
            return;
        }
        if (message->what == B_WEBKIT_PROCESS_EXITED) {
            if (m_waiting)
                Fail("a content process exited during storage verification");
            return;
        }
        if (message->what == B_WEBKIT_STATE_CHANGED && m_waiting) {
            BMessenger view;
            if (message->FindMessenger("view", &view) != B_OK || view != m_waitingView)
                return;
            const char* title = message->FindString("title");
            if (!title)
                return;
            BString prefix("SUMMIT CONTEXT ");
            prefix << steps[m_step].name;
            BString failed(prefix);
            failed << " FAIL";
            if (!std::strcmp(title, failed.String())) {
                Fail("JavaScript storage assertion or HTTP report failed");
                return;
            }
            BString passed(prefix);
            passed << " PASS";
            bool loading = true;
            if (message->FindBool("loading", &loading) != B_OK || loading || std::strcmp(title, passed.String()))
                return;
            CompleteStep();
            return;
        }
        BApplication::MessageReceived(message);
    }

    void Pulse() override
    {
        if (m_finished)
            return;
        if (m_waiting && system_time() - m_stepStarted > stepTimeout) {
            Fail("step exceeded its 60 second deadline");
            return;
        }
        if (!m_closing)
            return;
        bool closed = CloseWindows();
        auto helpers = ChildHelpers();
        if (closed && helpers.network.empty() && helpers.web.empty()) {
            if (BEntry(m_privateHint.String()).Exists()) {
                std::fputs("FAIL private path hint exists after context teardown\n", stderr);
                m_failed = true;
            }
            m_closing = false;
            m_knownNetworkPIDs.clear();
            m_contextPIDs.fill(-1);
            std::puts("TEARDOWN all native WebKit helpers exited; private path hint absent check complete");
            if (m_failed || m_step == steps.size())
                Finish();
            else
                PostMessage(continueTests);
            return;
        }
        if (system_time() - m_cleanupStarted > cleanupTimeout) {
            std::fprintf(stderr, "FAIL teardown deadline: %zu network helpers and %zu content helpers remain\n",
                helpers.network.size(), helpers.web.size());
            m_failed = true;
            Finish();
        }
    }

    bool QuitRequested() override
    {
        if (m_finished)
            return true;
        Fail("application quit requested before verification finished");
        return false;
    }

    int Result() const { return m_result; }

private:
    struct Tab { TestWindow* window { nullptr }; BWebKitView* view { nullptr }; };

    void StartStep()
    {
        const auto& step = steps[m_step];
        std::printf("BEGIN %s\n", step.name);
        std::fflush(stdout);
        if (!m_contexts[step.context]) {
            BString path(m_profileRoot);
            bool isPrivate = step.context >= 2;
            if (isPrivate)
                path = m_privateHint;
            else
                path << (step.context ? "/profile-b" : "/profile-a");
            auto context = std::make_shared<BWebKitContext>(path.String(), isPrivate);
            if (context->InitCheck() != B_OK || context->IsPrivate() != isPrivate) {
                std::fprintf(stderr, "Context InitCheck=%ld\n", static_cast<long>(context->InitCheck()));
                Fail("native context initialization failed");
                return;
            }
            m_contexts[step.context] = std::move(context);
        }
        auto& tab = m_tabs[step.tab];
        if (!tab.window) {
            tab.window = new TestWindow(step.tab);
            tab.view = new BWebKitView(tab.window->Bounds(), "context-test", BMessenger(this),
                B_FOLLOW_ALL, m_contexts[step.context]);
            tab.window->AddChild(tab.view);
            if (tab.view->InitCheck() != B_OK) {
                Fail("native web view initialization failed");
                return;
            }
            tab.window->Show();
        }
        BString url(m_baseURL);
        url << "/context-isolation.html?run=" << m_run << "&step=" << step.name
            << "&expected=" << step.expected << "&write=" << step.write;
        if (tab.window->LockWithTimeout(1000000) != B_OK) {
            Fail("could not lock native test window");
            return;
        }
        m_waitingView = BMessenger(tab.view);
        m_waiting = true;
        m_stepStarted = system_time();
        tab.view->LoadURL(url.String());
        tab.window->Unlock();
    }

    void CompleteStep()
    {
        const auto& step = steps[m_step];
        auto helpers = ChildHelpers();
        if (helpers.network.size() != step.networkProcesses) {
            std::fprintf(stderr, "Expected %zu network helpers, found %zu\n", step.networkProcesses, helpers.network.size());
            Fail("network processes are not separated by context");
            return;
        }
        for (team_id pid : m_knownNetworkPIDs) {
            if (!helpers.network.contains(pid)) {
                Fail("an existing context's network process unexpectedly changed");
                return;
            }
        }
        if (m_contextPIDs[step.context] < 0) {
            std::set<team_id> added;
            for (team_id pid : helpers.network) {
                if (!m_knownNetworkPIDs.contains(pid))
                    added.insert(pid);
            }
            if (added.size() != 1) {
                Fail("a new context did not obtain one distinct network process");
                return;
            }
            m_contextPIDs[step.context] = *added.begin();
        }
        if (!helpers.network.contains(m_contextPIDs[step.context])) {
            Fail("shared-context tabs lost their network process");
            return;
        }
        m_knownNetworkPIDs = std::move(helpers.network);
        std::printf("STEP %s PASS network_pid=%ld\n", step.name, static_cast<long>(m_contextPIDs[step.context]));
        std::fflush(stdout);
        m_waiting = false;
        ++m_step;
        if (m_step == 8 || m_step == steps.size())
            BeginCleanup();
        else
            PostMessage(continueTests);
    }

    bool CloseWindows()
    {
        bool complete = true;
        for (auto& tab : m_tabs) {
            if (!tab.window)
                continue;
            if (tab.window->LockWithTimeout(100000) != B_OK) {
                complete = false;
                continue;
            }
            auto* window = tab.window;
            tab = { };
            window->Quit();
        }
        if (complete) {
            for (auto& context : m_contexts)
                context.reset();
        }
        return complete;
    }

    void BeginCleanup()
    {
        m_waiting = false;
        if (m_closing)
            return;
        m_closing = true;
        m_cleanupStarted = system_time();
        CloseWindows();
        // Destroying native views queues application-thread WebKit teardown.
        // Pulse waits for those callbacks and their helper processes before
        // reopening contexts or permitting the application loop to exit.
    }

    void Fail(const char* reason)
    {
        std::fprintf(stderr, "FAIL %s: %s\n", m_step < steps.size() ? steps[m_step].name : "cleanup", reason);
        m_failed = true;
        BeginCleanup();
    }

    void Finish()
    {
        m_result = !m_failed && m_step == steps.size() ? 0 : 1;
        std::printf("CONTEXT_RESULT %s steps=%zu\n", m_result ? "FAIL" : "PASS", m_step);
        std::fflush(stdout);
        m_finished = true;
        PostMessage(B_QUIT_REQUESTED);
    }

    BString m_baseURL;
    BString m_run;
    BString m_profileRoot;
    BString m_privateHint;
    std::array<std::shared_ptr<BWebKitContext>, 4> m_contexts;
    std::array<Tab, 6> m_tabs;
    std::array<team_id, 4> m_contextPIDs;
    std::set<team_id> m_knownNetworkPIDs;
    BMessenger m_waitingView;
    size_t m_step { 0 };
    bigtime_t m_stepStarted { 0 };
    bigtime_t m_cleanupStarted { 0 };
    bool m_waiting { false };
    bool m_closing { false };
    bool m_finished { false };
    bool m_failed { false };
    int m_result { 1 };
};

int main(int argc, char** argv)
{
    if (argc != 4) {
        std::fputs("Usage: ModernContextTests BASE_URL RUN_TOKEN ABSOLUTE_PROFILE_ROOT\n", stderr);
        return 2;
    }
    status_t status = B_NO_INIT;
    ModernContextTests tests(argv[1], argv[2], argv[3], status);
    if (status != B_OK) {
        std::fprintf(stderr, "FAIL initialize the native context test application: %s\n", std::strerror(status));
        return 1;
    }
    tests.Run();
    return tests.Result();
}
