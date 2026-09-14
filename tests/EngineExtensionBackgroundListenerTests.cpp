#include "config.h"
#include "WebExtensionBackgroundListenersHaiku.h"
#include <Application.h>
#include <cstdio>
#include <wtf/MainThread.h>
#include <wtf/text/MakeString.h>

using namespace WebKit;

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

static Ref<JSON::Object> stateWith(const String& entries, const String& version = "4"_s)
{
    auto text = makeString("{\"BackgroundContentEventListenersVersion\":"_s, version,
        ",\"BackgroundContentEventListeners\":"_s, entries, '}');
    return JSON::Value::parseJSON(text)->asObject().releaseNonNull();
}

int main()
{
    BApplication application("application/x-vnd.Kunanyi-Summit-extension-background-listener-tests");
    WTF::initializeMainThread();
    using Event = WebExtensionEventListenerType;
    Ref state = JSON::Object::create();
    check(!readBackgroundListenersHaiku(state), "absent state requires discovery");
    state->setString("Unrelated"_s, "preserve me"_s);
    BackgroundListenerCountsHaiku listeners;
    check(saveBackgroundListenersHaiku(state, listeners), "first save records an empty listener set");
    auto restored = readBackgroundListenersHaiku(state);
    check(restored && restored->isEmpty(), "present empty state decodes successfully");
    check(!saveBackgroundListenersHaiku(state, listeners), "unchanged empty state avoids a write");

    listeners.add(Event::RuntimeOnMessage, 3);
    listeners.add(Event::RuntimeOnConnect, 2);
    listeners.add(Event::TabsOnUpdated, 1);
    check(saveBackgroundListenersHaiku(state, listeners), "new registrations change stored state");
    RefPtr object = state->getObject(backgroundContentEventListenersKeyHaiku);
    check(object->getDouble("RuntimeOnMessage"_s) == 3 && object->getDouble("RuntimeOnConnect"_s) == 2,
        "saved event names and multiplicities are explicit");
    check(object->size() == 3 && !object->getValue("onMessage"_s), "persistence uses unambiguous event identities");
    check(!saveBackgroundListenersHaiku(state, listeners), "unchanged registrations avoid a write");
    RefPtr reparsed = JSON::Value::parseJSON(state->toJSONString())->asObject();
    restored = readBackgroundListenersHaiku(*reparsed);
    check(restored && restored->count(Event::RuntimeOnMessage) == 3 && restored->count(Event::RuntimeOnConnect) == 2
        && restored->count(Event::TabsOnUpdated) == 1, "JSON round trip retains exact registration counts");
    restored->remove(Event::RuntimeOnMessage);
    check(restored->count(Event::RuntimeOnMessage) == 2, "one removal preserves other restored registrations");
    check(saveBackgroundListenersHaiku(state, *restored), "changed count updates stored state");
    check(readBackgroundListenersHaiku(state)->count(Event::RuntimeOnMessage) == 2, "updated count survives another read");
    check(state->getString("Unrelated"_s) == "preserve me"_s, "saving preserves unrelated context state");

    auto independent = stateWith("{\"TabsOnUpdated\":7,\"RuntimeOnMessage\":2,\"RuntimeOnConnect\":5}"_s);
    restored = readBackgroundListenersHaiku(independent);
    check(restored && restored->count(Event::TabsOnUpdated) == 7 && restored->count(Event::RuntimeOnMessage) == 2
        && restored->count(Event::RuntimeOnConnect) == 5, "independent reordered input resolves event names correctly");

    for (auto version : { "3"_s, "5"_s, "4.5"_s, "\"4\""_s, "true"_s, "null"_s })
        check(!readBackgroundListenersHaiku(stateWith("{}"_s, version)), "invalid or stale version requires discovery");
    for (auto entries : { "[]"_s, "null"_s, "42"_s, "\"text\""_s,
            "{\"Unknown\":1}"_s, "{\"NewUnknownEvent\":1}"_s, "{\"onMessage\":1}"_s,
            "{\"29\":1}"_s, "{\"RuntimeOnMessage\":0}"_s, "{\"RuntimeOnMessage\":-1}"_s,
            "{\"RuntimeOnMessage\":1.5}"_s, "{\"RuntimeOnMessage\":4294967296}"_s,
            "{\"RuntimeOnMessage\":\"2\"}"_s, "{\"RuntimeOnMessage\":true}"_s,
            "{\"RuntimeOnMessage\":null}"_s, "{\"RuntimeOnMessage\":{}}"_s,
            "{\"RuntimeOnMessage\":2,\"NewUnknownEvent\":1}"_s })
        check(!readBackgroundListenersHaiku(stateWith(entries)), "malformed listener state is rejected as a whole");

    for (double count : { std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() }) {
        RefPtr saved = independent->getObject(backgroundContentEventListenersKeyHaiku);
        saved->setDouble("RuntimeOnMessage"_s, count);
        check(!readBackgroundListenersHaiku(independent), "nonfinite counts require discovery");
    }

    listeners.clear();
    listeners.add(Event::RuntimeOnMessage, std::numeric_limits<unsigned>::max());
    saveBackgroundListenersHaiku(state, listeners);
    restored = readBackgroundListenersHaiku(state);
    check(restored && restored->count(Event::RuntimeOnMessage) == std::numeric_limits<unsigned>::max(),
        "full unsigned counts round trip without signed narrowing");

    clearBackgroundListenersHaiku(state);
    check(!readBackgroundListenersHaiku(state), "cleared cache requires listener discovery");
    check(!state->getValue(backgroundContentEventListenersKeyHaiku) && !state->getValue(backgroundContentEventListenersVersionKeyHaiku),
        "clearing removes both cache and format version");
    check(state->getString("Unrelated"_s) == "preserve me"_s, "clearing preserves unrelated context state");
    clearBackgroundListenersHaiku(state);
    check(state->size() == 1, "clearing is idempotent");

    std::printf("RuntimeOnMessage numeric identifier: %u\n", static_cast<unsigned>(Event::RuntimeOnMessage));
    std::printf("%u native background listener checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
