#pragma once
#include <string>
#include <fstream>
#include <functional>

// ============================================================================
// FILE: wal.h
// PART OF: vaultdb
//
// WHAT THIS FILE IS FOR:
// This declares the WAL (Write-Ahead Log) class — the component responsible
// for making sure no write is ever lost, even if the program crashes right
// after that write happens.
//
// THE RULE THIS CLASS ENFORCES:
//   Before we change anything in memory, we first record the operation here,
//   on disk, and confirm it's actually saved (not just buffered).
// ============================================================================

// Every operation our database supports is one of these two kinds.
enum class OpType {
    PUT,     // "set this key to this value"
    DELETE   // "remove this key"
};

// One single recorded operation — one line in the log file, essentially.
struct WALEntry {
    OpType type;
    std::string key;
    std::string value; // only meaningful when type == PUT
};

class WAL {
public:
    // Opens (or creates, if it doesn't exist yet) the log file at this path.
    explicit WAL(const std::string& filepath);
    ~WAL();

    // Records a PUT operation and guarantees it's written to disk
    // before this function returns.
    void logPut(const std::string& key, const std::string& value);

    // Records a DELETE operation and guarantees it's written to disk
    // before this function returns.
    void logDelete(const std::string& key);

    // Reads the log file from the beginning, and for every operation
    // found, calls `callback` with it — in the exact order they
    // originally happened. This is how we recover state after a restart.
    void replay(const std::function<void(const WALEntry&)>& callback);

    // NEW: wipes the log file completely empty. We call this right after
    // a successful flush to an SSTable — once data is safely saved there,
    // the WAL no longer needs to remember those same operations. This is
    // exactly what keeps the WAL from growing forever, and what keeps
    // recovery-on-startup fast even after the database has been running
    // for a long time.
    void clear();

private:
    std::string filepath_;
    std::ofstream out_; // the open file handle we write new entries to
};