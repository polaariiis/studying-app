#include <studyapp/core/Error.hpp>

#include <gtest/gtest.h>

namespace studyapp::core {
namespace {

Result<int> parsePositive(int value) {
    if (value <= 0) {
        return makeError(ErrorCode::InvalidArgument, "value must be positive");
    }
    return value;
}

TEST(ErrorTest, ResultCarriesValueOrError) {
    const auto ok = parsePositive(3);
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(*ok, 3);

    const auto failed = parsePositive(-1);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(failed.error().message, "value must be positive");
}

TEST(ErrorTest, VoidResult) {
    const Result<void> ok{};
    EXPECT_TRUE(ok.has_value());

    const Result<void> failed = makeError(ErrorCode::IoError, "disk full");
    EXPECT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error(), (Error{ErrorCode::IoError, "disk full"}));
}

TEST(ErrorTest, CodesHaveReadableNames) {
    EXPECT_EQ(toString(ErrorCode::ParseError), "parse error");
    EXPECT_EQ(toString(ErrorCode::NotFound), "not found");
    EXPECT_EQ(toString(ErrorCode::IoError), "I/O error");
    EXPECT_EQ(toString(ErrorCode::Conflict), "conflict");
}

} // namespace
} // namespace studyapp::core
