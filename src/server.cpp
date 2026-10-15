#include "server.h"
#include "protocol.h"
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <vector>
#include <thread>
#include <chrono>
#include <random>

// ============================================================================
// FILE: server.cpp
// PART OF: vaultdb
//
// NEW IN THIS VERSION: handleClient answers Command::MEMBERS requests
// by replying with this server's current membership view, so a Client
// can rebuild its HashRing from it (skipping nodes gossip has marked
// DEAD). Everything else is unchanged from the previous version,
// including the self-heartbeat line in gossipLoop().
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

Server::Server(uint16_t port, const std::string& dataDir,
               const std::string& myAddress,
               const std::vector<std::string>& seedPeers)
    : port_(port), store_(dataDir), listenSocket_(-1),
      myAddress_(myAddress), running_(true) {

#ifdef _WIN32
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        throw std::runtime_error("WSAStartup failed");
    }
#endif

    setupListenSocket();

    for (const auto& peer : seedPeers) {
        membership_.addMember(peer);
    }

    gossipThread_ = std::thread([this]() {
        gossipLoop();
    });
}

Server::~Server() {
    running_ = false;
    if (gossipThread_.joinable()) {
        gossipThread_.join();
    }

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

std::string Server::handleIncomingGossip(const std::string& incomingSerialized) {
    MembershipList incoming = MembershipList::deserialize(incomingSerialized);
    membership_.checkForFailures();
    membership_.merge(incoming);
    return membership_.serialize();
}

void Server::handleClient(int clientSocket) {
    std::cout << "[server] handling client...\n";

    while (true) {
        auto messageOpt = readMessage(clientSocket);
        if (!messageOpt.has_value()) {
            std::cout << "[server] client disconnected.\n";
            break;
        }

        std::string requestLine = *messageOpt;
        Request req = decodeRequest(requestLine);

        Response res;
        res.success = true;

        if (req.command == Command::PUT) {
            std::cout << "[server] received: " << requestLine << "\n";
            std::string combined = std::to_string(req.timestamp) + "|" + req.value;
            store_.put(req.key, combined);
            res.found = true;

        } else if (req.command == Command::GET) {
            std::cout << "[server] received: " << requestLine << "\n";
            auto stored = store_.get(req.key);
            if (stored.has_value()) {
                std::string combined = *stored;
                size_t sepPos = combined.find('|');
                res.found = true;
                res.timestamp = std::stoull(combined.substr(0, sepPos));
                res.value = combined.substr(sepPos + 1);
            } else {
                res.found = false;
            }

        } else if (req.command == Command::GOSSIP) {
            std::cout << "[server] received GOSSIP request\n";
            std::string ourView = handleIncomingGossip(req.value);
            res.found = true;
            res.value = ourView;

        } else if (req.command == Command::MEMBERS) {
            // NEW: a client is asking for our current view of the
            // cluster. Refresh our own failure detection first, so
            // the statuses we report are current, then reply with
            // the serialized list. Deliberately NOT logged: clients
            // will poll this every few seconds, and printing each
            // one would flood the console just like gossip would.
            membership_.checkForFailures();
            res.found = true;
            res.value = membership_.serialize();

        } else { // Command::DELETE
            std::cout << "[server] received: " << requestLine << "\n";
            store_.remove(req.key);
            res.found = true;
        }

        std::string responseLine = encodeResponse(res);
        if (!sendMessage(clientSocket, responseLine)) {
            std::cout << "[server] failed to send response.\n";
            break;
        }
    }

    closeSocket(clientSocket);
}

void Server::gossipLoop() {
    std::random_device rd;
    std::mt19937 rng(rd());

    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (!running_) break;

        membership_.checkForFailures();

        // Directly assert our own liveness every round (a node can
        // never gossip with itself, so its own entry would otherwise
        // go stale).
        membership_.recordHeartbeat(myAddress_);

        auto alive = membership_.getAliveMembers();
        std::cout << "[server:" << port_ << "] alive peers: ";
        for (auto& a : alive) std::cout << a << " ";
        std::cout << std::endl;

        auto allMembers = membership_.getAllMembers();
        if (allMembers.empty()) {
            continue;
        }

        std::uniform_int_distribution<size_t> dist(0, allMembers.size() - 1);
        std::string targetAddress = allMembers[dist(rng)].address;

        size_t colonPos = targetAddress.find(':');
        if (colonPos == std::string::npos) continue;
        std::string host = targetAddress.substr(0, colonPos);
        uint16_t targetPort = static_cast<uint16_t>(
            std::stoi(targetAddress.substr(colonPos + 1))
        );

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) continue;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(targetPort);
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

        if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            closeSocket(sock);
            continue;
        }

        Request req;
        req.command = Command::GOSSIP;
        req.value = membership_.serialize();

        std::string encoded = encodeRequest(req);
        if (sendMessage(sock, encoded)) {
            auto responseOpt = readMessage(sock);
            if (responseOpt.has_value()) {
                Response res = decodeResponse(*responseOpt);
                if (res.success && res.found) {
                    MembershipList theirView = MembershipList::deserialize(res.value);
                    membership_.merge(theirView);
                    membership_.recordHeartbeat(targetAddress);
                }
            }
        }

        closeSocket(sock);
    }
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