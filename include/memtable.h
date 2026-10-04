#pragma once
#include <map>
#include <string>
#include <optional>

// ============================================================================
// FILE: memtable.h
// PART OF: vaultdb
//
// WHAT THIS FILE IS FOR:
// This is the in-memory (RAM) data structure holding the current, "live"
// key-value data. The WAL (previous files) is our safety net for surviving
// crashes; this class is what actually gives us FAST reads and writes,
// since RAM access is enormously faster than reading/writing files on disk.
//
// IMPORTANT: this class knows NOTHING about disk, files, or crash recovery.
// It's a pure in-memory structure. Combining it with the WAL for durability
// happens in a separate file (kvstore.h/.cpp), which we'll write after this.
// Keeping this class "pure" like this is a deliberate design choice — it's
// simpler to write, simpler to test, and easier to reason about.
// ============================================================================

class Memtable {
public:
    // Insert a new key, or overwrite it if it already exists.
    void put(const std::string& key, const std::string& value);

    // Mark a key as deleted. NOTE: this does NOT erase it from the map —
    // it stores a "tombstone" (see explanation above the class, and the
    // .cpp file) so that a delete is remembered even after we introduce
    // multiple storage layers later (SSTables).
    void remove(const std::string& key);

    // Look up a key's current value.
    // Returns std::nullopt in TWO possible cases, which look identical
    // from the caller's point of view:
    //   1. the key was never set at all
    //   2. the key was set, but has since been deleted (tombstoned)
    std::optional<std::string> get(const std::string& key) const;

    // How many entries (including tombstones) are currently held.
    // We'll use this later to decide when the memtable is "full" and
    // needs to be flushed to disk as an SSTable.
    size_t size() const;

     // NEW: hands over the ENTIRE internal map, read-only, so KVStore
    // can pass it straight into SSTable::create() during a flush.
    // Returning a `const&` (const reference) means no copying happens —
    // the caller just gets to LOOK at our real data_, not a duplicate
    // of it, and `const` means they're not allowed to modify it through
    // this reference either.
    const std::map<std::string, std::optional<std::string>>& getAll() const {
        return data_;
    }

    // NEW: wipes the memtable completely empty. We call this right
    // after a successful flush — the data now lives safely in a new
    // SSTable file, so RAM doesn't need to hold it anymore.
    void clear() {
        data_.clear();
    }

private:
    // The core data structure. Every key maps to EITHER a real value,
    // or std::nullopt (a tombstone marking "explicitly deleted").
    std::map<std::string, std::optional<std::string>> data_;
};