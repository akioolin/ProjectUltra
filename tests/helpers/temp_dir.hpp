#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

namespace ultra::test {

class TempDir {
public:
    explicit TempDir(const std::string& prefix) {
        std::error_code ec;
        auto base = std::filesystem::temp_directory_path(ec);
        if (ec) {
            return;
        }

        static std::atomic<unsigned long long> seq{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = base / (prefix + "_" + std::to_string(stamp) + "_" +
                        std::to_string(seq.fetch_add(1, std::memory_order_relaxed)));

        std::filesystem::remove_all(path_, ec);
        ec.clear();
        std::filesystem::create_directories(path_, ec);
        if (ec) {
            path_.clear();
        }
    }

    ~TempDir() {
        cleanup();
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    TempDir(TempDir&& other) noexcept
        : path_(std::move(other.path_)) {
        other.path_.clear();
    }

    TempDir& operator=(TempDir&& other) noexcept {
        if (this != &other) {
            cleanup();
            path_ = std::move(other.path_);
            other.path_.clear();
        }
        return *this;
    }

    bool valid() const {
        return !path_.empty();
    }

    const std::filesystem::path& path() const {
        return path_;
    }

    std::filesystem::path child(const std::filesystem::path& relative) const {
        return path_ / relative;
    }

    void cleanup() {
        if (path_.empty()) {
            return;
        }
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        path_.clear();
    }

private:
    std::filesystem::path path_;
};

}  // namespace ultra::test
