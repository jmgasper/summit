#include "config.h"
#include "WebExtensionDeferredReply.h"
#include <Application.h>
#include <cstdio>
#include <expected>
#include <memory>
#include <wtf/MainThread.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

using namespace WebKit;
using Result = std::expected<String, String>;

static unsigned checks;
static unsigned failures;
static void check(bool condition, const char* message)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}

int main()
{
    BApplication application("application/x-vnd.Kunanyi-Summit-extension-deferred-reply-tests");
    WTF::initializeMainThread();
    unsigned calls = 0;
    Result last = String { "initial"_s };
    auto callback = [&]() -> CompletionHandler<void(Result&&)> {
        return [&](Result&& result) { ++calls; last = WTF::move(result); };
    };
    auto cancelled = []() -> Result { return std::unexpected(String { "background unavailable"_s }); };

    {
        auto reply = deferWebExtensionReply(callback(), cancelled());
        check(!calls, "deferring does not complete a live reply");
    }
    check(calls == 1 && !last && last.error() == "background unavailable"_s, "discarded pending reply completes with its fallback");

    calls = 0;
    RefPtr<DeferredWebExtensionReply<Result>> retained;
    {
        auto reply = deferWebExtensionReply(callback(), cancelled());
        retained = reply.ptr();
    }
    check(!calls, "retained pending state does not cancel early");
    retained = nullptr;
    check(calls == 1 && !last, "last pending reference triggers one cancellation");

    calls = 0;
    {
        auto reply = deferWebExtensionReply(callback(), cancelled());
        auto completion = resumeWebExtensionReply(reply.copyRef());
        check(!calls, "resuming alone does not deliver a result");
        completion(String { "loaded"_s });
        check(calls == 1 && last && *last == "loaded"_s, "resumed handler forwards a successful result");
    }
    check(calls == 1 && last, "completed state does not later send its fallback");

    calls = 0;
    {
        auto reply = deferWebExtensionReply(callback(), cancelled());
        auto first = resumeWebExtensionReply(reply.copyRef());
        auto second = resumeWebExtensionReply(WTF::move(reply));
        first(std::unexpected(String { "specific error"_s }));
        second(String { "late success"_s });
        check(calls == 1 && !last && last.error() == "specific error"_s, "first explicit error wins over a later result");
    }
    check(calls == 1, "duplicate triggers and destruction still complete once");

    calls = 0;
    Vector<Function<void()>> tasks;
    {
        auto reply = deferWebExtensionReply(callback(), cancelled());
        tasks.append([reply = WTF::move(reply)]() mutable {
            auto completion = resumeWebExtensionReply(WTF::move(reply));
            completion(String { "ready"_s });
        });
    }
    check(!calls, "queued task owns its pending reply");
    tasks.clear();
    check(calls == 1 && !last, "discarding an unstarted task settles the reply normally");

    calls = 0;
    {
        auto reply = deferWebExtensionReply(callback(), cancelled());
        tasks.append([reply = WTF::move(reply)]() mutable {
            auto completion = resumeWebExtensionReply(WTF::move(reply));
            completion(String { "ready"_s });
        });
    }
    auto readyTask = tasks.takeLast();
    readyTask();
    readyTask = nullptr;
    check(calls == 1 && last && *last == "ready"_s, "started task transfers and completes its pending reply");

    unsigned movedValue = 0;
    {
        auto reply = deferWebExtensionReply<std::unique_ptr<unsigned>>(
            [&](std::unique_ptr<unsigned>&& value) { movedValue = *value; }, std::make_unique<unsigned>(17));
    }
    check(movedValue == 17, "fallback supports move-only ownership");
    {
        auto reply = deferWebExtensionReply<std::unique_ptr<unsigned>>(
            [&](std::unique_ptr<unsigned>&& value) { movedValue = *value; }, std::make_unique<unsigned>(17));
        auto completion = resumeWebExtensionReply(WTF::move(reply));
        completion(std::make_unique<unsigned>(31));
    }
    check(movedValue == 31, "resumed result transfers move-only ownership");

    using VoidResult = std::expected<void, String>;
    unsigned voidCalls = 0;
    {
        auto reply = deferWebExtensionReply<VoidResult>([&](VoidResult&& result) {
            ++voidCalls;
            check(!result && result.error() == "cancelled"_s, "void result preserves cancellation errors");
        }, std::unexpected(String { "cancelled"_s }));
    }
    check(voidCalls == 1, "void reply cancellation executes once");

    using PageResult = std::expected<std::optional<uint64_t>, String>;
    unsigned pageCalls = 0;
    {
        auto reply = deferWebExtensionReply<PageResult>([&](PageResult&& result) {
            ++pageCalls;
            check(result && !result->has_value(), "optional result can default to an absent page");
        }, PageResult { std::optional<uint64_t> { } });
    }
    check(pageCalls == 1, "optional reply executes once");

    std::printf("%u native deferred reply checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
