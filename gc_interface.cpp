/*
Create Garbage_collecotor.cpp
1) Problem Statement
We have a system that periodically creates snapshots (files/blobs/metadata) of some dataset/state. Over time, old snapshots consume storage. We need a Snapshot GC that:

Selects snapshots eligible for deletion (based on retention + references).
Deletes them safely (no data loss, no deleting active/needed snapshots).
Handles failures (disk corruption, partial deletes, retries, audit trail). [Handling S...t Failures | Word]


2) Key Requirements (Design)
Functional

Retention policy:

Keep last N snapshots, or keep snapshots newer than T days.
Optionally keep daily/weekly/monthly “checkpoints”.


Reference protection:

A snapshot cannot be deleted if it is referenced by:

restore points / tags (e.g., “release”, “prod-pin”)
active readers/leases
replication or incremental chains (parent dependency)




Atomicity & safety:

Two‑phase delete: mark then sweep (delete).
Detect and avoid deleting snapshots currently in use.



Non‑Functional

Crash-safe: if process restarts, it should resume without corruption.
Observable: metrics, logs, audit records of deletions.
Scalable: handle millions of snapshots with pagination.
Configurable: policy changes without code changes.


3) Data Model
Snapshot metadata (stored in a persistent “Snapshot Catalog”)

snapshotId (string/uuid)
createdTime (timestamp)
sizeBytes
state ∈ {Active, MarkedForDeletion, Deleting, Deleted}
parentId (optional)  // for incremental chains
tags (set)   // pins like “keep”, “release”
refCount or leases // active references/leases count
lastAccessTime (optional)

Catalog interface

list snapshots by time / state
get snapshot by id
set state transitions
add/remove tags or leases
record deletion events


4) GC Algorithm (Mark–Sweep for Snapshots)
MARK phase
Compute “live set”:

Snapshots that must be kept:

Newer than retention cutoff
In the latest N
Tagged/pinned (e.g., “keep”, “release”)
Have active leases/refcount > 0
Are parents of a live snapshot (dependency closure)



Everything else becomes candidate.
SWEEP phase
For candidates:

Transition Active → MarkedForDeletion
After a grace period (optional), transition MarkedForDeletion → Deleting
Delete underlying storage objects
Transition Deleting → Deleted
On failure: record error + backoff + retry; possibly quarantine.

This two-stage approach is also friendly for “remembering failures across restarts” as highlighted in the snapshot failure doc. [Handling S...t Failures | Word]

5) Concurrency & Safety

Leases: readers take a lease on snapshotId; GC refuses to delete if lease exists.
Locking: use catalog-level optimistic concurrency (etag/version) per snapshot record.
Rate limiting: delete at bounded throughput to avoid IO storms.
Verification: optionally “scrub” metadata/store to detect corruption early. [Handling S...t Failures | Word]


6) C++ Skeleton Design (Clean Interfaces)

This is intentionally “design-program style”: interfaces + core logic.
You can plug in your storage (local FS/Azure blob/etc.) and DB (SQLite/Cosmos/etc.).

*/

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>
#include <iostream>

using TimePoint = std::chrono::system_clock::time_point;

enum class SnapshotState {
    Active,
    MarkedForDeletion,
    Deleting,
    Deleted
};

struct SnapshotMeta {
    std::string id;
    TimePoint created;
    uint64_t sizeBytes{0};

    SnapshotState state{SnapshotState::Active};

    std::optional<std::string> parentId;                // for incremental chain
    std::unordered_set<std::string> tags;               // "keep", "release", ...
    uint32_t leaseCount{0};                             // active readers
};

struct RetentionPolicy {
    // Keep at least lastN snapshots always.
    size_t lastN{10};

    // Keep snapshots newer than maxAge.
    std::chrono::hours maxAge{std::chrono::hours(24 * 30)}; // 30 days default

    // Optional: keep every K-th snapshot older than maxAge (e.g., weekly checkpoints)
    bool enableCheckpointing{false};
    std::chrono::hours checkpointInterval{std::chrono::hours(24 * 7)}; // 7 days
};

struct GCConfig {
    size_t maxDeletesPerRun{1000};
    bool dryRun{false};
    std::chrono::seconds deletionGracePeriod{std::chrono::seconds(0)};
};

// --- Interfaces ---

class ISnapshotCatalog {
public:
    virtual ~ISnapshotCatalog() = default;

    // List snapshots (typically sorted by created descending)
    virtual std::vector<SnapshotMeta> ListAll() = 0;

    virtual std::optional<SnapshotMeta> Get(const std::string& id) = 0;

    // CAS-style update: only update if current state matches expected.
    virtual bool TransitionState(const std::string& id,
                                 SnapshotState expected,
                                 SnapshotState desired) = 0;

    virtual void RecordEvent(const std::string& snapshotId,
                             const std::string& eventType,
                             const std::string& details) = 0;
};

class IStorageBackend {
public:
    virtual ~IStorageBackend() = default;
    virtual bool DeleteSnapshotPayload(const std::string& snapshotId) = 0;
    virtual bool Exists(const std::string& snapshotId) = 0;
};

// --- Snapshot GC ---

class SnapshotGC {
public:
    SnapshotGC(ISnapshotCatalog& catalog, IStorageBackend& storage,
               RetentionPolicy policy, GCConfig cfg)
        : catalog_(catalog), storage_(storage), policy_(policy), cfg_(cfg) {}

    void RunOnce() {
        auto all = catalog_.ListAll();
        if (all.empty()) return;

        // 1) Build live set
        auto live = ComputeLiveSet(all);

        // 2) Mark candidates
        std::vector<std::string> candidates;
        candidates.reserve(all.size());
        for (auto& s : all) {
            if (s.state != SnapshotState::Active) continue;
            if (live.count(s.id) == 0) {
                candidates.push_back(s.id);
            }
        }

        // limit deletions
        if (candidates.size() > cfg_.maxDeletesPerRun)
            candidates.resize(cfg_.maxDeletesPerRun);

        // Mark
        for (const auto& id : candidates) {
            if (cfg_.dryRun) {
                catalog_.RecordEvent(id, "DryRun-Mark", "Would mark for deletion");
                continue;
            }
            bool ok = catalog_.TransitionState(id, SnapshotState::Active,
                                               SnapshotState::MarkedForDeletion);
            catalog_.RecordEvent(id, ok ? "Marked" : "MarkFailed",
                                 ok ? "MarkedForDeletion" : "State transition failed");
        }

        // 3) Sweep (delete) those already marked and safe to delete
        SweepMarked(all);
    }

private:
    std::unordered_set<std::string> ComputeLiveSet(std::vector<SnapshotMeta>& all) {
        std::unordered_set<std::string> live;

        // Sort assumption: ListAll returns newest->oldest. If not, sort here.
        const auto now = std::chrono::system_clock::now();
        const auto cutoff = now - policy_.maxAge;

        // Keep lastN
        for (size_t i = 0; i < all.size() && i < policy_.lastN; i++) {
            MarkLiveWithParents(all[i], live);
        }

        // Keep newer than cutoff, pinned, leased
        for (const auto& s : all) {
            if (s.created >= cutoff) {
                MarkLiveWithParents(s, live);
                continue;
            }
            if (s.leaseCount > 0) {
                MarkLiveWithParents(s, live);
                continue;
            }
            if (s.tags.count("keep") || s.tags.count("pin") || s.tags.count("release")) {
                MarkLiveWithParents(s, live);
                continue;
            }
        }

        // Optional checkpointing for older snapshots
        if (policy_.enableCheckpointing) {
            // Keep one snapshot per checkpointInterval (rough sketch)
            TimePoint lastKept = TimePoint{};
            bool first = true;
            for (const auto& s : all) {
                if (s.created >= cutoff) continue; // already covered
                if (first) {
                    MarkLiveWithParents(s, live);
                    lastKept = s.created;
                    first = false;
                } else {
                    auto delta = lastKept - s.created;
                    if (delta >= policy_.checkpointInterval) {
                        MarkLiveWithParents(s, live);
                        lastKept = s.created;
                    }
                }
            }
        }

        return live;
    }

    void MarkLiveWithParents(const SnapshotMeta& s, std::unordered_set<std::string>& live) {
        if (!live.insert(s.id).second) return; // already present
        if (s.parentId) {
            // dependency closure: parent cannot be deleted if child is live
            auto parent = catalog_.Get(*s.parentId);
            if (parent) MarkLiveWithParents(*parent, live);
        }
    }

    void SweepMarked(const std::vector<SnapshotMeta>& all) {
        for (const auto& s : all) {
            if (s.state != SnapshotState::MarkedForDeletion) continue;
            if (s.leaseCount > 0) {
                catalog_.RecordEvent(s.id, "SkipSweep", "Has active leases");
                continue;
            }
            if (s.tags.count("keep") || s.tags.count("pin") || s.tags.count("release")) {
                catalog_.RecordEvent(s.id, "SkipSweep", "Pinned via tag");
                continue;
            }

            if (cfg_.dryRun) {
                catalog_.RecordEvent(s.id, "DryRun-Delete", "Would delete payload");
                continue;
            }

            // Transition to Deleting
            if (!catalog_.TransitionState(s.id, SnapshotState::MarkedForDeletion,
                                          SnapshotState::Deleting)) {
                catalog_.RecordEvent(s.id, "SweepFailed", "State transition failed");
                continue;
            }

            // Delete payload
            bool deleted = storage_.DeleteSnapshotPayload(s.id);
            if (!deleted) {
                catalog_.RecordEvent(s.id, "DeleteFailed", "Storage delete failed");
                // best-effort rollback or leave as Deleting for retry
                continue;
            }

            // Transition to Deleted
            bool finalized = catalog_.TransitionState(s.id, SnapshotState::Deleting,
                                                     SnapshotState::Deleted);
            catalog_.RecordEvent(s.id, finalized ? "Deleted" : "FinalizeFailed",
                                 finalized ? "Deleted successfully" : "Failed to finalize");
        }
    }

private:
    ISnapshotCatalog& catalog_;
    IStorageBackend& storage_;
    RetentionPolicy policy_;
    GCConfig cfg_;
};
