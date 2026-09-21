/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "APIError.h"
#include "APINavigation.h"
#include "APIPageConfiguration.h"
#include "PageLoadState.h"
#include "ProcessTerminationReason.h"
#include "WKAPICast.h"
#include "WKPreferencesRef.h"
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
#include <WebCore/ResourceRequest.h>
#include <cstdio>
#include <array>
#include <cstdlib>
#include <memory>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <wtf/JSONValues.h>
#include <wtf/UUID.h>
#include <wtf/text/MakeString.h>

using namespace WTF;
using namespace WebKit;
namespace fs = std::filesystem;
namespace {
unsigned checks, failures, completedSteps, completedCommands;
void check(bool pass, const char* label)
{
    ++checks;
    if (!pass) ++failures;
    printf("%s %s\n", pass ? "PASS" : "FAIL", label);
    fflush(stdout);
}
Ref<JSON::Object> object(const char* json)
{
    RefPtr value = JSON::Value::parseJSON(String::fromUTF8(json));
    RefPtr result = value ? value->asObject() : nullptr;
    RELEASE_ASSERT(result);
    return result.releaseNonNull();
}
std::string readFile(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    RELEASE_ASSERT(input);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
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

class PrivacyTests final : public BApplication {
public:
    PrivacyTests(fs::path root, fs::path package)
        : BApplication("application/x-vnd.Kunanyi-Summit-ExtensionPrivacyTests")
        , m_root(std::move(root)), m_package(std::move(package)) { }
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
        m_nonce = WTF::UUID::createVersion4().toString();
        m_store = WebsiteDataStore::createNonPersistent();
        m_privateStore = WebsiteDataStore::createNonPersistent();
        Ref configuration = WebExtensionControllerConfiguration::createDefault();
        configuration->setStorageDirectory(String::fromUTF8((m_root / "profile").c_str()));
        configuration->setDefaultWebsiteDataStore(m_store.get());
        m_controller = WebExtensionController::create(WTF::move(configuration));
        m_controller->setTestingMode(true);
        m_preferences = WebPreferences::create("privacy-fixture"_s, emptyString(), emptyString());
        m_preferences->setAcceleratedCompositingEnabled(false);
        m_preferences->setForceCompositingMode(false);
        m_preferences->setThreadedScrollingEnabled(false);
        m_preferences->setLinkPrefetchEnabled(false);
        m_preferences->setSpeculationRulesPrefetchEnabled(false);
        addRegularPage();
        addPrivatePage();
        if (!load(0, "installation-a"_s, 10) || !load(1, "installation-b"_s, 20)) {
            close();
            return;
        }
        plan();
        unsigned commands = 0;
        for (auto& step : m_steps) commands += step.context.has_value();
        printf("PRIVACY_PLAN steps=%zu commands=%u\n", m_steps.size(), commands);
        advance();
    }
    void Pulse() final
    {
        if (m_closing) {
            if (!hasChildren()) {
                check(true, "owned web and network processes drain after teardown");
                PostMessage(B_QUIT_REQUESTED);
            } else if (system_time() > m_deadline) {
                check(false, "owned processes finish teardown before the deadline");
                PostMessage(B_QUIT_REQUESTED);
            }
            return;
        }
        if (system_time() > m_deadline) {
            check(false, "real extension command completes before its deadline");
            close();
            return;
        }
        auto& step = m_steps[m_index];
        RELEASE_ASSERT(step.context);
        RefPtr context = m_contexts[*step.context];
        if (context->backgroundContentLoadError()) {
            check(false, "extension background loads without error");
            close();
            return;
        }
        RefPtr page = context->backgroundWebView();
        if (!page) return;
        auto title = page->pageLoadState().title();
        if (title.startsWith("SUMMIT PRIVACY FAIL "_s)) {
            printf("%s\n", title.utf8().legacyCStringPointer());
            check(false, "extension initializes real privacy bindings");
            close();
            return;
        }
        auto prefix = makeString("SUMMIT PRIVACY "_s, m_nonce, ' ', m_index, ' ');
        if (!title.startsWith(prefix)) return;
        RefPtr value = JSON::Value::parseJSON(title.substring(prefix.length()));
        RefPtr reply = value ? value->asObject() : nullptr;
        if (!reply || reply->getString("extension"_s) != context->uniqueIdentifier()
            || reply->getString("nonce"_s) != m_nonce || reply->getInteger("sequence"_s) != m_index) {
            check(false, "reply belongs to the current real extension and command");
            close();
            return;
        }
        printf("PRIVACY_RESPONSE %s\n", reply->toJSONString().utf8().legacyCStringPointer());
        step.verify(*reply);
        ++completedCommands;
        ++completedSteps;
        ++m_index;
        if (failures) close(); else advance();
    }
private:
    struct Step {
        const char* label;
        std::optional<unsigned> context;
        const char* json;
        std::function<void(JSON::Object&)> verify;
        std::function<void()> action;
    };
    void command(unsigned context, const char* label, const char* json, std::function<void(JSON::Object&)> verify)
    {
        m_steps.append({label, context, json, std::move(verify), { }});
    }
    void local(const char* label, std::function<void()> action)
    {
        m_steps.append({label, std::nullopt, nullptr, { }, std::move(action)});
    }
    void success(unsigned context, const char* label, const char* json, std::function<void()> after = { })
    {
        command(context, label, json, [label, after = std::move(after)](JSON::Object& reply) {
            check(reply.getBoolean("ok"_s) == true, label);
            if (RefPtr callbacks = reply.getValue("callbacks"_s))
                check(reply.getInteger("callbacks"_s) == 1 && reply.getBoolean("lastErrorCleared"_s) == true,
                    "callback runs once and runtime.lastError expires after callback");
            if (after) after();
        });
    }
    void error(unsigned context, const char* label, const char* json, const char* contains)
    {
        command(context, label, json, [label, contains](JSON::Object& reply) {
            check(reply.getBoolean("ok"_s) == false && !reply.getString("error"_s).isEmpty()
                && reply.getString("error"_s).contains(String::fromUTF8(contains)), label);
            if (reply.getValue("callbacks"_s))
                check(reply.getInteger("callbacks"_s) == 1 && reply.getBoolean("lastErrorCleared"_s) == true,
                    "callback error is scoped to exactly one invocation");
        });
    }
    void get(unsigned context, const char* label, const char* json, bool value, const char* control, std::optional<bool> specific = std::nullopt)
    {
        command(context, label, json, [label, value, control, specific](JSON::Object& reply) {
            RefPtr result = reply.getObject("result"_s);
            check(reply.getBoolean("ok"_s) == true && result && result->getBoolean("value"_s) == value
                && result->getString("levelOfControl"_s) == String::fromUTF8(control)
                && (specific ? result->getBoolean("incognitoSpecific"_s) == specific : !result->getValue("incognitoSpecific"_s)), label);
        });
    }
    void advance()
    {
        while (m_index < m_steps.size()) {
            auto& step = m_steps[m_index];
            printf("PRIVACY_STEP %zu %s\n", m_index, step.label);
            fflush(stdout);
            if (step.context) {
                Ref input = object(step.json);
                input->setInteger("sequence"_s, m_index);
                input->setString("nonce"_s, m_nonce);
                m_deadline = system_time() + 35000000;
                m_contexts[*step.context]->sendTestMessage("summit-privacy-command"_s, WTF::move(input));
                return;
            }
            step.action();
            ++completedSteps;
            ++m_index;
            if (failures) {
                close();
                return;
            }
        }
        check(completedSteps == m_steps.size(), "every planned API and lifecycle step completed");
        close();
    }
    bool load(unsigned index, const String& installation, uint64_t order)
    {
        if (!m_contexts[index]) {
            RefPtr<API::Error> error;
            Ref extension = WebExtension::create(String::fromUTF8(m_package.c_str()), error);
            if (error || !extension->manifestParsedSuccessfully()) {
                check(false, "privacy fixture package parses successfully");
                return false;
            }
            m_contexts[index] = WebExtensionContext::create(WTF::move(extension));
            m_contexts[index]->setUniqueIdentifier(makeString("summit-privacy-"_s, index));
            check(m_contexts[index]->setNativeInstallationMetadata(installation, order), "host supplies immutable installation identity and order");
            m_contexts[index]->setPermissionState(WebExtensionContext::PermissionState::GrantedExplicitly, "privacy"_s);
        }
        auto result = m_controller->load(*m_contexts[index]);
        check(result && *result, "real controller loads the approved privacy fixture");
        return result && *result;
    }
    void rememberProcesses()
    {
        for (Ref process : m_controller->allProcesses()) m_processes.add(process);
    }
    void unload(unsigned index)
    {
        rememberProcesses();
        auto result = m_controller->unload(*m_contexts[index]);
        check(result && *result && !m_contexts[index]->isLoaded(), "real controller unloads the extension");
    }
    Ref<WebView> page(WebsiteDataStore& store)
    {
        Ref configuration = API::PageConfiguration::create();
        configuration->setWebsiteDataStore(&store);
        configuration->setWebExtensionController(m_controller.get());
        configuration->setPreferences(RefPtr { m_preferences });
        Ref view = WebView::createForExtensionBackground(WTF::move(configuration));
        view->page()->loadRequest(WebCore::ResourceRequest { URL { "data:text/html,<title>Privacy preference page</title>"_s } });
        return view;
    }
    void addRegularPage() { m_regularPages.append(page(*m_store)); }
    void addPrivatePage() { RELEASE_ASSERT(!m_privatePage); m_privatePage = page(*m_privateStore); }
    void closePage(WebView& view)
    {
        view.page()->forEachWebContentProcess([&](auto& process, auto) { m_processes.add(process); });
        view.close();
    }
    void effective(bool regular, bool incognito, const char* key = "LinkPrefetchEnabled")
    {
        for (Ref view : m_regularPages)
            check(view->page()->preferencesStore().getBoolValueForKey(String::fromUTF8(key)) == regular,
                "existing and future regular pages receive the effective preference");
        if (m_privatePage)
            check(m_privatePage->page()->preferencesStore().getBoolValueForKey(String::fromUTF8(key)) == incognito,
                "private page receives only its eligible effective preference");
        if (std::string_view(key) == "LinkPrefetchEnabled") {
            for (Ref view : m_regularPages)
                check(view->page()->preferencesStore().getBoolValueForKey("SpeculationRulesPrefetchEnabled"_s) == regular,
                    "the network setting also updates speculation prefetch");
        }
    }
    void blockState()
    {
        m_blockedPath = fs::path(m_contexts[0]->storageDirectory().utf8().legacyCStringPointer());
        m_savedState = readFile(m_blockedPath / "State.json");
        fs::rename(m_blockedPath, m_root / "saved-state-directory");
        std::ofstream(m_blockedPath) << "unwritable parent fixture";
        m_stateBlocked = true;
    }
    void restoreState()
    {
        if (!m_stateBlocked) return;
        fs::remove(m_blockedPath);
        fs::rename(m_root / "saved-state-directory", m_blockedPath);
        m_stateBlocked = false;
        check(readFile(m_blockedPath / "State.json") == m_savedState, "failed API write preserves the exact committed state file");
    }
    void close()
    {
        if (m_closing) return;
        m_closing = true;
        m_deadline = system_time() + 20000000;
        restoreState();
        if (m_controller) {
            rememberProcesses();
            m_controller->unloadAll();
        }
        for (Ref view : m_regularPages) closePage(view);
        m_regularPages.clear();
        if (m_privatePage) closePage(*m_privatePage);
        m_privatePage = nullptr;
        for (auto& context : m_contexts) context = nullptr;
        m_controller = nullptr;
        m_preferences = nullptr;
        for (Ref process : m_processes) process->requestTermination(ProcessTerminationReason::RequestedByClient);
        m_processes.clear();
        if (m_store) m_store->terminateNetworkProcess();
        if (m_privateStore) m_privateStore->terminateNetworkProcess();
        m_store = nullptr;
        m_privateStore = nullptr;
    }
    void plan();

    std::unique_ptr<BMessageRunner> m_heartbeat;
    fs::path m_root, m_package, m_blockedPath;
    std::string m_savedState;
    RefPtr<WebsiteDataStore> m_store, m_privateStore;
    RefPtr<WebExtensionController> m_controller;
    std::array<RefPtr<WebExtensionContext>, 3> m_contexts;
    RefPtr<WebPreferences> m_preferences;
    Vector<Ref<WebView>> m_regularPages;
    RefPtr<WebView> m_privatePage;
    HashSet<Ref<WebProcessProxy>> m_processes;
    Vector<Step> m_steps;
    String m_nonce;
    size_t m_index { 0 };
    bigtime_t m_deadline { 0 };
    bool m_closing { false }, m_stateBlocked { false };
};

void PrivacyTests::plan()
{
    for (unsigned index : {0U, 1U}) {
        command(index, "real namespace exposes both aliases and all three setting objects", R"({"op":"probe"})", [](JSON::Object& reply) {
            RefPtr result = reply.getObject("result"_s);
            RefPtr names = result ? result->getArray("held"_s) : nullptr;
            check(reply.getBoolean("ok"_s) == true && result && result->getBoolean("aliases"_s) == true
                && names && names->length() == 3, "real privacy objects expose network, hyperlink and explicit unsupported RTC settings");
        });
    }
    get(0, "network get reflects the real disabled embedder baseline",
        R"({"op":"get","setting":"network","details":{}})", false, "controllable_by_this_extension");
    get(0, "hyperlink callback get reflects the real enabled embedder baseline",
        R"({"op":"get","setting":"hyperlink","mode":"callback","details":{}})", true, "controllable_by_this_extension");
    error(0, "get rejects nonboolean incognito details", R"({"op":"get","setting":"network","details":{"incognito":"yes"}})", "boolean");
    error(0, "set rejects nonboolean values", R"({"op":"set","setting":"network","details":{"value":"false"}})", "boolean");
    error(0, "set rejects an unknown scope", R"({"op":"set","setting":"network","details":{"value":true,"scope":"unknown"}})", "scope");
    error(0, "private query requires private access", R"({"op":"get","setting":"network","details":{"incognito":true}})", "private");
    error(0, "private setter requires private access", R"({"op":"set","setting":"network","details":{"value":true,"scope":"incognito_persistent"}})", "private");
    error(0, "unsupported RTC policy rejects its Promise", R"({"op":"set","setting":"rtc","details":{"value":"disable_non_proxied_udp"}})", "not available");
    error(0, "unsupported RTC callback uses lastError", R"({"op":"get","setting":"rtc","mode":"callback","details":{}})", "not available");

    success(0, "older extension sets its regular prediction preference", R"({"op":"set","setting":"network","details":{"value":true}})", [this] {
        effective(true, false);
        check(!m_preferences->linkPrefetchEnabled() && !m_preferences->speculationRulesPrefetchEnabled(),
            "extension overrides leave the shared embedder preference object intact");
    });
    get(1, "newer extension may control an older extension's value", R"({"op":"get","setting":"network","details":{}})", true, "controllable_by_this_extension");
    local("new regular page inherits the active override", [this] { addRegularPage(); effective(true, false); });
    success(1, "newer installation takes priority through callback setter", R"({"op":"set","setting":"network","mode":"callback","details":{"value":false}})", [this] { effective(false, false); });
    get(0, "older extension sees the newer controller", R"({"op":"get","setting":"network","details":{}})", false, "controlled_by_other_extensions");
    success(0, "older setter succeeds without displacing the newer installation", R"({"op":"set","setting":"network","details":{"value":true}})", [this] { effective(false, false); });
    success(1, "clearing the newer override reveals the older contribution", R"({"op":"clear","setting":"network","mode":"callback","details":{}})", [this] { effective(true, false); });
    command(0, "onChange reports effective changes without a duplicate older setter event", R"({"op":"events"})", [](JSON::Object& reply) {
        RefPtr events = reply.getArray("result"_s);
        bool matches = reply.getBoolean("ok"_s) == true && events && events->length() == 3;
        const bool expected[] = {true, false, true};
        const char* levels[] = {"controlled_by_this_extension", "controlled_by_other_extensions", "controlled_by_this_extension"};
        if (matches) {
            for (unsigned i = 0; i < 3; ++i) {
                RefPtr event = events->get(i)->asObject();
                RefPtr details = event ? event->getObject("details"_s) : nullptr;
                matches &= event && event->getString("setting"_s) == "network"_s && details
                    && details->getBoolean("value"_s) == expected[i]
                    && details->getString("levelOfControl"_s) == String::fromUTF8(levels[i])
                    && !details->getValue("incognitoSpecific"_s);
            }
        }
        check(matches, "onChange delivers exactly the three expected values and control levels");
    });
    local("disable removes the active override", [this] { unload(0); effective(false, false); });
    local("re-enable restores the saved override and original priority", [this] { load(0, "installation-a"_s, 10); effective(true, false); });
    get(0, "reloaded background reads its saved contribution", R"({"op":"get","setting":"network","details":{}})", true, "controlled_by_this_extension");

    local("grant private access to the older extension", [this] { m_contexts[0]->setHasAccessToPrivateData(true); effective(true, true); });
    success(0, "regular false is inherited by private pages", R"({"op":"set","setting":"network","details":{"value":false}})", [this] { effective(false, false); });
    success(0, "regular-only true does not change private inheritance", R"({"op":"set","setting":"network","details":{"value":true,"scope":"regular_only"}})", [this] { effective(true, false); });
    success(0, "persistent private true applies to private pages", R"({"op":"set","setting":"network","details":{"value":true,"scope":"incognito_persistent"}})", [this] { effective(true, true); });
    success(0, "private session false takes precedence", R"({"op":"set","setting":"network","details":{"value":false,"scope":"incognito_session_only"}})", [this] { effective(true, false); });
    get(0, "private get identifies the session contribution", R"({"op":"get","setting":"network","details":{"incognito":true}})", false, "controlled_by_this_extension", true);
    error(1, "other extensions do not gain private access", R"({"op":"get","setting":"network","mode":"callback","details":{"incognito":true}})", "private");
    local("disable and re-enable within a private session retain its session override", [this] {
        unload(0); effective(false, false); load(0, "installation-a"_s, 10); effective(true, false);
    });
    get(0, "private session survives a background reload", R"({"op":"get","setting":"network","details":{"incognito":true}})", false, "controlled_by_this_extension", true);
    local("ending the last private page drops session state", [this] { closePage(*m_privatePage); m_privatePage = nullptr; });
    error(0, "session setter fails with no private session", R"({"op":"set","setting":"network","details":{"value":false,"scope":"incognito_session_only"}})", "session");
    local("a later private session restores only persistent values", [this] { addPrivatePage(); effective(true, true); });
    get(0, "private get now exposes the persistent contribution", R"({"op":"get","setting":"network","details":{"incognito":true}})", true, "controlled_by_this_extension", true);
    local("revoke privacy permission", [this] {
        m_contexts[0]->setPermissionState(WebExtensionContext::PermissionState::DeniedExplicitly, "privacy"_s);
        effective(false, false);
    });
    error(0, "previously obtained setting object cannot bypass permission revocation", R"({"op":"set","setting":"network","held":true,"mode":"callback","details":{"value":true}})", "privacy");
    local("regrant privacy permission", [this] {
        m_contexts[0]->setPermissionState(WebExtensionContext::PermissionState::GrantedExplicitly, "privacy"_s);
        effective(true, true);
    });
    local("private-access revocation removes only private overrides", [this] { m_contexts[0]->setHasAccessToPrivateData(false); effective(true, false); });
    error(0, "private query fails after access revocation", R"({"op":"get","setting":"network","details":{"incognito":true}})", "private");
    local("private access can be granted again", [this] { m_contexts[0]->setHasAccessToPrivateData(true); effective(true, true); });
    success(0, "clear private persistence restores regular inheritance", R"({"op":"clear","setting":"network","details":{"scope":"incognito_persistent"}})", [this] { effective(true, false); });
    success(0, "clear regular-only restores regular false", R"({"op":"clear","setting":"network","details":{"scope":"regular_only"}})", [this] { effective(false, false); });
    success(0, "clear regular contribution restores the actual baseline", R"({"op":"clear","setting":"network","details":{}})", [this] { effective(false, false); });

    success(0, "hyperlink auditing can be disabled through its real API", R"({"op":"set","setting":"hyperlink","details":{"value":false}})", [this] {
        effective(false, false, "HyperlinkAuditingEnabled");
        check(m_preferences->store().getBoolValueForKey("HyperlinkAuditingEnabled"_s), "shared original hyperlink preference remains true");
    });
    success(1, "newer extension without private access controls regular hyperlink auditing", R"({"op":"set","setting":"hyperlink","details":{"value":true}})", [this] { effective(true, false, "HyperlinkAuditingEnabled"); });
    get(0, "regular hyperlink get reports the competing installation", R"({"op":"get","setting":"hyperlink","details":{}})", true, "controlled_by_other_extensions", false);
    get(0, "private hyperlink get retains the eligible older installation", R"({"op":"get","setting":"hyperlink","details":{"incognito":true}})", false, "controlled_by_this_extension", false);
    success(1, "clearing newer hyperlink control reveals older false", R"({"op":"clear","setting":"hyperlink","details":{}})", [this] { effective(false, false, "HyperlinkAuditingEnabled"); });
    local("make the extension state parent unwritable", [this] { blockState(); });
    error(0, "failed state write reports an API error", R"({"op":"set","setting":"hyperlink","mode":"callback","details":{"value":true}})", "saved");
    local("failed write changes neither page preferences nor committed state", [this] { effective(false, false, "HyperlinkAuditingEnabled"); restoreState(); });
    get(0, "failed setter does not mutate live controller state", R"({"op":"get","setting":"hyperlink","details":{}})", false, "controlled_by_this_extension", false);
    local("host changes its baseline while an extension override is active", [this] {
        WKPreferencesSetHyperlinkAuditingEnabled(toAPI(m_preferences.get()), false);
        effective(false, false, "HyperlinkAuditingEnabled");
    });
    success(0, "clear reveals the host's current baseline", R"({"op":"clear","setting":"hyperlink","details":{}})", [this] { effective(false, false, "HyperlinkAuditingEnabled"); });
    local("host updates propagate after the extension clears", [this] {
        WKPreferencesSetHyperlinkAuditingEnabled(toAPI(m_preferences.get()), true);
        effective(true, true, "HyperlinkAuditingEnabled");
    });
    get(0, "get observes the restored host baseline", R"({"op":"get","setting":"hyperlink","details":{}})", true, "controllable_by_this_extension", false);
    success(0, "save a contribution before context destruction", R"({"op":"set","setting":"hyperlink","details":{"value":false}})");
    local("destroy and recreate the context with the same installation identity", [this] {
        unload(0); m_contexts[0] = nullptr; load(0, "installation-a"_s, 10);
        effective(false, true, "HyperlinkAuditingEnabled");
    });
    get(0, "new context reads the persisted contribution", R"({"op":"get","setting":"hyperlink","details":{}})", false, "controlled_by_this_extension");
    local("reinstall uses a new immutable identity", [this] {
        unload(0); m_contexts[0] = nullptr; load(0, "installation-a-reinstalled"_s, 30);
        effective(true, true, "HyperlinkAuditingEnabled");
    });
    get(0, "reinstall cannot inherit an earlier installation's override", R"({"op":"get","setting":"hyperlink","details":{}})", true, "controllable_by_this_extension");
    local("legacy host loads without installation metadata", [this] { load(2, emptyString(), 0); });
    error(2, "unranked context cannot silently control profile preferences", R"({"op":"set","setting":"hyperlink","details":{"value":false}})", "installation priority");
}
} // namespace

int main()
{
    const char* package = getenv("SUMMIT_PRIVACY_PACKAGE");
    if (!package || !fs::path(package).is_absolute()) return 2;
    auto temporary = (fs::temp_directory_path() / "summit-privacy-api-XXXXXX").string();
    if (!mkdtemp(temporary.data())) return 2;
    fs::path root(temporary);
    {
        PrivacyTests application(root, package);
        application.Run();
    }
    check(!hasChildren(), "no owned helper process remains at exit");
    std::error_code error;
    fs::remove_all(root, error);
    check(!error && !fs::exists(root), "isolated fixture profile is removed");
    printf("PRIVACY_API_RESULT %s steps=%u commands=%u checks=%u failures=%u\n",
        failures ? "FAIL" : "PASS", completedSteps, completedCommands, checks, failures);
    return failures ? 1 : 0;
}
