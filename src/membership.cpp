#include "membership.h"
#include <sstream>
#include <utility>

// ============================================================================
// FILE: membership.cpp
// PART OF: vaultdb
// The actual logic behind everything declared in membership.h.
//
// THRESHOLDS: SUSPECT=5s, DEAD=12s, against a 2s gossip interval.
// Tighter values (3s/6s) caused false-positive flickering, because a
// healthy peer can be randomly skipped for a few gossip rounds in a
// row. The wider margin tolerates that normal randomness.
//
// KNOWN, ACCEPTED LIMITATION: if two nodes agree on a member's
// incarnation but disagree on its status, and neither has fresher
// direct contact, this simplified design cannot break the tie. A full
// SWIM-style "refutation" would fix it; we are not building that.
// ============================================================================

static constexpr int SUSPECT_TIMEOUT_SECONDS = 5;
static constexpr int DEAD_TIMEOUT_SECONDS = 12;

MembershipList::MembershipList(std::map<std::string, MemberInfo> members)
    : members_(std::move(members)) {}

// Private helper: caller MUST already hold mutex_.
void MembershipList::addMemberLocked(const std::string& address) {
    MemberInfo info;
    info.address = address;
    info.status = MemberStatus::ALIVE;
    info.lastHeartbeat = std::chrono::steady_clock::now();
    info.incarnation = 0;
    members_[address] = info;
}

void MembershipList::addMember(const std::string& address) {
    // lock_guard locks now and unlocks automatically when this
    // function ends (same RAII pattern as KVStore).
    std::lock_guard<std::mutex> guard(mutex_);
    addMemberLocked(address);
}

void MembershipList::recordHeartbeat(const std::string& address) {
    std::lock_guard<std::mutex> guard(mutex_);

    auto it = members_.find(address);
    if (it == members_.end()) {
        // We already hold the lock, so use the Locked helper here.
        // Calling addMember() would lock a second time and deadlock.
        addMemberLocked(address);
        return;
    }

    it->second.lastHeartbeat = std::chrono::steady_clock::now();
    it->second.incarnation++;
    it->second.status = MemberStatus::ALIVE;
}

void MembershipList::checkForFailures() {
    std::lock_guard<std::mutex> guard(mutex_);

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
            info.status = MemberStatus::ALIVE;
        }
    }
}

void MembershipList::merge(const MembershipList& other) {
    // Merging a list into itself would try to lock the same mutex
    // twice. It would also be pointless, so just do nothing.
    if (this == &other) return;

    // scoped_lock locks BOTH mutexes together using a deadlock-
    // avoiding algorithm, so two threads merging in opposite
    // directions (A into B, B into A) can't block each other forever.
    std::scoped_lock lock(mutex_, other.mutex_);

    for (const auto& [address, otherInfo] : other.members_) {
        auto it = members_.find(address);

        if (it == members_.end()) {
            // Brand new member: adopt it, but stamp it with OUR OWN
            // clock, not the sender's.
            MemberInfo adopted = otherInfo;
            adopted.lastHeartbeat = std::chrono::steady_clock::now();
            members_[address] = adopted;
            continue;
        }

        // Only incarnation is compared. Status is never copied.
        if (otherInfo.incarnation > it->second.incarnation) {
            it->second.incarnation = otherInfo.incarnation;
            it->second.lastHeartbeat = std::chrono::steady_clock::now();
        }
    }
}

std::vector<std::string> MembershipList::getAliveMembers() const {
    std::lock_guard<std::mutex> guard(mutex_);

    std::vector<std::string> result;
    for (const auto& [address, info] : members_) {
        if (info.status == MemberStatus::ALIVE) {
            result.push_back(address);
        }
    }
    return result;
}

std::vector<MemberInfo> MembershipList::getAllMembers() const {
    std::lock_guard<std::mutex> guard(mutex_);

    std::vector<MemberInfo> result;
    for (const auto& [address, info] : members_) {
        result.push_back(info);
    }
    return result;
}

static std::string statusToString(MemberStatus status) {
    switch (status) {
        case MemberStatus::ALIVE: return "ALIVE";
        case MemberStatus::SUSPECTED: return "SUSPECTED";
        case MemberStatus::DEAD: return "DEAD";
    }
    return "ALIVE";
}

static MemberStatus statusFromString(const std::string& s) {
    if (s == "SUSPECTED") return MemberStatus::SUSPECTED;
    if (s == "DEAD") return MemberStatus::DEAD;
    return MemberStatus::ALIVE;
}

std::string MembershipList::serialize() const {
    std::lock_guard<std::mutex> guard(mutex_);

    std::ostringstream oss;
    bool first = true;
    for (const auto& [address, info] : members_) {
        if (!first) oss << ";";
        oss << address << "," << statusToString(info.status)
            << "," << info.incarnation;
        first = false;
    }
    return oss.str();
}

MembershipList MembershipList::deserialize(const std::string& data) {
    // Build into a plain local map first. No lock is needed because
    // nobody else can see this map yet.
    std::map<std::string, MemberInfo> parsed;

    if (!data.empty()) {
        std::istringstream entryStream(data);
        std::string entry;

        while (std::getline(entryStream, entry, ';')) {
            std::istringstream fieldStream(entry);
            std::string address, statusStr, incarnationStr;

            std::getline(fieldStream, address, ',');
            std::getline(fieldStream, statusStr, ',');
            std::getline(fieldStream, incarnationStr, ',');

            MemberInfo info;
            info.address = address;
            info.status = statusFromString(statusStr);
            info.incarnation = std::stoull(incarnationStr);
            // Our own clock, not anything from the sender.
            info.lastHeartbeat = std::chrono::steady_clock::now();

            parsed[address] = info;
        }
    }

    // Returning a freshly constructed object directly means no copy
    // or move is needed (guaranteed in C++17), which matters because
    // MembershipList cannot be copied.
    return MembershipList(std::move(parsed));
}