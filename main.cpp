// main.cpp
#include <filesystem>
#include <iostream>

// GC headers (from previous implementation)
#include "snapshot_gc.hpp"
#include "journal_catalog.hpp"
#include "filesystem_storage.hpp"
#include "file_lock_leader.hpp"

int main(int argc, char** argv) {
    try {
        // ---------- Paths ----------
        std::filesystem::path snapshotRoot = "./snapshots";     // payload root
        std::filesystem::path catalogLog  = "./catalog.log";    // metadata journal
        std::filesystem::path leaderLock  = "./gc.lock";        // leader election lock

        std::filesystem::create_directories(snapshotRoot);

        // ---------- Adapters ----------
        JournalCatalog catalog(catalogLog);
        FilesystemStorage storage(snapshotRoot);
        FileLockLeaderElector leader(leaderLock);

        // ---------- Retention Policy ----------
        RetentionPolicy policy;
        policy.keepLastN = 3;                                   // keep last 3 snapshots
        policy.maxAge = std::chrono::hours(24 * 14);            // keep 14 days
        policy.enableCheckpointing = false;

        // ---------- GC Options ----------
        GCOptions opts;
