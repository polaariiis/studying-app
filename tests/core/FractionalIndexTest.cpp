#include <studyapp/core/FractionalIndex.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <random>
#include <utility>
#include <vector>

namespace studyapp::core {
namespace {

FractionalIndex key(std::string_view text) {
    auto parsed = FractionalIndex::parse(text);
    EXPECT_TRUE(parsed.has_value()) << text;
    return parsed ? *parsed : FractionalIndex::first();
}

void expectValid(const FractionalIndex& k) {
    EXPECT_TRUE(FractionalIndex::parse(k.value()).has_value()) << "invalid key " << k.value();
}

void expectStrictlyIncreasing(const std::vector<FractionalIndex>& keys) {
    for (std::size_t i = 1; i < keys.size(); ++i) {
        ASSERT_LT(keys[i - 1], keys[i])
            << "at " << i << ": " << keys[i - 1].value() << " vs " << keys[i].value();
    }
}

TEST(FractionalIndexTest, FirstIsIntegerZero) {
    EXPECT_EQ(FractionalIndex::first().value(), "a0");
}

TEST(FractionalIndexTest, AppendAndPrependChangeIntegerPart) {
    EXPECT_EQ(FractionalIndex::after(FractionalIndex::first()).value(), "a1");
    EXPECT_EQ(FractionalIndex::before(FractionalIndex::first()).value(), "Zz");
    EXPECT_EQ(FractionalIndex::after(key("az")).value(), "b00");
    EXPECT_EQ(FractionalIndex::before(key("b00")).value(), "az");
    EXPECT_EQ(FractionalIndex::before(key("a1V")).value(), "a1"); // strips the fraction
}

TEST(FractionalIndexTest, ParseValidatesKeys) {
    for (const char* good : {"a0", "a1Z", "Zz", "b00", "a0V"}) {
        EXPECT_TRUE(FractionalIndex::parse(good).has_value()) << good;
    }
    for (const char* bad :
         {"", "a", "b0", "0", "a0V0", "a-", "a ", "a0\xC3\xA9", "A00000000000000000000000000"}) {
        const auto parsed = FractionalIndex::parse(bad);
        ASSERT_FALSE(parsed.has_value()) << "accepted: " << bad;
        EXPECT_EQ(parsed.error().code, ErrorCode::ParseError);
    }
}

TEST(FractionalIndexTest, OrderingIsBytewise) {
    EXPECT_LT(key("a1"), key("a2"));
    EXPECT_LT(key("a1"), key("a1V"));
    EXPECT_LT(key("Zz"), key("a0"));
    EXPECT_LT(key("az"), key("b00"));
    EXPECT_EQ(key("a1V"), key("a1V"));
}

TEST(FractionalIndexTest, BetweenProducesKeyStrictlyInside) {
    const std::pair<const char*, const char*> cases[] = {
        {"a0", "a1"},  {"a0", "a0V"}, {"a0V", "a0W"}, {"Zz", "a0"},
        {"a0", "b00"}, {"az", "b00"}, {"a0z", "a1"},  {"a0zz", "a0zzz"},
    };
    for (const auto& [lo, hi] : cases) {
        const auto mid = FractionalIndex::between(key(lo), key(hi));
        ASSERT_TRUE(mid.has_value());
        EXPECT_LT(key(lo), *mid) << lo << " < " << mid->value();
        EXPECT_LT(*mid, key(hi)) << mid->value() << " < " << hi;
        expectValid(*mid);
    }
}

TEST(FractionalIndexTest, OpenBounds) {
    const FractionalIndex k = key("a0V");
    const auto afterK = FractionalIndex::between(k, std::nullopt);
    const auto beforeK = FractionalIndex::between(std::nullopt, k);
    ASSERT_TRUE(afterK && beforeK);
    EXPECT_LT(k, *afterK);
    EXPECT_LT(*beforeK, k);
    EXPECT_EQ(*afterK, FractionalIndex::after(k));
    EXPECT_EQ(*beforeK, FractionalIndex::before(k));
    EXPECT_EQ(FractionalIndex::between(std::nullopt, std::nullopt)->value(), "a0");
}

TEST(FractionalIndexTest, RejectsOutOfOrderBounds) {
    for (const auto& [lo, hi] : {std::pair{"a2", "a1"}, std::pair{"a1V", "a1V"}}) {
        const auto result = FractionalIndex::between(key(lo), key(hi));
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
    }
}

TEST(FractionalIndexTest, IsDeterministic) {
    EXPECT_EQ(FractionalIndex::between(key("a1"), key("a2")),
              FractionalIndex::between(key("a1"), key("a2")));
    EXPECT_EQ(FractionalIndex::after(key("a9")), FractionalIndex::after(key("a9")));
}

TEST(FractionalIndexTest, RepeatedAppendAndPrependStayOrderedAndShort) {
    std::vector<FractionalIndex> keys{FractionalIndex::first()};
    for (int i = 0; i < 5000; ++i) {
        keys.push_back(FractionalIndex::after(keys.back()));
        keys.insert(keys.begin(), FractionalIndex::before(keys.front()));
    }
    expectStrictlyIncreasing(keys);
    for (const auto& k : keys) {
        expectValid(k);
        EXPECT_LE(k.value().size(), 4U) << "key grew unexpectedly: " << k.value();
    }
}

TEST(FractionalIndexTest, RepeatedInsertionAtSamePosition) {
    // Always inserting directly after the first key is the worst case for key growth.
    std::vector<FractionalIndex> keys{key("a0"), key("a1")};
    for (int i = 0; i < 2000; ++i) {
        auto mid = FractionalIndex::between(keys[0], keys[1]);
        ASSERT_TRUE(mid.has_value());
        keys.insert(keys.begin() + 1, *mid);
    }
    expectStrictlyIncreasing(keys);
}

TEST(FractionalIndexTest, RandomInsertionsKeepTotalOrder) {
    std::mt19937 random(12345); // fixed seed: deterministic
    std::vector<FractionalIndex> keys{FractionalIndex::first()};
    for (int i = 0; i < 3000; ++i) {
        const auto slot = std::uniform_int_distribution<std::size_t>(0, keys.size())(random);
        const std::optional<FractionalIndex> lower =
            slot == 0 ? std::nullopt : std::optional(keys[slot - 1]);
        const std::optional<FractionalIndex> upper =
            slot == keys.size() ? std::nullopt : std::optional(keys[slot]);
        auto k = FractionalIndex::between(lower, upper);
        ASSERT_TRUE(k.has_value());
        keys.insert(keys.begin() + static_cast<std::ptrdiff_t>(slot), *k);
    }
    expectStrictlyIncreasing(keys);
    for (const auto& k : keys) {
        expectValid(k);
    }
}

} // namespace
} // namespace studyapp::core
