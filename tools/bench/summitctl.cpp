// Team-targeted scripting helper for Summit benchmarks and stress tests.
//
// Unlike tests/BrowserProbe.cpp this never addresses Summit by signature alone:
// other engineers run their own Summit instances in the same VM, so every
// command requires the team id (== pid) of the instance the caller launched.
//
//   summitctl --team ID state                 one JSON line with window/tab state
//   summitctl --team ID navigate URL
//   summitctl --team ID newtab URL
//   summitctl --team ID closetab [TABID]
//   summitctl --team ID selecttab TABID
//   summitctl --team ID back|forward|reload|sidebar
//   summitctl --team ID frame LEFT TOP RIGHT BOTTOM   standard BWindow "Frame" scripting property
//   summitctl --team ID framestats            frame counts since the targeted scroll burst began
//   summitctl --team ID quit                  B_QUIT_REQUESTED to that team only
//   summitctl find NAME                       every team whose application image is called NAME
//                                             (Summit is single-launch: a second instance forwards
//                                             its URL to the first, whatever --profile says)
//   summitctl group TEAM                      teams in TEAM's process group with full image paths
//                                             (ps and team_info.args truncate long bundle paths)
//   summitctl sample INTERVAL_MS [TEAM...]    system CPU/memory + per-team CPU, threads and
//                                             (for the listed teams) area memory; needs no Summit
//
// Exit codes: 0 ok, 2 usage, 3 team is not a running Summit, 4 no reply in time
// (hang indicator), 5 send failed.
#include "ui/Messages.h"
#include <Application.h>
#include <Message.h>
#include <Messenger.h>
#include <Rect.h>
#include <OS.h>
#include <image.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

static std::string Escape(const char* text)
{
    std::string out;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text ? text : ""); *p; ++p) {
        char buffer[8];
        switch (*p) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (*p < 0x20) { std::snprintf(buffer, sizeof buffer, "\\u%04x", *p); out += buffer; }
                else out += static_cast<char>(*p);
        }
    }
    return out;
}

static const char* String(const BMessage& message, const char* name)
{
    const char* value = "";
    message.FindString(name, &value);
    return value;
}

static std::string AppImage(team_id team)
{
    int32 cookie = 0; image_info image;
    while (get_next_image_info(team, &cookie, &image) == B_OK)
        if (image.type == B_APP_IMAGE) return image.name;
    return {};
}

static int Find(int argc, char** argv)
{
    if (argc < 3) return 2;
    const std::string wanted = std::string("/") + argv[2];
    int32 cookie = 0; team_info info; bool comma = false;
    std::printf("[");
    while (get_next_team_info(&cookie, &info) == B_OK) {
        if (info.team <= 1) continue;
        const std::string image = AppImage(info.team);
        if (image.size() < wanted.size() || image.compare(image.size() - wanted.size(), wanted.size(), wanted)) continue;
        std::printf("%s{\"team\":%ld,\"group\":%ld,\"image\":\"%s\"}", comma ? "," : "", long(info.team),
            long(getpgid(info.team)), Escape(image.c_str()).c_str());
        comma = true;
    }
    std::puts("]");
    return 0;
}

static int Group(int argc, char** argv)
{
    if (argc < 3) return 2;
    const pid_t group = static_cast<pid_t>(std::strtol(argv[2], nullptr, 10));
    int32 cookie = 0; team_info info; bool comma = false;
    std::printf("[");
    while (get_next_team_info(&cookie, &info) == B_OK) {
        if (info.team <= 1 || getpgid(info.team) != group) continue;
        std::printf("%s{\"team\":%ld,\"image\":\"%s\",\"threads\":%ld}", comma ? "," : "", long(info.team),
            Escape(AppImage(info.team).c_str()).c_str(), long(info.thread_count));
        comma = true;
    }
    std::puts("]");
    return 0;
}

// One JSON line: whole-system CPU utilisation over the interval, memory totals,
// every team that used more than 2% of a core, and detailed rows for TEAM...
static int Sample(int argc, char** argv)
{
    if (argc < 3) return 2;
    const bigtime_t interval = bigtime_t(std::max(50L, std::strtol(argv[2], nullptr, 10))) * 1000;
    std::set<team_id> wanted;
    for (int i = 3; i < argc; ++i) wanted.insert(static_cast<team_id>(std::strtol(argv[i], nullptr, 10)));
    system_info system;
    get_system_info(&system);
    const uint32 cpus = system.cpu_count;
    std::vector<cpu_info> before(cpus), after(cpus);
    auto teamTimes = [] {
        std::map<team_id, bigtime_t> times;
        int32 cookie = 0; team_info info;
        while (get_next_team_info(&cookie, &info) == B_OK) {
            team_usage_info usage;
            if (get_team_usage_info(info.team, B_TEAM_USAGE_SELF, &usage) == B_OK)
                times[info.team] = usage.user_time + usage.kernel_time;
        }
        return times;
    };
    get_cpu_info(0, cpus, before.data());
    const auto first = teamTimes();
    const bigtime_t start = system_time();
    snooze(interval);
    get_cpu_info(0, cpus, after.data());
    const auto second = teamTimes();
    const double elapsed = double(system_time() - start);
    double busy = 0;
    for (uint32 i = 0; i < cpus; ++i) busy += double(after[i].active_time - before[i].active_time);
    get_system_info(&system);
    std::printf("{\"cpuCount\":%u,\"intervalMicros\":%.0f,\"busyCores\":%.3f,\"usedBytes\":%llu,"
        "\"cachedBytes\":%llu,\"maxBytes\":%llu,\"teamCount\":%u,\"threadCount\":%u,\"teams\":[",
        cpus, elapsed, busy / elapsed, (unsigned long long)system.used_pages * B_PAGE_SIZE,
        (unsigned long long)system.cached_pages * B_PAGE_SIZE, (unsigned long long)system.max_pages * B_PAGE_SIZE,
        system.used_teams, system.used_threads);
    bool comma = false;
    for (const auto& [id, time] : second) {
        const auto old = first.find(id);
        const double cores = old == first.end() ? 0 : double(time - old->second) / elapsed;
        const bool detailed = wanted.count(id) != 0;
        if (!detailed && cores < 0.02) continue;
        team_info info;
        if (get_team_info(id, &info) != B_OK) continue;
        std::printf("%s{\"team\":%ld,\"cores\":%.3f,\"threads\":%ld,\"areas\":%ld,\"cpuMicros\":%lld,\"args\":\"%s\"",
            comma ? "," : "", long(id), cores, long(info.thread_count), long(info.area_count),
            static_cast<long long>(time), Escape(info.args).c_str());
        comma = true;
        std::printf(",\"image\":\"%s\"", Escape(AppImage(id).c_str()).c_str());
        if (detailed) {
            ssize_t cookie = 0; area_info area; unsigned long long ram = 0, size = 0;
            while (get_next_area_info(id, &cookie, &area) == B_OK) { ram += area.ram_size; size += area.size; }
            std::printf(",\"ramBytes\":%llu,\"virtualBytes\":%llu", ram, size);
        }
        std::printf("}");
    }
    std::puts("]}");
    return 0;
}

int main(int argc, char** argv)
{
    if (argc > 1 && !std::strcmp(argv[1], "sample")) return Sample(argc, argv);
    if (argc > 1 && !std::strcmp(argv[1], "group")) return Group(argc, argv);
    if (argc > 1 && !std::strcmp(argv[1], "find")) return Find(argc, argv);
    team_id team = -1;
    bigtime_t timeout = 5000000;
    int index = 1;
    while (index + 1 < argc && argv[index][0] == '-') {
        char* end = nullptr;
        errno = 0;
        long parsed = std::strtol(argv[index + 1], &end, 10);
        if (errno || *end || parsed <= 0 || parsed > INT_MAX) return 2;
        if (!std::strcmp(argv[index], "--team")) team = static_cast<team_id>(parsed);
        else if (!std::strcmp(argv[index], "--timeout-ms")) timeout = bigtime_t(parsed) * 1000;
        else return 2;
        index += 2;
    }
    if (team < 0 || index >= argc) {
        std::fputs("usage: summitctl --team ID [--timeout-ms N] state|navigate URL|newtab URL|closetab [ID]|selecttab ID|back|forward|reload|scroll N MS DELTA|framestats|quit\n", stderr);
        return 2;
    }
    const std::string command = argv[index++];
    const char* argument = index < argc ? argv[index] : nullptr;

    status_t status = B_NO_INIT;
    BApplication application("application/x-vnd.Kunanyi-Summit-benchctl", &status);
    if (status != B_OK) { std::fprintf(stderr, "BApplication: %s\n", std::strerror(status)); return 5; }
    BMessenger app("application/x-vnd.Kunanyi-Summit", team, &status);
    if (status != B_OK || !app.IsValid() || app.Team() != team) {
        std::fprintf(stderr, "team %ld is not a running Summit: %s\n", long(team), std::strerror(status));
        return 3;
    }
    if (command == "quit") {
        BMessage quit(B_QUIT_REQUESTED);
        return app.SendMessage(&quit, static_cast<BHandler*>(nullptr), timeout) == B_OK ? 0 : 5;
    }
    BMessage request(B_GET_PROPERTY), reply;
    request.AddSpecifier("Window", int32(0));
    BMessenger window;
    status = app.SendMessage(&request, &reply, timeout, timeout);
    if (status == B_TIMED_OUT || status == B_WOULD_BLOCK) { std::fputs("application did not reply\n", stderr); return 4; }
    if (status != B_OK || reply.FindMessenger("result", &window) != B_OK || !window.IsValid()) {
        std::fprintf(stderr, "no browser window: %s\n", std::strerror(status));
        return 5;
    }
    if (command == "state") {
        BMessage ask(summit::kBrowserState), state;
        const bigtime_t before = system_time();
        status = window.SendMessage(&ask, &state, timeout, timeout);
        const bigtime_t elapsed = system_time() - before;
        if (status == B_TIMED_OUT || status == B_WOULD_BLOCK) { std::fputs("window did not reply\n", stderr); return 4; }
        if (status != B_OK) { std::fprintf(stderr, "state: %s\n", std::strerror(status)); return 5; }
        int32 count = -1; int64 selected = -1; bool closing = false, scrollActive = false;
        state.FindInt32("count", &count); state.FindInt64("selected", &selected); state.FindBool("closing", &closing);
        state.FindBool("scroll_active", &scrollActive);
        std::printf("{\"team\":%ld,\"replyMicros\":%lld,\"count\":%ld,\"selected\":%lld,\"closing\":%s,"
            "\"scrollActive\":%s,\"scrollRequested\":%ld,\"scrollSent\":%ld,\"scrollStatus\":%ld,\"scrollDurationMicros\":%lld,"
            "\"address\":\"%s\",\"status\":\"%s\",\"backend\":\"%s\",\"webkit\":\"%s\",\"haikuWebkit\":\"%s\","
            "\"webkitRevision\":\"%s\",\"tabs\":[",
            long(team), static_cast<long long>(elapsed), long(count), static_cast<long long>(selected),
            closing ? "true" : "false", scrollActive ? "true" : "false",
            long(state.GetInt32("scroll_requested", 0)), long(state.GetInt32("scroll_sent", 0)),
            long(state.GetInt32("scroll_status", 0)), static_cast<long long>(state.GetInt64("scroll_duration_us", 0)),
            Escape(String(state, "address")).c_str(), Escape(String(state, "status")).c_str(),
            Escape(String(state, "backend")).c_str(), Escape(String(state, "webkit")).c_str(),
            Escape(String(state, "haiku_webkit")).c_str(), Escape(String(state, "webkit_revision")).c_str());
        BMessage tab;
        for (int32 i = 0; state.FindMessage("tab", i, &tab) == B_OK; ++i) {
            int64 id = -1; bool loading = false, loadError = false;
            tab.FindInt64("id", &id); tab.FindBool("loading", &loading); tab.FindBool("loadError", &loadError);
            std::printf("%s{\"id\":%lld,\"url\":\"%s\",\"title\":\"%s\",\"loading\":%s,\"loadError\":%s,"
                "\"loadErrorText\":\"%s\",\"loadOutcome\":\"%s\"}",
                i ? "," : "", static_cast<long long>(id), Escape(String(tab, "url")).c_str(),
                Escape(String(tab, "title")).c_str(), loading ? "true" : "false", loadError ? "true" : "false",
                Escape(String(tab, "loadErrorText")).c_str(), Escape(String(tab, "loadOutcome")).c_str());
        }
        std::puts("]}");
        return 0;
    }
    if (command == "frame") {
        if (index + 3 >= argc) return 2;
        BMessage set(B_SET_PROPERTY), done;
        set.AddSpecifier("Frame");
        set.AddRect("data", BRect(std::atof(argv[index]), std::atof(argv[index + 1]),
            std::atof(argv[index + 2]), std::atof(argv[index + 3])));
        status = window.SendMessage(&set, &done, timeout, timeout);
        if (status == B_TIMED_OUT || status == B_WOULD_BLOCK) return 4;
        return status == B_OK ? 0 : 5;
    }
    if (command == "scroll") {
        // scroll COUNT INTERVAL_MS DELTA - one wheel notch every INTERVAL_MS.
        if (index + 2 >= argc) return 2;
        BMessage burst(summit::kSimulateScroll), done;
        burst.AddInt32("count", int32(std::strtol(argv[index], nullptr, 10)));
        burst.AddInt32("interval_ms", int32(std::strtol(argv[index + 1], nullptr, 10)));
        burst.AddFloat("delta", float(std::atof(argv[index + 2])));
        status = window.SendMessage(&burst, &done, timeout, timeout);
        if (status == B_TIMED_OUT || status == B_WOULD_BLOCK) { std::fputs("window did not reply\n", stderr); return 4; }
        if (status != B_OK) { std::fprintf(stderr, "scroll: %s\n", std::strerror(status)); return 5; }
        const char* error = nullptr;
        if (done.FindString("error", &error) == B_OK && error) {
            std::fprintf(stderr, "scroll refused: %s\n", error);
            return 6;
        }
        std::printf("{\"count\":%ld,\"intervalMs\":%ld}\n",
            long(done.GetInt32("count", 0)), long(done.GetInt32("interval_ms", 0)));
        return 0;
    }
    if (command == "framestats") {
        BMessage ask(summit::kFrameStats), stats;
        status = window.SendMessage(&ask, &stats, timeout, timeout);
        if (status == B_TIMED_OUT || status == B_WOULD_BLOCK) { std::fputs("window did not reply\n", stderr); return 4; }
        if (status != B_OK) { std::fprintf(stderr, "framestats: %s\n", std::strerror(status)); return 5; }
        int64 elapsed = -1;
        if (stats.FindInt64("elapsed_us", &elapsed) != B_OK) { std::fputs("frame stats unavailable\n", stderr); return 6; }
        std::printf("{\"elapsedMicros\":%lld,\"frames\":%ld,\"longestGapMicros\":%lld,\"firstFrameDelayMicros\":%lld,\"longestInterframeGapMicros\":%lld,\"longGaps\":%ld,\"pendingGapMicros\":%lld,\"queueMaxMicros\":%lld,\"queueOver33\":%ld}\n",
            static_cast<long long>(elapsed), long(stats.GetInt32("frames", 0)),
            static_cast<long long>(stats.GetInt64("longest_gap_us", 0)),
            static_cast<long long>(stats.GetInt64("first_frame_delay_us", 0)),
            static_cast<long long>(stats.GetInt64("longest_interframe_gap_us", 0)),
            long(stats.GetInt32("long_gaps", 0)),
            static_cast<long long>(stats.GetInt64("pending_gap_us", 0)),
            static_cast<long long>(stats.GetInt64("queue_max_us", 0)), long(stats.GetInt32("queue_over_33", 0)));
        return 0;
    }
    uint32 what = 0;
    bool wantsURL = false, wantsID = false;
    if (command == "navigate") { what = summit::kNavigate; wantsURL = true; }
    else if (command == "newtab") { what = summit::kNewTab; wantsURL = true; }
    else if (command == "closetab") { what = summit::kCloseTab; wantsID = argument != nullptr; }
    else if (command == "selecttab") { what = summit::kSelectTab; wantsID = true; }
    else if (command == "back") what = summit::kBack;
    else if (command == "forward") what = summit::kForward;
    else if (command == "reload") what = summit::kReload;
    else if (command == "sidebar") what = summit::kToggleSidebar;
    else return 2;
    BMessage message(what);
    if (wantsURL) { if (!argument) return 2; message.AddString("url", argument); }
    if (wantsID) { if (!argument) return 2; message.AddInt64("id", std::strtoll(argument, nullptr, 10)); }
    status = window.SendMessage(&message, static_cast<BHandler*>(nullptr), timeout);
    if (status == B_TIMED_OUT || status == B_WOULD_BLOCK) { std::fputs("window port is full\n", stderr); return 4; }
    return status == B_OK ? 0 : 5;
}
