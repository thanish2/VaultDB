#include "wal.h"
#include <sstream>

// ============================================================================
// FILE: wal.cpp
// PART OF: vaultdb
// This is the actual logic behind everything declared in wal.h.
// ============================================================================

// CONSTRUCTOR
// Runs once, when someone writes: WAL myWal("somefile.log");
WAL::WAL(const std::string& filepath) : filepath_(filepath) {
    // std::ios::app = "append mode". Every write to `out_` goes to the
    // END of the file. Old entries are never touched or overwritten.
    // If the file doesn't exist yet, it gets created automatically.
    out_.open(filepath_, std::ios::app);

    if (!out_.is_open()) {
        // If we can't even open our own log file, something is seriously
        // wrong (bad permissions, disk full, etc.) — we throw an error
        // rather than silently pretending everything is fine.
        throw std::runtime_error("Could not open WAL file: " + filepath_);
    }
}

// DESTRUCTOR
// Runs automatically when a WAL object goes out of scope / is destroyed.
WAL::~WAL() {
    if (out_.is_open()) {
        out_.close();
    }
}

// LOG A PUT
void WAL::logPut(const std::string& key, const std::string& value) {
    // Write one line to the file in the format:  PUT|key|value
    // We use '|' as a simple separator between fields.
    out_ << "PUT|" << key << "|" << value << "\n";

    // THIS IS THE MOST IMPORTANT LINE IN THE ENTIRE PROJECT SO FAR.
    // out_ << "..." only writes to a memory BUFFER by default — the
    // operating system doesn't necessarily push it to the physical disk
    // right away, for performance reasons.
    // .flush() forces it to actually leave the buffer and reach the disk
    // RIGHT NOW. Without this line, a crash immediately after this
    // function could lose the write — defeating the entire purpose of
    // having a WAL in the first place.
    out_.flush();
}

// LOG A DELETE
void WAL::logDelete(const std::string& key) {
    // Format:  DELETE|key   (no value needed for a delete)
    out_ << "DELETE|" << key << "\n";
    out_.flush();
}

// REPLAY THE LOG
void WAL::replay(const std::function<void(const WALEntry&)>& callback) {
    // We open a SEPARATE read handle here (ifstream = input file stream),
    // completely independent from out_ (which is only for writing).
    std::ifstream in(filepath_);

    if (!in.is_open()) {
        // No log file yet means this is a brand new database with
        // nothing to recover. That's a completely normal case, not
        // an error — we just return with nothing to do.
        return;
    }             

    std::string line;
    // std::getline reads one line at a time, until the file ends.
    while (std::getline(in, line)) {
        if (line.empty()) continue; // skip blank lines defensively

        std::istringstream iss(line);
        std::string opStr, key, value;

        // Split the line on '|'. First piece is always the operation type.
        std::getline(iss, opStr, '|');
        // Second piece is always the key.
        std::getline(iss, key, '|');

        WALEntry entry;
        entry.key = key;

        if (opStr == "PUT") {
            // Whatever's left on the line after the second '|' is the value.
            std::getline(iss, value);
            entry.type = OpType::PUT;
            entry.value = value;
        } else if (opStr == "DELETE") {
            entry.type = OpType::DELETE;
        } else {
            // A line we don't recognize — skip it rather than crash.
            continue;
        }

        // Hand this reconstructed entry back to whoever called replay().
        callback(entry);
    }
}

void WAL::clear() {
    // Close the current file handle first...
    out_.close();

    // ...then reopen the SAME filepath, but this time with trunc instead
    // of app. trunc wipes the file's contents completely, leaving it
    // empty, ready for fresh writes to start accumulating again.
    out_.open(filepath_, std::ios::trunc);
}