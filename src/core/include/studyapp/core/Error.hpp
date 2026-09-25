#pragma once

#include <tl/expected.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace studyapp::core {

/// Broad classification of an expected failure. Programming errors are not reported
/// through `Error`; they are assertions.
enum class ErrorCode {
    InvalidArgument,
    ParseError,
    NotFound,
    AlreadyExists,
    IoError,
    Unsupported,
    Internal,
};

[[nodiscard]] std::string_view toString(ErrorCode code) noexcept;

struct Error {
    ErrorCode code = ErrorCode::Internal;
    std::string message;

    [[nodiscard]] friend bool operator==(const Error&, const Error&) = default;
};

/// Result of an operation that can fail in an expected way (I/O, parsing, validation).
/// Alias of `tl::expected` so it can become `std::expected` once the project moves to C++23.
template <class T>
using Result = tl::expected<T, Error>;

[[nodiscard]] inline tl::unexpected<Error> makeError(ErrorCode code, std::string message) {
    return tl::unexpected<Error>(Error{code, std::move(message)});
}

} // namespace studyapp::core
