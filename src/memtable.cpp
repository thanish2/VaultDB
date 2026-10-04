#include "memtable.h"

// ============================================================================
// FILE: memtable.cpp
// PART OF: vaultdb
// The actual logic behind everything declared in memtable.h.
// This file is short — the memtable's job is simple, all the complexity
// (durability, disk I/O) lives elsewhere.
// ============================================================================

void Memtable::put(const std::string& key, const std::string& value) {
    // data_[key] = value :
    // If "key" doesn't exist yet in the map, this INSERTS a new entry.
    // If "key" already exists, this OVERWRITES its current value.
    // Either way, the optional now holds a real string -> has_value() == true.
    data_[key] = value;
}

void Memtable::remove(const std::string& key) {
    // We do NOT call data_.erase(key) here.
    // Instead we explicitly set this key's optional to "empty"
    // (std::nullopt) — this is the tombstone.
    //
    // Why not just erase()? Because right now, with only a memtable,
    // erasing would work fine. But once SSTables exist (a future file),
    // an old SSTable on disk might still have this key. If we just
    // erased it from the memtable, a lookup would check the memtable
    // (nothing found), then fall through to check the old SSTable
    // (finds the STALE value) — incorrectly resurrecting deleted data.
    // The tombstone here is what stops that from happening.
    data_[key] = std::nullopt;
}

std::optional<std::string> Memtable::get(const std::string& key) const {
    // .find() looks up the key without inserting anything if it's missing
    // (careful: using data_[key] here instead would ACCIDENTALLY INSERT
    // a new empty entry for a key that was never set — a classic std::map
    // footgun. .find() avoids that.)
    auto it = data_.find(key);

    if (it == data_.end()) {
        // The key was never put() at all.
        return std::nullopt;
    }

    // it->second is itself an std::optional<std::string> already —
    // so we just return it directly. If it's a tombstone, this
    // correctly returns std::nullopt. If it's a real value, this
    // correctly returns that value.
    return it->second;
}

size_t Memtable::size() const {
    return data_.size();
}