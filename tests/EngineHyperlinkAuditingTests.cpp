/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "APINavigation.h"
#include "APIPageConfiguration.h"
#include "PageLoadState.h"
#include "ProcessTerminationReason.h"
#include "WKAPICast.h"
#include "WKPreferencesRef.h"
#include "WebKitView.h"
#include "WebPageProxy.h"
#include "WebPreferences.h"
#include "WebProcessProxy.h"
#include "WebViewPrivate.h"
#include "WebsiteDataStore.h"
#include <Application.h>
#include <MessageRunner.h>
#include <OS.h>
#include <WebCore/ResourceRequest.h>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <unistd.h>
#include <wtf/text/MakeString.h>

using namespace WTF;
using namespace WebKit;

namespace {
unsigned passed, failed;
void check(bool result, const char* label)
{
    ++(result ? passed : failed);
    printf("%s %s\n", result ? "PASS" : "FAIL", label);
    fflush(stdout);
}
bool hasChildren()
{
    team_info info;
    int32 cookie = 0;
    while (get_next_team_info(&cookie, &info) == B_OK) {
        if (info.parent == getpid())
            return true;
    }
    return false;
}

class HyperlinkTests final : public BApplication {
public:
    HyperlinkTests(String base, String nonce)
        : BApplication("application/x-vnd.Kunanyi-Summit-HyperlinkAuditingTests")
        , m_base(WTF::move(base)), m_nonce(WTF::move(nonce)) { }

    void ReadyToRun() final
    {
        // BApplication::SetPulseRate rounds sub-100ms intervals down to zero.
        BMessage heartbeat(B_PULSE);
        m_heartbeat = std::make_unique<BMessageRunner>(BMessenger(this), &heartbeat, 50000);
        check(m_heartbeat->InitCheck() == B_OK, "native test heartbeat initializes");
        if (m_heartbeat->InitCheck() != B_OK) {
            PostMessage(B_QUIT_REQUESTED);
            return;
        }
        if (BWebKitInitialize() != B_OK) {
            check(false, "WebKit initializes on the native application loop");
            close();
            return;
        }
        m_store = WebsiteDataStore::createNonPersistent();
        createView(false);
        startRound();
    }
    void Pulse() final
    {
        if (m_closing) {
            if (!hasChildren()) {
                check(true, "owned web and network processes exit after teardown");
                PostMessage(B_QUIT_REQUESTED);
            } else if (system_time() > m_deadline) {
                check(false, "owned processes finish teardown before the deadline");
                PostMessage(B_QUIT_REQUESTED);
            }
            return;
        }
        if (system_time() > m_deadline) {
            check(false, "navigation completes before the round deadline");
            close();
            return;
        }
        if (m_quietUntil) {
            if (system_time() < m_quietUntil)
                return;
            m_quietUntil = 0;
            if (++m_round == 4) {
                close();
                return;
            }
            if (m_round == 3) {
                closeView();
                createView(true);
            }
            startRound();
            return;
        }
        auto expected = makeString("SUMMIT HYPERLINK "_s, m_nonce, ' ', m_round, " ready"_s);
        if (m_view->page()->pageLoadState().title() == expected) {
            check(true, "actual anchor navigation reaches the HTTP destination");
            printf("HYPERLINK_ROUND %u %s %s\n", m_round, enabled() ? "enabled" : "disabled", m_nonce.utf8().legacyCStringPointer());
            fflush(stdout);
            m_quietUntil = system_time() + 2000000;
        }
    }
private:
    bool enabled() const { return m_round == 0 || m_round == 2; }
    void createView(bool initiallyDisabled)
    {
        Ref configuration = API::PageConfiguration::create();
        configuration->setWebsiteDataStore(m_store.get());
        auto& preferences = configuration->preferences();
        preferences.setAcceleratedCompositingEnabled(false);
        preferences.setForceCompositingMode(false);
        preferences.setThreadedScrollingEnabled(false);
        check(WKPreferencesGetHyperlinkAuditingEnabled(toAPI(&preferences)), "new configuration enables hyperlink auditing by default");
        if (initiallyDisabled)
            WKPreferencesSetHyperlinkAuditingEnabled(toAPI(&preferences), false);
        // Uses the real native page client and processes; no extension code is injected.
        m_view = WebView::createForExtensionBackground(WTF::move(configuration));
        check(WKPreferencesGetHyperlinkAuditingEnabled(toAPI(&m_view->page()->preferences())) == !initiallyDisabled,
            "new page preserves its configured hyperlink preference");
    }
    void startRound()
    {
        if (m_round == 1 || m_round == 2)
            WKPreferencesSetHyperlinkAuditingEnabled(toAPI(&m_view->page()->preferences()), enabled());
        check(WKPreferencesGetHyperlinkAuditingEnabled(toAPI(&m_view->page()->preferences())) == enabled(),
            "public preference getter reflects the requested state");
        m_deadline = system_time() + 25000000;
        m_view->page()->loadRequest(WebCore::ResourceRequest { URL { makeString(m_base, "source/"_s, m_round) } });
    }
    void closeView()
    {
        if (!m_view)
            return;
        m_view->page()->forEachWebContentProcess([&](auto& process, auto) { m_processes.add(process); });
        m_view->close();
        m_view = nullptr;
    }
    void close()
    {
        if (m_closing)
            return;
        m_closing = true;
        m_deadline = system_time() + 15000000;
        closeView();
        for (Ref process : m_processes)
            process->requestTermination(ProcessTerminationReason::RequestedByClient);
        m_processes.clear();
        if (m_store)
            m_store->terminateNetworkProcess();
        m_store = nullptr;
    }
    std::unique_ptr<BMessageRunner> m_heartbeat;
    String m_base, m_nonce;
    RefPtr<WebView> m_view;
    RefPtr<WebsiteDataStore> m_store;
    HashSet<Ref<WebProcessProxy>> m_processes;
    unsigned m_round { 0 };
    bigtime_t m_deadline { 0 }, m_quietUntil { 0 };
    bool m_closing { false };
};
}

int main()
{
    auto* base = getenv("SUMMIT_HYPERLINK_BASE_URL");
    auto* nonce = getenv("SUMMIT_HYPERLINK_NONCE");
    if (!base || !nonce || !*nonce)
        return 2;
    HyperlinkTests application(String::fromUTF8(base), String::fromUTF8(nonce));
    application.Run();
    printf("%u checks passed, %u failed\n", passed, failed);
    return failed ? 1 : 0;
}
