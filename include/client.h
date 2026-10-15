#pragma once
#include "protocol.h"
#include "hashring.h"
#include <string>
#include <cstdint>
#include <optional>
#include <map>
#include <vector>
#include <chrono>

// ============================================================================
// FILE: client.h
// PART OF: vaultdb
//
// QUORUM REPLICATION (unchanged): N=3 replicas per key, W=2 to confirm
// a write, R=2 to answer a read.
//
// NEW IN THIS VERSION: GOSSIP-AWARE ROUTING.
// The client no longer routes using a ring frozen at startup. Every
// few seconds it asks ONE server for that server's membership view
// (a MEMBERS request), then rebuilds its own HashRing from every node
// that isn't marked DEAD. So when gossip decides a node is dead, the
// client stops routing keys to it, and when the node comes back, the
// client starts using it again, with no recompiling and no manual
// edits.
//
// WHY WE ONLY DROP "DEAD" NODES, NOT "SUSPECTED" ONES:
// SUSPECTED can flicker on for a healthy node just from random gossip
// timing (we saw this in testing). Dropping suspected nodes would make
// healthy nodes briefly vanish from the ring and reroute keys for no
// reason. DEAD takes 12 seconds of silence, which chance alone almost
// never produces.
//
// THE ORIGINAL SERVER LIST IS STILL USED, but only as a BOOTSTRAP: it
// is the list of addresses the client is allowed to ask for a
// membership view. It no longer decides routing by itself.
// ============================================================================

struct ServerInfo {
    std::string host;
    uint16_t port;
};

class Client {
public:
    explicit Client(const std::vector<ServerInfo>& serverList);
    ~Client();

    Response put(const std::string& key, const std::string& value);
    Response get(const std::string& key);
    Response remove(const std::string& key);

private:
    static constexpr int N = 3; // replicas per key
    static constexpr int W = 2; // write quorum
    static constexpr int R = 2; // read quorum

    // NEW: how many seconds a view of the cluster is trusted before
    // the client asks for a fresh one.
    static constexpr int RING_REFRESH_SECONDS = 3;

    HashRing ring_;

    // NEW: the original "host:port" addresses from the constructor.
    // Kept so the client always has someone it is allowed to ask for
    // a membership view, even after ring_ has been rebuilt.
    std::vector<std::string> seedAddresses_;

    // NEW: when ring_ was last refreshed (our own steady clock).
    std::chrono::steady_clock::time_point lastRingRefresh_;

    std::map<std::string, int> connections_;

    int getOrCreateConnection(const std::string& serverAddress);

    // NEW: closes and forgets a cached connection, so the next request
    // to that server opens a brand new one. Needed because a server
    // that dies and later restarts leaves our old socket broken, and
    // without this the client would keep reusing it forever.
    void dropConnection(const std::string& serverAddress);

    Response sendRequestToServer(const std::string& serverAddress, const Request& req);
    uint64_t currentTimestampMillis() const;

    // NEW: called at the start of put/get/remove. Does nothing unless
    // RING_REFRESH_SECONDS have passed since the last refresh.
    void refreshRingIfStale();

    // NEW: actually fetches a membership view and rebuilds ring_.
    void refreshRing();
};