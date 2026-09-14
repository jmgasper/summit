#include "config.h"
#include "WebExtensionTestMessageQueueHaiku.h"
#include <Application.h>
#include <cstdio>
#include <wtf/RefCounted.h>
#include <wtf/JSONValues.h>
#include <wtf/HashMap.h>
#include <wtf/WeakHashCountedSet.h>
#include <wtf/MainThread.h>
#include <wtf/Vector.h>

using namespace WebKit;
static unsigned checks, failures;
static void check(bool condition, const char* message)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}

struct Message {
    int identifier;
    Ref<JSON::Value> argument;
};

static Message message(int identifier)
{
    return { identifier, JSON::Value::create(identifier) };
}

struct TestFrame : RefCounted<TestFrame>, CanMakeWeakPtr<TestFrame> { };
using ListenerMap = HashMap<WebExtensionEventListenerTypeWorldPair, WeakHashCountedSet<TestFrame>>;

static void testLiveListeners()
{
    using Event = WebExtensionEventListenerType;
    using World = WebExtensionContentWorldType;
    ListenerMap listeners;
    Ref first = adoptRef(*new TestFrame);
    Ref second = adoptRef(*new TestFrame);
    check(!hasWebExtensionTestEventListenersHaiku(Event::TestOnMessage, listeners), "an empty registry has no test listeners");
    for (auto type : { Event::TestOnMessage, Event::TestOnTestStarted, Event::TestOnTestFinished }) {
        for (auto world : { World::Main, World::ContentScript, World::WebPage }) {
            listeners.clear();
            auto& frames = listeners.add({ type, world }, WeakHashCountedSet<TestFrame> { }).iterator->value;
            check(!hasWebExtensionTestEventListenersHaiku(type, listeners), "an empty registry bucket is unavailable");
            frames.add(first.get());
            frames.add(first.get());
            frames.add(second.get());
            check(hasWebExtensionTestEventListenersHaiku(type, listeners), "live test listeners are found in every routed world");
            frames.remove(first.get());
            check(hasWebExtensionTestEventListenersHaiku(type, listeners), "one removal retains duplicate registrations");
            frames.remove(first.get());
            check(hasWebExtensionTestEventListenersHaiku(type, listeners), "another live frame keeps delivery available");
            frames.remove(second.get());
            check(!hasWebExtensionTestEventListenersHaiku(type, listeners), "removing all registrations stops delivery despite a retained bucket");
        }
    }
    listeners.clear();
    auto& frames = listeners.add({ Event::TestOnMessage, World::Main }, WeakHashCountedSet<TestFrame> { }).iterator->value;
    {
        Ref transient = adoptRef(*new TestFrame);
        frames.add(transient.get());
        check(hasWebExtensionTestEventListenersHaiku(Event::TestOnMessage, listeners), "a temporary frame initially provides a listener");
    }
    check(!hasWebExtensionTestEventListenersHaiku(Event::TestOnMessage, listeners), "destroyed weak frames do not keep delivery available");
    frames.add(first.get());
    check(hasWebExtensionTestEventListenersHaiku(Event::TestOnMessage, listeners), "a live frame is found alongside dead weak entries");
    check(!hasWebExtensionTestEventListenersHaiku(Event::TestOnTestStarted, listeners), "listeners for another test event do not enable this event");
    listeners.add({ Event::RuntimeOnMessage, World::Main }, WeakHashCountedSet<TestFrame> { }).iterator->value.add(first.get());
    check(!hasWebExtensionTestEventListenersHaiku(Event::RuntimeOnMessage, listeners), "ordinary extension events are not test events");

    Deque<Message> queue;
    queue.append(message(1));
    queue.append(message(2));
    Vector<int> delivered;
    auto canSend = [&] { return hasWebExtensionTestEventListenersHaiku(Event::TestOnMessage, listeners); };
    drainWebExtensionTestMessagesHaiku(queue, canSend, [&](Message value) {
        delivered.append(value.identifier);
        // Registry insertion can move buckets, so retrieve the set again.
        listeners.find({ Event::TestOnMessage, World::Main })->value.remove(first.get());
    });
    check(delivered == Vector<int> { 1 } && queue.size() == 1, "last-listener removal during draining retains the unsent message");
    drainWebExtensionTestMessagesHaiku(queue, canSend, [&](Message value) { delivered.append(value.identifier); });
    check(delivered == Vector<int> { 1 } && queue.size() == 1, "a later drain without listeners still retains the message");
    listeners.add({ Event::TestOnMessage, World::WebPage }, WeakHashCountedSet<TestFrame> { }).iterator->value.add(second.get());
    drainWebExtensionTestMessagesHaiku(queue, canSend, [&](Message value) { delivered.append(value.identifier); });
    check(delivered == Vector<int> { 1, 2 } && queue.isEmpty(), "a new listener in another world resumes the queued delivery");
}

int main()
{
    BApplication application("application/x-vnd.Kunanyi-Summit-extension-test-queue-tests");
    WTF::initializeMainThread();
    testLiveListeners();
    Deque<Message> queue;
    Vector<int> delivered;
    auto ready = [] { return true; };
    auto receive = [&](Message value) { delivered.append(value.identifier); };
    drainWebExtensionTestMessagesHaiku(queue, ready, receive);
    check(delivered.isEmpty() && queue.isEmpty(), "empty queues produce no delivery");

    queue.append(message(1));
    queue.append(message(2));
    drainWebExtensionTestMessagesHaiku(queue, [] { return false; }, receive);
    check(delivered.isEmpty() && queue.size() == 2, "unavailable delivery retains queued messages");
    check(queue.first().identifier == 1 && queue.first().argument->asInteger() == 1, "waiting retains order and payload");
    queue.append(message(3));
    drainWebExtensionTestMessagesHaiku(queue, ready, receive);
    check(delivered == Vector<int> { 1, 2, 3 } && queue.isEmpty(), "ready delivery drains in FIFO order");

    delivered.clear();
    bool available = true;
    for (int identifier : { 4, 5, 6 })
        queue.append(message(identifier));
    drainWebExtensionTestMessagesHaiku(queue, [&] { return available; }, [&](Message value) {
        receive(WTF::move(value));
        available = false;
    });
    check(delivered == Vector<int> { 4 } && queue.size() == 2 && queue.first().identifier == 5,
        "losing availability stops before removing the next message");
    available = true;
    drainWebExtensionTestMessagesHaiku(queue, [&] { return available; }, receive);
    check(delivered == Vector<int> { 4, 5, 6 } && queue.isEmpty(), "delivery resumes without duplicates or reordering");

    delivered.clear();
    queue.append(message(7));
    queue.append(message(8));
    drainWebExtensionTestMessagesHaiku(queue, ready, [&](Message value) {
        delivered.append(value.identifier);
        queue.append(WTF::move(value));
    });
    check(delivered == Vector<int> { 7, 8 } && queue.size() == 2, "requeueing every message cannot prolong a drain indefinitely");
    check(queue.first().identifier == 7 && queue.last().identifier == 8, "requeued messages keep their relative order");
    delivered.clear();
    drainWebExtensionTestMessagesHaiku(queue, ready, receive);
    check(delivered == Vector<int> { 7, 8 } && queue.isEmpty(), "requeued messages remain available for the next drain");

    delivered.clear();
    queue.append(message(9));
    queue.append(message(10));
    drainWebExtensionTestMessagesHaiku(queue, ready, [&](Message value) {
        if (value.identifier == 9)
            queue.append(message(11));
        receive(WTF::move(value));
    });
    check(delivered == Vector<int> { 9, 10 } && queue.size() == 1 && queue.first().identifier == 11,
        "messages appended during delivery wait behind the original batch");
    drainWebExtensionTestMessagesHaiku(queue, ready, receive);
    check(delivered == Vector<int> { 9, 10, 11 }, "the next drain delivers an appended message once");

    delivered.clear();
    queue.append(message(12));
    queue.append(message(13));
    drainWebExtensionTestMessagesHaiku(queue, ready, [&](Message value) {
        receive(WTF::move(value));
        queue.clear();
    });
    check(delivered == Vector<int> { 12 } && queue.isEmpty(), "clearing pending messages during delivery does not read an empty queue");

    JSON::Value* retainedArgument;
    {
        Ref object = JSON::Object::create();
        object->setString("text"_s, String::fromUTF8("Hobart \u2014 \u5c71\nnext line"));
        retainedArgument = object.ptr();
        queue.append({ 14, WTF::move(object) });
    }
    bool retained = false;
    drainWebExtensionTestMessagesHaiku(queue, ready, [&](Message value) {
        retained = value.argument.ptr() == retainedArgument
            && value.argument->asObject()->getString("text"_s) == String::fromUTF8("Hobart \u2014 \u5c71\nnext line");
    });
    check(retained && queue.isEmpty(), "queued JSON keeps identity and content after the caller releases its reference");
    queue.append({ 15, JSON::Value::null() });
    queue.append({ 16, JSON::Value::create(false) });
    queue.append({ 17, JSON::Value::create(String { "value"_s }) });
    Vector<String> payloads;
    drainWebExtensionTestMessagesHaiku(queue, ready, [&](Message value) { payloads.append(value.argument->toJSONString()); });
    check(payloads == Vector<String> { "null"_s, "false"_s, "\"value\""_s }, "JSON fragments retain their types");

    Deque<Message> anotherQueue;
    anotherQueue.append(message(18));
    queue.append(message(19));
    drainWebExtensionTestMessagesHaiku(queue, ready, receive);
    check(anotherQueue.size() == 1 && anotherQueue.first().identifier == 18, "draining one event queue leaves another untouched");
    delivered.clear();
    for (int identifier = 0; identifier < 10000; ++identifier)
        queue.append(message(identifier));
    drainWebExtensionTestMessagesHaiku(queue, ready, receive);
    bool ordered = delivered.size() == 10000;
    for (size_t index = 0; index < delivered.size(); ++index)
        ordered &= delivered[index] == static_cast<int>(index);
    check(ordered && queue.isEmpty(), "ten thousand messages drain in order without recursive dispatch");
    std::printf("%u native extension test-queue checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
