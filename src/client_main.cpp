#include "client.h"
#include <iostream>
#include <sstream>

// ============================================================================
// FILE: client_main.cpp
// PART OF: vaultdb
//
// WHAT THIS FILE DOES:
// This is the actual runnable client program. It:
//   1. Defines the list of servers in the cluster (hardcoded for now —
//      a real system would discover this dynamically, via gossip).
//   2. Creates ONE Client object using that list. The Client's
//      constructor builds its own HashRing from these addresses.
//   3. Runs an interactive loop: read what you type, figure out
//      which Client function to call (put/get/remove), call it,
//      print the result. Repeats until you type "exit".
// ============================================================================

int main() {
    // STEP 1: the full list of servers in our cluster. Update these
    // to match whatever ports you actually started your servers on.
    std::vector<ServerInfo> servers = {
        {"127.0.0.1", 5000},
        {"127.0.0.1", 5001},
        {"127.0.0.1", 5002}
    };

    try {
        // STEP 2: create ONE Client, connected (logically — actual
        // sockets are opened lazily, on demand) to the whole cluster.
        Client client(servers);

        std::cout << "Connected to a " << servers.size()
                  << "-node vaultdb cluster.\n";
        std::cout << "Commands: put <key> <value> | get <key> | "
                     "delete <key> | exit\n";

        // STEP 3: the interactive loop.
        std::string line;
        while (true) {
            std::cout << "> ";
            if (!std::getline(std::cin, line)) break;
            if (line == "exit") break;

            std::istringstream iss(line);
            std::string cmd, key, value;
            iss >> cmd >> key;

            if (key.empty()) {
                std::cout << "Need at least a key. Try: get <key>\n";
                continue;
            }

            Response res;
            if (cmd == "put") {
                iss >> value;
                res = client.put(key, value);
            } else if (cmd == "get") {
                res = client.get(key);
            } else if (cmd == "delete") {
                res = client.remove(key);
            } else {
                std::cout << "Unrecognized command.\n";
                continue;
            }

            if (!res.success) {
                std::cout << "Server returned an error.\n";
            } else if (res.found) {
                std::cout << "OK: " << res.value << "\n";
            } else {
                std::cout << "OK (not found)\n";
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[client] error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}