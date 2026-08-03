// Collab/LoopbackTransport.h - a whole session in one process, no sockets.
//
// This is what makes the collaboration layer testable at all. A hub owns N client
// endpoints; bytes handed to one side appear on the other when the owner pumps. No
// thread, no port, no timing, no teardown race - so --test-collab can drive two
// clients racing for the same lock DETERMINISTICALLY, which over a real socket is a
// flaky test that only fails on someone else's machine.
//
// It deliberately does NOT preserve message boundaries: Send appends to a byte buffer
// and Receive hands back whatever has accumulated. That keeps the framing code on the
// same path the socket transport will use, instead of letting the loopback paper over
// a split-frame bug until the day sockets land.
#pragma once

#include "Collab/Transport.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace hbe::collab {

class LoopbackHub;

// The client half. Handed out by LoopbackHub::CreateClient.
class LoopbackClientTransport final : public IClientTransport {
public:
    LoopbackClientTransport(LoopbackHub* hub, ConnId id) : hub_(hub), id_(id) {}

    bool Connected() const override;
    void Receive(std::vector<u8>& out) override;
    void Send(const u8* data, usize n) override;
    void Disconnect() override;

    ConnId Id() const { return id_; }

private:
    LoopbackHub* hub_ = nullptr;
    ConnId id_ = 0;
};

class LoopbackHub final : public IServerTransport {
public:
    // Creates a connection and returns the client end. The server learns about it on
    // its next PollNewConnections - never synchronously, because a real transport
    // cannot do that either and code that assumed it would break on sockets.
    LoopbackClientTransport* CreateClient();

    // IServerTransport
    void PollNewConnections(std::vector<ConnId>& out) override;
    void PollDisconnects(std::vector<ConnId>& out) override;
    void Receive(ConnId c, std::vector<u8>& out) override;
    void Send(ConnId c, const u8* data, usize n) override;
    void Disconnect(ConnId c) override;

    // Called by the client half.
    void ClientSend(ConnId c, const u8* data, usize n);
    void ClientReceive(ConnId c, std::vector<u8>& out);
    void ClientDisconnect(ConnId c);
    bool IsConnected(ConnId c) const;

private:
    struct Pipe {
        std::vector<u8> toServer;
        std::vector<u8> toClient;
        bool open = true;
    };
    std::unordered_map<ConnId, Pipe> pipes_;
    std::vector<std::unique_ptr<LoopbackClientTransport>> owned_;
    std::vector<ConnId> pendingNew_, pendingGone_;
    ConnId nextId_ = 1;
};

} // namespace hbe::collab
