#include <studyapp/core/Color.hpp>

#include <gtest/gtest.h>

namespace studyapp::core {
namespace {

TEST(ColorTest, DefaultIsOpaqueBlack) {
    constexpr Color c;
    EXPECT_EQ(c, Color::black());
    EXPECT_TRUE(c.isOpaque());
}

TEST(ColorTest, Argb32RoundTrip) {
    constexpr Color c = Color::fromRgba(0x12, 0x34, 0x56, 0x78);
    static_assert(c.toArgb32() == 0x78123456U);
    static_assert(Color::fromArgb32(0x78123456U) == c);
    EXPECT_EQ(Color::fromArgb32(0xFFFFFFFFU), Color::white());
    EXPECT_EQ(Color::fromArgb32(0x00000000U), Color::transparent());
}

TEST(ColorTest, ParsesSixDigitHex) {
    const auto c = Color::fromHex("#1a2B3c");
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(*c, Color::fromRgba(0x1A, 0x2B, 0x3C));
}

TEST(ColorTest, ParsesEightDigitHexWithAlpha) {
    const auto c = Color::fromHex("#FF000080");
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(*c, Color::fromRgba(0xFF, 0x00, 0x00, 0x80));
}

TEST(ColorTest, ParsesShortHex) {
    const auto c = Color::fromHex("#f80");
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(*c, Color::fromRgba(0xFF, 0x88, 0x00));
}

TEST(ColorTest, RejectsMalformedHex) {
    for (const char* text : {"", "#", "123456", "#12345", "#1234567", "#GG0000", "#12 456"}) {
        const auto c = Color::fromHex(text);
        ASSERT_FALSE(c.has_value()) << "accepted: " << text;
        EXPECT_EQ(c.error().code, ErrorCode::ParseError);
    }
}

TEST(ColorTest, HexRoundTrip) {
    EXPECT_EQ(Color::fromRgba(0x0A, 0xBC, 0xDE).toHex(), "#0ABCDE");
    EXPECT_EQ(Color::fromRgba(0x0A, 0xBC, 0xDE, 0x7F).toHex(), "#0ABCDE7F");

    for (const Color c :
         {Color::black(), Color::white(), Color::transparent(), Color::fromRgba(1, 2, 3, 4)}) {
        const auto parsed = Color::fromHex(c.toHex());
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, c);
    }
}

TEST(ColorTest, WithAlpha) {
    constexpr Color c = Color::white().withAlpha(0x40);
    EXPECT_EQ(c, Color::fromRgba(0xFF, 0xFF, 0xFF, 0x40));
    EXPECT_FALSE(c.isOpaque());
}

} // namespace
} // namespace studyapp::core
