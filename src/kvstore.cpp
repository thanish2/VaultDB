#include "kvstore.h"
#include <filesystem>
#include <iostream>
#include <regex>
#include <iomanip>
#include <sstream>

// ============================================================================
// FILE: kvstore.cpp
// PART OF: vaultdb
//
// NEW IN THIS VERSION: put(), get(), and remove() each lock mutex_
// before touching any data, using std::lock_guard. flush() and
// recover() are DELIBERATELY left unlocked — flush() is only ever
// called from inside put()/remove(), which have ALREADY locked the
// mutex by that point (locking again would deadlock, since std::mutex
// isn't re-entrant); recover() only runs once, from the constructor,
// before any other thread could possibly have a reference to this
// object yet.
// ============================================================================

KVStore::KVStore(const std::string& dataDir) : dataDir_(dataDir) {
    std::filesystem::create_directories(dataDir_);
    wal_ = std::make_unique<WAL>(dataDir_ + "/wal.log");
    recover();
}

void KVStore::recover() {
    std::regex pattern("sstable_(\\d+)\\.dat");
    int highestId = -1;

    for (const auto& entry : std::filesystem::directory_iterator(dataDir_)) {
        std::string filename = entry.path().filename().string();
        std::smatch match;
        if (std::regex_match(filename, match, pattern)) {
            int id = std::stoi(match[1]);
            if (id > highestId) highestId = id;
            sstables_.emplace_back(entry.path().string());
        }
    }
    nextSSTableId_ = highestId + 1;

    int count = 0;
    wal_->replay([this, &count](const WALEntry& entry) {
        if (entry.type == OpType::PUT) {
            memtable_.put(entry.key, entry.value);
        } else {
            memtable_.remove(entry.key);
        }
        count++;
    });

    if (count > 0) {
        std::cout << "[recovery] replayed " << count << " operations from WAL\n";
    }
    if (!sstables_.empty()) {
        std::cout << "[recovery] found " << sstables_.size()
                  << " existing SSTable file(s), next ID will be "
                  << nextSSTableId_ << "\n";
    }
}

void KVStore::flush() {
    std::ostringstream filename;
    filename << "sstable_" << std::setfill('0') << std::setw(4)
              << nextSSTableId_ << ".dat";
    std::string fullPath = dataDir_ + "/" + filename.str();

    SSTable newTable = SSTable::create(fullPath, memtable_.getAll());
    sstables_.push_back(newTable);
    nextSSTableId_++;

    memtable_.clear();
    wal_->clear();

    std::cout << "[flush] wrote " << fullPath << ", memtable and WAL cleared\n";
}

void KVStore::put(const std::string& key, const std::string& value) {
    // Locks the instant this line runs, and automatically unlocks
    // when `guard` goes out of scope at the end of this function —
    // same RAII pattern as WAL's constructor/destructor pair, just
    // applied to a lock instead of a file handle.
    std::lock_guard<std::mutex> guard(mutex_);

    wal_->logPut(key, value);
    memtable_.put(key, value);

    if (memtable_.size() >= FLUSH_THRESHOLD) {
        flush(); // safe: we already hold the lock, flush() doesn't re-lock
    }
}

void KVStore::remove(const std::string& key) {
    std::lock_guard<std::mutex> guard(mutex_);

    wal_->logDelete(key);
    memtable_.remove(key);

    if (memtable_.size() >= FLUSH_THRESHOLD) {
        flush();
    }
}

std::optional<std::string> KVStore::get(const std::string& key) const {
    // Reads need locking too: if one thread is mid-write while another
    // reads at the same instant, the read can see corrupted data.
    std::lock_guard<std::mutex> guard(mutex_);

    auto result = memtable_.get(key);
    if (result.has_value()) {
        return result;
    }

    for (auto it = sstables_.rbegin(); it != sstables_.rend(); ++it) {
        auto sstResult = it->get(key);
        if (sstResult.has_value()) {
            return sstResult;
        }
    }

    return std::nullopt;
}