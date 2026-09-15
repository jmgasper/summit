/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "APIHTTPCookieStore.h"
#include "WebKitView.h"
#include "WebsiteDataStore.h"
#include <Application.h>
#include <OS.h>
#include <cstdio>
#include <functional>
#include <unistd.h>
#include <wtf/RunLoop.h>
#include <wtf/UUID.h>
#include <wtf/WallTime.h>
#include <wtf/text/MakeString.h>

using namespace WTF;
using namespace WebKit;

namespace {
unsigned passed;
unsigned failed;
void check(bool result, const char* label)
{
    ++(result ? passed : failed);
    printf("%s %s\n", result ? "PASS" : "FAIL", label);
    fflush(stdout);
}

bool hasChildren()
{
    team_info team;
    int32 cookie = 0;
    while (get_next_team_info(&cookie, &team) == B_OK) {
        if (team.parent == getpid())
            return true;
    }
    return false;
}

class Observer final : public API::HTTPCookieStoreObserver {
public:
    std::function<void(const Vector<WebCore::CookieChange>&)> changed;
    void cookiesDidChange(API::HTTPCookieStore&) final
    {
        check(false, "native observer receives populated change details");
    }
    void cookiesDidChange(API::HTTPCookieStore&, const Vector<WebCore::CookieChange>& changes) final
    {
        check(RunLoop::isMain(), "cookie observer executes on the application main loop");
        if (changed)
            changed(changes);
    }
};

class CookieObserverTests final : public BApplication {
public:
    CookieObserverTests()
        : BApplication("application/x-vnd.Kunanyi-Summit-CookieObserverTests") { }

    void ReadyToRun() final
    {
        check(BWebKitInitialize() == B_OK, "native WebKit initializes on the application thread");
        SetPulseRate(100000);
        m_dataStore = WebsiteDataStore::createNonPersistent();
        m_store = &m_dataStore->cookieStore();
        m_observer = adoptRef(*new Observer);
        m_observer->changed = [this](const auto& changes) {
            for (const auto& change : changes)
                m_events.append(change);
        };
        m_nonce = UUID::createVersion4().toString();
        m_store->registerObserver(*m_observer);
        writeCookie();
    }

    void Pulse() final
    {
        if (m_closing) {
            if (!hasChildren()) {
                check(true, "owned NetworkProcess exits after teardown");
                PostMessage(B_QUIT_REQUESTED);
            } else if (system_time() > m_deadline) {
                check(false, "owned NetworkProcess finishes teardown before the deadline");
                PostMessage(B_QUIT_REQUESTED);
            }
            return;
        }
        if (system_time() > m_deadline) {
            printf("OBSERVER_PHASE %u events=%zu writeConfirmed=%d queryFinished=%d\n",
                m_phase, m_events.size(), m_writeConfirmed, m_queryFinished);
            check(false, "cookie observation finishes before the deadline");
            return close();
        }
        if (!m_queryFinished)
            return;
        if (m_phase == 2) {
            if (system_time() < m_quietUntil)
                return;
            check(m_events.isEmpty(), "unregistered observer receives no event for a confirmed cookie write");
            return close();
        }
        if (m_events.isEmpty())
            return;
        check(m_events.size() == 1, "one committed insertion produces exactly one observer event");
        const auto& change = m_events.first();
        check(!change.removed && change.cause == WebCore::CookieChangeCause::Explicit
            && change.cookie.name == m_expected.name && change.cookie.value == m_nonce
            && change.cookie.domain == m_expected.domain && change.cookie.path == "/"_s
            && change.cookie.session && !change.cookie.secure && !change.cookie.httpOnly,
            "typed event preserves the actual committed cookie identity, value and cause");
        check(m_writeConfirmed && hasChildren(), "cookie receipt and event use the actual owned NetworkProcess");
        ++m_phase;
        m_events.clear();
        m_queryFinished = false;
        m_deadline = system_time() + 15000000;
        if (m_phase == 1) {
            // A last page closing and a replacement opening unregisters and
            // registers the same store without waiting for the network queue.
            for (unsigned round = 0; round < 32; ++round) {
                m_store->unregisterObserver(*m_observer);
                m_store->registerObserver(*m_observer);
            }
        } else
            m_store->unregisterObserver(*m_observer);
        // The query reply confirms the network has processed all preceding
        // ordinary messages as well as the prioritized registration messages.
        m_store->cookies([this](auto&&) {
            if (!m_closing)
                writeCookie();
        });
    }

private:
    void writeCookie()
    {
        printf("OBSERVER_PHASE %u starting\n", m_phase);
        fflush(stdout);
        m_deadline = system_time() + 15000000;
        m_writeConfirmed = false;
        m_queryFinished = false;
        m_expected = { };
        m_expected.name = makeString("summit-observer-"_s, m_phase);
        m_expected.value = m_nonce;
        m_expected.domain = "summit-observer.invalid"_s;
        m_expected.path = "/"_s;
        m_expected.session = true;
        m_expected.created = WallTime::now().secondsSinceEpoch().milliseconds();
        m_store->setCookieWithResult(m_expected, [this](bool success, std::optional<WebCore::Cookie>&& receipt) {
            if (m_closing)
                return;
            check(RunLoop::isMain(), "cookie write completion executes on the application main loop");
            m_writeConfirmed = success && receipt && receipt->name == m_expected.name && receipt->value == m_nonce;
            check(m_writeConfirmed, "network confirms the actual stored cookie");
            m_store->cookies([this](Vector<WebCore::Cookie>&& cookies) {
                if (m_closing)
                    return;
                bool found = false;
                for (auto& cookie : cookies)
                    found |= cookie.name == m_expected.name && cookie.value == m_nonce;
                check(found, "subsequent network query reads the committed cookie");
                m_queryFinished = true;
                m_quietUntil = system_time() + 250000;
            });
        });
    }

    void close()
    {
        if (m_closing)
            return;
        m_closing = true;
        m_deadline = system_time() + 10000000;
        if (m_store && m_observer)
            m_store->unregisterObserver(*m_observer);
        m_observer = nullptr;
        m_store = nullptr;
        if (m_dataStore)
            m_dataStore->terminateNetworkProcess();
        m_dataStore = nullptr;
    }

    RefPtr<WebsiteDataStore> m_dataStore;
    RefPtr<API::HTTPCookieStore> m_store;
    RefPtr<Observer> m_observer;
    Vector<WebCore::CookieChange> m_events;
    WebCore::Cookie m_expected;
    String m_nonce;
    unsigned m_phase { 0 };
    bigtime_t m_deadline { 0 };
    bigtime_t m_quietUntil { 0 };
    bool m_writeConfirmed { false };
    bool m_queryFinished { false };
    bool m_closing { false };
};
}

int main()
{
    CookieObserverTests application;
    application.Run();
    check(!hasChildren(), "no cookie fixture helper remains at exit");
    printf("%u checks passed, %u failed\n", passed, failed);
    return failed ? 1 : 0;
}
