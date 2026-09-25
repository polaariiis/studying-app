#pragma once

#include <studyapp/core/IdGenerator.hpp>
#include <studyapp/core/Uuid.hpp>

#include <cstddef>
#include <cstdint>

namespace studyapp::testing {

/// Deterministic id generator: 00000000-0000-7000-8000-000000000001, ...002, ...
/// Ids are increasing, so creation order equals id order in tests.
class SequentialIds final : public core::IdGenerator {
public:
    [[nodiscard]] core::Uuid next() override {
        ++counter_;
        core::Uuid::Bytes bytes{};
        bytes[6] = 0x70; // version 7
        bytes[8] = 0x80; // RFC variant
        for (std::size_t i = 0; i < 6; ++i) {
            bytes[15 - i] = static_cast<std::uint8_t>((counter_ >> (8U * i)) & 0xFFU);
        }
        return core::Uuid{bytes};
    }

    [[nodiscard]] std::uint64_t issued() const noexcept { return counter_; }

private:
    std::uint64_t counter_ = 0;
};

} // namespace studyapp::testing
