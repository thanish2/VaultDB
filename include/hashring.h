#pragma once
#include <string>
#include <map>
#include <cstdint>
#include <vector>

// ============================================================================
// FILE: hashring.h
// PART OF: vaultdb
//
// NEW IN THIS VERSION: getServersForKey(key, N)
// Returns the N DISTINCT physical servers responsible for a key —
// the primary owner (same as getServerForKey), plus the next N-1
// DISTINCT servers found walking further clockwise. This is the
// foundation for REPLICATION: instead of one server owning a key,
// N servers each hold a copy, so the key survives up to N-1 server
// failures.
//
// getServerForKey(key) is KEPT for backward compatibility / simplicity
// in places that only need the primary owner, but internally it will
// just call getServersForKey(key, 1) and return the first result.
// ============================================================================

class HashRing {
public:
    void addServer(const std::string& serverAddress);
    void removeServer(const std::string& serverAddress);

    // Existing single-server lookup — unchanged behavior.
    std::string getServerForKey(const std::string& key) const;

    // NEW: returns up to N DISTINCT physical servers responsible for
    // this key, in clockwise order starting from the key's own
    // position. If fewer than N distinct physical servers exist in
    // the whole cluster, returns as many as actually exist (capped,
    // never duplicated).
    std::vector<std::string> getServersForKey(const std::string& key, int n) const;

    size_t serverCount() const;

private:
    static constexpr int VIRTUAL_NODES_PER_SERVER = 150;
    std::map<uint32_t, std::string> ring_;
    std::vector<std::string> physicalServers_;
    uint32_t hashString(const std::string& input) const;
};