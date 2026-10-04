#include "hashring.h"
#include <stdexcept>
#include <cstring>
#include <set>

// ============================================================================
// FILE: hashring.cpp
// PART OF: vaultdb
// The actual logic behind everything declared in hashring.h.
// ============================================================================

uint32_t HashRing::hashString(const std::string& input) const {
    const uint32_t c1 = 0xcc9e2d51;
    const uint32_t c2 = 0x1b873593;
    uint32_t seed = 0;
    uint32_t hash = seed;

    const char* data = input.data();
    size_t len = input.size();
    size_t numBlocks = len / 4;

    for (size_t i = 0; i < numBlocks; i++) {
        uint32_t k;
        std::memcpy(&k, data + i * 4, 4);
        k *= c1;
        k = (k << 15) | (k >> 17);
        k *= c2;
        hash ^= k;
        hash = (hash << 13) | (hash >> 19);
        hash = hash * 5 + 0xe6546b64;
    }

    const char* tail = data + numBlocks * 4;
    uint32_t k1 = 0;
    size_t remaining = len & 3;
    if (remaining == 3) k1 ^= static_cast<uint32_t>(static_cast<unsigned char>(tail[2])) << 16;
    if (remaining >= 2) k1 ^= static_cast<uint32_t>(static_cast<unsigned char>(tail[1])) << 8;
    if (remaining >= 1) {
        k1 ^= static_cast<uint32_t>(static_cast<unsigned char>(tail[0]));
        k1 *= c1;
        k1 = (k1 << 15) | (k1 >> 17);
        k1 *= c2;
        hash ^= k1;
    }

    hash ^= static_cast<uint32_t>(len);
    hash ^= (hash >> 16);
    hash *= 0x85ebca6b;
    hash ^= (hash >> 13);
    hash *= 0xc2b2ae35;
    hash ^= (hash >> 16);

    return hash;
}

void HashRing::addServer(const std::string& serverAddress) {
    for (int i = 0; i < VIRTUAL_NODES_PER_SERVER; i++) {
        std::string virtualNodeId = serverAddress + "#" + std::to_string(i);
        uint32_t position = hashString(virtualNodeId);
        ring_[position] = serverAddress;
    }
    physicalServers_.push_back(serverAddress);
}

void HashRing::removeServer(const std::string& serverAddress) {
    for (int i = 0; i < VIRTUAL_NODES_PER_SERVER; i++) {
        std::string virtualNodeId = serverAddress + "#" + std::to_string(i);
        uint32_t position = hashString(virtualNodeId);
        ring_.erase(position);
    }
    for (auto it = physicalServers_.begin(); it != physicalServers_.end(); ++it) {
        if (*it == serverAddress) {
            physicalServers_.erase(it);
            break;
        }
    }
}

std::string HashRing::getServerForKey(const std::string& key) const {
    auto servers = getServersForKey(key, 1);
    if (servers.empty()) {
        throw std::runtime_error("HashRing has no servers");
    }
    return servers[0];
}

// NEW: the actual replication-aware lookup.
std::vector<std::string> HashRing::getServersForKey(const std::string& key, int n) const {
    if (ring_.empty()) {
        throw std::runtime_error("HashRing has no servers");
    }

    // Never try to return more DISTINCT servers than physically exist.
    int target = std::min(n, static_cast<int>(physicalServers_.size()));

    std::vector<std::string> result;
    // A set lets us quickly check "have I already picked this server?"
    // without scanning the result vector each time.
    std::set<std::string> alreadyPicked;

    uint32_t keyPosition = hashString(key);
    auto it = ring_.upper_bound(keyPosition);

    // Walk clockwise, starting from the key's position, collecting
    // DISTINCT physical servers until we have `target` of them.
    // We may need to pass through MANY virtual node entries (up to
    // 150 per server) before finding the next NEW physical server —
    // that's expected and fine, this loop just keeps going.
    //
    // We cap the number of ring entries we're willing to examine at
    // ring_.size(), so that even in a pathological case (e.g. n is
    // somehow larger than physicalServers_.size(), which we already
    // guarded against above) this loop can never spin forever.
    size_t entriesExamined = 0;
    size_t maxEntriesToExamine = ring_.size();

    while (result.size() < static_cast<size_t>(target) &&
           entriesExamined < maxEntriesToExamine) {

        if (it == ring_.end()) {
            // Wrap around, same as getServerForKey's original logic.
            it = ring_.begin();
        }

        const std::string& server = it->second;
        if (alreadyPicked.find(server) == alreadyPicked.end()) {
            result.push_back(server);
            alreadyPicked.insert(server);
        }

        ++it;
        ++entriesExamined;
    }

    return result;
}

size_t HashRing::serverCount() const {
    return physicalServers_.size();
}