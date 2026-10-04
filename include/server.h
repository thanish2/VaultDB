#pragma once
#include "kvstore.h"
#include "membership.h"
#include <string>
#include <cstdint>
#include <vector>
#include <thread>
#include <atomic>

// ============================================================================
// FILE: server.h
// PART OF: vaultdb
//
// NEW IN THIS VERSION: gossip.
// Server now owns a MembershipList (its own local view of the
// cluster) and runs a BACKGROUND THREAD that periodically picks a
// random peer, sends it a GOSSIP request, and merges whatever comes
// back. It also now handles INCOMING GOSSIP requests from other
// servers in handleClient (added in server.cpp), merging those too
// and replying with its own current view.
// ============================================================================

class Server {
public:
    // port/dataDir: same as before.
    // myAddress: this server's OWN address, e.g. "127.0.0.1:5000" —
    //            needed so it can include itself in its membership
    //            list (so OTHER nodes learn about it via gossip).
    // seedPeers: a few other addresses to start gossiping with —
    //            the manual "bootstrap" starting point every gossip
    //            system needs, same idea as client_main.cpp's
    //            hardcoded server list, just here for server-to-server
    //            discovery instead of client-to-server routing.
    Server(uint16_t port, const std::string& dataDir,
           const std::string& myAddress,
           const std::vector<std::string>& seedPeers);
    ~Server();

    void run();

private:
    uint16_t port_;
    KVStore store_;
    int listenSocket_;

    std::string myAddress_;
    MembershipList membership_;

    // The background gossip thread, and a flag to cleanly stop it
    // when the Server is destroyed. std::atomic<bool> is used
    // (instead of a plain bool) because this flag is read by the
    // gossip thread and written by the destructor — two different
    // threads touching the same variable, so it needs to be safe
    // from the same kind of race condition we solved with mutex_ in
    // KVStore, just using a simpler tool suited to a single flag.
    std::thread gossipThread_;
    std::atomic<bool> running_;

    void setupListenSocket();
    void handleClient(int clientSocket);

    // NEW: the background loop itself — runs on gossipThread_, picks
    // a random peer periodically, gossips with it.
    void gossipLoop();

    // NEW: handles ONE incoming GOSSIP request (called from
    // handleClient when it sees Command::GOSSIP) — merges the
    // incoming list, returns our own current list as the response.
    std::string handleIncomingGossip(const std::string& incomingSerialized);
};