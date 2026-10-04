#include "sstable.h"
#include <fstream>
#include <sstream>
#include <stdexcept>

// ============================================================================
// FILE: sstable.cpp
// PART OF: vaultdb
// The actual logic behind everything declared in sstable.h.
// ============================================================================

// CREATE — writes a brand new SSTable file from a memtable snapshot.
// Remember: this is a STATIC function, not a constructor. It builds and
// returns a whole SSTable object.
SSTable SSTable::create(
    const std::string& filepath,
    const std::map<std::string, std::optional<std::string>>& entries
) {
    // std::ios::trunc means "if this file already exists, wipe it first."
    // We use trunc (not app, unlike WAL!) because we're writing a
    // COMPLETE, ONE-TIME snapshot here, not appending to a growing log.
    std::ofstream out(filepath, std::ios::trunc);

    if (!out.is_open()) {
        throw std::runtime_error("Could not create SSTable file: " + filepath);
    }

    // Walk through every entry in the map. Since `entries` is a std::map,
    // this loop visits keys IN SORTED ORDER automatically — we don't
    // need to sort anything ourselves, std::map already guarantees it.
    for (const auto& [key, value] : entries) {
        if (value.has_value()) {
            // A real value exists for this key.
            out << "PUT|" << key << "|" << *value << "\n";
        } else {
            // This key was tombstoned (deleted) — we write that fact
            // down too, so this deletion is remembered even after
            // the memtable that knew about it is gone.
            out << "DELETE|" << key << "\n";
        }
    }

    out.flush();
    out.close();

    // Build an SSTable object representing this now-fully-written file.
    // This is where the PRIVATE default constructor gets used — notice
    // we're INSIDE the class here, so we're allowed to use it.
    SSTable table;
    table.filepath_ = filepath;
    return table;
}

// CONSTRUCTOR — for opening a file that ALREADY exists.
// Notice this does almost nothing! It doesn't need to read the whole
// file into memory or anything expensive — it just remembers the path.
// The actual reading only happens later, inside get(), and only for
// the specific key being searched for.
SSTable::SSTable(const std::string& filepath) : filepath_(filepath) {}

// GET — search this file for a key.
std::optional<std::string> SSTable::get(const std::string& key) const {
    std::ifstream in(filepath_);
    if (!in.is_open()) {
        return std::nullopt; // file missing entirely - treat as "not found"
    }

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;

        std::istringstream iss(line);
        std::string opStr, fileKey, value;
        std::getline(iss, opStr, '|');
        std::getline(iss, fileKey, '|');

        // THE EARLY-EXIT OPTIMIZATION mentioned before we wrote this file:
        // Since the file is SORTED, if the key we just read is already
        // alphabetically PAST the key we're searching for, our key
        // cannot possibly appear later in the file - we can stop
        // scanning right now instead of reading to the very end.
        if (fileKey > key) {
            break;
        }

        // Only bother fully parsing this line if it's actually the key
        // we're looking for.
        if (fileKey == key) {
            if (opStr == "PUT") {
                std::getline(iss, value);
                return value; // found it - a real value
            } else if (opStr == "DELETE") {
                return std::nullopt; // found it - but it's a tombstone
            }
        }
        // otherwise: fileKey < key, keep scanning forward
    }

    // Reached the end of the file (or broke out early) without finding
    // the key at all.
    return std::nullopt;
}