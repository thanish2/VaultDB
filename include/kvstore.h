#pragma once
#include "wal.h"
#include "memtable.h"
#include "sstable.h"
#include <string>
#include <optional>
#include <memory>
#include <vector>
#include <mutex>

// ============================================================================
// FILE: kvstore.h
// PART OF: vaultdb
//
// NEW IN THIS VERSION:
// KVStore now owns a std::mutex, and every public operation (put, get,
// remove) locks it before touching any internal data, and automatically
// unlocks when done. This makes KVStore safe to call from MULTIPLE
// THREADS at the same time — which matters now that the server handles
// multiple clients concurrently, each potentially calling into the
// SAME KVStore object simultaneously.
// ============================================================================

class KVStore {
public:
    explicit KVStore(const std::string& dataDir);

    void put(const std::string& key, const std::string& value);
    void remove(const std::string& key);
    std::optional<std::string> get(const std::string& key) const;

private:
    std::string dataDir_;
    std::unique_ptr<WAL> wal_;
    Memtable memtable_;
    std::vector<SSTable> sstables_;
    int nextSSTableId_ = 0;
    static constexpr size_t FLUSH_THRESHOLD = 5;

    // NEW: the lock protecting all data above. "mutable" lets get()
    // (a const function) still lock/unlock this, since locking doesn't
    // change the actual key-value data — just this bookkeeping variable.
    mutable std::mutex mutex_;

    void recover();
    void flush();
};