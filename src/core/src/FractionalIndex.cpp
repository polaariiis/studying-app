#include <studyapp/core/FractionalIndex.hpp>

#include <cstddef>

// Algorithm: "fractional indexing" as popularised by David Greenspan / rocicorp
// (https://observablehq.com/@dgreensp/implementing-fractional-indexing), base 62.
//
// Key = integer part + optional fraction.
//   * The integer part's first character encodes its length: 'a'..'z' -> 2..27 characters
//     (non-negative integers, "a0" is zero), 'Z'..'A' -> 2..27 characters (negative).
//   * Appending/prepending increments/decrements the integer part, so keys used for
//     append-heavy collections grow only logarithmically.
//   * Inserting between two keys with the same integer part extends the fraction.
// All keys compare correctly as plain byte strings.

namespace studyapp::core {

namespace {

constexpr std::string_view kDigits =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
constexpr int kBase = static_cast<int>(kDigits.size()); // 62
constexpr char kZero = '0';
constexpr char kMaxDigit = 'z';

int digitValue(char c) noexcept {
    const auto pos = kDigits.find(c);
    return pos == std::string_view::npos ? -1 : static_cast<int>(pos);
}

char digitAt(int value) noexcept {
    return kDigits[static_cast<std::size_t>(value)];
}

/// Length of the integer part announced by its head character, or 0 if invalid.
std::size_t integerLength(char head) noexcept {
    if (head >= 'a' && head <= 'z') {
        return static_cast<std::size_t>(head - 'a') + 2;
    }
    if (head >= 'A' && head <= 'Z') {
        return static_cast<std::size_t>('Z' - head) + 2;
    }
    return 0;
}

/// The smallest representable integer ("A" followed by 26 zeros). It cannot be
/// decremented, so it is never used as a complete key on its own.
const std::string& smallestInteger() {
    static const std::string value = std::string(1, 'A') + std::string(26, kZero);
    return value;
}

/// Digit string strictly between the fractions 0.lower and 0.upper (nullopt = 1).
/// Preconditions: lower < upper, neither ends in '0'. The result never ends in '0'.
std::string midpoint(std::string_view lower, std::optional<std::string_view> upper) {
    if (upper) {
        std::size_t n = 0;
        while (n < upper->size() && (n < lower.size() ? lower[n] : kZero) == (*upper)[n]) {
            ++n;
        }
        if (n > 0) {
            const std::string_view rest = n < lower.size() ? lower.substr(n) : std::string_view{};
            return std::string(upper->substr(0, n)) + midpoint(rest, upper->substr(n));
        }
    }

    const int digitLower = lower.empty() ? 0 : digitValue(lower.front());
    const int digitUpper = upper ? digitValue(upper->front()) : kBase;

    if (digitUpper - digitLower > 1) {
        return std::string(1, digitAt((digitLower + digitUpper + 1) / 2));
    }
    if (upper && upper->size() > 1) {
        return std::string(1, upper->front());
    }
    const std::string_view restLower = lower.empty() ? std::string_view{} : lower.substr(1);
    return std::string(1, digitAt(digitLower)) + midpoint(restLower, std::nullopt);
}

/// Next integer, or nullopt if `integer` is already the largest representable one.
std::optional<std::string> incrementInteger(std::string integer) {
    const char head = integer.front();
    bool carry = true;
    for (std::size_t i = integer.size() - 1; carry && i >= 1; --i) {
        const int d = digitValue(integer[i]) + 1;
        if (d == kBase) {
            integer[i] = kZero;
        } else {
            integer[i] = digitAt(d);
            carry = false;
        }
    }
    if (!carry) {
        return integer;
    }
    // All digits overflowed: move to the next length class.
    if (head == 'Z') {
        return std::string("a") + kZero;
    }
    if (head == 'z') {
        return std::nullopt;
    }
    const char nextHead = static_cast<char>(head + 1);
    std::string digits = integer.substr(1);
    if (nextHead > 'a') {
        digits.push_back(kZero); // positive integers get longer
    } else {
        digits.pop_back(); // negative integers get shorter
    }
    return std::string(1, nextHead) + digits;
}

/// Previous integer, or nullopt if `integer` is already the smallest representable one.
std::optional<std::string> decrementInteger(std::string integer) {
    const char head = integer.front();
    bool borrow = true;
    for (std::size_t i = integer.size() - 1; borrow && i >= 1; --i) {
        const int d = digitValue(integer[i]) - 1;
        if (d == -1) {
            integer[i] = kMaxDigit;
        } else {
            integer[i] = digitAt(d);
            borrow = false;
        }
    }
    if (!borrow) {
        return integer;
    }
    if (head == 'a') {
        return std::string("Z") + kMaxDigit;
    }
    if (head == 'A') {
        return std::nullopt;
    }
    const char previousHead = static_cast<char>(head - 1);
    std::string digits = integer.substr(1);
    if (previousHead < 'Z') {
        digits.push_back(kMaxDigit); // negative integers get longer
    } else {
        digits.pop_back(); // positive integers get shorter
    }
    return std::string(1, previousHead) + digits;
}

std::string_view integerPart(std::string_view key) noexcept {
    return key.substr(0, integerLength(key.front()));
}

std::string_view fractionPart(std::string_view key) noexcept {
    return key.substr(integerLength(key.front()));
}

} // namespace

FractionalIndex FractionalIndex::first() {
    return FractionalIndex(std::string("a") + kZero);
}

Result<FractionalIndex> FractionalIndex::parse(std::string_view text) {
    const auto invalid = [&](std::string_view why) {
        return makeError(ErrorCode::ParseError, "invalid fractional index '" + std::string(text) +
                                                    "': " + std::string(why));
    };
    if (text.empty()) {
        return invalid("empty");
    }
    const std::size_t intLength = integerLength(text.front());
    if (intLength == 0) {
        return invalid("bad integer head");
    }
    if (text.size() < intLength) {
        return invalid("integer part too short");
    }
    for (const char c : text) {
        if (digitValue(c) < 0) {
            return invalid("invalid digit");
        }
    }
    if (text == smallestInteger()) {
        return invalid("smallest integer is reserved");
    }
    if (text.size() > intLength && text.back() == kZero) {
        return invalid("fraction must not end in '0'");
    }
    return FractionalIndex(std::string(text));
}

Result<FractionalIndex> FractionalIndex::between(const std::optional<FractionalIndex>& lower,
                                                 const std::optional<FractionalIndex>& upper) {
    if (lower && upper && !(*lower < *upper)) {
        return makeError(ErrorCode::InvalidArgument, "fractional index bounds out of order: '" +
                                                         lower->value_ + "' >= '" + upper->value_ +
                                                         "'");
    }

    if (!lower && !upper) {
        return first();
    }

    if (!lower) {
        const std::string_view key = upper->value_;
        const std::string_view intUpper = integerPart(key);
        if (intUpper == smallestInteger()) {
            return FractionalIndex(std::string(intUpper) + midpoint({}, fractionPart(key)));
        }
        if (intUpper.size() < key.size()) {
            return FractionalIndex(std::string(intUpper)); // strip the fraction
        }
        // Cannot be nullopt: only the smallest integer has no predecessor (handled above).
        return FractionalIndex(std::move(*decrementInteger(std::string(intUpper))));
    }

    const std::string_view lowerKey = lower->value_;
    const std::string_view intLower = integerPart(lowerKey);
    const std::string_view fracLower = fractionPart(lowerKey);

    if (!upper) {
        auto incremented = incrementInteger(std::string(intLower));
        if (!incremented) {
            return FractionalIndex(std::string(intLower) + midpoint(fracLower, std::nullopt));
        }
        return FractionalIndex(std::move(*incremented));
    }

    const std::string_view upperKey = upper->value_;
    const std::string_view intUpper = integerPart(upperKey);
    if (intLower == intUpper) {
        return FractionalIndex(std::string(intLower) + midpoint(fracLower, fractionPart(upperKey)));
    }
    auto incremented = incrementInteger(std::string(intLower));
    if (incremented && *incremented < upperKey) {
        return FractionalIndex(std::move(*incremented));
    }
    return FractionalIndex(std::string(intLower) + midpoint(fracLower, std::nullopt));
}

// Neither can fail: without an upper bound the integer part either increments or the
// fraction is extended; without a lower bound the smallest integer is handled by
// extending the fraction instead of decrementing.
FractionalIndex FractionalIndex::after(const FractionalIndex& key) {
    return *between(key, std::nullopt);
}

FractionalIndex FractionalIndex::before(const FractionalIndex& key) {
    return *between(std::nullopt, key);
}

} // namespace studyapp::core
