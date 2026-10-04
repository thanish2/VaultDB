#include "kvstore.h"
#include <iostream>
#include <cassert>

// ============================================================================
// FILE: main.cpp
// PART OF: vaultdb
//
// WHAT THIS FILE IS FOR:
// This is the actual runnable program — the entry point. Everything we've
// built so far (WAL, Memtable, SSTable, KVStore) is just classes sitting
// in files; this is what actually CREATES a KVStore and uses it, so we
// can prove the whole thing works end to end.
//
// THE TEST THIS PROGRAM RUNS:
// We simulate two SEPARATE executions of the program, to prove crash
// recovery actually works across BOTH the WAL and SSTables now:
//
//   RUN 1 ("run1"): write 12 keys. Since FLUSH_THRESHOLD is 5, this
//                   should trigger TWO automatic flushes along the way
//                   (after key #5, and again after key #10), leaving
//                   only keys #11 and #12 sitting in the memtable/WAL
//                   when we "crash."
//
//   RUN 2 ("run2"): start completely fresh (new process, empty RAM),
//                   and check that ALL 12 keys are still correctly
//                   readable — some coming from SSTable files on disk,
//                   some coming from WAL replay — even though NEITHER
//                   the WAL nor any single SSTable holds all 12 keys
//                   by itself.
// ============================================================================

int main(int argc, char** argv) {
    // argv[0] is always the program's own name. argv[1], if provided,
    // is the first actual argument someone typed after the program name.
    // e.g. running "./vaultdb run1" means argv[1] is the string "run1".
    // If no argument is given at all, we default to "run1" so the
    // program doesn't crash from trying to read a missing argument.
    std::string mode = (argc > 1) ? argv[1] : "run1";

    // Creating this object runs KVStore's constructor, which opens (or
    // creates) the data folder, opens the WAL, scans for any existing
    // SSTable files from a previous run, and replays the WAL into the
    // memtable — all of this happens automatically, right here.
    KVStore store("./data");

    if (mode == "run1") {
        std::cout << "== RUN 1: writing 12 keys (threshold is 5, expect 2 flushes) ==\n";

        // Write keys key1 -> key12, each with a matching value.
        // std::to_string(i) converts the integer i into a string,
        // e.g. std::to_string(5) becomes "5", so "key" + "5" = "key5".
        for (int i = 1; i <= 12; i++) {
            store.put("key" + std::to_string(i), "value" + std::to_string(i));
        }
        // By the time this loop finishes, you should have already seen
        // TWO "[flush]" messages printed automatically by KVStore::put(),
        // since we cross the FLUSH_THRESHOLD (5) twice during these
        // 12 writes.

        std::cout << "Exiting without graceful shutdown.\n";
        std::cout.flush(); // make sure this prints before we hard-exit

        // std::_Exit() terminates the program IMMEDIATELY — it skips
        // destructors and any cleanup code. This is deliberately much
        // closer to a real crash than a normal "return 0;" would be,
        // which DOES run destructors cleanly. We want to prove recovery
        // works even in the messy, uncontrolled case, not just when
        // the program shuts down nicely.
        std::_Exit(0);

    } else {
        std::cout << "== RUN 2: verifying all 12 keys are recoverable ==\n";

        // Check every key from key1 to key12. Some of these will be
        // answered from an SSTable file (the ones flushed during run1),
        // and some from the WAL that was just replayed a moment ago in
        // KVStore's constructor (the ones written AFTER the last flush).
        // From the outside, get() hides this difference completely —
        // we don't need to know or care where each answer came from.
        for (int i = 1; i <= 12; i++) {
            auto val = store.get("key" + std::to_string(i));
            std::string expected = "value" + std::to_string(i);

            std::cout << "key" << i << " = "
                      << (val ? *val : "NOT FOUND");

            // assert() crashes the program immediately with an error
            // message if the condition is false. We use it here as a
            // strict correctness check — if recovery didn't work
            // properly, we want to know immediately and loudly, not
            // silently continue with wrong data.
            assert(val.has_value() && *val == expected);

            std::cout << " [OK]\n";
        }

        std::cout << "\nAll 12 keys correctly recovered across WAL + SSTables.\n";
    }

    return 0;
}