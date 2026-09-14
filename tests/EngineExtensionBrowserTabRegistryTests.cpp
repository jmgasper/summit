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
int main()
{
    BApplication application("application/x-vnd.Kunanyi-Summit-browser-tab-registry-tests");
    WTF::initializeMainThread();
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
    application.RemoveHandler(&secondHandler);
    application.RemoveHandler(&firstHandler);
    printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
