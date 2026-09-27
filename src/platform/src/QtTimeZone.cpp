#include <studyapp/platform/QtTimeZone.hpp>

#include <QByteArray>
#include <QDateTime>
#include <QTimeZone>

namespace studyapp::platform {

struct QtTimeZone::Impl {
    QTimeZone zone;
};

QtTimeZone::QtTimeZone() : impl_(std::make_unique<Impl>(Impl{QTimeZone::systemTimeZone()})) {}

QtTimeZone::QtTimeZone(const std::string& ianaId)
    : impl_(std::make_unique<Impl>(Impl{QTimeZone(QByteArray::fromStdString(ianaId))})) {}

QtTimeZone::~QtTimeZone() = default;

std::chrono::minutes QtTimeZone::utcOffset(core::Timestamp instant) const {
    if (!impl_->zone.isValid()) {
        return std::chrono::minutes{0};
    }
    const QDateTime at =
        QDateTime::fromMSecsSinceEpoch(instant.time_since_epoch().count(), QTimeZone::UTC);
    return std::chrono::minutes{impl_->zone.offsetFromUtc(at) / 60};
}

bool QtTimeZone::isValid() const noexcept {
    return impl_->zone.isValid();
}

} // namespace studyapp::platform
