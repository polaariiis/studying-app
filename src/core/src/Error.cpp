#include <studyapp/core/Error.hpp>

namespace studyapp::core {

std::string_view toString(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::InvalidArgument:
        return "invalid argument";
    case ErrorCode::ParseError:
        return "parse error";
    case ErrorCode::NotFound:
        return "not found";
    case ErrorCode::AlreadyExists:
        return "already exists";
    case ErrorCode::IoError:
        return "I/O error";
    case ErrorCode::Unsupported:
        return "unsupported";
    case ErrorCode::Internal:
        return "internal error";
    }
    return "unknown error";
}

} // namespace studyapp::core
