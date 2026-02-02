// filesystem_storage.hpp
#pragma once
#include "gc_interfaces.hpp"
#include <filesystem>

class FilesystemStorage final : public IStorageBackend {
public:
    explicit FilesystemStorage(std::filesystem::path root) : root_(std::move(root)) {}

    bool DeleteSnapshotPayload(const std::string& snapshotId, std::string& err) override {
        try {
            auto p = root_ / snapshotId;
            if (!std::filesystem::exists(p)) return true; // already gone
            std::filesystem::remove_all(p);
            err.clear();
            return true;
        } catch (const std::exception& ex) {
            err = ex.what();
            return false;
        }
    }

    bool DeleteSnapshotPayloadBatch(const std::vector<std::string>& ids,
                                    std::vector<std::string>& failed,
                                    std::string& err) override {
        // For filesystem, batch == loop; for blob storage you’d call a bulk API.
        bool ok = true;
        for (auto& id : ids) {
            std::string e;
            if (!DeleteSnapshotPayload(id, e)) { ok = false; failed.push_back(id); }
        }
        err.clear();
        return ok;
    }

    bool Exists(const std::string& snapshotId) override {
        return std::filesystem::exists(root_ / snapshotId);
    }

private:
    std::filesystem::path root_;
};
