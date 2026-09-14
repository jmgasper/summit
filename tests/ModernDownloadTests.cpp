#include <WebKit/WebKitView.h>
#include <Application.h>
#include <OS.h>
#include <Window.h>
#include <image.h>
#include <unistd.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>

namespace {
constexpr uint32 nextStep = 'dtnx';
enum class Action { Direct, Navigate, Cancel, Crash, CloseView };
struct Step { const char* name; Action action; uint32 result; };
constexpr std::array steps {
    Step { "direct", Action::Direct, B_WEBKIT_DOWNLOAD_SUCCEEDED },
    Step { "collision", Action::Navigate, B_WEBKIT_DOWNLOAD_SUCCEEDED },
    Step { "unsupported", Action::Navigate, B_WEBKIT_DOWNLOAD_SUCCEEDED },
    Step { "cancel", Action::Cancel, B_WEBKIT_DOWNLOAD_CANCELLED },
    Step { "truncated", Action::Direct, B_WEBKIT_DOWNLOAD_FAILED },
    Step { "crash", Action::Crash, B_WEBKIT_DOWNLOAD_FAILED },
    Step { "recovery", Action::Direct, B_WEBKIT_DOWNLOAD_SUCCEEDED },
    Step { "view-closed", Action::CloseView, B_WEBKIT_DOWNLOAD_SUCCEEDED },
};
std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}
std::set<team_id> Helpers(const std::string& bundle, const char* only = nullptr)
{
    std::set<team_id> result;
    team_info team;
    int32 cookie = 0;
    while (get_next_team_info(&cookie, &team) == B_OK) {
        if (team.parent != getpid()) continue;
        int32 imageCookie = 0;
        image_info image;
        while (get_next_image_info(team.team, &imageCookie, &image) == B_OK) {
            if (image.type != B_APP_IMAGE) continue;
            for (const char* leaf : { "NetworkProcess", "WebProcess" }) {
                if ((!only || !std::strcmp(leaf, only)) && bundle + "/" + leaf == image.name)
                    result.insert(team.team);
            }
            break;
        }
    }
    return result;
}
}

class ModernDownloadTests final : public BApplication {
public:
    ModernDownloadTests(const char* url, const char* directory, const char* bundle, status_t& status)
        : BApplication("application/x-vnd.Kunanyi-Summit-DownloadTests", &status)
        , m_url(url), m_directory(directory), m_bundle(bundle) { }
    int Result() const { return m_failed ? 1 : 0; }
    void ReadyToRun() override
    {
        SetPulseRate(100000);
        if (!Check(BWebKitInitialize() == B_OK, "initialize native WebKit")) return;
        m_context = std::make_shared<BWebKitContext>(nullptr, true);
        if (!Check(m_context->InitCheck() == B_OK, "create private download context")) return;
        if (!Check(m_context->SetDownloadDirectory("relative") == B_BAD_VALUE,
            "reject relative download directory")) return;
        if (!Check(m_context->SetDownloadDirectory(m_directory.c_str()) == B_OK
            && m_context->SetDownloadListener(BMessenger(this)) == B_OK,
            "configure native destination and application listener")) return;
        PostMessage(nextStep);
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what == nextStep) { StartStep(); return; }
        if (message->what != B_WEBKIT_DOWNLOAD_STARTED && message->what != B_WEBKIT_DOWNLOAD_PROGRESS
            && message->what != B_WEBKIT_DOWNLOAD_FINISHED) {
            BApplication::MessageReceived(message);
            return;
        }
        uint64 id = 0;
        if (!Check(message->FindUInt64("identifier", &id) == B_OK && id,
            "download notification carries an identifier")) return;
        if (m_closing) return;
        if (!Check(!m_completed.contains(id), "completed download emits no later notifications")) return;
        if (message->what == B_WEBKIT_DOWNLOAD_STARTED) {
            if (!Check(!m_identifier, "one start notification for the requested download")) return;
            m_identifier = id;
            // Small transfers can finish before the queued STARTED notification
            // reaches this looper. Only the deliberately slow fixtures promise
            // that the operation is still pending at notification delivery.
            if (steps[m_step].action == Action::Cancel || steps[m_step].action == Action::Crash) {
                if (!Check(m_context->HasPendingDownloads()
                    && m_context->SetDownloadDirectory(m_directory.c_str()) == B_BUSY,
                    "active download retains context state and prevents reconfiguration")) return;
            }
            if (steps[m_step].action == Action::CloseView) CloseWindow();
            return;
        }
        if (!Check(id == m_identifier, "progress and completion match the started download")) return;
        uint64 bytes = 0;
        int64 expected = -1;
        const char* path = nullptr;
        if (!Check(message->FindUInt64("current_size", &bytes) == B_OK
            && message->FindInt64("expected_size", &expected) == B_OK
            && message->FindString("path", &path) == B_OK && bytes >= m_bytes,
            "native byte counters are present and monotonic")) return;
        m_bytes = bytes;
        if (path && *path) {
            m_path = path;
            if (!Check(m_path.parent_path() == m_directory,
                "download destination stays inside the configured directory")) return;
        }
        if (message->what == B_WEBKIT_DOWNLOAD_PROGRESS) {
            if (!bytes || m_interrupted) return;
            if (steps[m_step].action == Action::Cancel) {
                m_interrupted = true;
                m_context->CancelDownload(id);
            } else if (steps[m_step].action == Action::Crash) {
                auto helpers = Helpers(m_bundle, "NetworkProcess");
                if (!Check(helpers.size() == 1, "find the exact owned network executable before interruption")) return;
                m_interrupted = true;
                Check(kill_team(*helpers.begin()) == B_OK, "interrupt the owned transfer process");
            }
            return;
        }
        uint32 result = ~0U;
        int32 error = B_OK;
        if (!Check(message->FindUInt32("result", &result) == B_OK
            && message->FindInt32("error", &error) == B_OK && result == steps[m_step].result,
            "terminal result matches the transfer outcome")) return;
        if (result == B_WEBKIT_DOWNLOAD_SUCCEEDED) {
            auto payload = std::string("summit download ") + steps[m_step].name + "\n";
            if (!Check(!m_path.empty() && ReadFile(m_path) == payload
                && bytes == payload.size() && expected == static_cast<int64>(payload.size()),
                "completed file and final byte counts match the exact HTTP body")) return;
            if (m_step == 0) m_firstPath = m_path;
            if (m_step == 1 && !Check(m_path != m_firstPath
                && ReadFile(m_firstPath) == "summit download direct\n",
                "colliding attachment preserves the existing file")) return;
        } else {
            if (!Check(error != B_OK, "failed or cancelled transfer reports its error")) return;
            if (steps[m_step].action == Action::Cancel && !Check(m_interrupted && !m_path.empty()
                && !std::filesystem::exists(m_path), "cancel removes the observed partial file")) return;
            if (steps[m_step].action == Action::Crash && !Check(m_interrupted,
                "failure follows the intentional process interruption")) return;
        }
        m_completed.insert(id);
        m_terminal = true;
    }
    void Pulse() override
    {
        if (m_finished) return;
        if (m_closing) {
            if (m_context && m_context->HasPendingDownloads()) {
                m_context->CancelAllDownloads();
            } else {
                CloseWindow();
                m_context.reset();
                if (Helpers(m_bundle).empty() && !BWebKitHasPendingNativeUI()) {
                    m_finished = true;
                    std::printf("DOWNLOAD_RESULT %s steps=%zu checks=%u\n",
                        m_failed ? "FAIL" : "PASS", m_step, m_checks);
                    PostMessage(B_QUIT_REQUESTED);
                    return;
                }
            }
            if (system_time() - m_started > 30000000) {
                std::fputs("FAIL native download teardown deadline\n", stderr);
                _Exit(1);
            }
            return;
        }
        if (system_time() - m_started > 30000000) {
            Check(false, "download completes before its deadline");
            return;
        }
        if (m_terminal && !m_context->HasPendingDownloads()) {
            std::printf("STEP %s PASS\n", steps[m_step].name);
            std::fflush(stdout);
            ++m_step;
            m_terminal = false;
            if (m_step == steps.size()) BeginCleanup();
            else PostMessage(nextStep);
        }
    }
    bool QuitRequested() override { if (!m_finished) BeginCleanup(); return m_finished; }
private:
    bool Check(bool value, const char* text)
    {
        ++m_checks;
        std::printf("%s %s\n", value ? "PASS" : "FAIL", text);
        std::fflush(stdout);
        if (!value) { m_failed = true; BeginCleanup(); }
        return value;
    }
    void StartStep()
    {
        if (m_closing || m_step >= steps.size()) return;
        m_identifier = m_bytes = 0;
        m_path.clear();
        m_interrupted = false;
        m_started = system_time();
        if (!m_window) {
            m_window = new BWindow(BRect(80, 80, 680, 430), "Summit download tests",
                B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS);
            m_view = new BWebKitView(m_window->Bounds(), "download-page", BMessenger(this), B_FOLLOW_ALL, m_context);
            m_window->AddChild(m_view);
            if (!Check(m_view->InitCheck() == B_OK, "create native download view")) return;
            m_window->Show();
        }
        if (!Check(m_window->Lock(), "lock native view to request download")) return;
        const auto url = m_url + "?case=" + steps[m_step].name;
        if (steps[m_step].action == Action::Navigate) m_view->LoadURL(url.c_str());
        else m_view->DownloadURL(url.c_str());
        m_window->Unlock();
    }
    void CloseWindow()
    {
        if (!m_window || !m_window->Lock()) return;
        auto* window = m_window;
        m_window = nullptr;
        m_view = nullptr;
        window->Quit();
    }
    void BeginCleanup()
    {
        if (m_closing) return;
        m_closing = true;
        m_started = system_time();
        if (m_context) m_context->CancelAllDownloads();
    }
    std::string m_url, m_directory, m_bundle;
    std::shared_ptr<BWebKitContext> m_context;
    BWindow* m_window { nullptr };
    BWebKitView* m_view { nullptr };
    std::filesystem::path m_path, m_firstPath;
    std::set<uint64> m_completed;
    size_t m_step { 0 };
    unsigned m_checks { 0 };
    uint64 m_identifier { 0 }, m_bytes { 0 };
    bigtime_t m_started { 0 };
    bool m_failed { false }, m_closing { false }, m_finished { false };
    bool m_terminal { false }, m_interrupted { false };
};

int main(int argc, char** argv)
{
    if (argc != 4) return 2;
    status_t status;
    ModernDownloadTests app(argv[1], argv[2], argv[3], status);
    if (status != B_OK) return 1;
    app.Run();
    return app.Result();
}
