#pragma once
#include <string>
#include <cstdint>

// ============================================================================
// FILE: protocol.h
// PART OF: vaultdb
//
// NEW IN THIS VERSION: Command::GOSSIP
// A new request type servers send to EACH OTHER (not something a
// normal client ever sends) to exchange MembershipList information.
// For a GOSSIP request, the `value` field carries a SERIALIZED
// MembershipList (built in membership.h/.cpp) instead of a normal
// database value, and `key` is unused/empty. The response's `value`
// field carries the RESPONDING server's own serialized list back —
// so one request/response pair is a full two-way gossip exchange.
// ============================================================================

enum class Command {
    PUT,
    GET,
    DELETE,
    GOSSIP
};

struct Request {
    Command command;
    std::string key;
    std::string value;
    uint64_t timestamp = 0;
};

struct Response {
    bool success;
    bool found;
    std::string value;
    uint64_t timestamp = 0;
};

std::string encodeRequest(const Request& req);
Request decodeRequest(const std::string& line);
std::string encodeResponse(const Response& res);
Response decodeResponse(const std::string& line);