// platform::QtTimeZone: the Qt time-zone database behind study::TimeZone, with the real
// daylight-saving rules of named zones (the planner's local days and times).

#include <studyapp/platform/QtTimeZone.hpp>

#include <QTest>

#include <chrono>

using namespace studyapp;
using namespace std::chrono_literals;

namespace {

core::Timestamp utc(int y, unsigned m, unsigned d, int hour, int minute = 0) {
    const study::CalendarDate date{std::chrono::year{y}, std::chrono::month{m},
                                   std::chrono::day{d}};
    return core::Timestamp(std::chrono::sys_days(date)) + std::chrono::hours{hour} +
           std::chrono::minutes{minute};
}

} // namespace

class QtTimeZoneTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void namedZonesFollowDaylightSavingRules() {
        const platform::QtTimeZone berlin("Europe/Berlin");
        const platform::QtTimeZone pacific("America/Los_Angeles");
        if (!berlin.isValid() || !pacific.isValid()) {
            QSKIP("no IANA time-zone data on this system");
        }
        QCOMPARE(berlin.utcOffset(utc(2026, 1, 15, 12)).count(), 60);
        QCOMPARE(berlin.utcOffset(utc(2026, 7, 15, 12)).count(), 120);
        // The change on 29 March 2026 happens at 01:00 UTC.
        QCOMPARE(berlin.utcOffset(utc(2026, 3, 29, 0, 59)).count(), 60);
        QCOMPARE(berlin.utcOffset(utc(2026, 3, 29, 1)).count(), 120);
        QCOMPARE(pacific.utcOffset(utc(2026, 1, 15, 12)).count(), -480);
        QCOMPARE(pacific.utcOffset(utc(2026, 7, 15, 12)).count(), -420);
        // Local dates and midnights through the study functions.
        QVERIFY(
            study::localDate(utc(2026, 7, 1, 22, 30), berlin) ==
            (study::CalendarDate{std::chrono::year{2026}, std::chrono::July, std::chrono::day{2}}));
        const study::CalendarDate springDay{std::chrono::year{2026}, std::chrono::March,
                                            std::chrono::day{29}};
        const auto next =
            study::CalendarDate(std::chrono::sys_days(springDay) + std::chrono::days{1});
        QCOMPARE((study::startOfDay(next, berlin) - study::startOfDay(springDay, berlin)) /
                     std::chrono::hours{1},
                 23);
    }

    void unknownNamesFallBackToUtcAndTheSystemZoneWorks() {
        const platform::QtTimeZone unknown("Not/AZone");
        QVERIFY(!unknown.isValid());
        QCOMPARE(unknown.utcOffset(utc(2026, 1, 1, 0)).count(), 0);
        const platform::QtTimeZone system;
        QVERIFY(system.isValid());
        const auto offset = system.utcOffset(utc(2026, 1, 1, 0));
        QVERIFY(offset >= -14h && offset <= 14h);
    }
};

QTEST_GUILESS_MAIN(QtTimeZoneTest)
#include "QtTimeZoneTest.moc"
