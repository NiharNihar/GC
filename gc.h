// snapshot_gc.hpp
#pragma once
#include "snapshot_gc_interfaces.hpp"
#include <algorithm>
#include <cmath>
#include <unordered_set>

struct GCMetrics {
    std::size_t scanned{0};
    std::size_t tombstoned{0};
    std::size_t deleted{0};
    std::size_t quarantined{0};
    std::size_t deleteFailed{0};
    std::size_t inactiveLoadedSignals{0};
};

class SnapshotGC {
public:
    SnapshotGC(ISnapshotCatalog& catalog,
               IStorageBackend& storage,
               RetentionPolicy policy,
               GCOptions opts,
               ILeaderElector* leader = nullptr,
               ICorruptionTracker* corruption = nullptr)
        : catalog_(catalog),
          storage_(storage),
          policy_(std::move(policy)),
          opts_(std::move(opts)),
          leader_(leader),
          corruption_(corruption) {}

    GCMetrics RunOnce() {
        if (leader_ && !leader_->TryAcquire()) {
            // Not leader; exit quickly
            return {};
        }

        GCMetrics metrics{};
        auto all = catalog_.ListAll();
        metrics.scanned = all.size();

        // Build live set
        auto live = ComputeLiveSet(all);

        // 1) Tombstone stage (soft delete): mark unreferenced as Tombstoned.
        if (opts_.enableTombstoneStage) {
            TombstoneCandidates(all, live, metrics);
        }

        // 2) Hard delete stage: delete tombstoned whose grace expired, with retries/backoff.
        if (opts_.enableHardDeleteStage) {
            HardDeleteEligible(metrics);
        }

        if (leader_) leader_->Release();
        return metrics;
    }

private:
    ISnapshotCatalog& catalog_;
    IStorageBackend& storage_;
    RetentionPolicy policy_;
    GCOptions opts_;
    ILeaderElector* leader_{nullptr};
    ICorruptionTracker* corruption_{nullptr};

    std::unordered_set<std::string> ComputeLiveSet(const std::vector<SnapshotMeta>& all) {
        std::unordered_set<std::string> live;
        auto now = Clock::now();
        auto cutoff = now - policy_.maxAge;

        // Keep last N newest by creation time
        auto sorted = all;
        std::sort(sorted.begin(), sorted.end(),
                  [](auto& a, auto& b){ return a.created > b.created; });

        for (std::size_t i = 0; i < sorted.size() && i < policy_.keepLastN; ++i) {
            MarkLiveWithParents(sorted[i], live);
        }

        // Keep pinned, leased, newer-than-cutoff
        for (auto& s : all) {
            if (s.state == SnapshotState::Deleted) continue;
            if (s.created >= cutoff) MarkLiveWithParents(s, live);
            if (s.leaseCount > 0)    MarkLiveWithParents(s, live);
            if (s.tags.count("pin") || s.tags.count("retain") || s.tags.count("legal")) {
                MarkLiveWithParents(s, live);
            }
        }

        return live;
    }

    void MarkLiveWithParents(const SnapshotMeta& s, std::unordered_set<std::string>& live) {
        if (!live.insert(s.id).second) return;
        if (s.parentId) {
            auto p = catalog_.Get(*s.parentId);
            if (p) MarkLiveWithParents(*p, live);
        }
    }

    void TombstoneCandidates(const std::vector<SnapshotMeta>& all,
                             const std::unordered_set<std::string>& live,
                             GCMetrics& metrics) {
        auto now = Clock::now();

        for (auto& s : all) {
            if (s.state != SnapshotState::Active) continue;
            if (live.count(s.id)) continue;
            if (s.leaseCount > 0) continue;

            if (opts_.dryRun) {
                catalog_.RecordEvent({now, s.id, "DRYRUN_TOMBSTONE", "Would tombstone"});
                continue;
            }

            // Soft delete == Tombstoned. Attempting to load should fail; log signals. [1](https://eng.ms/docs/experiences-devices/oxo/office-shared/wacbohemia/fluid-framework-internal/fluid-framework/docs/partners/advanced/gc-early-adopters)
            if (catalog_.TransitionState(s.id, SnapshotState::Active, SnapshotState::Tombstoned)) {
                auto cur = catalog_.Get(s.id);
                if (cur) {
                    SnapshotMeta m = *cur;

                    // Set hardDeleteAfter NOW (+ grace) and persist it so policy updates later don’t change it. [3](https://dev.azure.com/powerbi/3a3467dc-0814-4e9d-8eec-555851655f69/_workitems/edit/1858649)
                    m.hardDeleteAfter = now + opts_.gracePeriod;
                    m.nextRetryAfter.reset();
                    m.lastError.clear();
                    catalog_.Upsert(m);

                    catalog_.RecordEvent({now, s.id, "TOMBSTONE", "Soft-deleted; hardDeleteAfter set"});
                    metrics.tombstoned++;
                }
            }
        }

        // Inactive-object signal: log if an unreferenced object becomes inactive and is accessed. [1](https://eng.ms/docs/experiences-devices/oxo/office-shared/wacbohemia/fluid-framework-internal/fluid-framework/docs/partners/advanced/gc-early-adopters)
        // Here we simply emit a periodic signal for snapshots that are old and unreferenced; actual “loaded” signal
        // would happen at access time in the serving path.
        for (auto& s : all) {
            if (s.state != SnapshotState::Active) continue;
            if (live.count(s.id)) continue;

            if (s.lastAccess.time_since_epoch().count() > 0) {
                auto inactiveAfter = s.lastAccess + opts_.inactiveTimeout;
                if (Clock::now() >= inactiveAfter) {
                    catalog_.RecordEvent({Clock::now(), s.id, "INACTIVE_ELIGIBLE",
                                          "Unreferenced long enough to be considered inactive"});
                    metrics.inactiveLoadedSignals++;
                }
            }
        }
    }

    void HardDeleteEligible(GCMetrics& metrics) {
        auto now = Clock::now();
        auto all = catalog_.ListAll();

        // Collect eligible tombstoned snapshots whose grace has expired and retry window allows.
        std::vector<SnapshotMeta> eligible;
        for (auto& s : all) {
            if (s.state != SnapshotState::Tombstoned) continue;
            if (s.leaseCount > 0) continue;
            if (!s.hardDeleteAfter) continue;
            if (now < *s.hardDeleteAfter) continue;

            if (s.nextRetryAfter && now < *s.nextRetryAfter) continue; // backoff
            eligible.push_back(s);
        }

        // Limit and batch
        if (eligible.size() > opts_.maxDeletesPerRun) eligible.resize(opts_.maxDeletesPerRun);

        for (std::size_t i = 0; i < eligible.size(); i += opts_.batchDeleteSize) {
            std::vector<std::string> batchIds;
            for (std::size_t j = i; j < eligible.size() && j < i + opts_.batchDeleteSize; ++j) {
                batchIds.push_back(eligible[j].id);
            }

            if (opts_.dryRun) {
                for (auto& id : batchIds)
                    catalog_.RecordEvent({now, id, "DRYRUN_DELETE", "Would hard-delete payload"});
                continue;
            }

            // Transition each to Deleting first (prevents double-delete by another GC instance)
            std::vector<std::string> deletingIds;
            deletingIds.reserve(batchIds.size());
            for (auto& id : batchIds) {
                if (catalog_.TransitionState(id, SnapshotState::Tombstoned, SnapshotState::Deleting))
                    deletingIds.push_back(id);
            }
            if (deletingIds.empty()) continue;

            // Batch delete payloads
            std::vector<std::string> failed;
            std::string err;
            bool ok = storage_.DeleteSnapshotPayloadBatch(deletingIds, failed, err);

            // Finalize each
            for (auto& id : deletingIds) {
                auto cur = catalog_.Get(id);
                if (!cur) continue;
                SnapshotMeta m = *cur;

                auto isFailed = std::find(failed.begin(), failed.end(), id) != failed.end();
                if (!ok && failed.empty() && !err.empty()) isFailed = true; // catastrophic batch error

                if (!isFailed) {
                    catalog_.TransitionState(id, SnapshotState::Deleting, SnapshotState::Deleted);
                    catalog_.RecordEvent({Clock::now(), id, "DELETE_OK", "Payload permanently deleted"});
                    metrics.deleted++;

                    // If you track corruption offsets, forget for this snapshot when GC deletes it. [4](https://microsoft.sharepoint.com/teams/ObjectStore/_layouts/15/Doc.aspx?sourcedoc=%7B3165AC23-2AFB-44FC-8F02-EBA2E8C3ADAF%7D&file=Handling%20Snapshot%20Failures.docx&action=default&mobileredirect=true&DefaultItemOpen=1)
                    if (corruption_) corruption_->ForgetCorruptionForSnapshot(id);
                } else {
                    metrics.deleteFailed++;
                    m.deleteFailures++;
                    m.lastError = err.empty() ? "Delete failed" : err;

                    // exponential backoff
                    auto backoff = opts_.baseRetryBackoff * (1u << std::min<std::uint32_t>(m.deleteFailures, 10));
                    m.nextRetryAfter = Clock::now() + backoff;

                    // quarantine if too many failures
                    if (m.deleteFailures >= opts_.maxDeleteFailuresBeforeQuarantine) {
                        catalog_.TransitionState(id, SnapshotState::Deleting, SnapshotState::Quarantined);
                        catalog_.RecordEvent({Clock::now(), id, "QUARANTINE",
                                              "Too many delete failures: " + m.lastError});
                        metrics.quarantined++;
                    } else {
                        // revert to Tombstoned to retry later
                        catalog_.TransitionState(id, SnapshotState::Deleting, SnapshotState::Tombstoned);
                        catalog_.RecordEvent({Clock::now(), id, "DELETE_FAIL",
                                              "Will retry after backoff; err=" + m.lastError});
                    }
                    catalog_.Upsert(m);
                }
            }
        }
    }
};
