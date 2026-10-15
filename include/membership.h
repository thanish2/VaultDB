#pragma once
#include <string>
#include <map>
#include <vector>
#include <chrono>
#include <mutex>
#include <cstdint>

// ============================================================================
// FILE: membership.h
// PART OF: vaultdb
//
// WHAT THIS FILE IS FOR:
// This defines the data structure gossip actually operates on: each
// node's local, ever-evolving VIEW of who else is in the cluster, and
// whether each of them currently looks alive or not. No networking
// code lives here at all.
//
// NEW IN THIS VERSION: THREAD SAFETY.
// A single MembershipList is now touched by several threads at once:
//   - the gossip thread (gossipLoop)
//   - every client-handler thread (incoming GOSSIP and MEMBERS requests)
// Without protection, two threads modifying the underlying std::map at
// the same time is a data race (the same class of bug we fixed in
// KVStore with a mutex). So every public method now locks mutex_.
//
// CONSEQUENCES OF HAVING A MUTEX MEMBER:
//   1. std::mutex cannot be copied, so MembershipList cannot be copied
//      either. Nothing in the project needs to copy one (they are only
//      ever created in place, or returned straight from deserialize()).
//   2. deserialize() builds a plain std::map first and hands it to a
//      private constructor, so the finished object is returned
//      directly with no copy or move needed (C++17 guarantees this).
//   3. recordHeartbeat() sometimes needs to add a member. Calling the
//      public addMember() from inside it would try to lock mutex_
//      twice on the same thread, which deadlocks (std::mutex is not
//      re-entrant, same lesson as flush() in KVStore). So both use a
//      private addMemberLocked() that assumes the lock is ALREADY held.
// ============================================================================

enum class MemberStatus {
    ALIVE,
    SUSPECTED, // hasn't been heard from in a while, but not yet given up on
    DEAD       // hasn't been heard from for a long time, considered gone
};

// What one node knows/believes about ONE other member of the cluster.
struct MemberInfo {
    std::string address;         // e.g. "127.0.0.1:5000"
    MemberStatus status = MemberStatus::ALIVE;

    // The LOCAL time (this node's own clock) when we last had direct
    // or gossiped-and-trusted evidence this member was alive. We only
    // ever compare this against OUR OWN current time, never against
    // another node's clock, so clock skew between machines is not a
    // concern here.
    std::chrono::steady_clock::time_point lastHeartbeat;

    // A simple logical counter used to decide, when merging two nodes'
    // views of the same member, whose information is newer. Higher
    // counter wins, regardless of any clock.
    uint64_t incarnation = 0;
};

class MembershipList {
public:
    MembershipList() = default;

    // Not copyable, because it owns a mutex (see header comment).
    MembershipList(const MembershipList&) = delete;
    MembershipList& operator=(const MembershipList&) = delete;

    // Adds a brand new member, marked ALIVE, with a fresh timestamp.
    void addMember(const std::string& address);

    // Direct evidence a member is alive: refreshes their timestamp,
    // bumps their incarnation, marks them ALIVE.
    void recordHeartbeat(const std::string& address);

    // Re-derives every member's status from OUR OWN clock:
    // ALIVE -> SUSPECTED -> DEAD as silence gets longer.
    void checkForFailures();

    // Merges ANOTHER node's view into ours (the gossip logic). Only
    // incarnation is compared; status is never copied, it is always
    // recomputed locally by checkForFailures().
    void merge(const MembershipList& other);

    // Addresses currently believed ALIVE.
    std::vector<std::string> getAliveMembers() const;

    // A copy of everything we currently know.
    std::vector<MemberInfo> getAllMembers() const;

    // Flat text form for sending inside a GOSSIP/MEMBERS message:
    //   "address,status,incarnation;address,status,incarnation;..."
    // lastHeartbeat is deliberately NOT included (it is meaningless
    // outside the process that recorded it).
    std::string serialize() const;

    // Rebuilds a MembershipList from that text form.
    static MembershipList deserialize(const std::string& data);

private:
    // Used only by deserialize(), to construct a finished list directly.
    explicit MembershipList(std::map<std::string, MemberInfo> members);

    // Does the work of addMember() but ASSUMES mutex_ is already held
    // by the caller. Exists so recordHeartbeat() can add a member
    // without locking twice (which would deadlock).
    void addMemberLocked(const std::string& address);

    // "mutable" so const methods (serialize, getAllMembers, ...) can
    // still lock it; locking changes the mutex's internal state, not
    // the membership data itself.
    mutable std::mutex mutex_;
    std::map<std::string, MemberInfo> members_;
};