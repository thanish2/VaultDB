#pragma once
#include <string>
#include <cstdint>

// ============================================================================
// FILE: protocol.h
// PART OF: vaultdb
//
// NEW IN THIS VERSION: Command::MEMBERS
// A request a CLIENT sends to a server to ask "what is your current
// view of the cluster?". Unlike GOSSIP (which is a two-way exchange
// between servers), MEMBERS is one-way: the client sends nothing
// useful, and the server's response `value` carries its serialized
// MembershipList. The client uses this to rebuild its HashRing so
// routing skips nodes that gossip has marked DEAD.
// ============================================================================

enum class Command {
    PUT,
    GET,
    DELETE,
    GOSSIP,
    MEMBERS
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