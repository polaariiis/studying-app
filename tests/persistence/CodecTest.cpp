#include <studyapp/persistence/Sha256.hpp>
#include <studyapp/persistence/StrokeCodec.hpp>
#include <studyapp/testing/ResultMacros.hpp>

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

namespace studyapp::persistence {
namespace {

using document::StrokePoint;

std::span<const std::uint8_t> bytesOf(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

// ---------------------------------------------------------------------------- stroke codec

TEST(StrokeCodecTest, HeaderLayoutIsLittleEndianV1) {
    const std::vector<StrokePoint> points{{1.0F, -2.0F, 0.5F}};
    const auto blob = encodeStrokePoints(points);
    ASSERT_EQ(blob.size(), 6U + 12U);
    EXPECT_EQ(blob[0], 1); // version
    EXPECT_EQ(blob[1], 3); // channels
    EXPECT_EQ(blob[2], 1); // count, little-endian
    EXPECT_EQ(blob[3], 0);
    EXPECT_EQ(blob[4], 0);
    EXPECT_EQ(blob[5], 0);
    // x = 1.0f = 0x3F800000, little-endian
    EXPECT_EQ(blob[6], 0x00);
    EXPECT_EQ(blob[7], 0x00);
    EXPECT_EQ(blob[8], 0x80);
    EXPECT_EQ(blob[9], 0x3F);
}

TEST(StrokeCodecTest, RoundTripIsBitExact) {
    const std::vector<StrokePoint> points{
        {0.0F, -0.0F, 0.0F},
        {0.1F, 1.0F / 3.0F, 1.0F},
        {std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::max(), 0.25F},
        {-123456.789F, 1e-30F, 0.999999F},
    };
    auto decoded = decodeStrokePoints(encodeStrokePoints(points));
    ASSERT_OK(decoded);
    ASSERT_EQ(decoded->size(), points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        EXPECT_EQ(std::bit_cast<std::uint32_t>((*decoded)[i].x),
                  std::bit_cast<std::uint32_t>(points[i].x));
        EXPECT_EQ(std::bit_cast<std::uint32_t>((*decoded)[i].y),
                  std::bit_cast<std::uint32_t>(points[i].y));
        EXPECT_EQ(std::bit_cast<std::uint32_t>((*decoded)[i].pressure),
                  std::bit_cast<std::uint32_t>(points[i].pressure));
    }
}

TEST(StrokeCodecTest, RoundTripOfManyPoints) {
    std::vector<StrokePoint> points;
    for (int i = 0; i < 10'000; ++i) {
        points.push_back({static_cast<float>(i) * 0.5F, static_cast<float>(-i), 0.5F});
    }
    auto decoded = decodeStrokePoints(encodeStrokePoints(points));
    ASSERT_OK(decoded);
    EXPECT_EQ(*decoded, points);
}

TEST(StrokeCodecTest, RejectsMalformedBlobs) {
    const auto valid = encodeStrokePoints(std::vector<StrokePoint>{{1, 2, 1}, {3, 4, 1}});

    EXPECT_FALSE(decodeStrokePoints({}).has_value());                        // empty
    EXPECT_FALSE(decodeStrokePoints(std::span(valid).first(5)).has_value()); // header cut
    EXPECT_FALSE(decodeStrokePoints(std::span(valid).first(valid.size() - 1)).has_value());
    auto extra = valid;
    extra.push_back(0);
    EXPECT_FALSE(decodeStrokePoints(extra).has_value()); // trailing byte

    auto version = valid;
    version[0] = 2;
    const auto unknownVersion = decodeStrokePoints(version);
    ASSERT_FALSE(unknownVersion.has_value());
    EXPECT_EQ(unknownVersion.error().code, core::ErrorCode::ParseError);

    auto channels = valid;
    channels[1] = 2;
    EXPECT_FALSE(decodeStrokePoints(channels).has_value());

    auto count = valid;
    count[2] = 3; // claims three points, holds two
    EXPECT_FALSE(decodeStrokePoints(count).has_value());

    auto huge = valid;
    huge[5] = 0xFF; // claims ~4 billion points; must not allocate or overrun
    EXPECT_FALSE(decodeStrokePoints(huge).has_value());

    EXPECT_FALSE(decodeStrokePoints(encodeStrokePoints({})).has_value()); // zero points
}

// ---------------------------------------------------------------------------- SHA-256

TEST(Sha256Test, KnownVectors) {
    // FIPS 180-4 / NIST examples.
    EXPECT_EQ(toHex(Sha256::of({})),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(toHex(Sha256::of(bytesOf("abc"))),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(
        toHex(Sha256::of(bytesOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    const std::vector<std::uint8_t> million(1'000'000, 'a');
    EXPECT_EQ(toHex(Sha256::of(million)),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256Test, IncrementalUpdatesMatchOneShot) {
    std::vector<std::uint8_t> data(1000);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::uint8_t>(i * 7 + 3);
    }
    const Sha256Digest expected = Sha256::of(data);
    for (const std::size_t chunk : {1U, 3U, 55U, 56U, 63U, 64U, 65U, 999U}) {
        Sha256 hash;
        for (std::size_t offset = 0; offset < data.size(); offset += chunk) {
            hash.update(std::span(data).subspan(offset, std::min(chunk, data.size() - offset)));
        }
        EXPECT_EQ(hash.finish(), expected) << "chunk size " << chunk;
    }
}

TEST(Sha256Test, HexRoundTrip) {
    const Sha256Digest digest = Sha256::of(bytesOf("abc"));
    auto parsed = parseSha256(toHex(digest));
    ASSERT_OK(parsed);
    EXPECT_EQ(*parsed, digest);
    EXPECT_OK(parseSha256("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"));
    EXPECT_FALSE(parseSha256("abc").has_value());
    EXPECT_FALSE(parseSha256(std::string(64, 'g')).has_value());
}

} // namespace
} // namespace studyapp::persistence
