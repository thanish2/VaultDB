#include "server.h"
#include <iostream>

// ============================================================================
// FILE: server_main.cpp
// PART OF: vaultdb
//
// WHAT THIS FILE DOES:
// The actual runnable server program. Takes the PORT and DATA FOLDER
// as command-line arguments, so the same compiled .exe can run as any
// node in the cluster — e.g.:
//   .\server 5000 ./data-node1
//   .\server 5001 ./data-node2
//   .\server 5002 ./data-node3
// Falls back to port 5000 / ./data if no arguments are given, so it
// still works exactly like before for simple single-server testing.
// ============================================================================

int main(int argc, char** argv) {
    // argv[1], if given, is the port number (as text) — convert it to
    // an actual number with std::stoi, then to uint16_t (same type
    // Server's constructor expects).
    uint16_t port = (argc > 1) ? static_cast<uint16_t>(std::stoi(argv[1])) : 5000;

    // argv[2], if given, is the data folder path.
    std::string dataDir = (argc > 2) ? argv[2] : "./data";

    try {
        Server server(port, dataDir);
        server.run();
    } catch (const std::exception& e) {
        std::cerr << "[server] fatal error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}