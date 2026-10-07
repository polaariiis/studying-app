#include <studyapp/core/BuildInfo.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace studyapp::core::build {
namespace {

// The version users see is the numeric version, plus "-<label>" for a pre-release
// ("1.2.0-dev"). Package names and the package smoke tests rely on this form.
TEST(BuildInfoTest, VersionIsTheNumericVersionWithAnOptionalLabel) {
    const std::string numeric = std::to_string(kVersionMajor) + "." +
                                std::to_string(kVersionMinor) + "." + std::to_string(kVersionPatch);
    const std::string version(kVersion);
    ASSERT_TRUE(version.starts_with(numeric)) << version;
    const std::string label = version.substr(numeric.size());
    if (label.empty()) {
        return;
    }
    ASSERT_GE(label.size(), 2U) << version;
    EXPECT_EQ(label.front(), '-') << version;
    EXPECT_TRUE(std::all_of(label.begin() + 1, label.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.';
    })) << version;
}

} // namespace
} // namespace studyapp::core::build
