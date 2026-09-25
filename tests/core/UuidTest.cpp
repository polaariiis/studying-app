#include <studyapp/core/Uuid.hpp>

#include <gtest/gtest.h>

#include <unordered_set>

namespace studyapp::core {
namespace {

constexpr Uuid::Bytes kSampleBytes{0x01, 0x8f, 0x3a, 0x4b, 0x5c, 0x6d, 0x7e, 0x8f,
                                   0x90, 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x07};

TEST(UuidTest, DefaultIsNil) {
    constexpr Uuid uuid;
    EXPECT_TRUE(uuid.isNil());
    EXPECT_EQ(uuid, Uuid::nil());
    EXPECT_EQ(uuid.version(), 0);
    EXPECT_EQ(uuid.toString(), "00000000-0000-0000-0000-000000000000");
}

TEST(UuidTest, ToStringIsCanonicalLowerCase) {
    const Uuid uuid{kSampleBytes};
    EXPECT_EQ(uuid.toString(), "018f3a4b-5c6d-7e8f-90a1-b2c3d4e5f607");
    EXPECT_EQ(uuid.version(), 7);
    EXPECT_TRUE(uuid.hasRfcVariant());
}

TEST(UuidTest, ParseRoundTrip) {
    const Uuid original{kSampleBytes};
    const auto parsed = Uuid::parse(original.toString());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, original);
    EXPECT_EQ(parsed->bytes(), kSampleBytes);
}

TEST(UuidTest, ParseAcceptsUpperCase) {
    const auto parsed = Uuid::parse("018F3A4B-5C6D-7E8F-90A1-B2C3D4E5F607");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, Uuid{kSampleBytes});
}

TEST(UuidTest, ParseRejectsMalformedInput) {
    for (const char* text : {
             "",
             "018f3a4b5c6d7e8f90a1b2c3d4e5f607",      // no hyphens
             "018f3a4b-5c6d-7e8f-90a1-b2c3d4e5f60",   // too short
             "018f3a4b-5c6d-7e8f-90a1-b2c3d4e5f6077", // too long
             "018f3a4b-5c6d-7e8f-90a1-b2c3d4e5f60g",  // non-hex
             "018f3a4-b5c6d-7e8f-90a1-b2c3d4e5f607",  // misplaced hyphen
             "{18f3a4b-5c6d-7e8f-90a1-b2c3d4e5f607}", // braces
         }) {
        const auto parsed = Uuid::parse(text);
        ASSERT_FALSE(parsed.has_value()) << "accepted: " << text;
        EXPECT_EQ(parsed.error().code, ErrorCode::ParseError);
    }
}

TEST(UuidTest, UnixMillisOnlyForVersion7) {
    const Uuid v7{kSampleBytes};
    ASSERT_TRUE(v7.unixMillis().has_value());
    EXPECT_EQ(*v7.unixMillis(), 0x018f3a4b5c6dULL);

    Uuid::Bytes v4Bytes = kSampleBytes;
    v4Bytes[6] = 0x4e;
    EXPECT_FALSE(Uuid{v4Bytes}.unixMillis().has_value());
}

TEST(UuidTest, OrderingIsBytewise) {
    Uuid::Bytes smaller{};
    Uuid::Bytes larger{};
    larger[0] = 1;
    smaller[15] = 0xFF;
    EXPECT_LT(Uuid{smaller}, Uuid{larger});
    EXPECT_GT(Uuid{larger}, Uuid{smaller});
}

TEST(UuidTest, HashDistinguishesValues) {
    std::unordered_set<Uuid> set;
    Uuid::Bytes bytes{};
    for (std::uint8_t i = 0; i < 100; ++i) {
        bytes[15] = i;
        set.insert(Uuid{bytes});
    }
    EXPECT_EQ(set.size(), 100U);
    EXPECT_TRUE(set.contains(Uuid{bytes}));
}

} // namespace
} // namespace studyapp::core
