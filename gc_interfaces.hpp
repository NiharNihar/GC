// snapshot_gc_interfaces.hpp
#pragma once
#include "snapshot_gc_types.hpp"
#include <optional>

struct GCEvent {
    TimePoint when{};
    std::string snapshotId;
    std::string type;     // "TOMBSTONE", "DELETE_OK", "DELETE_FAIL", "INACTIVE_LOADED", ...
    std::string details;
};

class ISnapshotCatalog {
public:
    virtual ~ISnapshotCatalog() = default;

    virtual std::vector<SnapshotMeta> ListAll() = 0;
    virtual std::optional<SnapshotMeta> Get(const std::string& id) = 0;

    // Optimistic-state transition.
    virtual bool TransitionState(const std::string& id,
                                 SnapshotState expected,
                                 SnapshotState desired) = 0;

    // Update full record (with simple versioning if desired).
    virtual bool Upsert(const SnapshotMeta& m) = 0;

    virtual void RecordEvent(const GCEvent& e) = 0;
};

class IStorageBackend {
public:
    virtual ~IStorageBackend() = default;

    // Delete snapshot payload (may include multiple files/blobs)
    virtual bool DeleteSnapshotPayload(const std::string& snapshotId, std::string& err) = 0;

    // Optional: batch delete optimization
    virtual bool DeleteSnapshotPayloadBatch(const std::vector<std::string>& ids,
                                            std::vector<std::string>& failed,
                                            std::string& err) {
        // default fallback: call single deletes
        bool allOk = true;
        for (auto& id : ids) {
            std::string e;
            if (!DeleteSnapshotPayload(id, e)) {
                allOk = false;
                failed.push_back(id);
            }
        }
        err.clear();
        return allOk;
    }

    virtual bool Exists(const std::string& snapshotId) = 0;
};

class ICorruptionTracker {
public:
    virtual ~ICorruptionTracker() = default;

    // Remember corrupt offsets/locations across restarts. [4](https://microsoft.sharepoint.com/teams/ObjectStore/_layouts/15/Doc.aspx?sourcedoc=%7B3165AC23-2AFB-44FC-8F02-EBA2E8C3ADAF%7D&file=Handling%20Snapshot%20Failures.docx&action=default&mobileredirect=true&DefaultItemOpen=1)
    virtual void RecordCorruptOffset(const std::string& file, std::uint64_t offset) = 0;
    virtual void ForgetCorruptionForSnapshot(const std::string& snapshotId) = 0; // forget when snapshotted or GC’ed [4](https://microsoft.sharepoint.com/teams/ObjectStore/_layouts/15/Doc.aspx?sourcedoc=%7B3165AC23-2AFB-44FC-8F02-EBA2E8C3ADAF%7D&file=Handling%20Snapshot%20Failures.docx&action=default&mobileredirect=true&DefaultItemOpen=1)
};

class ILeaderElector {
public:
    virtual ~ILeaderElector() = default;
    virtual bool TryAcquire() = 0;
    virtual void Release() = 0;
};
