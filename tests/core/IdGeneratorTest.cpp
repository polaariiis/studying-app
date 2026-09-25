#include <studyapp/core/IdGenerator.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <unordered_set>
#include <vector>

namespace studyapp::core {
namespace {

using std::chrono::milliseconds;

class ManualClock final : public Clock {
public:
    explicit ManualClock(std::int64_t unixMillis) : now_(milliseconds(unixMillis)) {}
    [[nodiscard]] Timestamp now() const override { return now_; }
    void set(std::int64_t unixMillis) { now_ = Timestamp(milliseconds(unixMillis)); }
    void advance(std::int64_t millis) { now_ += milliseconds(millis); }

private:
    Timestamp now_;
};

constexpr std::int64_t kSomeTime = 1'790'000'000'000; // 2026-09-21T...Z

TEST(UuidV7GeneratorTest, ProducesVersion7WithRfcVariant) {
    const ManualClock clock(kSomeTime);
    UuidV7Generator generator(clock, 42);
    for (int i = 0; i < 100; ++i) {
        const Uuid id = generator.next();
        EXPECT_EQ(id.version(), 7);
        EXPECT_TRUE(id.hasRfcVariant());
        EXPECT_FALSE(id.isNil());
    }
}

TEST(UuidV7GeneratorTest, EmbedsClockTimestamp) {
    ManualClock clock(kSomeTime);
    UuidV7Generator generator(clock, 1);
    EXPECT_EQ(generator.next().unixMillis(), static_cast<std::uint64_t>(kSomeTime));
    clock.advance(1500);
    EXPECT_EQ(generator.next().unixMillis(), static_cast<std::uint64_t>(kSomeTime + 1500));
}

TEST(UuidV7GeneratorTest, StrictlyIncreasingWithinOneMillisecond) {
    const ManualClock clock(kSomeTime); // clock never advances
    UuidV7Generator generator(clock, 7);
    Uuid previous = generator.next();
    // More ids than the 12-bit counter can hold forces the generator to advance time.
    for (int i = 0; i < 10'000; ++i) {
        const Uuid current = generator.next();
        ASSERT_LT(previous, current) << "at iteration " << i;
        previous = current;
    }
}

TEST(UuidV7GeneratorTest, MonotonicWhenClockGoesBackwards) {
    ManualClock clock(kSomeTime);
    UuidV7Generator generator(clock, 3);
    const Uuid before = generator.next();
    clock.set(kSomeTime - 60'000); // wall clock adjusted back by a minute
    const Uuid after = generator.next();
    EXPECT_LT(before, after);
    EXPECT_GE(after.unixMillis(), before.unixMillis());
}

TEST(UuidV7GeneratorTest, SeededGeneratorsAreDeterministic) {
    const ManualClock clock(kSomeTime);
    UuidV7Generator a(clock, 1234);
    UuidV7Generator b(clock, 1234);
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(a.next(), b.next());
    }
}

TEST(UuidV7GeneratorTest, RandomlySeededGeneratorsProduceUniqueIds) {
    const SystemClock clock;
    UuidV7Generator a(clock);
    UuidV7Generator b(clock);
    std::unordered_set<Uuid> seen;
    for (int i = 0; i < 5'000; ++i) {
        ASSERT_TRUE(seen.insert(a.next()).second);
        ASSERT_TRUE(seen.insert(b.next()).second);
    }
}

TEST(UuidV7GeneratorTest, IdsSurviveStringRoundTrip) {
    const SystemClock clock;
    UuidV7Generator generator(clock);
    for (int i = 0; i < 100; ++i) {
        const Uuid id = generator.next();
        const auto parsed = Uuid::parse(id.toString());
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, id);
    }
}

TEST(SystemClockTest, ReturnsCurrentTime) {
    const SystemClock clock;
    const auto before = std::chrono::floor<milliseconds>(std::chrono::system_clock::now());
    const Timestamp now = clock.now();
    const auto after = std::chrono::floor<milliseconds>(std::chrono::system_clock::now());
    EXPECT_LE(before, now);
    EXPECT_LE(now, after);
}

} // namespace
} // namespace studyapp::core
