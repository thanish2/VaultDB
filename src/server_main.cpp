#include "server.h"
#include <iostream>
#include <vector>

// ============================================================================
// FILE: server_main.cpp
// PART OF: vaultdb
//
// UPDATED: now also takes THIS server's own address and a list of
// seed peers, since Server's constructor needs them for gossip.
//
// Usage: .\server <port> <dataDir> <myAddress> <seedPeer1> [seedPeer2] ...
// Example:
//   .\server 5000 ./data-node1 127.0.0.1:5000 127.0.0.1:5001 127.0.0.1:5002
// ============================================================================

int main(int argc, char** argv) {
    uint16_t port = (argc > 1) ? static_cast<uint16_t>(std::stoi(argv[1])) : 5000;
    std::string dataDir = (argc > 2) ? argv[2] : "./data";
    std::string myAddress = (argc > 3) ? argv[3] : ("127.0.0.1:" + std::to_string(port));

    // Every argument from index 4 onward is treated as a seed peer.
    std::vector<std::string> seedPeers;
    for (int i = 4; i < argc; i++) {
        seedPeers.push_back(argv[i]);
    }

    try {
        Server server(port, dataDir, myAddress, seedPeers);
        server.run();
    } catch (const std::exception& e) {
        std::cerr << "[server] fatal error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}