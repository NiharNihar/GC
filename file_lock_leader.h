// file_lock_leader.hpp
#pragma once
#include "snapshot_gc_interfaces.hpp"
#include <filesystem>
#include <fstream>

class FileLockLeaderElector final : public ILeaderElector {
public:
    explicit FileLockLeaderElector(std::filesystem::path lockPath)
        : lockPath_(std::move(lockPath)) {}

    bool TryAcquire() override {
        try {
            // CREATE_NEW semantics: fail if exists
            if (std::filesystem::exists(lockPath_)) return false;
            std::ofstream f(lockPath_);
            f << "gc-leader\n";
            acquired_ = true;
            return true;
        } catch (...) {
            return false;
        }
    }

    void Release() override {
        if (!acquired_) return;
        std::error_code ec;
        std::filesystem::remove(lockPath_, ec);
        acquired_ = false;
    }

    ~FileLockLeaderElector() override { Release(); }

private:
    std::filesystem::path lockPath_;
    bool acquired_{false};
};
