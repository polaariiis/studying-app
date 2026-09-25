#pragma once

#include <studyapp/core/Error.hpp>
#include <studyapp/document/Element.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace studyapp::persistence {

// Stroke point blob codec v1 (docs/DATA_MODEL.md §8), stored in `stroke.points`:
//
//   u8  version   = 1
//   u8  channels  = 3            (x, y, pressure)
//   u32 count                    little-endian
//   count × { f32 x, f32 y, f32 pressure }   little-endian IEEE 754
//
// Floats are stored bit-exactly, so a decode returns exactly the encoded values. Readers
// must support every version ever written; writers write the newest (`point_format`).

inline constexpr std::uint8_t kStrokePointFormatV1 = 1;
inline constexpr std::uint8_t kStrokePointFormat = kStrokePointFormatV1;

[[nodiscard]] std::vector<std::uint8_t>
encodeStrokePoints(std::span<const document::StrokePoint> points);

/// Rejects (ParseError) unknown versions, wrong channel counts, truncated or oversized
/// blobs and empty point lists. Values are not range-checked here; the document
/// invariants (finite coordinates, pressure in [0, 1]) are checked when the loaded
/// element is applied to a Workspace.
[[nodiscard]] core::Result<std::vector<document::StrokePoint>>
decodeStrokePoints(std::span<const std::uint8_t> blob);

} // namespace studyapp::persistence
