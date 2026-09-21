/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "APIError.h"
#include "APINavigation.h"
#include "APINavigationClient.h"
#include "APIPageConfiguration.h"
#include "AuthenticationChallengeProxy.h"
#include "AuthenticationDecisionListener.h"
#include "JavaScriptEvaluationResult.h"
#include "PageLoadState.h"
#include "ProcessTerminationReason.h"
#include "RunJavaScriptParameters.h"
#include "WebExtension.h"
#include "WebExtensionContext.h"
#include "WebExtensionController.h"
#include "WebExtensionControllerConfiguration.h"
#include "WebKitView.h"
#include "WebPageProxy.h"
#include "WebPreferences.h"
#include "WebProcessProxy.h"
#include "WebViewPrivate.h"
#include "WebsiteDataStore.h"
#include <Application.h>
#include <MessageRunner.h>
#include <OS.h>
#include <WebCore/AuthenticationChallenge.h>
#include <WebCore/CertificateInfo.h>
#include <WebCore/Credential.h>
#include <WebCore/ProtectionSpace.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/RunJavaScriptParameters.h>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <unistd.h>
#include <wtf/JSONValues.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>

using namespace WTF;
using namespace WebKit;
namespace fs = std::filesystem;
namespace {
unsigned failures, completedRounds, completedCommands, acceptedPins;
void check(bool condition, const char* label)
{
    if (!condition) ++failures;
    printf("%s %s\n", condition ? "PASS" : "FAIL", label);
    fflush(stdout);
}
bool hasChildren()
{
    team_info info;
    int32 cookie = 0;
    while (get_next_team_info(&cookie, &info) == B_OK) {
        if (info.parent == getpid()) return true;
    }
    return false;
}

class FixtureNavigationClient final : public API::NavigationClient {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(FixtureNavigationClient);
public:
    FixtureNavigationClient(String certificate, uint16_t port)
        : m_certificate(WTF::move(certificate)), m_port(port) { }
    void didReceiveAuthenticationChallenge(WebPageProxy&, AuthenticationChallengeProxy& challenge) final
    {
        const auto& space = challenge.core().protectionSpace();
        const auto& chain = space.certificateInfo().certificateChain();
        bool matches = space.authenticationScheme() == WebCore::ProtectionSpace::AuthenticationScheme::ServerTrustEvaluationRequested
            && !space.isProxy() && space.host() == "127.0.0.1"_s && space.port() == m_port
            && chain.size() == 1 && String::fromUTF8(chain[0].span()) == m_certificate;
        check(matches, "TLS challenge matches the owned loopback port and exact fixture certificate");
        if (!matches) {
            challenge.listener().completeChallenge(AuthenticationChallengeDisposition::Cancel);
            return;
        }
        ++acceptedPins;
        // This is an explicit, request-scoped acceptance of the known fixture
        // leaf. No default trust store or global TLS verification is changed.
        challenge.listener().completeChallenge(AuthenticationChallengeDisposition::UseCredential,
            WebCore::Credential { "owned-loopback-fixture"_s, emptyString(), WebCore::CredentialPersistence::None });
    }
private:
    String m_certificate;
    uint16_t m_port;
};

class PrivacyNetworkTests final : public BApplication {
public:
    PrivacyNetworkTests(URL base, String nonce, String certificate, fs::path package)
        : BApplication("application/x-vnd.Kunanyi-Summit-ExtensionPrivacyNetworkTests")
        , m_base(WTF::move(base)), m_nonce(WTF::move(nonce)), m_certificate(WTF::move(certificate))
        , m_package(std::move(package)) { }
    void ReadyToRun() final
    {
        // BApplication::SetPulseRate rounds sub-100ms intervals down to zero.
        BMessage heartbeat(B_PULSE);
        m_heartbeat = std::make_unique<BMessageRunner>(BMessenger(this), &heartbeat, 30000);
        check(m_heartbeat->InitCheck() == B_OK, "native test heartbeat initializes");
        if (m_heartbeat->InitCheck() != B_OK) {
            PostMessage(B_QUIT_REQUESTED);
            return;
        }
        if (BWebKitInitialize() != B_OK) {
            check(false, "native WebKit initializes");
            close();
            return;
        }
        m_store = WebsiteDataStore::createNonPersistent();
        Ref configuration = WebExtensionControllerConfiguration::createNonPersistent();
        configuration->setDefaultWebsiteDataStore(m_store.get());
        m_controller = WebExtensionController::create(WTF::move(configuration));
        m_controller->setTestingMode(true);
        RefPtr<API::Error> error;
        Ref extension = WebExtension::create(String::fromUTF8(m_package.c_str()), error);
        if (error || !extension->manifestParsedSuccessfully()) {
            check(false, "real privacy extension package parses");
            close();
            return;
        }
        m_context = WebExtensionContext::create(WTF::move(extension));
        m_context->setUniqueIdentifier("summit-privacy-network"_s);
        check(m_context->setNativeInstallationMetadata("privacy-network-installation"_s, 10), "fixture has a real installation identity and priority");
        m_context->setPermissionState(WebExtensionContext::PermissionState::GrantedExplicitly, "privacy"_s);
        auto loaded = m_controller->load(*m_context);
        if (!loaded || !*loaded) {
            check(false, "native controller loads the privacy extension");
            close();
            return;
        }
        startRound();
    }
    void Pulse() final
    {
        if (m_closing) {
            if (!hasChildren()) {
                check(true, "owned browser and network processes drain");
                PostMessage(B_QUIT_REQUESTED);
            } else if (system_time() > m_deadline) {
                check(false, "owned process teardown meets its deadline");
                PostMessage(B_QUIT_REQUESTED);
            }
            return;
        }
        if (failures || system_time() > m_deadline) {
            if (!failures) check(false, "network phase completes before its deadline");
            close();
            return;
        }
        if (m_phase == Phase::API) {
            inspectAPI();
            return;
        }
        if (m_phase == Phase::Quiet) {
            if (system_time() < m_quietUntil) return;
            printf("PRIVACY_NETWORK_ROUND %u %s %s\n", m_round, enabled() ? "enabled" : "disabled", m_nonce.utf8().legacyCStringPointer());
            ++completedRounds;
            if (++m_round == 5) {
                check(completedCommands == 12 && acceptedPins, "all twelve extension setters completed with verified local TLS challenges");
                close();
            } else startRound();
            return;
        }
        auto title = m_view->page()->pageLoadState().title();
        if (title.startsWith("SUMMIT NETWORK FAIL "_s)) {
            printf("%s\n", title.utf8().legacyCStringPointer());
            check(false, "network fixture JavaScript completes without error");
            close();
            return;
        }
        auto prefix = makeString("SUMMIT NETWORK "_s, m_nonce, ' ', m_round, ' ');
        if (m_phase == Phase::Ready && title == makeString(prefix, "ready"_s)) {
            check(true, "real HTTPS document and its initially parsed candidates are ready");
            if (m_round == 1)
                setBoth(false, [this] { trigger(); });
            else trigger();
        } else if (m_phase == Phase::Destination && title == makeString(prefix, "destination"_s)) {
            check(true, "ordinary script, beacon queueing and actual anchor navigation completed");
            m_phase = Phase::Quiet;
            m_quietUntil = system_time() + 1000000;
        }
    }
private:
    enum class Phase { API, Ready, Destination, Quiet };
    bool enabled() const { return m_round == 0 || m_round == 4; }
    void setBoth(bool value, std::function<void()> after)
    {
        set("network"_s, value, false, [this, value, after = std::move(after)]() mutable {
            set("hyperlink"_s, value, true, std::move(after));
        });
    }
    void set(const String& setting, bool value, bool callback, std::function<void()> after)
    {
        Ref input = JSON::Object::create();
        input->setString("op"_s, "set"_s);
        input->setString("setting"_s, setting);
        input->setString("mode"_s, callback ? "callback"_s : "promise"_s);
        input->setString("nonce"_s, m_nonce);
        input->setInteger("sequence"_s, ++m_sequence);
        Ref details = JSON::Object::create();
        details->setBoolean("value"_s, value);
        input->setObject("details"_s, WTF::move(details));
        m_afterAPI = std::move(after);
        m_phase = Phase::API;
        m_deadline = system_time() + 35000000;
        m_context->sendTestMessage("summit-privacy-command"_s, WTF::move(input));
    }
    void inspectAPI()
    {
        if (m_context->backgroundContentLoadError()) {
            check(false, "extension background has no load error");
            close();
            return;
        }
        RefPtr page = m_context->backgroundWebView();
        if (!page) return;
        auto title = page->pageLoadState().title();
        if (title.startsWith("SUMMIT PRIVACY FAIL "_s)) {
            printf("%s\n", title.utf8().legacyCStringPointer());
            check(false, "real privacy extension bindings initialize");
            close();
            return;
        }
        auto prefix = makeString("SUMMIT PRIVACY "_s, m_nonce, ' ', m_sequence, ' ');
        if (!title.startsWith(prefix)) return;
        RefPtr parsed = JSON::Value::parseJSON(title.substring(prefix.length()));
        RefPtr response = parsed ? parsed->asObject() : nullptr;
        bool valid = response && response->getBoolean("ok"_s) == true
            && response->getString("nonce"_s) == m_nonce && response->getInteger("sequence"_s) == m_sequence
            && response->getString("extension"_s) == m_context->uniqueIdentifier();
        check(valid, "actual extension setter completes through privileged IPC");
        if (!valid) {
            printf("PRIVACY_NETWORK_API_ERROR %s\n", title.utf8().legacyCStringPointer());
            close();
            return;
        }
        printf("PRIVACY_NETWORK_API %s\n", response->toJSONString().utf8().legacyCStringPointer());
        ++completedCommands;
        auto next = std::exchange(m_afterAPI, { });
        next();
    }
    void startRound()
    {
        if (m_round == 3) closeView();
        // Round1 disables after parsing. Round2 parses while disabled; round3
        // also starts with a new page. Round4 verifies re-enabling.
        setBoth(m_round == 0 || m_round == 1 || m_round == 4, [this] {
            if (!m_view) {
                Ref configuration = API::PageConfiguration::create();
                configuration->setWebsiteDataStore(m_store.get());
                configuration->setWebExtensionController(m_controller.get());
                configuration->preferences().setAcceleratedCompositingEnabled(false);
                configuration->preferences().setForceCompositingMode(false);
                configuration->preferences().setThreadedScrollingEnabled(false);
                m_view = WebView::createForExtensionBackground(WTF::move(configuration));
                m_view->page()->setNavigationClient(makeUniqueRef<FixtureNavigationClient>(m_certificate, *m_base.port()));
            }
            m_phase = Phase::Ready;
            m_deadline = system_time() + 35000000;
            m_view->page()->loadRequest(WebCore::ResourceRequest { URL { makeString(m_base.string(), "source/"_s, m_round) } });
        });
    }
    void trigger()
    {
        m_phase = Phase::Destination;
        m_deadline = system_time() + 35000000;
        // Sent after the API setter receipt: this page's preference IPC is
        // already queued before the subsequent DOM command on its connection.
        auto script = IPC::TransferString::create(String { "window.runNetworkRound(); true;"_s });
        if (!script) {
            check(false, "native DOM command allocates its IPC source string");
            close();
            return;
        }
        m_view->page()->runJavaScriptInMainFrame(WebKit::RunJavaScriptParameters {
            WTF::move(*script), JSC::SourceTaintedOrigin::Untainted, URL { },
            WebCore::RunAsAsyncFunction::No, std::nullopt, WebCore::ForceUserGesture::Yes,
            WebCore::RemoveTransientActivation::Yes }, true, [this](auto&& result) {
                if (!m_closing && !result) {
                    check(false, "native page command executes the fixture's actual DOM actions");
                    close();
                }
            });
    }
    void closeView()
    {
        if (!m_view) return;
        m_view->page()->forEachWebContentProcess([&](auto& process, auto) { m_processes.add(process); });
        m_view->close();
        m_view = nullptr;
    }
    void close()
    {
        if (m_closing) return;
        m_closing = true;
        m_afterAPI = { };
        m_deadline = system_time() + 20000000;
        if (m_controller) {
            for (Ref process : m_controller->allProcesses()) m_processes.add(process);
            m_controller->unloadAll();
        }
        closeView();
        m_context = nullptr;
        m_controller = nullptr;
        for (Ref process : m_processes) process->requestTermination(ProcessTerminationReason::RequestedByClient);
        m_processes.clear();
        if (m_store) m_store->terminateNetworkProcess();
        m_store = nullptr;
    }

    std::unique_ptr<BMessageRunner> m_heartbeat;
    URL m_base;
    String m_nonce, m_certificate;
    fs::path m_package;
    RefPtr<WebsiteDataStore> m_store;
    RefPtr<WebExtensionController> m_controller;
    RefPtr<WebExtensionContext> m_context;
    RefPtr<WebView> m_view;
    HashSet<Ref<WebProcessProxy>> m_processes;
    std::function<void()> m_afterAPI;
    unsigned m_round { 0 }, m_sequence { 0 };
    Phase m_phase { Phase::API };
    bigtime_t m_deadline { 0 }, m_quietUntil { 0 };
    bool m_closing { false };
};
}

int main()
{
    const char* base = getenv("SUMMIT_PRIVACY_NETWORK_BASE");
    const char* nonce = getenv("SUMMIT_PRIVACY_NETWORK_NONCE");
    const char* certificate = getenv("SUMMIT_PRIVACY_NETWORK_CERTIFICATE");
    const char* package = getenv("SUMMIT_PRIVACY_PACKAGE");
    if (!base || !nonce || !certificate || !package) return 2;
    URL url { String::fromUTF8(base) };
    if (!url.isValid() || !url.protocolIs("https"_s) || url.host() != "127.0.0.1"_s || !url.port()
        || std::string(nonce).size() != 32 || !fs::path(certificate).is_absolute() || !fs::path(package).is_absolute()) return 2;
    std::ifstream input(certificate, std::ios::binary);
    std::string pem {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (pem.empty() || pem.size() > 16384) return 2;
    {
        PrivacyNetworkTests application(WTF::move(url), String::fromUTF8(nonce), String::fromUTF8(pem.c_str()), package);
        application.Run();
    }
    check(!hasChildren(), "no owned process remains at exit");
    check(completedRounds == 5 && completedCommands == 12 && acceptedPins > 0, "all planned network rounds and API commands completed");
    printf("PRIVACY_NETWORK_RESULT %s rounds=%u api_commands=%u tls_pins=%u failures=%u\n",
        failures ? "FAIL" : "PASS", completedRounds, completedCommands, acceptedPins, failures);
    return failures ? 1 : 0;
}
