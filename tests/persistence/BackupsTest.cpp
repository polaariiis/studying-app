// Backup naming and rotation (Phase 9, docs/DATABASE_SCHEMA.md §9).

#include <studyapp/persistence/Backups.hpp>

#include <studyapp/persistence/Migrations.hpp>
#include <studyapp/testing/ResultMacros.hpp>
#include <studyapp/testing/TempDirectory.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

namespace studyapp::persistence {
namespace {

using namespace std::chrono_literals;

core::Timestamp at(int year, unsigned month, unsigned day, int hour = 12) {
    const std::chrono::sys_days date{std::chrono::year{year} / std::chrono::month{month} /
                                     std::chrono::day{day}};
    return core::Timestamp(date) + std::chrono::hours{hour};
}

TEST(BackupsTest, FileTimestampsRoundTrip) {
    for (const core::Timestamp time : {at(2026, 9, 30, 23) + 59min + 59s + 999ms, at(1970, 1, 1, 0),
                                       at(2024, 2, 29, 7), at(1969, 12, 31, 23)}) {
        const auto parsed = parseFileTimestamp(fileTimestamp(time));
        ASSERT_TRUE(parsed.has_value()) << fileTimestamp(time);
        EXPECT_EQ(*parsed, time);
    }
    for (const char* bad : {"", "20260930T120000000", "20261330T120000000Z", "2026x930T120000000Z",
                            "20260930T250000000Z", "20260930 120000000Z"}) {
        EXPECT_FALSE(parseFileTimestamp(bad).has_value()) << bad;
    }
}

TEST(BackupsTest, RotationKeepsDailyAndWeeklySnapshots) {
    // Two backups a day for 60 days.
    std::vector<BackupFile> backups;
    for (int day = 0; day < 60; ++day) {
        for (const int hour : {9, 18}) {
            const core::Timestamp time = at(2026, 8, 1, hour) + std::chrono::days{day};
            backups.push_back({.file = "auto-" + fileTimestamp(time) + ".db", .time = time});
        }
    }
    const auto removed = backupsToRemove(backups, 7, 4);
    std::vector<BackupFile> kept;
    for (const BackupFile& backup : backups) {
        if (std::find(removed.begin(), removed.end(), backup.file) == removed.end()) {
            kept.push_back(backup);
        }
    }
    // The newest of each of the last 7 days, plus the newest of up to 4 weeks (some of
    // which are among the daily ones).
    ASSERT_GE(kept.size(), 7U);
    EXPECT_LE(kept.size(), 11U);
    EXPECT_EQ(kept.back().time, backups.back().time); // the newest is kept
    for (std::size_t i = kept.size() - 7; i < kept.size(); ++i) {
        EXPECT_EQ(std::chrono::floor<std::chrono::hours>(kept[i].time).time_since_epoch().count() %
                      24,
                  18); // the later of each day
    }
    EXPECT_LT(kept.front().time, backups.back().time - std::chrono::days{14}); // weeks back
    // Nothing to remove when within the limits.
    EXPECT_TRUE(backupsToRemove(std::span(backups).last(1), 7, 4).empty());
    EXPECT_TRUE(backupsToRemove({}, 7, 4).empty());
}

TEST(BackupsTest, ListingFindsOnlyTheLabelledSnapshots) {
    testing::TempDirectory dir;
    const WorkspaceLayout layout{dir / "ws"};
    ASSERT_OK(listBackups(layout, kAutoBackupLabel)); // no directory yet: empty
    std::filesystem::create_directories(layout.backups());
    const auto touch = [&](const std::string& name) {
        std::ofstream(layout.backups() / name) << "x";
    };
    touch("auto-" + fileTimestamp(at(2026, 9, 2)) + ".db");
    touch("auto-" + fileTimestamp(at(2026, 9, 1)) + ".db");
    touch("pre-migration-v1-" + fileTimestamp(at(2026, 9, 3)) + ".db");
    touch("auto-garbage.db");
    touch("auto-" + fileTimestamp(at(2026, 9, 4)) + ".txt");
    auto listed = listBackups(layout, kAutoBackupLabel);
    ASSERT_OK(listed);
    ASSERT_EQ(listed->size(), 2U);
    EXPECT_EQ(listed->front().time, at(2026, 9, 1)); // oldest first
    EXPECT_EQ(listed->back().time, at(2026, 9, 2));
}

} // namespace
} // namespace studyapp::persistence
