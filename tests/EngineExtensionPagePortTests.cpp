#include "config.h"
#include "WebExtensionPagePortMap.h"
#include <Application.h>
#include <cstdio>
#include <wtf/MainThread.h>

using namespace WebKit;

static unsigned checks;
static unsigned failures;

static void check(bool value, const char* message)
{
    ++checks;
    if (!value) {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}

int main()
{
    BApplication application("application/x-vnd.Kunanyi-Summit-extension-page-port-tests");
    WTF::initializeMainThread();
    using World = WebExtensionContentWorldType;
    using Channel = WebExtensionPagePortMap::Channel;
    const WebPageProxyIdentifier firstPage { 1 }, secondPage { 2 };
    const WebExtensionPortChannelIdentifier firstID { 11 }, secondID { 12 };
    const Channel first { World::Main, World::ContentScript, firstID };
    const Channel second { World::Main, World::ContentScript, secondID };
    const Channel reverse { World::ContentScript, World::Main, firstID };
    WebExtensionPagePortMap ports;

    check(!ports.contains(firstPage), "new map has no page");
    check(ports.take(firstPage).isEmpty(), "taking an absent page is empty");
    ports.remove(firstPage, first);
    check(!ports.contains(firstPage), "removing an absent page is harmless");
    ports.add(firstPage, first, 0);
    check(!ports.contains(firstPage), "zero listeners do not create a page");

    ports.add(firstPage, first, 3);
    ports.add(firstPage, first, 2);
    check(ports.contains(firstPage), "listener batch creates page ownership");
    auto batch = ports.take(firstPage);
    check(batch.size() == 1 && batch.count(first) == 5, "listener counts accumulate across registration batches");
    check(!ports.contains(firstPage), "taking ports removes page ownership");
    check(ports.take(firstPage).isEmpty(), "page teardown can be repeated");

    ports.add(firstPage, first, 3);
    ports.remove(firstPage, first);
    check(ports.contains(firstPage), "removing one listener preserves the others");
    batch = ports.take(firstPage);
    check(batch.count(first) == 2, "teardown receives every remaining listener");

    ports.add(firstPage, first, 1);
    ports.add(firstPage, second, 4);
    ports.remove(firstPage, first);
    check(ports.contains(firstPage), "closing one channel preserves other channels on the page");
    ports.remove(firstPage, first);
    batch = ports.take(firstPage);
    check(!batch.contains(first), "closed channel is absent from teardown");
    check(batch.count(second) == 4, "duplicate channel removal preserves unrelated listeners");

    ports.add(firstPage, first, 1);
    ports.add(secondPage, first, 2);
    ports.remove(firstPage, first);
    check(!ports.contains(firstPage), "last listener removes its page entry");
    check(ports.contains(secondPage), "same channel on another page survives");
    check(ports.take(secondPage).count(first) == 2, "other page keeps its exact listener count");

    ports.add(firstPage, first, 2);
    ports.add(firstPage, reverse, 3);
    ports.remove(firstPage, first);
    batch = ports.take(firstPage);
    check(batch.count(first) == 1, "source world is part of the channel key");
    check(batch.count(reverse) == 3, "reverse world endpoints keep independent counts");

    ports.add(firstPage, first, 1);
    batch = ports.take(firstPage);
    ports.add(firstPage, second, 2);
    check(batch.count(first) == 1 && !batch.contains(second), "teardown snapshot is independent of new registrations");
    ports.remove(firstPage, first);
    check(ports.take(firstPage).count(second) == 2, "late removal from a teardown snapshot preserves new channels");

    ports.add(firstPage, first, 1);
    ports.add(secondPage, second, 1);
    ports.clear();
    check(!ports.contains(firstPage) && !ports.contains(secondPage), "context unload clears every page");
    ports.add(firstPage, first, 1);
    ports.remove(firstPage, first);
    check(!ports.contains(firstPage), "map can be reused after unload");

    check(ports.pages(World::Main, firstID).isEmpty(), "empty ownership map has no disconnect recipients");
    ports.add(firstPage, first, 3);
    ports.add(firstPage, { World::Main, World::Native, firstID }, 1);
    ports.add(firstPage, second, 1);
    ports.add(secondPage, reverse, 1);
    auto recipients = ports.pages(World::Main, firstID);
    check(recipients.size() == 1 && recipients.contains(firstPage), "same page is selected once across multiple ports and target worlds");
    check(ports.pages(World::ContentScript, firstID) == Vector<WebPageProxyIdentifier> { secondPage }, "disconnect recipients use the endpoint source world");
    check(ports.pages(World::Main, secondID) == Vector<WebPageProxyIdentifier> { firstPage }, "disconnect recipients use the requested channel");
    check(ports.pages(World::Native, firstID).isEmpty(), "native target alone does not make a page a native endpoint");
    ports.add(secondPage, first, 2);
    recipients = ports.pages(World::Main, firstID);
    check(recipients.size() == 2 && recipients.contains(firstPage) && recipients.contains(secondPage), "every page owning the channel is selected");
    ports.take(firstPage);
    check(ports.pages(World::Main, firstID) == Vector<WebPageProxyIdentifier> { secondPage }, "removed page is absent from future disconnect delivery");
    check(recipients.size() == 2, "recipient snapshot survives changes to the ownership map");
#if ENABLE(INSPECTOR_EXTENSIONS)
    ports.add(firstPage, { World::Inspector, World::Native, firstID }, 1);
    recipients = ports.pages(World::Main, firstID);
    check(recipients.size() == 2 && recipients.contains(firstPage), "main-world recipients include inspector alias");
    check(ports.pages(World::Inspector, firstID).size() == 2, "inspector-world recipients include main alias");
#endif
    ports.clear();
    check(ports.pages(World::Main, firstID).isEmpty(), "unload removes every disconnect recipient");

    std::printf("%u page-port ownership checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
