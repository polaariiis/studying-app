#include <studyapp/persistence/Backups.hpp>

#include "Utf8Path.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <set>
#include <string>

namespace studyapp::persistence {

namespace {

using core::ErrorCode;
using core::makeError;

constexpr std::int64_t kMillisPerDay = 86'400'000;

/// Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's days_from_civil).
std::int64_t daysFromCivil(std::int64_t year, unsigned month, unsigned day) noexcept {
    year -= month <= 2 ? 1 : 0;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const auto yearOfEra = static_cast<unsigned>(year - era * 400);
    const unsigned dayOfYear = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + static_cast<std::int64_t>(dayOfEra) - 719468;
}

bool number(std::string_view text, std::int64_t& out) {
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc{} && ptr == text.data() + text.size();
}

std::int64_t millisOf(core::Timestamp time) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}

std::int64_t floorDiv(std::int64_t a, std::int64_t b) {
    return a / b - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0);
}

} // namespace

std::optional<core::Timestamp> parseFileTimestamp(std::string_view text) {
    // YYYYMMDD T HHMMSS mmm Z
    if (text.size() != 19 || text[8] != 'T' || text[18] != 'Z') {
        return std::nullopt;
    }
    std::int64_t year = 0;
    std::int64_t month = 0;
    std::int64_t day = 0;
    std::int64_t hour = 0;
    std::int64_t minute = 0;
    std::int64_t second = 0;
    std::int64_t milli = 0;
    if (!number(text.substr(0, 4), year) || !number(text.substr(4, 2), month) ||
        !number(text.substr(6, 2), day) || !number(text.substr(9, 2), hour) ||
        !number(text.substr(11, 2), minute) || !number(text.substr(13, 2), second) ||
        !number(text.substr(15, 3), milli)) {
        return std::nullopt;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59) {
        return std::nullopt;
    }
    const std::int64_t days =
        daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    const std::int64_t millis =
        days * kMillisPerDay + ((hour * 60 + minute) * 60 + second) * 1000 + milli;
    return core::Timestamp(std::chrono::milliseconds(millis));
}

core::Result<std::vector<BackupFile>> listBackups(const WorkspaceLayout& layout,
                                                  std::string_view label) {
    std::vector<BackupFile> backups;
    std::error_code ec;
    if (!std::filesystem::is_directory(layout.backups(), ec)) {
        return backups;
    }
    const std::string prefix = std::string(label) + "-";
    // increment(ec), not a range-for: operator++ throws on I/O errors.
    for (auto it = std::filesystem::directory_iterator(layout.backups(), ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        const auto& entry = *it;
        std::error_code typeError;
        if (!entry.is_regular_file(typeError) || entry.path().extension() != ".db") {
            continue;
        }
        const std::string stem = detail::utf8(entry.path().stem());
        if (!stem.starts_with(prefix)) {
            continue;
        }
        if (const auto time = parseFileTimestamp(std::string_view(stem).substr(prefix.size()))) {
            backups.push_back({.file = entry.path(), .time = *time});
        }
    }
    if (ec) {
        return makeError(ErrorCode::IoError,
                         "cannot list '" + detail::utf8(layout.backups()) + "': " + ec.message());
    }
    std::sort(backups.begin(), backups.end(),
              [](const BackupFile& a, const BackupFile& b) { return a.time < b.time; });
    return backups;
}

std::vector<std::filesystem::path> backupsToRemove(std::span<const BackupFile> backups,
                                                   int keepDays, int keepWeeks) {
    std::vector<const BackupFile*> newestFirst;
    newestFirst.reserve(backups.size());
    for (const BackupFile& backup : backups) {
        newestFirst.push_back(&backup);
    }
    std::sort(newestFirst.begin(), newestFirst.end(),
              [](const BackupFile* a, const BackupFile* b) { return a->time > b->time; });
    std::set<const BackupFile*> keep;
    std::set<std::int64_t> days;
    std::set<std::int64_t> weeks;
    for (const BackupFile* backup : newestFirst) {
        const std::int64_t day = floorDiv(millisOf(backup->time), kMillisPerDay);
        const std::int64_t week = floorDiv(day + 3, 7); // weeks start on Monday (1970-01-01: Thu)
        if (keep.empty()) {
            keep.insert(backup); // the newest, always
        }
        if (!days.contains(day) && std::ssize(days) < keepDays) {
            days.insert(day);
            keep.insert(backup);
        }
        if (!weeks.contains(week) && std::ssize(weeks) < keepWeeks) {
            weeks.insert(week);
            keep.insert(backup);
        }
    }
    std::vector<std::filesystem::path> remove;
    for (const BackupFile* backup : newestFirst) {
        if (!keep.contains(backup)) {
            remove.push_back(backup->file);
        }
    }
    return remove;
}

} // namespace studyapp::persistence
