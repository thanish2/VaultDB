#include "client.h"
#include <iostream>
#include <stdexcept>
#include <vector>
#include <chrono>
#include <algorithm>

// ============================================================================
// FILE: client.cpp
// PART OF: vaultdb
// The actual logic behind everything declared in client.h.
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
        int sent = send(socket, buffer + totalSent, numBytes - totalSent, 0);
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
    }

    if (ring_.serverCount() == 0) {
        throw std::runtime_error("Client created with an empty server list");
    }
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

uint64_t Client::currentTimestampMillis() const {
    // std::chrono is C++'s standard time library. This gets "now" as
    // a duration since a fixed reference point (the "epoch"), then
    // converts that duration into milliseconds, then extracts the
    // raw number out of it.
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    );
    return static_cast<uint64_t>(ms.count());
}

// SEND TO ONE SPECIFIC SERVER — same logic as the old sendRequest,
// just now taking the target explicitly instead of computing it
// internally. If this particular server is unreachable, we catch
// that as a FAILED response rather than letting the whole put()/get()
// crash — a single down replica should never take down the whole
// operation, that's the entire point of quorum.
Response Client::sendRequestToServer(const std::string& serverAddress, const Request& req) {
    Response failure;
    failure.success = false;
    failure.found = false;

    try {
        int sock = getOrCreateConnection(serverAddress);

        std::string encoded = encodeRequest(req);
        if (!sendMessage(sock, encoded)) {
            return failure;
        }

        auto responseOpt = readMessage(sock);
        if (!responseOpt.has_value()) {
            return failure;
        }

        return decodeResponse(*responseOpt);
    } catch (const std::exception&) {
        // Could not even connect to this replica — treat it exactly
        // like any other failure, not a program-ending error.
        return failure;
    }
}

// PUT — sends to ALL N replicas, requires at least W to succeed.
Response Client::put(const std::string& key, const std::string& value) {
    std::vector<std::string> replicas = ring_.getServersForKey(key, N);

    Request req;
    req.command = Command::PUT;
    req.key = key;
    req.value = value;
    // ALL replicas get the SAME timestamp for this one logical write —
    // generated ONCE here, not separately per replica.
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
        // Fewer than W replicas confirmed — we cannot honestly tell
        // the caller this write succeeded.
        finalResponse.success = false;
        finalResponse.found = false;
        std::cout << "[client] WRITE FAILED: only " << successCount
                  << "/" << W << " required replicas confirmed\n";
    }

    return finalResponse;
}

// GET — reads from replicas, picks the HIGHEST-timestamp answer,
// requires at least R replicas to have responded.
Response Client::get(const std::string& key) {
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

    // Among all responses that actually FOUND a value, pick the one
    // with the highest timestamp (last-write-wins).
    Response* best = nullptr;
    for (auto& res : responses) {
        if (res.found) {
            if (best == nullptr || res.timestamp > best->timestamp) {
                best = &res;
            }
        }
    }

    if (best == nullptr) {
        // Every replica that responded said "not found."
        Response notFound;
        notFound.success = true;
        notFound.found = false;
        return notFound;
    }

    return *best;
}

// REMOVE — sends DELETE to all N replicas. (No timestamp comparison
// needed here since DELETE carries no timestamp — see the earlier
// discussion on this known simplification.)
Response Client::remove(const std::string& key) {
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