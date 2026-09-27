#pragma once

#include <studyapp/study/Planning.hpp>

#include <memory>
#include <string>

namespace studyapp::platform {

/// study::TimeZone on top of QTimeZone: the system time zone, or a named IANA zone
/// ("Europe/Berlin") for tests. Offsets follow the zone's daylight-saving rules.
class QtTimeZone final : public study::TimeZone {
public:
    /// The system's time zone.
    QtTimeZone();
    /// A named zone; falls back to UTC if the name is unknown (see isValid()).
    explicit QtTimeZone(const std::string& ianaId);
    ~QtTimeZone() override;
    QtTimeZone(const QtTimeZone&) = delete;
    QtTimeZone& operator=(const QtTimeZone&) = delete;
    QtTimeZone(QtTimeZone&&) = delete;
    QtTimeZone& operator=(QtTimeZone&&) = delete;

    [[nodiscard]] std::chrono::minutes utcOffset(core::Timestamp instant) const override;
    [[nodiscard]] bool isValid() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace studyapp::platform
