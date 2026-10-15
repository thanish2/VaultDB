#include "client.h"
#include "membership.h"
#include <iostream>
#include <stdexcept>
#include <vector>
#include <chrono>
#include <algorithm>
#include <random>
#include <set>

// ============================================================================
// FILE: client.cpp
// PART OF: vaultdb
// The actual logic behind everything declared in client.h.
//
// NEW IN THIS VERSION: GOSSIP-AWARE ROUTING.
//   - refreshRingIfStale(): called at the start of put/get/remove. If
//     more than RING_REFRESH_SECONDS have passed, it triggers a refresh.
//   - refreshRing(): asks ONE server for its membership view (a MEMBERS
//     request), then rebuilds ring_ from every node that is not DEAD.
//   - dropConnection(): forgets a broken cached socket so the next
//     request to that server reconnects instead of reusing a dead one.
// ============================================================================

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    #ifdef DELETE
        #undef DELETE
    #endif
    #define closeSocket closesocket
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #define closeSocket close
#endif

// On Linux, writing to a socket whose other end has closed raises the
// SIGPIPE signal, which by default KILLS the whole process. We hit this
// for real in testing: killing a server crashed any client that still
// had an open connection to it. MSG_NOSIGNAL makes send() just return
// an error instead, which our code already handles. Windows has no
// SIGPIPE and doesn't define MSG_NOSIGNAL, so there it falls back to 0.
#ifdef MSG_NOSIGNAL
    #define SEND_FLAGS MSG_NOSIGNAL
#else
    #define SEND_FLAGS 0
#endif

static bool readExactly(int socket, char* buffer, int numBytes) {
    int totalReceived = 0;
    while (totalReceived < numBytes) {
        int received = recv(socket, buffer + totalReceived,
                              numBytes - totalReceived, 0);
        if (received <= 0) return false;
        totalReceived += received;
    }
    return true;
}

static bool sendAll(int socket, const char* buffer, int numBytes) {
    int totalSent = 0;
    while (totalSent < numBytes) {
        int sent = send(socket, buffer + totalSent, numBytes - totalSent, SEND_FLAGS);
        if (sent <= 0) return false;
        totalSent += sent;
    }
    return true;
}

static bool sendMessage(int socket, const std::string& message) {
    uint32_t length = static_cast<uint32_t>(message.size());
    uint32_t networkLength = htonl(length);
    if (!sendAll(socket, reinterpret_cast<const char*>(&networkLength), 4)) {
        return false;
    }
    return sendAll(socket, message.data(), message.size());
}

static std::optional<std::string> readMessage(int socket) {
    uint32_t networkLength;
    if (!readExactly(socket, reinterpret_cast<char*>(&networkLength), 4)) {
        return std::nullopt;
    }
    uint32_t length = ntohl(networkLength);

    std::vector<char> buffer(length);
    if (length > 0) {
        if (!readExactly(socket, buffer.data(), length)) {
            return std::nullopt;
        }
    }
    return std::string(buffer.begin(), buffer.end());
}

Client::Client(const std::vector<ServerInfo>& serverList) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        throw std::runtime_error("WSAStartup failed");
    }
#endif

    for (const auto& server : serverList) {
        std::string address = server.host + ":" + std::to_string(server.port);
        ring_.addServer(address);
        // Remember the original addresses: they are the servers we are
        // always allowed to ask for a membership view.
        seedAddresses_.push_back(address);
    }

    if (ring_.serverCount() == 0) {
        throw std::runtime_error("Client created with an empty server list");
    }

    // Pretend the last refresh happened RING_REFRESH_SECONDS ago, so
    // the very first put/get/remove fetches a fresh view of the
    // cluster instead of trusting the hardcoded list.
    lastRingRefresh_ = std::chrono::steady_clock::now()
                     - std::chrono::seconds(RING_REFRESH_SECONDS);
}

Client::~Client() {
    for (auto& [address, sock] : connections_) {
        closeSocket(sock);
    }
#ifdef _WIN32
    WSACleanup();
#endif
}

int Client::getOrCreateConnection(const std::string& serverAddress) {
    auto it = connections_.find(serverAddress);
    if (it != connections_.end()) {
        return it->second;
    }

    size_t colonPos = serverAddress.find(':');
    std::string host = serverAddress.substr(0, colonPos);
    uint16_t port = static_cast<uint16_t>(
        std::stoi(serverAddress.substr(colonPos + 1))
    );

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        throw std::runtime_error("Failed to create socket for " + serverAddress);
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &address.sin_addr);

    int result = connect(
        sock,
        reinterpret_cast<sockaddr*>(&address),
        sizeof(address)
    );
    if (result < 0) {
        closeSocket(sock);
        throw std::runtime_error("Failed to connect to " + serverAddress);
    }

    connections_[serverAddress] = sock;
    return sock;
}

// NEW: close a cached connection and forget it, so the next request to
// this server opens a brand new one.
void Client::dropConnection(const std::string& serverAddress) {
    auto it = connections_.find(serverAddress);
    if (it == connections_.end()) return;
    closeSocket(it->second);
    connections_.erase(it);
}

uint64_t Client::currentTimestampMillis() const {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    );
    return static_cast<uint64_t>(ms.count());
}

// CHANGED: any failure now also drops the cached connection (a failed
// send/receive means the socket is broken, e.g. the server died).
Response Client::sendRequestToServer(const std::string& serverAddress, const Request& req) {
    Response failure;
    failure.success = false;
    failure.found = false;

    try {
        int sock = getOrCreateConnection(serverAddress);

        std::string encoded = encodeRequest(req);
        if (!sendMessage(sock, encoded)) {
            dropConnection(serverAddress);
            return failure;
        }

        auto responseOpt = readMessage(sock);
        if (!responseOpt.has_value()) {
            dropConnection(serverAddress);
            return failure;
        }

        return decodeResponse(*responseOpt);
    } catch (const std::exception&) {
        dropConnection(serverAddress);
        return failure;
    }
}

// NEW: does nothing unless our view of the cluster is older than
// RING_REFRESH_SECONDS.
void Client::refreshRingIfStale() {
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - lastRingRefresh_
    );
    if (elapsed.count() >= RING_REFRESH_SECONDS) {
        refreshRing();
    }
}

// NEW: ask one server who is in the cluster, rebuild ring_ from the answer.
void Client::refreshRing() {
    // Try the seed servers in RANDOM order, so we don't always lean on
    // the same one, and stop at the first that answers. If the first is
    // dead, we simply move on to the next.
    std::vector<std::string> candidates = seedAddresses_;
    std::random_device rd;
    std::mt19937 rng(rd());
    std::shuffle(candidates.begin(), candidates.end(), rng);

    for (const auto& address : candidates) {
        Request req;
        req.command = Command::MEMBERS;

        Response res = sendRequestToServer(address, req);
        if (!res.success || !res.found) {
            continue; // this one didn't answer, try the next
        }

        MembershipList view = MembershipList::deserialize(res.value);

        // Route to everyone that is NOT marked DEAD (SUSPECTED nodes
        // stay in: suspicion can flicker for healthy nodes).
        std::set<std::string> routable;
        for (const auto& member : view.getAllMembers()) {
            if (member.status != MemberStatus::DEAD) {
                routable.insert(member.address);
            }
        }
        // The server that just answered us is obviously alive, even if
        // its own list hadn't yet recorded itself.
        routable.insert(address);

        // Rebuild the ring from scratch from the routable nodes.
        HashRing newRing;
        for (const auto& node : routable) {
            newRing.addServer(node);
        }
        ring_ = newRing;

        // Close cached connections to nodes we no longer route to.
        // (Collect first, then drop, so we don't erase from the map
        // while looping over it.)
        std::vector<std::string> toDrop;
        for (const auto& entry : connections_) {
            if (routable.find(entry.first) == routable.end()) {
                toDrop.push_back(entry.first);
            }
        }
        for (const auto& node : toDrop) {
            dropConnection(node);
        }

        // Print only when the routable set actually CHANGES, so you can
        // see failure detection reach the client. "static" means this
        // variable remembers the last printed set between calls.
        static std::set<std::string> lastLogged;
        if (routable != lastLogged) {
            std::cout << "[client] cluster view updated, routing to "
                      << routable.size() << " node(s):";
            for (const auto& node : routable) std::cout << " " << node;
            std::cout << std::endl;
            lastLogged = routable;
        }

        break; // one successful answer is enough
    }

    // Record the attempt even if nobody answered, so a fully-down
    // cluster doesn't make EVERY request retry the refresh. If nobody
    // answered, ring_ simply stays as it was.
    lastRingRefresh_ = std::chrono::steady_clock::now();
}

Response Client::put(const std::string& key, const std::string& value) {
    refreshRingIfStale();

    std::vector<std::string> replicas = ring_.getServersForKey(key, N);

    Request req;
    req.command = Command::PUT;
    req.key = key;
    req.value = value;
    req.timestamp = currentTimestampMillis();

    int successCount = 0;
    for (const auto& replica : replicas) {
        Response res = sendRequestToServer(replica, req);
        if (res.success) {
            successCount++;
        } else {
            std::cout << "[client] warning: replica " << replica
                      << " did not confirm the write\n";
        }
    }

    Response finalResponse;
    if (successCount >= W) {
        finalResponse.success = true;
        finalResponse.found = true;
    } else {
        finalResponse.success = false;
        finalResponse.found = false;
        std::cout << "[client] WRITE FAILED: only " << successCount
                  << "/" << W << " required replicas confirmed\n";
    }

    return finalResponse;
}

Response Client::get(const std::string& key) {
    refreshRingIfStale();

    std::vector<std::string> replicas = ring_.getServersForKey(key, N);

    std::vector<Response> responses;
    int responseCount = 0;

    for (const auto& replica : replicas) {
        Request req;
        req.command = Command::GET;
        req.key = key;

        Response res = sendRequestToServer(replica, req);
        if (res.success) {
            responseCount++;
            responses.push_back(res);
        } else {
            std::cout << "[client] warning: replica " << replica
                      << " did not respond to read\n";
        }
    }

    if (responseCount < R) {
        Response failure;
        failure.success = false;
        failure.found = false;
        std::cout << "[client] READ FAILED: only " << responseCount
                  << "/" << R << " required replicas responded\n";
        return failure;
    }

    Response* best = nullptr;
    for (auto& res : responses) {
        if (res.found) {
            if (best == nullptr || res.timestamp > best->timestamp) {
                best = &res;
            }
        }
    }

    if (best == nullptr) {
        Response notFound;
        notFound.success = true;
        notFound.found = false;
        return notFound;
    }

    return *best;
}

Response Client::remove(const std::string& key) {
    refreshRingIfStale();

    std::vector<std::string> replicas = ring_.getServersForKey(key, N);

    Request req;
    req.command = Command::DELETE;
    req.key = key;

    int successCount = 0;
    for (const auto& replica : replicas) {
        Response res = sendRequestToServer(replica, req);
        if (res.success) {
            successCount++;
        }
    }

    Response finalResponse;
    finalResponse.success = (successCount >= W);
    finalResponse.found = true;
    return finalResponse;
}