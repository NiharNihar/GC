// snapshot_gc_types.hpp (C++17)
#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

using Clock = std::chrono::system_clock;
using TimePoint = std::chrono::time_point<Clock>;

enum class SnapshotState {
    Active,
    Tombstoned,          // soft-deleted: should fail if requested, but payload may still exist [1](https://eng.ms/docs/experiences-devices/oxo/office-shared/wacbohemia/fluid-framework-internal/fluid-framework/docs/partners/advanced/gc-early-adopters)
    Deleting,            // in progress
    Deleted,             // fully deleted
    Quarantined          // repeated failures; manual investigation
};

struct SnapshotMeta {
    std::string id;
    TimePoint created{};
    std::uint64_t sizeBytes{0};

    SnapshotState state{SnapshotState::Active};
    std::optional<std::string> parentId;                 // incremental chains
    std::unordered_set<std::string> tags;                // e.g., "retain", "legal", "pin"
    std::uint32_t leaseCount{0};                         // active readers
    TimePoint lastAccess{};                              // optional: used for "Inactive" detection [1](https://eng.ms/docs/experiences-devices/oxo/office-shared/wacbohemia/fluid-framework-internal/fluid-framework/docs/partners/advanced/gc-early-adopters)

    // Extensions: soft-delete expiry is computed at tombstone-time and stored
    // so later policy changes don't retroactively change it. [3](https://dev.azure.com/powerbi/3a3467dc-0814-4e9d-8eec-555851655f69/_workitems/edit/1858649)
    std::optional<TimePoint> hardDeleteAfter;

    // Failure/retry bookkeeping
    std::uint32_t deleteFailures{0};
    std::optional<TimePoint> nextRetryAfter;
    std::string lastError;
};

struct RetentionPolicy {
    std::size_t keepLastN{10};
    std::chrono::hours maxAge{24 * 30}; // 30 days
    bool enableCheckpointing{false};
    std::chrono::hours checkpointInterval{24 * 7};
};

struct GCOptions {
    bool dryRun{false};

    // Tombstone stage enabled by default, delete stage can be disabled [1](https://eng.ms/docs/experiences-devices/oxo/office-shared/wacbohemia/fluid-framework-internal/fluid-framework/docs/partners/advanced/gc-early-adopters)
    bool enableTombstoneStage{true};
    bool enableHardDeleteStage{true};

    std::chrono::hours inactiveTimeout{24 * 7};   // default 7 days (configurable) [1](https://eng.ms/docs/experiences-devices/oxo/office-shared/wacbohemia/fluid-framework-internal/fluid-framework/docs/partners/advanced/gc-early-adopters)
    std::chrono::hours gracePeriod{24 * 7};       // how long tombstoned payload is kept

    std::size_t maxDeletesPerRun{1000};
    std::size_t batchDeleteSize{50};

    std::uint32_t maxDeleteFailuresBeforeQuarantine{5};
    std::chrono::seconds baseRetryBackoff{10};
};
