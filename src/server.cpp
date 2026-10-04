#include "server.h"
#include "protocol.h"
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <vector>
#include <thread>
#include <sstream>

// ============================================================================
// FILE: server.cpp
// PART OF: vaultdb
//
// NEW IN THIS VERSION: handleClient now packs a PUT's timestamp
// together with its value into ONE combined string ("timestamp|value")
// before handing it to store_.put() — KVStore itself remains totally
// unaware timestamps or replication even exist; it just stores
// whatever string it's given, exactly as before. On GET, we split
// that combined string back apart before replying, so the CLIENT can
// see the timestamp and use it to pick the freshest value across
// multiple replicas.
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

static bool sendMessage(int socket, const std::string& message) {
    uint32_t length = static_cast<uint32_t>(message.size());
    uint32_t networkLength = htonl(length);
    if (!sendAll(socket, reinterpret_cast<const char*>(&networkLength), 4)) {
        return false;
    }
    return sendAll(socket, message.data(), message.size());
}

Server::Server(uint16_t port, const std::string& dataDir)
    : port_(port), store_(dataDir), listenSocket_(-1) {

#ifdef _WIN32
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        throw std::runtime_error("WSAStartup failed");
    }
#endif

    setupListenSocket();
}

Server::~Server() {
    if (listenSocket_ != -1) {
        closeSocket(listenSocket_);
    }
#ifdef _WIN32
    WSACleanup();
#endif
}

void Server::setupListenSocket() {
    listenSocket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocket_ < 0) {
        throw std::runtime_error("Failed to create socket");
    }

    int opt = 1;
    setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&opt, sizeof(opt));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port_);

    int bindResult = bind(
        listenSocket_,
        reinterpret_cast<sockaddr*>(&address),
        sizeof(address)
    );
    if (bindResult < 0) {
        throw std::runtime_error(
            "Failed to bind to port " + std::to_string(port_) +
            " (is something else already using it?)"
        );
    }

    int listenResult = listen(listenSocket_, 5);
    if (listenResult < 0) {
        throw std::runtime_error("Failed to listen on socket");
    }

    std::cout << "[server] listening on port " << port_ << "\n";
}

// HANDLE CLIENT — updated PUT/GET logic for timestamp packing/unpacking.
void Server::handleClient(int clientSocket) {
    std::cout << "[server] handling client...\n";

    while (true) {
        auto messageOpt = readMessage(clientSocket);
        if (!messageOpt.has_value()) {
            std::cout << "[server] client disconnected.\n";
            break;
        }

        std::string requestLine = *messageOpt;
        std::cout << "[server] received: " << requestLine << "\n";

        Request req = decodeRequest(requestLine);

        Response res;
        res.success = true;

        if (req.command == Command::PUT) {
            // Pack timestamp + value into ONE string before storing.
            // KVStore has NO idea this packing is happening — to it,
            // this is just "some string", exactly like every value
            // it has ever stored.
            std::string combined = std::to_string(req.timestamp) + "|" + req.value;
            store_.put(req.key, combined);
            res.found = true;

        } else if (req.command == Command::GET) {
            auto stored = store_.get(req.key);
            if (stored.has_value()) {
                // Split the combined string back apart: everything
                // before the first '|' is the timestamp, everything
                // after is the real value.
                std::string combined = *stored;
                size_t sepPos = combined.find('|');

                res.found = true;
                res.timestamp = std::stoull(combined.substr(0, sepPos));
                res.value = combined.substr(sepPos + 1);
            } else {
                res.found = false;
            }

        } else { // Command::DELETE
            // NOTE: deletes are NOT currently timestamped. This is a
            // known, honest simplification — a real system would
            // timestamp deletes too (as "tombstone" writes with their
            // own timestamp) so that a delete can correctly "win"
            // against an older concurrent write during quorum
            // resolution. We're keeping this simple for now.
            store_.remove(req.key);
            res.found = true;
        }

        std::string responseLine = encodeResponse(res);
        if (!sendMessage(clientSocket, responseLine)) {
            std::cout << "[server] failed to send response, client likely disconnected.\n";
            break;
        }
    }

    closeSocket(clientSocket);
}

void Server::run() {
    std::cout << "[server] ready to accept connections (multithreaded)...\n";

    while (true) {
        int clientSocket = accept(listenSocket_, nullptr, nullptr);
        if (clientSocket < 0) {
            std::cerr << "[server] accept() failed, continuing...\n";
            continue;
        }

        std::cout << "[server] a client connected!\n";

        std::thread clientThread([this, clientSocket]() {
            handleClient(clientSocket);
        });

        clientThread.detach();
    }
}