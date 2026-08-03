// Collab/LoopbackTransport.cpp
#include "Collab/LoopbackTransport.h"

#include <memory>

namespace hbe::collab {

LoopbackClientTransport* LoopbackHub::CreateClient() {
    const ConnId id = nextId_++;
    pipes_[id] = Pipe{};
    pendingNew_.push_back(id);
    owned_.push_back(std::make_unique<LoopbackClientTransport>(this, id));
    return owned_.back().get();
}

void LoopbackHub::PollNewConnections(std::vector<ConnId>& out) {
    out = pendingNew_;
    pendingNew_.clear();
}

void LoopbackHub::PollDisconnects(std::vector<ConnId>& out) {
    out = pendingGone_;
    pendingGone_.clear();
}

void LoopbackHub::Receive(ConnId c, std::vector<u8>& out) {
    const auto it = pipes_.find(c);
    if (it == pipes_.end()) return;
    // APPEND, never assign: the caller keeps a partial frame from last tick at the
    // front of this buffer, and overwriting it would corrupt the stream exactly when
    // a message straddles two pumps - the failure that only shows up under load.
    out.insert(out.end(), it->second.toServer.begin(), it->second.toServer.end());
    it->second.toServer.clear();
}

void LoopbackHub::Send(ConnId c, const u8* data, usize n) {
    const auto it = pipes_.find(c);
    if (it == pipes_.end() || !it->second.open || n == 0) return;
    it->second.toClient.insert(it->second.toClient.end(), data, data + n);
}

void LoopbackHub::Disconnect(ConnId c) {
    const auto it = pipes_.find(c);
    if (it == pipes_.end() || !it->second.open) return;
    it->second.open = false;
    pendingGone_.push_back(c);
}

void LoopbackHub::ClientSend(ConnId c, const u8* data, usize n) {
    const auto it = pipes_.find(c);
    if (it == pipes_.end() || !it->second.open || n == 0) return;
    it->second.toServer.insert(it->second.toServer.end(), data, data + n);
}

void LoopbackHub::ClientReceive(ConnId c, std::vector<u8>& out) {
    const auto it = pipes_.find(c);
    if (it == pipes_.end()) return;
    out.insert(out.end(), it->second.toClient.begin(), it->second.toClient.end());
    it->second.toClient.clear();
}

void LoopbackHub::ClientDisconnect(ConnId c) { Disconnect(c); }

bool LoopbackHub::IsConnected(ConnId c) const {
    const auto it = pipes_.find(c);
    return it != pipes_.end() && it->second.open;
}

bool LoopbackClientTransport::Connected() const { return hub_ && hub_->IsConnected(id_); }
void LoopbackClientTransport::Receive(std::vector<u8>& out) {
    if (hub_) hub_->ClientReceive(id_, out);
}
void LoopbackClientTransport::Send(const u8* data, usize n) {
    if (hub_) hub_->ClientSend(id_, data, n);
}
void LoopbackClientTransport::Disconnect() {
    if (hub_) hub_->ClientDisconnect(id_);
}

} // namespace hbe::collab
