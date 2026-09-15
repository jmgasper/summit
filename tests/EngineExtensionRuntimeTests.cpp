/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "APIError.h"
#include "APINavigation.h"
#include "APIPageConfiguration.h"
#include "PageLoadState.h"
#include "ProcessTerminationReason.h"
#include "WebExtension.h"
#include "WebExtensionContext.h"
#include "WebExtensionController.h"
#include "WebExtensionControllerConfiguration.h"
#include "WebExtensionMatchPattern.h"
#include "WebKitView.h"
#include "WebPageProxy.h"
#include "WebPreferences.h"
#include "WebProcessProxy.h"
#include "WebViewPrivate.h"
#include "WebsiteDataStore.h"
#include "WebsiteDataRecord.h"
#include <WebCore/ResourceRequest.h>
#include <Application.h>
#include <OS.h>
#include <image.h>
#include <wtf/JSONValues.h>
#include <wtf/RunLoop.h>
#include <wtf/UUID.h>
#include <wtf/text/MakeString.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string_view>
#include <unistd.h>

using namespace WTF;
using namespace WebKit;
namespace fs = std::filesystem;

namespace {
unsigned passed;
unsigned failed;
void check(bool condition, const char* description)
{
    ++(condition ? passed : failed);
    printf("%s %s\n", condition ? "PASS" : "FAIL", description);
    fflush(stdout);
}

constexpr auto backgroundScript = R"JS(
'use strict';
document.title = 'SUMMIT EXTENSION SCRIPT STARTED';
try {
    if (typeof browser !== 'object' || typeof chrome !== 'object')
        throw new Error('Missing browser or chrome namespace');
    browser.test.onMessage.addListener(async (message, input) => {
        if (message !== 'summit-native-start') return;
        try {
            const assert = (value, label) => {
                browser.test.assertTrue(!!value, label);
                if (!value) throw new Error(label);
            };
            assert(browser.runtime.id === 'summit-runtime-fixture', 'runtime identity');
            assert(chrome.runtime.getManifest().name === 'Summit runtime fixture', 'Chrome namespace manifest');
            assert(browser.runtime.getURL('background.html') === location.href, 'extension resource URL');
            const previous = (await browser.storage.local.get('run')).run;
            assert(input.round === 1 ? previous === undefined : previous.round === 1 && previous.nonce === input.nonce,
                'storage survives context destruction and reopening');
            await browser.storage.local.set({run: {round: input.round, nonce: input.nonce, nested: [true, null, '雪']}});
            const stored = await new Promise((resolve, reject) => {
                chrome.storage.local.get('run', value => {
                    const error = chrome.runtime.lastError;
                    if (error) reject(new Error(error.message)); else resolve(value.run);
                });
            });
            assert(stored.round === input.round && stored.nonce === input.nonce && stored.nested[2] === '雪',
                'promise write and callback read cross privileged IPC');
            browser.test.sendMessage('summit-runtime-complete', {round: input.round, nonce: input.nonce});
            browser.test.notifyPass('summit-runtime-round-' + input.round);
            document.title = 'SUMMIT EXTENSION PASS ' + input.round + ' ' + input.nonce;
        } catch (error) {
            browser.test.notifyFail(String(error));
            document.title = 'SUMMIT EXTENSION FAIL ' + String(error);
        }
    });
    document.title = 'SUMMIT EXTENSION LISTENING';
} catch (error) {
    document.title = 'SUMMIT EXTENSION FAIL ' + String(error);
}
)JS";

struct Children {
    std::set<team_id> web;
    std::set<team_id> network;
};
Children children()
{
    Children result;
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
            auto name = fs::path(image.name).filename();
            if (name == "WebProcess") result.web.insert(team.team);
            if (name == "NetworkProcess") result.network.insert(team.team);
            break;
        }
    }
    return result;
}

class ExtensionTests final : public BApplication {
public:
    explicit ExtensionTests(const fs::path& root, bool cookies)
        : BApplication("application/x-vnd.Kunanyi-Summit-ExtensionRuntimeTests")
        , m_root(root)
        , m_cookies(cookies)
    {
    }

    void ReadyToRun() final
    {
        if (BWebKitInitialize() != B_OK) {
            check(false, "native WebKit initializes");
            PostMessage(B_QUIT_REQUESTED);
            return;
        }
        check(RunLoop::isMain(), "extension host runs on the engine main loop");
        m_nonce = WTF::UUID::createVersion4().toString();
        m_dataStore = WebsiteDataStore::createNonPersistent();
        Ref configuration = WebExtensionControllerConfiguration::createNonPersistent();
        configuration->setStorageDirectory(String::fromUTF8((m_root / "profile").c_str()));
        configuration->setDefaultWebsiteDataStore(m_dataStore.get());
        m_controller = WebExtensionController::create(WTF::move(configuration));
        m_controller->setTestingMode(true);
        SetPulseRate(100000);
        std::string startup = std::getenv("SUMMIT_EXTENSION_STARTUP") ?: "cold";
        printf("STARTUP_MODE %s\n", startup.c_str());
        if (startup == "page")
            startControl();
        else if (startup == "deferred")
            RunLoop::mainSingleton().dispatch([this] { startRound(); });
        else if (startup == "network") {
            m_waitingForNetwork = true;
            m_deadline = system_time() + 30000000;
            // A nonpersistent store fetch only visits an existing process.
            m_dataStore->networkProcess();
            m_dataStore->fetchData({ WebsiteDataType::LocalStorage }, { }, [this](Vector<WebsiteDataRecord>) {
                if (m_closing)
                    return;
                m_waitingForNetwork = false;
                check(!children().network.empty(), "owned network process answers before direct extension startup");
                startRound();
            });
        } else
            startRound();
    }

    void Pulse() final
    {
        if (m_closing) {
            auto active = children();
            if (active.web.empty() && active.network.empty()) {
                check(true, "owned WebProcess and NetworkProcess exit after teardown");
                PostMessage(B_QUIT_REQUESTED);
            } else if (system_time() > m_deadline) {
                check(false, "owned processes finish teardown before the deadline");
                PostMessage(B_QUIT_REQUESTED);
            }
            return;
        }
        if (m_waitingForNetwork) {
            if (system_time() > m_deadline) {
                check(false, "network process answers before the deadline");
                close();
            }
            return;
        }
        if (m_controlView) {
            Ref page = *m_controlView->page();
            auto phase = makeString("process="_s, page->hasRunningProcess(), " loading="_s,
                page->pageLoadState().isLoading(), " active="_s, page->pageLoadState().activeURL().string(),
                " provisional="_s, page->pageLoadState().provisionalURL().string());
            if (phase != m_lastPhase) {
                printf("CONTROL_PHASE %s\n", phase.utf8().legacyCStringPointer());
                fflush(stdout);
                m_lastPhase = phase;
            }
            if (page->pageLoadState().title() == makeString("SUMMIT CONTROL PASS "_s, m_nonce)) {
                check(true, "ordinary offscreen page executes JavaScript in the Extensions-enabled engine");
                closeControl();
                RunLoop::mainSingleton().dispatch([this] { startRound(); });
            } else if (system_time() > m_deadline) {
                check(false, "ordinary offscreen page executes JavaScript before the deadline");
                close();
            }
            return;
        }
        if (!m_context)
            return;
        if (auto error = m_context->backgroundContentLoadError()) {
            printf("Background error: %s\n", error->localizedDescription().utf8().legacyCStringPointer());
            check(false, "extension background document loads");
            close();
            return;
        }
        RefPtr page = m_context->backgroundWebView();
        auto phase = page ? makeString("page=yes process="_s, page->hasRunningProcess(), " loading="_s,
            page->pageLoadState().isLoading(), " active="_s, page->pageLoadState().activeURL().string(),
            " provisional="_s, page->pageLoadState().provisionalURL().string()) : "page=no"_s;
        if (phase != m_lastPhase) {
            printf("BACKGROUND_PHASE %s\n", phase.utf8().legacyCStringPointer());
            fflush(stdout);
            m_lastPhase = phase;
        }
        if (page) {
            auto title = page->pageLoadState().title();
            if (title != m_lastTitle) {
                printf("BACKGROUND_TITLE %s\n", title.utf8().legacyCStringPointer());
                fflush(stdout);
                m_lastTitle = title;
            }
            if (title.startsWith("SUMMIT EXTENSION FAIL "_s)) {
                check(false, "actual extension JavaScript assertions pass");
                close();
                return;
            }
            if (title == makeString("SUMMIT EXTENSION PASS "_s, m_round, ' ', m_nonce)) {
                check(true, "background DOM confirms completed extension API operations");
                auto active = children();
                check(!active.web.empty() && !active.network.empty(), "extension uses actual WebProcess and NetworkProcess children");
                page = nullptr;
                auto processes = m_controller->allProcesses();
                auto unloaded = m_controller->unload(*m_context);
                check(unloaded && *unloaded && !m_context->isLoaded() && !m_context->backgroundWebView(), "native unload clears the extension and its background page");
                for (Ref process : processes)
                    process->requestTermination(ProcessTerminationReason::RequestedByClient);
                m_context = nullptr;
                if (m_round == 1)
                    RunLoop::mainSingleton().dispatch([this] { startRound(); });
                else
                    close();
                return;
            }
        }
        if (system_time() > m_deadline) {
            check(false, "extension completes its native message and API round trip before the deadline");
            close();
        }
    }

private:
    void startControl()
    {
        // Exercise the same native offscreen page client without extension
        // configuration or resource handling before loading the package.
        Ref configuration = API::PageConfiguration::create();
        configuration->setWebsiteDataStore(m_dataStore.get());
        configuration->preferences().setAcceleratedCompositingEnabled(false);
        configuration->preferences().setForceCompositingMode(false);
        configuration->preferences().setThreadedScrollingEnabled(false);
        m_controlView = WebView::createForExtensionBackground(WTF::move(configuration));
        m_deadline = system_time() + 30000000;
        URL url { makeString("data:text/html,<script>document.title='SUMMIT CONTROL PASS "_s, m_nonce, "'</script>"_s) };
        m_controlView->page()->loadRequest(WebCore::ResourceRequest { WTF::move(url) });
    }

    void closeControl()
    {
        if (!m_controlView)
            return;
        Vector<Ref<WebProcessProxy>> processes;
        m_controlView->page()->forEachWebContentProcess([&](auto& process, auto) { processes.append(process); });
        m_controlView->close();
        m_controlView = nullptr;
        for (Ref process : processes)
            process->requestTermination(ProcessTerminationReason::RequestedByClient);
    }

    void startRound()
    {
        ++m_round;
        m_deadline = system_time() + 90000000;
        m_lastTitle = { };
        m_lastPhase = { };
        RefPtr<API::Error> error;
        Ref extension = WebExtension::create(String::fromUTF8((m_root / "package").c_str()), error);
        if (error || !extension->manifestParsedSuccessfully() || extension->nativeResourceFingerprint().isEmpty()) {
            if (error) printf("Package error: %s\n", error->localizedDescription().utf8().legacyCStringPointer());
            check(false, "native package snapshot and manifest are valid");
            close();
            return;
        }
        check(true, "native package snapshot and manifest are valid");
        m_context = WebExtensionContext::create(WTF::move(extension));
        m_context->setUniqueIdentifier("summit-runtime-fixture"_s);
        // This is the test package's explicit grant, scoped to its isolated profile.
        auto permission = m_cookies ? "cookies"_s : "storage"_s;
        m_context->setPermissionState(WebExtensionContext::PermissionState::GrantedExplicitly, permission);
        RefPtr cookiePattern = m_cookies ? WebExtensionMatchPattern::getOrCreate("https://summit-cookie.invalid/*"_s) : nullptr;
        if (cookiePattern)
            m_context->setPermissionState(WebExtensionContext::PermissionState::GrantedExplicitly, *cookiePattern);
        auto loaded = m_controller->load(*m_context);
        if (!loaded || !*loaded) {
            if (!loaded && loaded.error()) printf("Load error: %s\n", loaded.error()->localizedDescription().utf8().legacyCStringPointer());
            check(false, "controller accepts the native extension context");
            close();
            return;
        }
        check(m_context->isLoaded() && m_context->hasPermission(permission)
            && (!m_cookies || (cookiePattern && m_context->hasPermission(*cookiePattern))),
            "loaded context has the fixture's explicit API and host grants");
        Ref input = JSON::Object::create();
        input->setInteger("round"_s, m_round);
        input->setString("nonce"_s, m_nonce);
        // Queue before the background page has registered its JS listener.
        m_context->sendTestMessage("summit-native-start"_s, WTF::move(input));
    }

    void close()
    {
        if (m_closing)
            return;
        m_closing = true;
        m_deadline = system_time() + 30000000;
        closeControl();
        if (m_controller) {
            auto processes = m_controller->allProcesses();
            m_controller->unloadAll();
            for (Ref process : processes)
                process->requestTermination(ProcessTerminationReason::RequestedByClient);
        }
        m_context = nullptr;
        m_controller = nullptr;
        if (m_dataStore)
            m_dataStore->terminateNetworkProcess();
        m_dataStore = nullptr;
    }

    fs::path m_root;
    bool m_cookies;
    RefPtr<WebView> m_controlView;
    RefPtr<WebsiteDataStore> m_dataStore;
    RefPtr<WebExtensionController> m_controller;
    RefPtr<WebExtensionContext> m_context;
    String m_nonce;
    String m_lastTitle;
    String m_lastPhase;
    unsigned m_round { 0 };
    bigtime_t m_deadline { 0 };
    bool m_closing { false };
    bool m_waitingForNetwork { false };
};
} // namespace

int main()
{
    const char* fixture = getenv("SUMMIT_EXTENSION_FIXTURE");
    bool cookies = fixture && std::string_view(fixture) == "cookies";
    if (fixture && !cookies && std::string_view(fixture) != "storage")
        return 2;
    printf("EXTENSION_FIXTURE %s\n", cookies ? "cookies" : "storage");
    char temporary[] = "/tmp/summit-extension-runtime-XXXXXX";
    if (!mkdtemp(temporary))
        return 2;
    fs::path root(temporary);
    fs::create_directory(root / "package");
    if (cookies) {
        const char* source = getenv("SUMMIT_EXTENSION_PACKAGE_DIR");
        std::error_code error;
        if (!source || !fs::path(source).is_absolute()) {
            fs::remove_all(root, error);
            return 2;
        }
        for (const char* name : { "manifest.json", "background.html", "background.js" }) {
            fs::copy_file(fs::path(source) / name, root / "package" / name, error);
            if (error) {
                fprintf(stderr, "Cannot stage extension fixture: %s\n", error.message().c_str());
                fs::remove_all(root, error);
                return 2;
            }
        }
    } else {
        std::ofstream(root / "package/manifest.json") << R"JSON({"manifest_version":2,"name":"Summit runtime fixture","version":"1.0","permissions":["storage"],"background":{"page":"background.html","persistent":true}})JSON";
        std::ofstream(root / "package/background.html") << "<!doctype html><meta charset=utf-8><title>SUMMIT EXTENSION STARTING</title><script src=background.js></script>";
        std::ofstream(root / "package/background.js") << backgroundScript;
    }
    {
        ExtensionTests application(root, cookies);
        application.Run();
    }
    auto active = children();
    check(active.web.empty() && active.network.empty(), "no extension helper process remains at exit");
    std::error_code error;
    fs::remove_all(root, error);
    check(!error && !fs::exists(root), "isolated extension package and profile are removed");
    printf("%u checks passed, %u failed\n", passed, failed);
    return failed ? 1 : 0;
}
