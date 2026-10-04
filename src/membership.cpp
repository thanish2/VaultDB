#include "membership.h"
#include <sstream>

// ============================================================================
// FILE: membership.cpp
// PART OF: vaultdb
// The actual logic behind everything declared in membership.h.
//
// CORRECTED merge() LOGIC:
// status is NEVER copied directly from another node's gossip — only
// incarnation and lastHeartbeat are exchanged. status is ALWAYS
// recomputed locally by checkForFailures(), using only this node's
// own clock. This avoids a bug found during design review: two
// nodes with the SAME incarnation number but different status
// (one ALIVE, one DEAD) previously had no way to resolve their
// disagreement, since merge()'s only comparison was incarnation.
//
// KNOWN, ACCEPTED LIMITATION (documented, not silently hidden):
// If two nodes already agree on a member's incarnation number but
// disagree on status, and NEITHER currently has fresher direct
// contact with that member to break the tie, this simplified design
// has no mechanism to resolve the standoff. A full implementation
// (SWIM-style "refutation") would have the member itself detect it's
// been wrongly suspected and proactively bump its own incarnation to
// force a correction. We are not building that here.
// ============================================================================

static constexpr int SUSPECT_TIMEOUT_SECONDS = 3;
static constexpr int DEAD_TIMEOUT_SECONDS = 6;

void MembershipList::addMember(const std::string& address) {
    MemberInfo info;
    info.address = address;
    info.status = MemberStatus::ALIVE;
    info.lastHeartbeat = std::chrono::steady_clock::now();
    info.incarnation = 0;
    members_[address] = info;
}

void MembershipList::recordHeartbeat(const std::string& address) {
    auto it = members_.find(address);
    if (it == members_.end()) {
        addMember(address);
        return;
    }
    // Direct contact: refresh OUR OWN clock, bump OUR OWN incarnation
    // record for this peer, mark ALIVE immediately.
    it->second.lastHeartbeat = std::chrono::steady_clock::now();
    it->second.incarnation++;
    it->second.status = MemberStatus::ALIVE;
}

void MembershipList::checkForFailures() {
    auto now = std::chrono::steady_clock::now();
    for (auto& [address, info] : members_) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - info.lastHeartbeat
        );
        if (elapsed.count() >= DEAD_TIMEOUT_SECONDS) {
            info.status = MemberStatus::DEAD;
        } else if (elapsed.count() >= SUSPECT_TIMEOUT_SECONDS) {
            if (info.status != MemberStatus::DEAD) {
                info.status = MemberStatus::SUSPECTED;
            }
        } else {
            // Within the healthy window — correctly un-suspect if a
            // fresher heartbeat/merge has since come in.
            info.status = MemberStatus::ALIVE;
        }
    }
}

void MembershipList::merge(const MembershipList& other) {
    for (const auto& [address, otherInfo] : other.members_) {
        auto it = members_.find(address);

        if (it == members_.end()) {
            // Brand new member — adopt fully, but reset lastHeartbeat
            // to OUR OWN clock right now, not the sender's clock.
            MemberInfo adopted = otherInfo;
            adopted.lastHeartbeat = std::chrono::steady_clock::now();
            members_[address] = adopted;
            continue;
        }

        // ONLY compare incarnation. NEVER copy status directly.
        if (otherInfo.incarnation > it->second.incarnation) {
            it->second.incarnation = otherInfo.incarnation;
            it->second.lastHeartbeat = std::chrono::steady_clock::now();
        }
        // If incoming incarnation <= ours: ignore. Known tie-break
        // limitation documented above applies when they're EQUAL but
        // status disagrees.
    }
}

std::vector<std::string> MembershipList::getAliveMembers() const {
    std::vector<std::string> result;
    for (const auto& [address, info] : members_) {
        if (info.status == MemberStatus::ALIVE) {
            result.push_back(address);
        }
    }
    return result;
}

std::vector<MemberInfo> MembershipList::getAllMembers() const {
    std::vector<MemberInfo> result;
    for (const auto& [address, info] : members_) {
        result.push_back(info);
    }
    return result;
}

// Turns a MemberStatus enum into text, for serialization.
static std::string statusToString(MemberStatus status) {
    switch (status) {
        case MemberStatus::ALIVE: return "ALIVE";
        case MemberStatus::SUSPECTED: return "SUSPECTED";
        case MemberStatus::DEAD: return "DEAD";
    }
    return "ALIVE"; // defensive fallback, should never actually hit this
}

// Turns text back into a MemberStatus enum.
static MemberStatus statusFromString(const std::string& s) {
    if (s == "SUSPECTED") return MemberStatus::SUSPECTED;
    if (s == "DEAD") return MemberStatus::DEAD;
    return MemberStatus::ALIVE;
}

// SERIALIZE: "address,status,incarnation;address,status,incarnation;..."
std::string MembershipList::serialize() const {
    std::ostringstream oss;
    bool first = true;

    for (const auto& [address, info] : members_) {
        if (!first) {
            oss << ";"; // separate entries with ';'
        }
        oss << address << "," << statusToString(info.status)
            << "," << info.incarnation;
        first = false;
    }

    return oss.str();
}

// DESERIALIZE: parses that format back into a real MembershipList.
MembershipList MembershipList::deserialize(const std::string& data) {
    MembershipList result;

    if (data.empty()) {
        return result; // an empty payload just means "no members"
    }

    // Split on ';' first, to get each individual member's chunk.
    std::istringstream entryStream(data);
    std::string entry;

    while (std::getline(entryStream, entry, ';')) {
        // Now split THIS entry on ',' to get its 3 fields.
        std::istringstream fieldStream(entry);
        std::string address, statusStr, incarnationStr;

        std::getline(fieldStream, address, ',');
        std::getline(fieldStream, statusStr, ',');
        std::getline(fieldStream, incarnationStr, ',');

        MemberInfo info;
        info.address = address;
        info.status = statusFromString(statusStr);
        info.incarnation = std::stoull(incarnationStr);
        // lastHeartbeat deliberately set to NOW — our own clock,
        // not anything from the sender.
        info.lastHeartbeat = std::chrono::steady_clock::now();

        result.members_[address] = info;
    }

    return result;
}