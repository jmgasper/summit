/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "APIContentRuleList.h"
#include "APIContentRuleListStore.h"
#include "WebKitView.h"
#include <Application.h>
#include <OS.h>
#include <wtf/FileSystem.h>
#include <wtf/RunLoop.h>
#include <wtf/text/MakeString.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <unistd.h>

using namespace WTF;
using WebCore::ContentExtensions::CSSSelectorsAllowed;

namespace {
std::atomic<unsigned> passed { 0 };
std::atomic<unsigned> failed { 0 };
std::atomic<unsigned> callbacks { 0 };
std::atomic<unsigned> destroyedCaptures { 0 };
void check(bool condition, const char* description)
{
    ++(condition ? passed : failed);
    printf("%s %s\n", condition ? "PASS" : "FAIL", description);
    fflush(stdout);
}

struct CompletionCapture {
    ~CompletionCapture()
    {
        check(RunLoop::isMain(), "completion capture is destroyed on the main loop");
        ++destroyedCaptures;
    }
};

constexpr auto originalRules = R"JSON([{"trigger":{"url-filter":"tracker"},"action":{"type":"block"}}])JSON"_s;
constexpr auto replacementRules = R"JSON([{"trigger":{"url-filter":"replacement"},"action":{"type":"block"}}])JSON"_s;
constexpr auto invalidRegex = R"JSON([{"trigger":{"url-filter":"["},"action":{"type":"block"}}])JSON"_s;
constexpr auto cssRules = R"JSON([{"trigger":{"url-filter":".*"},"action":{"type":"css-display-none","selector":".advert"}}])JSON"_s;

class StoreTests final : public BApplication {
public:
    explicit StoreTests(const char* path)
        : BApplication("application/x-vnd.Kunanyi-Summit-ContentRuleStoreTests")
        , m_root(String::fromUTF8(path))
        , m_path(makeString(m_root, "/ContentRuleList-fixture"_s))
    {
    }

    void ReadyToRun() final
    {
        check(BWebKitInitialize() == B_OK, "actual native WebKit initializes on the application thread");
        check(RunLoop::isMain(), "application thread owns the WebKit main loop");
        m_store = API::ContentRuleListStore::storeWithPath(m_root);
        m_deadline = system_time() + 90000000;
        SetPulseRate(100000);
        next();
    }

    void Pulse() final
    {
        if (system_time() > m_deadline) {
            check(false, "persistent store callbacks complete before the deadline");
            PostMessage(B_QUIT_REQUESTED);
        }
    }

private:
    bool callback()
    {
        ++callbacks;
        bool main = RunLoop::isMain();
        check(main, "store callback runs on the main loop");
        check(!m_invoking.load(), "CSS-disabled store operation completes asynchronously");
        return main;
    }

    void next()
    {
        RunLoop::mainSingleton().dispatch([this] { step(); });
    }

    void compile(String path, String rules, bool success)
    {
        m_invoking = true;
        m_store->compileContentRuleListFile(WTF::move(path), "fixture"_s, WTF::move(rules), CSSSelectorsAllowed::No,
            [this, success, capture = std::make_unique<CompletionCapture>()](RefPtr<API::ContentRuleList> list, std::error_code error) {
                bool main = callback();
                check(success ? !!list && !error : !list && !!error, "compile reports the expected success or failure");
                if (main && list)
                    check(list->name() == "fixture"_s, "compiled rule list retains its identifier");
                next();
            });
        m_invoking = false;
    }

    void lookup(bool success)
    {
        m_invoking = true;
        m_store->lookupContentRuleListFile(String(m_path), "fixture"_s,
            [this, success, capture = std::make_unique<CompletionCapture>()](RefPtr<API::ContentRuleList> list, std::error_code error) {
                bool main = callback();
                check(success ? !!list && !error : !list && !!error, "disk lookup reports the expected result");
                if (main && list)
                    check(list->name() == "fixture"_s, "disk lookup restores the rule identifier");
                next();
            });
        m_invoking = false;
    }

    void source(ASCIILiteral expected)
    {
        m_invoking = true;
        m_store->getContentRuleListSource("fixture"_s,
            [this, expected, capture = std::make_unique<CompletionCapture>()](String json) {
                callback();
                check(json == expected, "persisted source matches the last successful compilation");
                next();
            });
        m_invoking = false;
    }

    void remove(bool success)
    {
        m_invoking = true;
        m_store->removeContentRuleListFile(String(m_path),
            [this, success, capture = std::make_unique<CompletionCapture>()](std::error_code error) {
                callback();
                check(success ? !error : !!error, "removal reports the expected result");
                next();
            });
        m_invoking = false;
    }

    void step()
    {
        check(RunLoop::isMain(), "next store operation starts on the main loop");
        switch (m_step++) {
        case 0:
            compile(String(m_path), "not JSON"_s, false);
            return;
        case 1:
            check(!FileSystem::fileExists(m_path), "malformed JSON creates no persistent list");
            compile(String(m_path), String(cssRules), false);
            return;
        case 2:
            compile(String(m_path), String(invalidRegex), false);
            return;
        case 3:
            compile(makeString(m_root, "/absent/list"_s), String(originalRules), false);
            return;
        case 4:
            check(!FileSystem::fileExists(m_path), "rejected and failed compilations leave no fixture list");
            compile(String(m_path), String(originalRules), true);
            return;
        case 5:
            check(FileSystem::fileExists(m_path), "successful compilation creates a real file");
            m_store = nullptr;
            m_store = API::ContentRuleListStore::storeWithPath(m_root);
            lookup(true);
            return;
        case 6:
            source(originalRules);
            return;
        case 7:
            compile(String(m_path), String(invalidRegex), false);
            return;
        case 8:
            lookup(true);
            return;
        case 9:
            source(originalRules);
            return;
        case 10:
            compile(String(m_path), String(replacementRules), true);
            return;
        case 11:
            source(replacementRules);
            return;
        case 12:
            m_invoking = true;
            m_store->getAvailableContentRuleListIdentifiers(
                [this, capture = std::make_unique<CompletionCapture>()](Vector<String> identifiers) {
                    callback();
                    check(identifiers.size() == 1 && identifiers[0] == "fixture"_s, "enumeration returns the single persisted identifier");
                    next();
                });
            m_invoking = false;
            return;
        case 13:
            remove(true);
            return;
        case 14:
            check(!FileSystem::fileExists(m_path), "successful removal deletes the file");
            lookup(false);
            return;
        case 15:
            remove(false);
            return;
        case 16: {
            // Drop the caller's final store reference while the worker still
            // owns pending work. Its completion and capture must return on main.
            m_store = nullptr;
            RefPtr transient = API::ContentRuleListStore::storeWithPath(m_root);
            m_invoking = true;
            transient->compileContentRuleListFile(String(m_path), "fixture"_s, String(originalRules), CSSSelectorsAllowed::No,
                [this, capture = std::make_unique<CompletionCapture>()](RefPtr<API::ContentRuleList> list, std::error_code error) {
                    callback();
                    check(list && !error, "pending compilation survives release of the caller's store reference");
                    next();
                });
            transient = nullptr;
            m_invoking = false;
            return;
        }
        case 17:
            m_store = API::ContentRuleListStore::storeWithPath(m_root);
            lookup(true);
            return;
        case 18:
            remove(true);
            return;
        default:
            check(callbacks == 19, "every requested asynchronous operation completed once");
            check(destroyedCaptures == callbacks, "all completion captures were released before shutdown");
            check(FileSystem::listDirectory(m_root).isEmpty(), "store work leaves no compiled or temporary files after cleanup");
            m_store = nullptr;
            PostMessage(B_QUIT_REQUESTED);
        }
    }

    String m_root;
    String m_path;
    RefPtr<API::ContentRuleListStore> m_store;
    std::atomic<bool> m_invoking { false };
    unsigned m_step { 0 };
    bigtime_t m_deadline { 0 };
};
} // namespace

int main()
{
    char directory[] = "/tmp/summit-content-rule-store-XXXXXX";
    if (!mkdtemp(directory))
        return 2;
    {
        StoreTests application(directory);
        application.Run();
    }
    check(rmdir(directory) == 0, "isolated rule-store directory is removed");
    printf("%u checks passed, %u failed\n", passed.load(), failed.load());
    return failed ? 1 : 0;
}
