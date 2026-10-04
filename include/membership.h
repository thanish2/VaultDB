#pragma once
#include <string>
#include <map>
#include <vector>
#include <chrono>

// ============================================================================
// FILE: membership.h
// PART OF: vaultdb
//
// WHAT THIS FILE IS FOR:
// This defines the data structure gossip actually operates on: each
// node's local, ever-evolving VIEW of who else is in the cluster, and
// whether each of them currently looks alive or not. No networking
// code lives here at all — this file is pure, testable bookkeeping
// logic, same philosophy as protocol.h before any sockets existed.
//
// THE CORE IDEA:
// Every node maintains a MembershipList — its own local belief about
// the cluster. Nodes periodically exchange (gossip) their lists with
// each other (built in a later file, gossip.h/.cpp), and MERGE what
// they receive into their own list, using "who has heard from this
// node more recently" as the tie-breaker. Over several rounds, this
// merging process lets accurate membership information spread across
// the whole cluster without any central coordinator.
//
// NEW IN THIS VERSION: serialize()/deserialize()
// These convert a MembershipList to/from the flat text format that
// travels inside a GOSSIP Request's `value` field:
//   "address,status,incarnation;address,status,incarnation;..."
// lastHeartbeat is DELIBERATELY never serialized — it's a
// steady_clock::time_point, meaningless outside the process that
// created it. The receiving node always resets it to ITS OWN current
// time when adopting new information (see membership.cpp).
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
    // or gossiped-and-trusted evidence this member was alive. Using
    // steady_clock (not system_clock) specifically because it only
    // ever moves forward and is never affected by clock adjustments —
    // and because we ONLY ever compare this against OUR OWN current
    // time, never against another node's clock, clock skew between
    // machines is simply not a concern here.
    std::chrono::steady_clock::time_point lastHeartbeat;

    // A simple logical counter, incremented every time WE personally
    // receive fresh evidence this member is alive (a direct heartbeat,
    // or a newer piece of gossip about it). This is what lets us
    // decide, when MERGING two members' views of the same node,
    // "whose information is actually newer" — higher counter wins,
    // regardless of real-world clock time. This avoids relying on
    // wall-clock comparison ACROSS different nodes' gossip messages,
    // which would reintroduce the clock-skew problem we just said we
    // were avoiding.
    uint64_t incarnation = 0;
};

class MembershipList {
public:
    // Adds a brand new member we didn't know about before, marked
    // ALIVE, with a fresh heartbeat timestamp.
    void addMember(const std::string& address);

    // Called when we get DIRECT evidence a member is alive (e.g. we
    // successfully gossiped with them, or successfully sent them a
    // request). Resets their status to ALIVE and refreshes their
    // heartbeat timestamp/incarnation.
    void recordHeartbeat(const std::string& address);

    // Checks every known member's lastHeartbeat against the current
    // time, and updates their status: ALIVE -> SUSPECTED if too long
    // since last heard, SUSPECTED -> DEAD if even longer. Meant to be
    // called periodically (e.g. once per second) by a background loop.
    void checkForFailures();

    // Merges ANOTHER node's membership view into our own — the actual
    // gossip logic. For every member mentioned in "other", if we don't
    // know about them, add them; if we do, keep whichever version has
    // the HIGHER incarnation number (i.e. more recent evidence).
    // NOTE: status is NEVER copied directly — only incarnation and a
    // freshened lastHeartbeat are adopted. status is always recomputed
    // locally by checkForFailures(), to avoid two nodes with EQUAL
    // incarnation numbers getting permanently stuck disagreeing on
    // status (a real issue found and fixed during development).
    void merge(const MembershipList& other);

    // Returns just the addresses currently believed to be ALIVE —
    // this is what HashRing will be rebuilt from.
    std::vector<std::string> getAliveMembers() const;

    // Returns a copy of everything we currently know — used when WE
    // are the one sending gossip to someone else.
    std::vector<MemberInfo> getAllMembers() const;

    // Turns this list into ONE flat string, suitable for sending
    // inside a GOSSIP request's value field.
    std::string serialize() const;

    // Parses a string in that same format back into a usable
    // MembershipList object. Used on the RECEIVING end of a gossip
    // exchange, to turn the incoming payload into something we can
    // actually call merge() with.
    static MembershipList deserialize(const std::string& data);

private:
    // Keyed by address, so looking up "do we know this member" and
    // updating them is a simple, fast map operation.
    std::map<std::string, MemberInfo> members_;
};