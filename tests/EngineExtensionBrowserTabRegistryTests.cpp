#include "config.h"
#include "BrowserTabRegistryHaiku.h"
#include <Application.h>
#include <Handler.h>
#include <wtf/MainThread.h>
#include <cstdio>

using namespace WebKit;
static unsigned checks;
static unsigned failures;
static void check(bool result, const char* label)
{
    ++checks;
    if (!result) {
        ++failures;
        printf("FAIL %s\n", label);
    }
}
static void checkObserver(const BMessenger& first, const BMessenger& second)
{
    using Snapshot = BrowserTabRegistryHaiku::Snapshot;
    BrowserTabRegistryHaiku registry;
    Vector<std::pair<Snapshot, Snapshot>> changes;
    registry.setObserver([&](const Snapshot& before, const Snapshot& after) {
        changes.append({ before, after });
        check(registry.windows().size() == after.windows.size() && registry.focusedWindow() == after.focusedWindow,
            "observer runs after the replacement state is committed");
        for (auto& window : after.windows) {
            check(registry.window(window.identifier)->pages == window.pages, "observers see complete tab ownership");
            for (auto page : window.pages)
                check(registry.tab(page)->windowIdentifier == window.identifier, "no intermediate duplicate tab owner is visible");
        }
    });
    check(changes.isEmpty(), "registering an observer does not synthesize events");
    auto a = WebPageProxyIdentifier::generate();
    auto b = WebPageProxyIdentifier::generate();
    auto c = WebPageProxyIdentifier::generate();
    auto one = registry.update(first, { a, b }, a, true);
    check(one && changes.size() == 1 && changes[0].first.windows.isEmpty()
        && changes[0].second.windows[0].pages == Vector { a, b }, "first window publishes one complete transition");
    check(!changes[0].first.focusedWindow && changes[0].second.focusedWindow == one,
        "initial focus transition is included with creation");
    registry.update(first, { a, b }, a, true);
    registry.removePage(c);
    registry.update(second, { }, std::nullopt, false);
    check(changes.size() == 1, "unchanged snapshots and absent removals do not notify");
    check(!registry.update(first, { a, a }, a, false)
        && !registry.update(first, { a }, c, false)
        && !registry.update(BMessenger(), { c }, c, true), "invalid mutations are rejected");
    check(changes.size() == 1, "invalid mutations do not notify observers");
    registry.update(first, { b, a }, b, true);
    check(changes.size() == 2 && changes.last().first.windows[0].pages == Vector { a, b }
        && changes.last().second.windows[0].pages == Vector { b, a }, "reorder preserves independent before and after snapshots");
    auto two = registry.update(second, { c }, c, true);
    check(two && changes.last().second.focusedWindow == two && changes.last().first.focusedWindow == one,
        "focus switches retain the previous window identifier");
    auto count = changes.size();
    registry.update(first, { b, a }, b, false);
    check(changes.size() == count, "late deactivation of the old window does not repeat a focus transition");
    registry.update(second, { c }, c, false);
    check(changes.size() == count + 1 && !changes.last().second.focusedWindow,
        "deactivation reports loss of focus without inventing a replacement window");
    registry.update(second, { a, b, c }, c, true);
    check(changes.last().first.windows.size() == 2 && changes.last().second.windows.size() == 1
        && changes.last().second.windows[0].identifier == *two, "moving all pages removes the source window atomically");
    registry.removePage(c);
    check(!changes.last().second.windows[0].activePage && changes.last().first.windows[0].activePage == c,
        "page removal captures the previous active page without inventing a new selection");
    registry.clear();
    check(changes.last().first.windows.size() == 1 && changes.last().second.windows.isEmpty()
        && !changes.last().second.focusedWindow, "clear publishes one transition for all removed windows");
    count = changes.size();
    registry.clear();
    check(changes.size() == count, "clearing an empty registry does not notify");
    check(changes[0].second.windows[0].pages == Vector { a, b }, "retained snapshots remain unchanged through later mutations");

    unsigned callbacks = 0;
    registry.setObserver([&](const Snapshot& before, const Snapshot& after) {
        ++callbacks;
        auto retained = after;
        if (callbacks == 1) {
            registry.update(second, { b }, b, true);
            check(before.windows.isEmpty() && after.windows.size() == 1 && after.windows[0].pages == Vector { a },
                "nested mutation does not invalidate outer snapshot arguments");
            check(retained.windows[0].identifier == after.windows[0].identifier, "outer snapshot retains its original window identity");
        }
    });
    registry.update(first, { a }, a, true);
    check(callbacks == 2 && registry.windows().size() == 2, "nested changes each publish one complete transition");
    registry.setObserver([&](const Snapshot&, const Snapshot& after) {
        ++callbacks;
        registry.setObserver({ });
        check(after.windows.isEmpty(), "an observer can unregister during its own callback");
    });
    registry.clear();
    registry.update(first, { a }, a, true);
    check(callbacks == 3, "unregistered observers receive no later transitions");
    registry.clear();
}

static void checkRegistry(BApplication& application)
{
    BHandler firstHandler("first-window"), secondHandler("second-window");
    application.AddHandler(&firstHandler);
    application.AddHandler(&secondHandler);
    BMessenger first(&firstHandler), second(&secondHandler);
    check(first.IsValid() && second.IsValid() && first != second, "distinct live native messengers");
    auto a = WebPageProxyIdentifier::generate();
    auto b = WebPageProxyIdentifier::generate();
    auto c = WebPageProxyIdentifier::generate();
    auto d = WebPageProxyIdentifier::generate();
    BrowserTabRegistryHaiku registry;
    check(registry.windows().isEmpty() && !registry.focusedWindow() && !registry.tab(a), "initial empty state");
    auto firstID = registry.update(first, { a, b, c }, b, true);
    check(firstID && *firstID && registry.windows().size() == 1, "create browser window");
    check(registry.focusedWindow() == *firstID, "focused window recorded");
    check(registry.tab(a)->index == 0 && !registry.tab(a)->active && registry.tab(b)->index == 1 && registry.tab(b)->active, "tab order and active tab");
    auto snapshot = registry.windows();
    auto sameID = registry.update(first, { c, a, b }, a, true);
    check(sameID == firstID && registry.tab(c)->index == 0 && registry.tab(a)->active, "reorder keeps window identity");
    check(snapshot[0].pages[0] == a && snapshot[0].activePage == b, "snapshots remain independent");
    auto duplicate = registry.update(first, { a, a }, a, false);
    check(!duplicate && duplicate.error() == B_BAD_VALUE && registry.tab(c)->index == 0 && registry.focusedWindow() == *firstID, "duplicate pages rejected without mutation");
    auto absentActive = registry.update(first, { a, b }, d, false);
    check(!absentActive && registry.window(*firstID)->pages.size() == 3, "active page must belong to window");
    auto invalidWindow = registry.update(BMessenger(), { d }, d, true);
    check(!invalidWindow && registry.windows().size() == 1 && !registry.tab(d), "invalid window messenger rejected");
    auto secondID = registry.update(second, { d }, d, false);
    check(secondID && *secondID != *firstID && registry.windows()[0].identifier == *firstID, "unfocused new window preserves order");
    check(registry.focusedWindow() == *firstID, "unfocused other window preserves focus");
    check(registry.update(second, { d }, d, true).has_value() && registry.windows()[0].identifier == *secondID, "focused window moves to front");
    check(registry.focusedWindow() == *secondID, "focus switches windows");
    check(registry.update(first, { c, a, b }, a, false).has_value() && registry.focusedWindow() == *secondID, "unfocused old window cannot clear new focus");
    check(registry.update(second, { d }, std::nullopt, false).has_value() && !registry.focusedWindow() && !registry.tab(d)->active, "clear focused and active state explicitly");
    check(registry.update(second, { d, a }, a, true).has_value(), "move active tab across windows");
    check(registry.tab(a)->windowIdentifier == *secondID && registry.tab(a)->index == 1 && registry.tab(a)->active, "moved tab has one new owner");
    check(registry.window(*firstID)->pages == Vector<WebPageProxyIdentifier> { c, b } && !registry.window(*firstID)->activePage, "old active selection cleared after move");
    registry.removePage(a);
    check(!registry.tab(a) && !registry.window(*secondID)->activePage && registry.tab(d)->index == 0, "remove active page without inventing selection");
    registry.removePage(d);
    check(!registry.window(*secondID) && !registry.focusedWindow(), "remove last page removes focused window");
    registry.removePage(d);
    check(registry.windows().size() == 1, "repeated page removal harmless");
    check(!registry.update(first, { }, c, false) && registry.window(*firstID), "empty window cannot retain an active page");
    auto removed = registry.update(first, { }, std::nullopt, false);
    check(removed == firstID && registry.windows().isEmpty(), "empty snapshot removes existing window");
    check(registry.update(first, { }, std::nullopt, false) == uint64_t { 0 }, "empty unknown window is idempotent");
    auto renewedID = registry.update(first, { a }, a, true);
    check(renewedID && *renewedID != *firstID && *renewedID != *secondID, "removed window IDs are not reused");
    registry.clear();
    auto afterClearID = registry.update(first, { a }, a, true);
    check(afterClearID && *afterClearID != *renewedID && !registry.window(*renewedID), "clear does not resurrect old window identity");
    BrowserTabRegistryHaiku otherProfile;
    check(otherProfile.windows().isEmpty() && !otherProfile.tab(a), "registry state is separate per owner");
    auto otherID = otherProfile.update(second, { a }, a, true);
    check(otherID && registry.tab(a)->windowIdentifier == *afterClearID, "another registry cannot move this profile's tab");
    registry.clear();
    Vector<WebPageProxyIdentifier> pages;
    for (unsigned i = 0; i < 512; ++i)
        pages.append(WebPageProxyIdentifier::generate());
    auto largeID = registry.update(first, pages, pages[255], true);
    check(largeID && registry.window(*largeID)->pages.size() == 512 && registry.tab(pages[255])->active, "full 512-tab browser window");
    for (size_t i = 0; i < pages.size(); ++i)
        check(registry.tab(pages[i])->index == i, "every tab index is consistent");
    for (auto page : pages)
        registry.removePage(page);
    check(registry.windows().isEmpty() && !registry.focusedWindow(), "all pages close without stale windows");
    registry.clear();
    otherProfile.clear();
    checkObserver(first, second);
    application.RemoveHandler(&secondHandler);
    application.RemoveHandler(&firstHandler);
}

class RegistryApplication final : public BApplication {
public:
    bool didRun { false };
    RegistryApplication() : BApplication("application/x-vnd.Kunanyi-Summit-browser-tab-registry-tests") { }
    void ReadyToRun() override
    {
        WTF::initializeMainThread();
        checkRegistry(*this);
        didRun = true;
        PostMessage(B_QUIT_REQUESTED);
    }
};

int main()
{
    RegistryApplication application;
    application.Run();
    check(application.didRun, "the native suite ran from BApplication::ReadyToRun");
    printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
