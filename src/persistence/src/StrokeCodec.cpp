#include <studyapp/persistence/StrokeCodec.hpp>

#include <bit>
#include <cstring>
#include <limits>
#include <string>

namespace studyapp::persistence {

using core::ErrorCode;
using core::makeError;
using core::Result;

namespace {

constexpr std::size_t kHeaderSize = 6;
constexpr std::uint8_t kChannels = 3;
constexpr std::size_t kPointSize = kChannels * sizeof(float);

static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
              "the stroke codec requires IEEE 754 binary32 floats");

void putU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

std::uint32_t getU32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(bytes[offset + i]) << (8U * i);
    }
    return value;
}

void putFloat(std::vector<std::uint8_t>& out, float value) {
    putU32(out, std::bit_cast<std::uint32_t>(value));
}

float getFloat(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return std::bit_cast<float>(getU32(bytes, offset));
}

} // namespace

std::vector<std::uint8_t> encodeStrokePoints(std::span<const document::StrokePoint> points) {
    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + points.size() * kPointSize);
    out.push_back(kStrokePointFormatV1);
    out.push_back(kChannels);
    putU32(out, static_cast<std::uint32_t>(points.size()));
    for (const document::StrokePoint& p : points) {
        putFloat(out, p.x);
        putFloat(out, p.y);
        putFloat(out, p.pressure);
    }
    return out;
}

Result<std::vector<document::StrokePoint>> decodeStrokePoints(std::span<const std::uint8_t> blob) {
    if (blob.size() < kHeaderSize) {
        return makeError(ErrorCode::ParseError, "stroke point blob is truncated (no header)");
    }
    if (blob[0] != kStrokePointFormatV1) {
        return makeError(ErrorCode::ParseError,
                         "unsupported stroke point format " + std::to_string(blob[0]));
    }
    if (blob[1] != kChannels) {
        return makeError(ErrorCode::ParseError, "stroke point blob has " + std::to_string(blob[1]) +
                                                    " channels; format 1 requires 3");
    }
    const std::uint32_t count = getU32(blob, 2);
    if (count == 0) {
        return makeError(ErrorCode::ParseError, "stroke point blob contains no points");
    }
    if ((blob.size() - kHeaderSize) / kPointSize != count ||
        (blob.size() - kHeaderSize) % kPointSize != 0) {
        return makeError(ErrorCode::ParseError,
                         "stroke point blob size " + std::to_string(blob.size()) +
                             " does not match its point count " + std::to_string(count));
    }
    std::vector<document::StrokePoint> points;
    points.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t offset = kHeaderSize + i * kPointSize;
        points.push_back(document::StrokePoint{.x = getFloat(blob, offset),
                                               .y = getFloat(blob, offset + 4),
                                               .pressure = getFloat(blob, offset + 8)});
    }
    return points;
}

} // namespace studyapp::persistence
