#include <studyapp/ui/AppIcon.hpp>

#include <QSize>
#include <QString>

#include <array>

namespace studyapp::ui {

QIcon applicationIcon() {
    static constexpr std::array kSizes{16, 32, 48, 64, 128, 256};
    QIcon icon;
    for (const int size : kSizes) {
        icon.addFile(QStringLiteral(":/icons/app/studyboard-%1.png").arg(size), QSize(size, size));
    }
    return icon;
}

} // namespace studyapp::ui
