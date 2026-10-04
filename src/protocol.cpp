#include "protocol.h"
#include <sstream>
#include <stdexcept>

// ============================================================================
// FILE: protocol.cpp
// PART OF: vaultdb
//
// NEW: GOSSIP wire format is "GOSSIP|payload" — simpler than PUT/GET,
// since gossip has no real "key", just one big payload string (the
// serialized MembershipList). We take EVERYTHING after the first '|'
// as the payload, rather than splitting further, since the payload
// itself will contain its own internal separators.
// ============================================================================

std::string encodeRequest(const Request& req) {
    std::ostringstream oss;

    if (req.command == Command::PUT) {
        oss << "PUT|" << req.key << "|" << req.timestamp << "|" << req.value;
    } else if (req.command == Command::GET) {
        oss << "GET|" << req.key;
    } else if (req.command == Command::GOSSIP) {
        oss << "GOSSIP|" << req.value;
    } else { // Command::DELETE
        oss << "DELETE|" << req.key;
    }

    return oss.str();
}

Request decodeRequest(const std::string& line) {
    // Find the FIRST '|' manually, rather than using istringstream +
    // getline, since GOSSIP's payload may itself contain characters
    // that would confuse a generic field-by-field parse.
    size_t firstSep = line.find('|');
    if (firstSep == std::string::npos) {
        throw std::runtime_error("Malformed request: " + line);
    }

    std::string cmdStr = line.substr(0, firstSep);
    std::string rest = line.substr(firstSep + 1);

    Request req;

    if (cmdStr == "GOSSIP") {
        req.command = Command::GOSSIP;
        req.value = rest; // the ENTIRE remainder is the payload
        return req;
    }

    // For PUT/GET/DELETE, parse normally.
    std::istringstream iss(rest);
    std::string key, timestampStr, value;
    std::getline(iss, key, '|');
    req.key = key;

    if (cmdStr == "PUT") {
        std::getline(iss, timestampStr, '|');
        std::getline(iss, value);
        req.command = Command::PUT;
        req.timestamp = std::stoull(timestampStr);
        req.value = value;
    } else if (cmdStr == "GET") {
        req.command = Command::GET;
    } else if (cmdStr == "DELETE") {
        req.command = Command::DELETE;
    } else {
        throw std::runtime_error("Unknown command in request: " + cmdStr);
    }

    return req;
}

std::string encodeResponse(const Response& res) {
    std::ostringstream oss;

    if (!res.success) {
        oss << "ERROR|";
    } else if (res.found) {
        oss << "OK|FOUND|" << res.timestamp << "|" << res.value;
    } else {
        oss << "OK|NOTFOUND|";
    }

    return oss.str();
}

Response decodeResponse(const std::string& line) {
    std::istringstream iss(line);
    std::string status, foundStr, timestampStr, value;

    std::getline(iss, status, '|');

    Response res;

    if (status == "ERROR") {
        res.success = false;
        res.found = false;
    } else if (status == "OK") {
        res.success = true;
        std::getline(iss, foundStr, '|');

        if (foundStr == "FOUND") {
            res.found = true;
            std::getline(iss, timestampStr, '|');
            std::getline(iss, value);
            res.timestamp = std::stoull(timestampStr);
            res.value = value;
        } else {
            res.found = false;
        }
    } else {
        throw std::runtime_error("Unknown status in response: " + status);
    }

    return res;
}