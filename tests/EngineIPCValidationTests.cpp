/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "Connection.h"
#include "Decoder.h"
#include "Encoder.h"
#include "MessageFlags.h"
#include "WebKitView.h"
#include <Application.h>
#include <OS.h>
#include <cstdio>
#include <functional>
#include <wtf/RunLoop.h>
#include <wtf/TZoneMallocInlines.h>

using namespace WTF;

namespace {
unsigned passed;
unsigned failed;
void check(bool result, const char* label)
{
    ++(result ? passed : failed);
    printf("%s %s\n", result ? "PASS" : "FAIL", label);
    fflush(stdout);
}

class ValidationClient final : public IPC::Connection::Client, public RefCounted<ValidationClient> {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(ValidationClient);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(ValidationClient);
public:
    void ref() const final { RefCounted<ValidationClient>::ref(); }
    void deref() const final { RefCounted<ValidationClient>::deref(); }
    std::function<void(IPC::Connection&)> rejected;
    unsigned invalid { 0 };
    unsigned received { 0 };
    unsigned closed { 0 };
    uint64_t payloadSum { 0 };

    void didReceiveMessage(IPC::Connection&, IPC::Decoder& decoder) final
    {
        check(RunLoop::isMain(), "valid message reaches the application main loop");
        auto payload = decoder.decode<uint64_t>();
        check(payload.has_value(), "valid message payload crosses the native socket");
        if (payload)
            payloadSum += *payload;
        ++received;
    }

    void didReceiveInvalidMessage(IPC::Connection& connection, IPC::MessageName name, const Vector<uint32_t>&) final
    {
        check(RunLoop::isMain(), "invalid dispatch flag is reported on the application main loop");
        check(name == IPC::MessageName::WebCookieManager_DeleteAllCookies, "rejection identifies the actual malformed message");
        ++invalid;
        if (rejected)
            rejected(connection);
    }

    void didClose(IPC::Connection&) final
    {
        check(RunLoop::isMain(), "connection close reaches the application main loop");
        ++closed;
    }
};

class ValidationTests final : public BApplication {
public:
    ValidationTests()
        : BApplication("application/x-vnd.Kunanyi-Summit-IPCValidationTests") { }

    void ReadyToRun() final
    {
        check(BWebKitInitialize() == B_OK, "native WebKit initializes on the application thread");
        SetPulseRate(100000);
        m_deadline = system_time() + 10000000;
        if (!openPair())
            return finish();
        m_receiver->rejected = [this](auto&) {
            if (m_receiver->invalid == 2) {
                send(IPC::MessageName::WebCookieManager_StartObservingCookieChanges, 40,
                    IPC::ShouldDispatchWhenWaitingForSyncReply::No, IPC::SendOption::DispatchMessageEvenWhenWaitingForSyncReply);
                send(IPC::MessageName::WebCookieManager_DeleteAllCookies, 2);
            }
        };
        // Set the wire flag directly, leaving sender options ordinary. This
        // deliberately models a malformed peer instead of a valid API sender.
        // DeleteAllCookies remains ordinary. Both endpoints use this fixture's
        // client and payload; no actual cookie manager handles these packets.
        send(IPC::MessageName::WebCookieManager_DeleteAllCookies, 1, IPC::ShouldDispatchWhenWaitingForSyncReply::Yes);
        send(IPC::MessageName::WebCookieManager_DeleteAllCookies, 1, IPC::ShouldDispatchWhenWaitingForSyncReply::YesDuringUnboundedIPC);
    }

    void Pulse() final
    {
        if (m_finished)
            return;
        if (system_time() > m_deadline) {
            check(false, "malformed IPC rejection and subsequent traffic finish before the deadline");
            return finish();
        }
        if (!m_fatal && m_receiver->invalid == 2 && m_receiver->received == 2) {
            check(m_receiver->payloadSum == 42, "both malformed messages are rejected before delivery and valid traffic continues");
            closePair();
            m_fatal = true;
            if (!openPair())
                return finish();
            m_receiver->rejected = [](auto& connection) { connection.invalidate(); };
            send(IPC::MessageName::WebCookieManager_DeleteAllCookies, 1, IPC::ShouldDispatchWhenWaitingForSyncReply::Yes);
        } else if (m_fatal && m_receiver->invalid == 1 && m_sender->closed == 1) {
            check(!m_receiver->received, "invalid-message callback can close its connection without delivering malformed data");
            finish();
        }
    }

private:
    bool openPair()
    {
        auto identifiers = IPC::Connection::createConnectionIdentifierPair();
        check(identifiers.has_value(), "native IPC socket pair is created");
        if (!identifiers)
            return false;
        m_receiver = adoptRef(*new ValidationClient);
        m_sender = adoptRef(*new ValidationClient);
        m_serverConnection = IPC::Connection::createServerConnection(WTF::move(identifiers->server));
        m_clientConnection = IPC::Connection::createClientConnection(IPC::Connection::Identifier { WTF::move(identifiers->client) });
        bool opened = m_serverConnection->open(*m_receiver) && m_clientConnection->open(*m_sender);
        check(opened, "both actual WebKit connection endpoints open");
        return opened;
    }

    void send(IPC::MessageName name, uint64_t payload,
        IPC::ShouldDispatchWhenWaitingForSyncReply wireFlag = IPC::ShouldDispatchWhenWaitingForSyncReply::No,
        OptionSet<IPC::SendOption> options = { })
    {
        auto encoder = makeUniqueRef<IPC::Encoder>(name, 0);
        encoder->setShouldDispatchMessageWhenWaitingForSyncReply(wireFlag);
        encoder.get() << payload;
        check(m_clientConnection->sendMessage(WTF::move(encoder), options) == IPC::Error::NoError, "fixture packet is queued on the real connection");
    }

    void closePair()
    {
        if (m_serverConnection)
            m_serverConnection->invalidate();
        if (m_clientConnection)
            m_clientConnection->invalidate();
        m_serverConnection = nullptr;
        m_clientConnection = nullptr;
        m_receiver = nullptr;
        m_sender = nullptr;
    }

    void finish()
    {
        m_finished = true;
        closePair();
        PostMessage(B_QUIT_REQUESTED);
    }

    RefPtr<ValidationClient> m_receiver;
    RefPtr<ValidationClient> m_sender;
    RefPtr<IPC::Connection> m_serverConnection;
    RefPtr<IPC::Connection> m_clientConnection;
    bigtime_t m_deadline { 0 };
    bool m_fatal { false };
    bool m_finished { false };
};
}

int main()
{
    ValidationTests application;
    application.Run();
    printf("%u checks passed, %u failed\n", passed, failed);
    return failed ? 1 : 0;
}
