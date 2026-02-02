// journal_catalog.hpp
#pragma once
#include "snapshot_gc_interfaces.hpp"
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>

class JournalCatalog final : public ISnapshotCatalog {
public:
    explicit JournalCatalog(std::filesystem::path journalPath)
        : journalPath_(std::move(journalPath)) {
        Replay();
    }

    std::vector<SnapshotMeta> ListAll() override {
        std::lock_guard<std::mutex> g(mu_);
        std::vector<SnapshotMeta> out;
        out.reserve(items_.size());
        for (auto& kv : items_) out.push_back(kv.second);
        return out;
    }

    std::optional<SnapshotMeta> Get(const std::string& id) override {
        std::lock_guard<std::mutex> g(mu_);
        auto it = items_.find(id);
        if (it == items_.end()) return std::nullopt;
        return it->second;
    }

    bool TransitionState(const std::string& id,
                         SnapshotState expected,
                         SnapshotState desired) override {
        std::lock_guard<std::mutex> g(mu_);
        auto it = items_.find(id);
        if (it == items_.end()) return false;
        if (it->second.state != expected) return false;
        it->second.state = desired;
        Append("STATE " + id + " " + std::to_string((int)expected) + " " + std::to_string((int)desired));
        return true;
    }

    bool Upsert(const SnapshotMeta& m) override {
        std::lock_guard<std::mutex> g(mu_);
        items_[m.id] = m;
        Append("UPSERT " + Serialize(m));
        return true;
    }

    void RecordEvent(const GCEvent& e) override {
        Append("EVENT " + e.snapshotId + " " + e.type + " " + Escape(e.details));
    }

private:
    std::filesystem::path journalPath_;
    std::mutex mu_;
    std::unordered_map<std::string, SnapshotMeta> items_;

    static std::string Escape(const std::string& s) {
        std::string o;
        o.reserve(s.size());
        for (char c : s) {
            if (c == '\n') o += "\\n";
            else if (c == '\r') o += "\\r";
            else if (c == '\\') o += "\\\\";
            else o += c;
        }
        return o;
    }

    static std::string Unescape(const std::string& s) {
        std::string o;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                char n = s[i + 1];
                if (n == 'n') { o += '\n'; ++i; }
                else if (n == 'r') { o += '\r'; ++i; }
                else if (n == '\\') { o += '\\'; ++i; }
                else o += s[i];
            } else o += s[i];
        }
        return o;
    }

    static std::string Serialize(const SnapshotMeta& m) {
        // Simple pipe-separated encoding for demo purposes
        // id|created_epoch_ms|size|state|lease|lastAccess_epoch_ms|hardDelete_epoch_ms_or_-1|failures|nextRetry_epoch_ms_or_-1|lastError
        auto toMs = [](TimePoint t) -> long long {
            return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
        };
        auto hd = m.hardDeleteAfter ? toMs(*m.hardDeleteAfter) : -1LL;
        auto nr = m.nextRetryAfter ? toMs(*m.nextRetryAfter) : -1LL;
        return m.id + "|" + std::to_string(toMs(m.created)) + "|" + std::to_string(m.sizeBytes) + "|" +
               std::to_string((int)m.state) + "|" + std::to_string(m.leaseCount) + "|" +
               std::to_string(toMs(m.lastAccess)) + "|" + std::to_string(hd) + "|" +
               std::to_string(m.deleteFailures) + "|" + std::to_string(nr) + "|" + Escape(m.lastError);
    }

    static SnapshotMeta Deserialize(const std::string& line) {
        auto fromMs = [](long long ms) -> TimePoint {
            return TimePoint(std::chrono::milliseconds(ms));
        };
        SnapshotMeta m;
        std::vector<std::string> parts;
        std::string cur;
        for (char c : line) {
            if (c == '|') { parts.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        parts.push_back(cur);

        m.id = parts.at(0);
        m.created = fromMs(std::stoll(parts.at(1)));
        m.sizeBytes = (std::uint64_t)std::stoull(parts.at(2));
        m.state = (SnapshotState)std::stoi(parts.at(3));
        m.leaseCount = (std::uint32_t)std::stoul(parts.at(4));
        m.lastAccess = fromMs(std::stoll(parts.at(5)));
        long long hd = std::stoll(parts.at(6));
        if (hd >= 0) m.hardDeleteAfter = fromMs(hd);
        m.deleteFailures = (std::uint32_t)std::stoul(parts.at(7));
        long long nr = std::stoll(parts.at(8));
        if (nr >= 0) m.nextRetryAfter = fromMs(nr);
        m.lastError = Unescape(parts.at(9));
        return m;
    }

    void Append(const std::string& rec) {
        std::ofstream f(journalPath_, std::ios::app);
        f << rec << "\n";
    }

    void Replay() {
        if (!std::filesystem::exists(journalPath_)) return;
        std::ifstream f(journalPath_);
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind("UPSERT ", 0) == 0) {
                auto payload = line.substr(7);
                SnapshotMeta m = Deserialize(payload);
                items_[m.id] = m;
            } else if (line.rfind("STATE ", 0) == 0) {
                // STATE id expected desired
                // For simplicity, apply desired unconditionally if record exists.
                // (Production: enforce expected to preserve CAS semantics.)
                std::istringstream iss(line);
                std::string tag, id; int expected, desired;
                iss >> tag >> id >> expected >> desired;
                auto it = items_.find(id);
                if (it != items_.end()) it->second.state = (SnapshotState)desired;
            } else if (line.rfind("EVENT ", 0) == 0) {
                // Optional: keep events separately; here we ignore replaying events
            }
        }
    }
};
