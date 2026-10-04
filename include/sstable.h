#pragma once
#include "wal.h"   // reusing WALEntry and OpType — same line format
#include <map>
#include <string>
#include <optional>

// ============================================================================
// FILE: sstable.h
// PART OF: vaultdb
//
// WHAT THIS FILE IS FOR:
// An SSTable ("Sorted String Table") is a FILE on disk holding a complete,
// sorted, IMMUTABLE snapshot of key-value data at the moment it was created.
// Once written, an SSTable is never modified again — no updates, no
// deletes performed on it directly. This immutability is a deliberate
// design choice: it makes SSTables simple to reason about, safe to read
// concurrently from multiple places, and easy to eventually merge/compact.
//
// WHY WE NEED THIS:
// The memtable (RAM) can't hold unlimited data, and the WAL (disk log)
// can't grow forever without recovery becoming slower and slower over
// time. SSTables solve both: once the memtable gets "full enough," we
// write its ENTIRE contents out to a new SSTable file, then clear both
// the memtable and the WAL — starting fresh, with old data now safely
// living on disk in this file instead.
// ============================================================================

class SSTable {
public:
    // Creates a BRAND NEW SSTable file on disk, writing out the given
    // sorted entries (typically taken directly from a Memtable that's
    // being flushed). Returns an SSTable object representing this new file.
    static SSTable create(
        const std::string& filepath,
        const std::map<std::string, std::optional<std::string>>& entries
    );

    // Opens an EXISTING SSTable file (one that was already created earlier,
    // possibly in a previous run of the program) so we can read from it.
    explicit SSTable(const std::string& filepath);

    // Searches this SSTable's file for the given key.
    // Returns the value if found, std::nullopt if the key was tombstoned
    // (explicitly deleted) OR simply never present in this particular file.
    std::optional<std::string> get(const std::string& key) const;

    // Lets callers (like KVStore) know which file this SSTable lives in —
    // useful for logging/debugging, and later for things like deciding
    // which files to merge during compaction.
    std::string filepath() const { return filepath_; }

private:
    std::string filepath_;

    // Private constructor used only by create() — forces callers to go
    // through the explicit create() function when making a NEW file,
    // rather than accidentally constructing an SSTable object that
    // claims to represent a file that doesn't actually have any data
    // written yet. The public constructor above is still used for
    // opening files that ALREADY exist.
    SSTable() = default;
};