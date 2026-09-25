#pragma once

#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>

namespace studyapp::testing {

/// A fresh, uniquely named directory under the system temporary directory, removed with
/// everything in it when the object is destroyed. The name contains non-ASCII characters
/// so that every persistence test also exercises Unicode path handling (UTF-16 paths on
/// Windows, UTF-8 for SQLite).
class TempDirectory {
public:
    TempDirectory() {
        std::random_device device;
        std::mt19937_64 random((std::uint64_t{device()} << 32U) ^ device());
        const auto base = std::filesystem::temp_directory_path() / "studyapp-tests";
        for (;;) {
            path_ = base / (u8"ws-étüde-" + toU8(std::to_string(random())));
            std::error_code ec;
            std::filesystem::create_directories(base, ec);
            if (std::filesystem::create_directory(path_, ec)) {
                break;
            }
        }
    }
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    TempDirectory(TempDirectory&&) = delete;
    TempDirectory& operator=(TempDirectory&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::filesystem::path operator/(const std::filesystem::path& child) const {
        return path_ / child;
    }

private:
    static std::u8string toU8(const std::string& ascii) { return {ascii.begin(), ascii.end()}; }

    std::filesystem::path path_;
};

} // namespace studyapp::testing
