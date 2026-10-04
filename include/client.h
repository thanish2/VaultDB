#pragma once
#include "protocol.h"
#include "hashring.h"
#include <string>
#include <cstdint>
#include <optional>
#include <map>
#include <vector>

// ============================================================================
// FILE: client.h
// PART OF: vaultdb
//
// NEW IN THIS VERSION: QUORUM REPLICATION.
// N = number of replicas per key. W = write quorum (replicas that
// must confirm a write). R = read quorum (replicas that must respond
// to a read). We use N=3, W=2, R=2, satisfying W+R > N (2+2 > 3),
// which guarantees any read overlaps with the most recent write.
//
// put() now sends the SAME write (with one shared timestamp) to ALL
// N replicas, and succeeds if at least W of them confirm.
// get() now reads from multiple replicas and returns whichever
// response has the HIGHEST timestamp (last-write-wins), requiring at
// least R replicas to have responded successfully.
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

    HashRing ring_;
    std::map<std::string, int> connections_;

    int getOrCreateConnection(const std::string& serverAddress);

    // CHANGED: now takes the target server EXPLICITLY, rather than
    // figuring it out internally via the ring. put()/get() decide
    // WHICH servers to talk to (via ring_.getServersForKey), and this
    // function just handles talking to ONE specific one of them.
    Response sendRequestToServer(const std::string& serverAddress, const Request& req);

    // Generates a timestamp representing "right now", used to tag
    // every PUT so replicas (and later reads) can agree on ordering.
    uint64_t currentTimestampMillis() const;
};