/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionWebRequestMergeHaiku.h"
#include <Application.h>
#include <cstdio>
#include <wtf/MainThread.h>

using namespace WebKit;
using Phase = WebExtensionWebRequestPhaseHaiku;
using Action = WebExtensionWebRequestIgnoredActionHaiku;
using Operation = WebExtensionWebRequestHeaderOperationHaiku;
using Header = WebExtensionWebRequestHeaderHaiku;
using Reply = WebExtensionWebRequestReplyHaiku;
using Constraint = WebExtensionWebRequestHeaderConstraintHaiku;
static unsigned checks, failures;
static void check(bool passed, const char* label)
{
    ++checks;
    if (!passed) {
        ++failures;
        printf("FAIL %s\n", label);
    }
}
static Header header(const String& name, const String& text)
{
    auto value = text.utf8();
    Vector<uint8_t> bytes;
    bytes.append(value.span());
    return { name, WTF::move(bytes) };
}
static Reply reply(const String& identity, uint64_t priority, const String& json, uint64_t listener = 1)
{
    auto decision = parseWebExtensionWebRequestDecisionHaiku(json);
    RELEASE_ASSERT(decision);
    return { identity, priority, listener, { }, WTF::move(*decision) };
}
static bool has(const Vector<Header>& headers, const String& name, const String& value)
{
    auto wanted = header(name, value);
    for (auto& item : headers) {
        if (equalIgnoringASCIICase(item.name, name) && item.value == wanted.value)
            return true;
    }
    return false;
}
static auto merge(Phase phase, Vector<Reply> replies, Vector<Header> headers = { }, Vector<Constraint> constraints = { }, const String& url = "https://site.test/source"_s)
{
    return mergeWebExtensionWebRequestRepliesHaiku(phase, URL { url }, headers, constraints, replies);
}
int main()
{
    BApplication application("application/x-vnd.Kunanyi-Summit-WebRequestMergeTests");
    WTF::initializeMainThread();
    auto older = reply("older"_s, 10, R"({"redirectUrl":"https://old.test/"})"_s);
    auto newer = reply("newer"_s, 20, R"({"redirectUrl":"https://new.test/"})"_s);
    auto result = merge(Phase::BeforeRequest, { older, newer });
    check(result && result->redirectURL && result->redirectURL->string() == "https://new.test/"_s
        && result->redirectingInstallation == "newer"_s, "installation priority is independent of reply arrival order");
    check(result->ignored.size() == 1 && result->ignored[0].installationIdentifier == "older"_s
        && result->ignored[0].action == Action::Redirect, "redirect conflict identifies the ignored installation");
    auto reversed = merge(Phase::BeforeRequest, { newer, older });
    check(reversed && reversed->redirectURL == result->redirectURL, "reversing IPC arrival order preserves the redirect winner");
    older.installationOrder = 9007199254740993ULL;
    newer.installationOrder = 9007199254740992ULL;
    result = merge(Phase::BeforeRequest, { newer, older });
    check(result && result->redirectingInstallation == "older"_s, "installation priorities retain exact uint64 values beyond JavaScript precision");
    auto maximum = older; maximum.installationOrder = UINT64_MAX;
    result = merge(Phase::BeforeRequest, { maximum, newer });
    check(result && result->redirectingInstallation == "older"_s, "the full positive uint64 priority range has no hash-table sentinel collisions");
    result = merge(Phase::BeforeRequest, { older, reply("cancel"_s, 1, R"({"cancel":true})"_s) });
    check(result && result->cancel && !result->redirectURL && !result->headers && !result->authCredentials
        && result->cancelingInstallation == "cancel"_s, "a lower priority cancellation suppresses all network mutations");
    for (auto phase : { Phase::BeforeRequest, Phase::BeforeSendHeaders, Phase::HeadersReceived, Phase::AuthRequired }) {
        result = merge(phase, { reply("cancel"_s, 1, R"({"cancel":true})"_s) });
        check(result && result->cancel, "cancellation applies at every blocking event phase");
    }
    result = merge(Phase::BeforeRequest, { newer, reply("blank"_s, 1, R"({"redirectUrl":"about:blank"})"_s) });
    check(result && result->redirectURL->string() == "about:blank"_s, "a cancellation-style redirect precedes ordinary redirects");
    result = merge(Phase::HeadersReceived, { newer, reply("data"_s, 1, R"({"redirectUrl":"data:text/plain,empty"})"_s) });
    check(result && result->redirectURL->protocolIs("data"_s), "data redirects retain cancellation-style precedence at response time");
    result = merge(Phase::BeforeRequest, { newer }, { }, { }, "wss://socket.test/"_s);
    check(result && !result->redirectURL, "a WebSocket handshake cannot be redirected");
    result = merge(Phase::BeforeRequest, { newer, reply("same"_s, 1, R"({"redirectUrl":"https://new.test/"})"_s) });
    check(result && result->ignored.isEmpty(), "identical redirects do not create conflicts");
    auto first = reply("same-extension"_s, 5, R"({"redirectUrl":"https://first.test/"})"_s, 1);
    auto second = reply("same-extension"_s, 5, R"({"redirectUrl":"https://second.test/"})"_s, 2);
    result = merge(Phase::BeforeRequest, { second, first });
    check(result && result->redirectURL->string() == "https://first.test/"_s, "retained listener order resolves same-extension replies deterministically");

    Vector<Header> baseline { header("X-A"_s, "original-a"_s), header("X-B"_s, "original-b"_s), header("Keep"_s, "unchanged"_s) };
    auto editA = reply("a"_s, 20, R"({"requestHeaders":[{"name":"X-A","value":"new-a"},{"name":"X-B","value":"original-b"},{"name":"Keep","value":"unchanged"}]})"_s);
    auto editB = reply("b"_s, 10, R"({"requestHeaders":[{"name":"X-A","value":"original-a"},{"name":"X-B","value":"new-b"},{"name":"Keep","value":"unchanged"}]})"_s);
    result = merge(Phase::BeforeSendHeaders, { editB, editA }, baseline);
    check(result && result->headers && has(*result->headers, "X-A"_s, "new-a"_s) && has(*result->headers, "X-B"_s, "new-b"_s)
        && has(*result->headers, "Keep"_s, "unchanged"_s) && result->ignored.isEmpty(), "independent request edits merge against the immutable original headers");
    auto conflict = reply("conflict"_s, 1, R"({"requestHeaders":[{"name":"x-a","value":"loser"},{"name":"X-B","value":"leak"},{"name":"Keep","value":"unchanged"}]})"_s);
    result = merge(Phase::BeforeSendHeaders, { conflict, editA }, baseline);
    check(result && result->headers && has(*result->headers, "X-B"_s, "original-b"_s)
        && !has(*result->headers, "X-B"_s, "leak"_s) && result->ignored.size() == 1,
        "a request header conflict discards the entire conflicting delta without partial edits");
    auto identical = editA; identical.installationIdentifier = "same-edit"_s; identical.installationOrder = 1;
    result = merge(Phase::BeforeSendHeaders, { editA, identical }, baseline);
    check(result && result->ignored.isEmpty() && result->headers->size() == 3, "identical request header edits are compatible");
    auto remove = reply("remove"_s, 30, R"({"requestHeaders":[{"name":"X-B","value":"original-b"},{"name":"Keep","value":"unchanged"}]})"_s);
    result = merge(Phase::BeforeSendHeaders, { remove, editA }, baseline);
    check(result && result->headers && result->headers->size() == 2 && result->ignored.size() == 1,
        "a higher priority deletion prevents a lower priority request header set");
    remove.installationOrder = 1;
    result = merge(Phase::BeforeSendHeaders, { remove, editA }, baseline);
    check(result && has(*result->headers, "X-A"_s, "new-a"_s) && result->ignored.size() == 1,
        "a higher priority request header set prevents lower priority deletion");
    auto removeAgain = remove; removeAgain.installationIdentifier = "remove-again"_s; removeAgain.installationOrder = 2;
    result = merge(Phase::BeforeSendHeaders, { remove, removeAgain }, baseline);
    check(result && result->ignored.isEmpty() && result->headers->size() == 2, "identical request deletions remain compatible");
    result = merge(Phase::BeforeSendHeaders, { reply("empty"_s, 1, R"({"requestHeaders":[]})"_s) }, baseline);
    check(result && result->headers && result->headers->isEmpty(), "an explicitly empty replacement removes all visible request headers");
    result = merge(Phase::BeforeSendHeaders, { reply("absent"_s, 1, "{}"_s) }, baseline);
    check(result && !result->headers, "an absent replacement leaves request headers untouched");
    auto hidden = reply("hidden"_s, 1, R"({"requestHeaders":[{"name":"Cookie","value":"forged"}]})"_s);
    hidden.hiddenHeaderNames.add("cookie"_s);
    result = merge(Phase::BeforeSendHeaders, { hidden }, { header("Cookie"_s, "secret"_s), header("Visible"_s, "value"_s) });
    check(result && result->headers && result->headers->size() == 1 && has(*result->headers, "Cookie"_s, "secret"_s),
        "omitting or forging a hidden request header cannot delete or change it");
    result = merge(Phase::BeforeSendHeaders, { editA }, baseline, { { "x-a"_s, Operation::Set } });
    check(result && !result->headers && result->ignored.size() == 1, "imperative header edits cannot overwrite a prior DNR set");
    result = merge(Phase::BeforeSendHeaders, { editA }, baseline, { { "X-A"_s, Operation::Append } });
    check(result && !result->headers && result->ignored.size() == 1, "imperative request replacement cannot overwrite a prior DNR append");
    result = merge(Phase::BeforeSendHeaders, { reply("add"_s, 1, R"({"requestHeaders":[{"name":"Removed","value":"restore"}]})"_s) }, { }, { { "removed"_s, Operation::Remove } });
    check(result && !result->headers && result->ignored.size() == 1, "imperative header edits cannot restore a DNR-removed request header");

    Vector<Header> cookies { header("Set-Cookie"_s, "one=1"_s), header("set-cookie"_s, "two=2"_s), header("Keep"_s, "unchanged"_s) };
    auto cookieOne = reply("one"_s, 30, R"({"responseHeaders":[{"name":"Set-Cookie","value":"one=changed"},{"name":"Set-Cookie","value":"two=2"},{"name":"Keep","value":"unchanged"}]})"_s);
    auto cookieTwo = reply("two"_s, 10, R"({"responseHeaders":[{"name":"Set-Cookie","value":"one=1"},{"name":"Set-Cookie","value":"two=changed"},{"name":"Keep","value":"unchanged"}]})"_s);
    result = merge(Phase::HeadersReceived, { cookieTwo, cookieOne }, cookies);
    check(result && result->headers && has(*result->headers, "set-cookie"_s, "one=changed"_s)
        && has(*result->headers, "set-cookie"_s, "two=changed"_s) && result->headers->size() == 3 && result->ignored.isEmpty(),
        "independent response line changes preserve duplicate header names");
    auto cookieConflict = reply("conflict"_s, 1, R"({"responseHeaders":[{"name":"Set-Cookie","value":"one=loser"},{"name":"Set-Cookie","value":"two=2"},{"name":"Keep","value":"leak"}]})"_s);
    result = merge(Phase::HeadersReceived, { cookieConflict, cookieOne }, cookies);
    check(result && has(*result->headers, "Keep"_s, "unchanged"_s) && !has(*result->headers, "Keep"_s, "leak"_s)
        && result->ignored.size() == 1 && result->ignored[0].action == Action::ResponseHeaders,
        "competing response line replacements reject the losing delta atomically");
    auto addLine = reply("add"_s, 20, R"({"responseHeaders":[{"name":"X-New","value":"same"}]})"_s);
    auto addSame = addLine; addSame.installationIdentifier = "duplicate"_s; addSame.installationOrder = 1;
    result = merge(Phase::HeadersReceived, { addLine, addSame });
    check(result && result->headers && result->headers->size() == 1 && result->ignored.isEmpty(), "duplicate response additions do not multiply header lines");
    auto addOther = reply("other"_s, 1, R"({"responseHeaders":[{"name":"X-New","value":"different"}]})"_s);
    result = merge(Phase::HeadersReceived, { addLine, addOther });
    check(result && result->headers->size() == 2 && result->ignored.isEmpty(), "distinct response values sharing a header name can coexist");
    auto binary = reply("bytes"_s, 1, R"({"responseHeaders":[{"name":"X-Bytes","binaryValue":[128,255]}]})"_s);
    result = merge(Phase::HeadersReceived, { binary });
    Vector<uint8_t> expectedBytes { 128, 255 };
    check(result && result->headers->at(0).value == expectedBytes, "header merging preserves non-UTF8 response bytes");
    hidden = reply("hidden"_s, 1, R"({"responseHeaders":[{"name":"Set-Cookie","value":"forged"}]})"_s);
    hidden.hiddenHeaderNames.add("set-cookie"_s);
    result = merge(Phase::HeadersReceived, { hidden }, cookies);
    check(result && result->headers && result->headers->size() == 2 && has(*result->headers, "set-cookie"_s, "one=1"_s)
        && has(*result->headers, "set-cookie"_s, "two=2"_s), "all hidden response header lines survive omission and forgery");
    result = merge(Phase::HeadersReceived, { addLine }, { }, { { "X-New"_s, Operation::Remove } });
    check(result && !result->headers && result->ignored.size() == 1, "a DNR response removal prevents imperative addition");
    result = merge(Phase::HeadersReceived, { addLine }, { }, { { "X-New"_s, Operation::Set } });
    check(result && !result->headers && result->ignored.size() == 1, "a DNR response set prevents imperative addition");
    result = merge(Phase::HeadersReceived, { addLine }, { }, { { "X-New"_s, Operation::Append } });
    check(result && result->headers && result->headers->size() == 1, "response additions coexist with prior DNR append permission");
    result = merge(Phase::HeadersReceived, { cookieOne }, cookies, { { "Set-Cookie"_s, Operation::Append } });
    check(result && !result->headers && result->ignored.size() == 1, "a prior DNR append cannot be deleted by response replacement");
    auto unchanged = reply("unchanged"_s, 1, R"({"responseHeaders":[{"name":"set-cookie","value":"one=1"},{"name":"SET-COOKIE","value":"two=2"},{"name":"Keep","value":"unchanged"}]})"_s);
    result = merge(Phase::HeadersReceived, { unchanged }, cookies);
    check(result && !result->headers, "header-name case changes alone do not synthesize response mutations");
    result = merge(Phase::HeadersReceived, { reply("empty"_s, 1, R"({"responseHeaders":[]})"_s) }, cookies);
    check(result && result->headers && result->headers->isEmpty(), "an empty response replacement removes visible lines");

    auto auth = reply("auth"_s, 40, R"({"authCredentials":{"username":"","password":""}})"_s);
    auto authConflict = reply("other-auth"_s, 10, R"({"authCredentials":{"username":"user","password":"pass"}})"_s);
    result = merge(Phase::AuthRequired, { authConflict, auth });
    check(result && result->authCredentials && result->authCredentials->username.isEmpty() && result->authCredentials->password.isEmpty()
        && result->ignored.size() == 1 && result->ignored[0].action == Action::Credentials,
        "highest priority credentials win including explicitly empty credentials");
    auto authSame = auth; authSame.installationIdentifier = "same-auth"_s; authSame.installationOrder = 1;
    result = merge(Phase::AuthRequired, { authSame, auth });
    check(result && result->ignored.isEmpty(), "identical credential replies do not conflict");
    result = merge(Phase::BeforeRequest, { editA, auth });
    check(result && !result->headers && !result->authCredentials, "wrong-phase headers and credentials cannot mutate a before-request transaction");
    result = merge(Phase::BeforeSendHeaders, { newer, auth });
    check(result && !result->redirectURL && !result->authCredentials, "wrong-phase redirects and credentials cannot mutate request headers");
    result = merge(Phase::AuthRequired, { newer, editA, cookieOne });
    check(result && !result->redirectURL && !result->headers, "authentication transactions cannot apply redirects or header changes");
    result = merge(Phase::HeadersReceived, { newer, auth, editA });
    check(result && result->redirectURL && !result->authCredentials && !result->headers,
        "response redirects are supported while request headers and credentials are ignored");

    check(!merge(Phase::BeforeRequest, { first, first }), "duplicate listener replies are rejected");
    second.installationIdentifier = "other-extension"_s;
    check(!merge(Phase::BeforeRequest, { first, second }), "two installations cannot claim the same priority");
    second.installationIdentifier = first.installationIdentifier; second.installationOrder = first.installationOrder + 1;
    check(!merge(Phase::BeforeRequest, { first, second }), "one installation cannot claim multiple priorities");
    first.installationOrder = 0;
    check(!merge(Phase::BeforeRequest, { first }), "legacy unranked installation metadata cannot authorize blocking decisions");
    first.installationOrder = 5; first.listenerOrder = 0;
    check(!merge(Phase::BeforeRequest, { first }), "a reply needs a retained listener identity");
    first.listenerOrder = 1; first.installationIdentifier = emptyString();
    check(!merge(Phase::BeforeRequest, { first }), "a reply needs a retained installation identity");
    check(!merge(Phase::BeforeSendHeaders, { }, { }, { { "X"_s, Operation::Set }, { "x"_s, Operation::Remove } }),
        "contradictory rule-engine header constraints are rejected");
    Vector<Reply> oversized;
    for (uint64_t i = 1; i <= 257; ++i)
        oversized.append(reply("many"_s, 1, "{}"_s, i));
    check(!merge(Phase::BeforeRequest, WTF::move(oversized)), "pending response fan-out is bounded");
    Vector<Header> tooLarge { { "X"_s, Vector<uint8_t>(FillWith { }, 2 * 1024 * 1024, uint8_t { 'x' }) } };
    check(!merge(Phase::BeforeSendHeaders, { }, WTF::move(tooLarge)), "aggregate header bytes are bounded before merging");
    Vector<Header> tooMany;
    for (unsigned i = 0; i < 8193; ++i)
        tooMany.append(header("X"_s, "v"_s));
    check(!merge(Phase::HeadersReceived, { }, WTF::move(tooMany)), "aggregate header entry count is bounded before merging");
    result = merge(Phase::BeforeRequest, { });
    check(result && !result->cancel && !result->redirectURL && !result->headers && !result->authCredentials,
        "no eligible replies produces no network decision");
    check(!merge(static_cast<Phase>(255), { }), "unknown event phases are rejected");
    check(!merge(static_cast<Phase>(255), { reply("cancel"_s, 1, R"({"cancel":true})"_s) }),
        "a cancellation cannot bypass event phase validation");
    printf("WEB_REQUEST_MERGE_RESULT %s checks=%u failures=%u\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
