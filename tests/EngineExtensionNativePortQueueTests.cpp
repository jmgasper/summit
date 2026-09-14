#include "config.h"
#include "WebExtensionNativePortMessageQueue.h"
#include <Application.h>
#include <cstdio>
#include <memory>
#include <string>
#include <wtf/MainThread.h>

using namespace WebKit;
using Queue = WebExtensionNativePortMessageQueue;
using Result = Queue::Result;
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
    BApplication application("application/x-vnd.Kunanyi-Summit-native-port-queue-tests");
    WTF::initializeMainThread();
    for (auto message : { "null"_s, "true"_s, "false"_s, "0"_s, "-1.25e+4"_s,
             "\"text\""_s, "[]"_s, "{}"_s, "{\"items\":[1,null,false]}"_s,
             " \n\t null\r "_s, "\"\\u0000\""_s, "\"\\ud83d\\ude00\""_s })
        check(Queue::isValidMessage(message), "JSON objects, arrays and fragments are accepted");
    for (auto message : { ""_s, " "_s, "undefined"_s, "NaN"_s, "Infinity"_s,
             "[1,]"_s, "{bad:1}"_s, "{}[]"_s, "null trailing"_s, "01"_s,
             "\"unterminated"_s, "/*comment*/null"_s })
        check(!Queue::isValidMessage(message), "malformed JSON is rejected");
    std::string oversized(webExtensionMaxMessageLength + 1, 'a');
    oversized.front() = '"';
    oversized.back() = '"';
    check(!Queue::isValidMessage(String::fromUTF8(oversized)), "oversized valid JSON is rejected");
    oversized.clear();
    oversized.shrink_to_fit();

    Ref queue = Queue::create();
    Vector<String> delivered;
    check(queue->receive({ "null"_s, "{\"first\":1}"_s }) == Result::Accepted, "messages wait for host handler");
    check(delivered.isEmpty(), "waiting messages do not get fabricated replies");
    queue->setMessageHandler(createSharedTask<void(const String&)>([&](const String& value) { delivered.append(value); }));
    check(delivered == Vector<String> { "null"_s, "{\"first\":1}"_s }, "installing handler drains pending JSON in order");
    queue->setMessageHandler(nullptr);
    check(queue->receive({ "2"_s }) == Result::Accepted && delivered.size() == 2, "removing handler pauses delivery");
    queue->setMessageHandler(createSharedTask<void(const String&)>([&](const String& value) { delivered.append(value); }));
    check(delivered.last() == "2"_s && delivered.size() == 3, "replacement drains pending message once");
    check(queue->receive({ "3"_s, "invalid"_s }) == Result::InvalidMessage && delivered.size() == 3, "invalid batch delivers no partial prefix");
    check(queue->receive({ }) == Result::Accepted && delivered.size() == 3, "empty batch is harmless");
    queue->close();
    queue->close();
    check(queue->isClosed(), "close is idempotent");
    check(queue->receive({ "4"_s }) == Result::Closed, "closed queue rejects new messages");
    queue->setMessageHandler(createSharedTask<void(const String&)>([&](const String&) { check(false, "closed queue cannot be reopened by a handler"); }));
    check(delivered.size() == 3, "closed queue keeps delivery count unchanged");

    Ref reentrant = Queue::create();
    delivered.clear();
    unsigned depth = 0;
    reentrant->setMessageHandler(createSharedTask<void(const String&)>([&](const String& value) {
        check(++depth == 1, "reentrant enqueue does not recursively call the handler");
        delivered.append(value);
        if (value == "1"_s)
            check(reentrant->receive({ "3"_s }) == Result::Accepted, "handler can enqueue another message");
        --depth;
    }));
    check(reentrant->receive({ "1"_s, "2"_s }) == Result::Accepted, "batch is accepted with active handler");
    check(delivered == Vector<String> { "1"_s, "2"_s, "3"_s }, "reentrant message follows the whole earlier batch");

    Ref replacing = Queue::create();
    delivered.clear();
    unsigned replacementCalls = 0;
    replacing->setMessageHandler(createSharedTask<void(const String&)>([&](const String& value) {
        delivered.append(value);
        replacing->setMessageHandler(createSharedTask<void(const String&)>([&](const String& next) { ++replacementCalls; delivered.append(next); }));
    }));
    replacing->receive({ "1"_s, "2"_s, "3"_s });
    check(replacementCalls == 2 && delivered.size() == 3, "self-replacement takes effect for the next queued message");

    Ref closing = Queue::create();
    unsigned closingCalls = 0;
    auto token = std::make_shared<int>(42);
    std::weak_ptr<int> lifetime = token;
    closing->setMessageHandler(createSharedTask<void(const String&)>([&, token](const String&) {
        ++closingCalls;
        closing->close();
        check(!lifetime.expired() && *token == 42, "active callback survives clearing its owning handler");
    }));
    token.reset();
    closing->receive({ "1"_s, "2"_s });
    check(closingCalls == 1 && closing->isClosed(), "disconnect during callback discards the rest of the batch");
    check(lifetime.expired(), "completed callback releases its captured state");

    Ref paused = Queue::create();
    unsigned pauseCalls = 0;
    paused->setMessageHandler(createSharedTask<void(const String&)>([&](const String&) { ++pauseCalls; paused->setMessageHandler(nullptr); }));
    paused->receive({ "1"_s, "2"_s });
    check(pauseCalls == 1, "handler can pause the remaining batch");
    paused->close();
    paused->setMessageHandler(createSharedTask<void(const String&)>([&](const String&) { ++pauseCalls; }));
    check(pauseCalls == 1, "close clears pending messages while paused");

    Ref limited = Queue::create();
    Vector<String> many;
    for (unsigned i = 0; i < 1024; ++i)
        many.append("null"_s);
    check(limited->receive(WTF::move(many)) == Result::Accepted, "queue permits its documented message-count limit");
    check(limited->receive({ "true"_s }) == Result::QueueFull, "message-count overflow is rejected");
    unsigned count = 0;
    limited->setMessageHandler(createSharedTask<void(const String&)>([&](const String& value) { ++count; if (value != "null"_s) check(false, "overflow message must not enter queue"); }));
    check(count == 1024, "overflow preserves every prior pending message exactly once");
    check(limited->receive({ "null"_s }) == Result::Accepted && count == 1025, "draining releases queue capacity");

    Ref lengthLimited = Queue::create();
    std::string chunkBytes(1024 * 1024, 'a');
    chunkBytes.front() = '"';
    chunkBytes.back() = '"';
    String chunk = String::fromUTF8(chunkBytes);
    Vector<String> chunks;
    for (unsigned i = 0; i < 64; ++i)
        chunks.append(chunk);
    check(lengthLimited->receive(WTF::move(chunks)) == Result::Accepted, "pending character budget permits exactly 64 Mi characters");
    check(lengthLimited->receive({ "0"_s }) == Result::QueueFull, "cumulative character budget rejects one extra character");
    unsigned chunkCount = 0;
    lengthLimited->setMessageHandler(createSharedTask<void(const String&)>([&](const String&) { ++chunkCount; }));
    check(chunkCount == 64, "character overflow preserves the existing batch");
    check(lengthLimited->receive({ "0"_s }) == Result::Accepted && chunkCount == 65, "draining releases the character budget");

    RefPtr<Queue> released = Queue::create();
    unsigned releasedCalls = 0;
    released->setMessageHandler(createSharedTask<void(const String&)>([&](const String&) { ++releasedCalls; released = nullptr; }));
    check(released->receive({ "1"_s, "2"_s }) == Result::Accepted, "delivery protects queue when host drops its last reference");
    check(!released && releasedCalls == 2, "protected queue finishes its batch without use-after-free");

    std::printf("%u native port queue checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
