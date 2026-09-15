#include "config.h"
#include "ArgumentCoders.h"
#include "Decoder.h"
#include "Encoder.h"
#include "GeneratedSerializers.h"
#include <WebCore/CookieChange.h>
#include <WebCore/CookieJarDB.h>
#include <pal/SessionID.h>
#include <wtf/SharedTask.h>
#include <wtf/WallTime.h>

using namespace WebCore;

// Cookie::operator== compares identity and secure only on Curl. A wire round
// trip must verify every serialized field, including value and timestamps.
static bool equalCookies(const Cookie& a, const Cookie& b)
{
    return a.isKeyEqual(b) && a.value == b.value && a.partitionKey == b.partitionKey
        && a.created == b.created && a.expires == b.expires && a.httpOnly == b.httpOnly
        && a.secure == b.secure && a.session == b.session && a.sameSite == b.sameSite
        && a.comment == b.comment && a.commentURL == b.commentURL && a.ports == b.ports;
}

static std::unique_ptr<IPC::Decoder> decodeCookieMessage(IPC::Encoder& encoder)
{
    return IPC::Decoder::create(encoder.span(), encoder.releaseAttachments());
}

void runCookieChangeIPCChecks(void (*check)(bool, const char*))
{
    auto session = PAL::SessionID::legacyPrivateSessionID();
    Cookie cookie;
    cookie.name = "wire-cookie"_s;
    cookie.value = String::fromUTF8("雪 \"value\"");
    cookie.domain = ".example.test"_s;
    cookie.path = "/area"_s;
    cookie.created = WallTime::now().secondsSinceEpoch().milliseconds();
    cookie.expires = cookie.created + 3600000;
    cookie.sameSite = Cookie::SameSitePolicy::Strict;
    cookie.httpOnly = true;
    cookie.secure = true;
    Vector<CookieChange> pair { { cookie, true, CookieChangeCause::Overwrite }, { cookie, false, CookieChangeCause::Explicit } };
    pair[1].cookie.value = "replacement"_s;
    IPC::Encoder encoder(IPC::MessageName::NetworkProcessProxy_CookiesDidChangeWithDetails, 0);
    encoder << session << pair;
    auto decoder = decodeCookieMessage(encoder);
    check(decoder && decoder->messageName() == IPC::MessageName::NetworkProcessProxy_CookiesDidChangeWithDetails,
        "the actual IPC decoder accepts the generated network cookie-change message identifier");
    auto decodedSession = decoder ? decoder->decode<PAL::SessionID>() : std::nullopt;
    auto decoded = decoder ? decoder->decode<Vector<CookieChange>>() : std::nullopt;
    check(decodedSession && *decodedSession == session && decoded && decoded->size() == 2,
        "generated cookie-change coders preserve the private session and whole ordered batch");
    if (decoded && decoded->size() == 2) {
        check((*decoded)[0].removed && (*decoded)[0].cause == CookieChangeCause::Overwrite
            && !(*decoded)[1].removed && (*decoded)[1].cause == CookieChangeCause::Explicit,
            "IPC preserves overwrite removal before explicit addition");
        check(equalCookies((*decoded)[0].cookie, cookie) && (*decoded)[1].cookie.value == "replacement"_s,
            "IPC preserves the previous cookie's Unicode value, flags, timestamps and SameSite");
        check((*decoded)[0].cookie.domain == ".example.test"_s && (*decoded)[0].cookie.path == "/area"_s,
            "IPC preserves domain-cookie and path identity");
    }
    for (auto cause : { CookieChangeCause::Explicit, CookieChangeCause::Overwrite, CookieChangeCause::Expired, CookieChangeCause::ExpiredOverwrite }) {
        IPC::Encoder event(IPC::MessageName::WebExtensionContextProxy_DispatchCookiesChangedEvent, 1);
        event << CookieChange { cookie, true, cause };
        auto reader = decodeCookieMessage(event);
        auto value = reader ? reader->decode<CookieChange>() : std::nullopt;
        check(value && value->removed && value->cause == cause && equalCookies(value->cookie, cookie),
            "every supported cookie-change cause survives the production generated serializer");
    }
    // Truncate at every possible byte boundary, including inside the second
    // cookie. The generated vector decoder must reject the entire batch.
    bool allTruncationsRejected = true;
    for (size_t length = 1; length < encoder.span().size(); ++length) {
        auto reader = IPC::Decoder::create(encoder.span().first(length), { });
        if (!reader)
            continue;
        auto restoredSession = reader->decode<PAL::SessionID>();
        auto changes = restoredSession ? reader->decode<Vector<CookieChange>>() : std::nullopt;
        if (changes) {
            allTruncationsRejected = false;
            break;
        }
    }
    check(allTruncationsRejected, "every truncated cookie packet is rejected without a partial batch");
    Vector<uint8_t> corrupt(encoder.span());
    corrupt.last() = 255;
    auto corruptReader = IPC::Decoder::create(corrupt.span(), { });
    auto corruptSession = corruptReader ? corruptReader->decode<PAL::SessionID>() : std::nullopt;
    auto corruptChanges = corruptSession ? corruptReader->decode<Vector<CookieChange>>() : std::nullopt;
    check(!corruptChanges, "the generated enum validator rejects an invalid cookie cause at the end of a batch");
    IPC::Encoder invalidSession(IPC::MessageName::NetworkProcessProxy_CookiesDidChangeWithDetails, 0);
    invalidSession << PAL::SessionID { uint64_t { 0 } };
    auto invalidReader = decodeCookieMessage(invalidSession);
    check(invalidReader && !invalidReader->decode<PAL::SessionID>(), "the generated session decoder rejects an invalid cookie-store identifier");

    CookieJarDB database(":memory:"_s);
    database.open();
    Vector<Vector<CookieChange>> received;
    bool allMessagesDecoded = true;
    database.setCookieChangeHandler(createSharedTask<void(Vector<CookieChange>&&)>([&](Vector<CookieChange>&& changes) {
        IPC::Encoder message(IPC::MessageName::NetworkProcessProxy_CookiesDidChangeWithDetails, 0);
        message << session << changes;
        auto reader = decodeCookieMessage(message);
        auto store = reader ? reader->decode<PAL::SessionID>() : std::nullopt;
        auto restored = store ? reader->decode<Vector<CookieChange>>() : std::nullopt;
        allMessagesDecoded &= store && *store == session && restored.has_value();
        if (restored)
            received.append(WTF::move(*restored));
    }));
    check(database.setCookie(cookie), "a real SQLite insertion reaches the IPC encoding observer");
    auto replacement = cookie;
    replacement.value = "committed replacement"_s;
    check(database.setCookie(replacement), "a real SQLite overwrite reaches the IPC encoding observer");
    auto expired = cookie;
    expired.expires = 0;
    check(database.setCookie(expired), "an actual expired replacement reaches the IPC encoding observer");
    check(allMessagesDecoded && received.size() == 3 && received[0].size() == 1 && received[1].size() == 2 && received[2].size() == 1,
        "database commits survive complete production encoding and decoding as three ordered batches");
    if (received.size() == 3 && received[0].size() == 1 && received[1].size() == 2 && received[2].size() == 1) {
        check(received[0][0].cookie.value == cookie.value && received[1][0].cookie.value == cookie.value
            && received[1][1].cookie.value == replacement.value && received[2][0].cookie.value == replacement.value,
            "the decoded events retain actual pre-mutation and post-mutation values");
        check(received[2][0].removed && received[2][0].cause == CookieChangeCause::ExpiredOverwrite && database.getAllCookies().isEmpty(),
            "an expired-overwrite wire event corresponds to the row actually deleted from SQLite");
    }
    database.setCookieChangeHandler(nullptr);
}
