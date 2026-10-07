// The PDF worker's messages (docs/PDF_WORKER.md §8, D53): round trips and every way a
// request or a reply can be malformed. Qt-free; runs in every CI configuration.

#include <studyapp/ipc/PdfInspection.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace studyapp::ipc::pdf {
namespace {

using core::ErrorCode;

core::JobId someJob(std::uint8_t seed = 1) {
    core::Uuid::Bytes bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(seed + i);
    }
    return core::JobId{core::Uuid(bytes)};
}

#ifdef _WIN32
const std::string kAbsolute = "C:\\Users\\a\\ws.studyws\\temporary\\0190.part";
#else
const std::string kAbsolute = "/home/a/ws.studyws/temporary/0190.part";
#endif

InspectReply okReply() {
    return {.job = someJob(),
            .status = InspectStatus::Ok,
            .detail = 0,
            .pages = {{.width = 612.0, .height = 792.0}, {.width = 1.0, .height = 14400.0}}};
}

ErrorCode requestError(const std::vector<std::byte>& bytes) {
    auto decoded = decodeRequest(bytes);
    return decoded ? ErrorCode::Internal : decoded.error().code;
}

bool replyRejected(const std::vector<std::byte>& bytes) {
    auto decoded = decodeReply(bytes);
    return !decoded && decoded.error().code == ErrorCode::ParseError;
}

void put32(std::vector<std::byte>& bytes, std::size_t at, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[at + i] = static_cast<std::byte>((value >> (8U * i)) & 0xFFU);
    }
}

TEST(PdfInspectionTest, RequestsRoundTripWithTheDocumentedLayout) {
    const InspectRequest request{.job = someJob(),
                                 .path = kAbsolute + "\xC3\xBC\xE2\x9C\x93"}; // ü ✓
    const auto bytes = encode(request);
    ASSERT_EQ(bytes.size(), kHeaderSize + request.path.size());
    EXPECT_EQ(std::memcmp(bytes.data(), "SBPQ", 4), 0);
    EXPECT_EQ(bytes[4], std::byte{1}); // version 1, little-endian
    EXPECT_EQ(bytes[6], std::byte{1}); // kind: inspect
    EXPECT_EQ(std::memcmp(bytes.data() + 8, request.job.value().bytes().data(), 16), 0);
    auto decoded = decodeRequest(bytes);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    EXPECT_EQ(*decoded, request);
    EXPECT_EQ(requestJob(bytes), request.job);
}

TEST(PdfInspectionTest, MalformedRequestsAreRejected) {
    const auto valid = encode(InspectRequest{.job = someJob(), .path = kAbsolute});
    std::vector<std::byte> bytes = valid;
    bytes.resize(kHeaderSize - 1);
    EXPECT_EQ(requestError(bytes), ErrorCode::ParseError); // shorter than the header
    EXPECT_TRUE(requestJob(bytes).isNull());
    bytes = valid;
    bytes[0] = std::byte{'X'};
    EXPECT_EQ(requestError(bytes), ErrorCode::ParseError); // magic
    bytes = valid;
    bytes.pop_back();
    EXPECT_EQ(requestError(bytes), ErrorCode::ParseError); // size vs. path length
    bytes = valid;
    bytes.push_back(std::byte{'x'});
    EXPECT_EQ(requestError(bytes), ErrorCode::ParseError);
    bytes = valid;
    put32(bytes, 28, 7);
    EXPECT_EQ(requestError(bytes), ErrorCode::ParseError); // reserved must be 0
    EXPECT_EQ(requestError(encode(InspectRequest{.job = {}, .path = kAbsolute})),
              ErrorCode::ParseError); // nil job
    EXPECT_EQ(requestError(encode(InspectRequest{.job = someJob(), .path = ""})),
              ErrorCode::ParseError);
    EXPECT_EQ(requestError(encode(InspectRequest{.job = someJob(), .path = "relative/x.pdf"})),
              ErrorCode::ParseError);
    EXPECT_EQ(requestError(encode(InspectRequest{.job = someJob(), .path = kAbsolute + "\xFF"})),
              ErrorCode::ParseError); // not UTF-8
    EXPECT_EQ(
        requestError(encode(InspectRequest{.job = someJob(), .path = kAbsolute + "\xC0\xAF"})),
        ErrorCode::ParseError); // overlong
    std::string withNul = kAbsolute;
    withNul.push_back('\0');
    withNul += "x";
    EXPECT_EQ(requestError(encode(InspectRequest{.job = someJob(), .path = withNul})),
              ErrorCode::ParseError);
    EXPECT_EQ(requestError(encode(InspectRequest{
                  .job = someJob(), .path = kAbsolute + std::string(kMaxPathBytes, 'a')})),
              ErrorCode::ParseError); // too long
}

TEST(PdfInspectionTest, OtherVersionsAndKindsAreUnsupported) {
    auto bytes = encode(InspectRequest{.job = someJob(), .path = kAbsolute});
    bytes[4] = std::byte{2};
    EXPECT_EQ(requestError(bytes), ErrorCode::Unsupported);
    EXPECT_EQ(requestJob(bytes), someJob()); // still answerable
    bytes = encode(InspectRequest{.job = someJob(), .path = kAbsolute});
    bytes[6] = std::byte{3}; // 1 inspect and 2 extract text are the known kinds
    EXPECT_EQ(requestError(bytes), ErrorCode::Unsupported);
}

TEST(PdfInspectionTest, RepliesRoundTripForEveryStatus) {
    const InspectReply ok = okReply();
    const auto bytes = encode(ok);
    ASSERT_EQ(bytes.size(), kHeaderSize + 2 * kPageSize);
    EXPECT_EQ(std::memcmp(bytes.data(), "SBPR", 4), 0);
    auto decoded = decodeReply(bytes);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    EXPECT_EQ(*decoded, ok);
    const std::vector<InspectReply> errors{
        {.job = someJob(), .status = InspectStatus::InvalidRequest, .detail = 0, .pages = {}},
        {.job = someJob(), .status = InspectStatus::UnsupportedRequest, .detail = 0, .pages = {}},
        {.job = someJob(), .status = InspectStatus::Unreadable, .detail = 1, .pages = {}},
        {.job = someJob(), .status = InspectStatus::Protected, .detail = 0, .pages = {}},
        {.job = someJob(), .status = InspectStatus::NoPages, .detail = 0, .pages = {}},
        {.job = someJob(), .status = InspectStatus::TooManyPages, .detail = 5001, .pages = {}},
        {.job = someJob(), .status = InspectStatus::BadPageSize, .detail = 3, .pages = {}},
        {.job = someJob(), .status = InspectStatus::Internal, .detail = 0, .pages = {}}};
    for (const InspectReply& reply : errors) {
        auto back = decodeReply(encode(reply));
        ASSERT_TRUE(back.has_value()) << static_cast<int>(reply.status);
        EXPECT_EQ(*back, reply);
    }
    InspectReply most = okReply();
    most.pages.assign(kMaxPages, {.width = 10.0, .height = 10.0});
    EXPECT_TRUE(decodeReply(encode(most)).has_value());
}

TEST(PdfInspectionTest, MalformedRepliesAreRejected) {
    const auto valid = encode(okReply());
    auto bytes = valid;
    bytes[1] = std::byte{'Q'};
    EXPECT_TRUE(replyRejected(bytes)); // magic
    bytes = valid;
    bytes[4] = std::byte{2};
    EXPECT_TRUE(replyRejected(bytes)); // version
    bytes = valid;
    bytes[6] = std::byte{9};
    EXPECT_TRUE(replyRejected(bytes)); // unknown status
    bytes = valid;
    bytes.pop_back();
    EXPECT_TRUE(replyRejected(bytes)); // size
    bytes.resize(10);
    EXPECT_TRUE(replyRejected(bytes));
    bytes = valid;
    put32(bytes, 28, 3);
    EXPECT_TRUE(replyRejected(bytes)); // page count vs. size
    InspectReply reply = okReply();
    reply.job = {};
    EXPECT_TRUE(replyRejected(encode(reply))); // nil job
    reply = okReply();
    reply.pages.clear();
    EXPECT_TRUE(replyRejected(encode(reply))); // Ok without pages
    reply = okReply();
    reply.pages.assign(kMaxPages + 1, {.width = 10.0, .height = 10.0});
    EXPECT_TRUE(replyRejected(encode(reply))); // too many pages
    reply = okReply();
    reply.detail = 4;
    EXPECT_TRUE(replyRejected(encode(reply))); // Ok with a detail
    EXPECT_TRUE(replyRejected(encode(InspectReply{
        .job = someJob(), .status = InspectStatus::NoPages, .detail = 0, .pages = {{1.0, 1.0}}})));
    EXPECT_TRUE(replyRejected(encode(InspectReply{
        .job = someJob(), .status = InspectStatus::TooManyPages, .detail = 12, .pages = {}})));
    EXPECT_TRUE(replyRejected(encode(InspectReply{
        .job = someJob(), .status = InspectStatus::BadPageSize, .detail = 0, .pages = {}})));
    EXPECT_TRUE(replyRejected(encode(InspectReply{
        .job = someJob(), .status = InspectStatus::Protected, .detail = 2, .pages = {}})));
}

TEST(PdfInspectionTest, PageSizesMustBeFinitePositiveAndSupported) {
    EXPECT_TRUE(validPageSize(612.0, 792.0));
    EXPECT_TRUE(validPageSize(kMaxPagePoints, kMaxPagePoints));
    const double bad[] = {0.0, -1.0, kMaxPagePoints * 1.0001,
                          std::numeric_limits<double>::quiet_NaN(),
                          std::numeric_limits<double>::infinity()};
    for (const double value : bad) {
        EXPECT_FALSE(validPageSize(value, 100.0)) << value;
        EXPECT_FALSE(validPageSize(100.0, value)) << value;
        InspectReply reply = okReply();
        reply.pages[0].height = value;
        EXPECT_TRUE(replyRejected(encode(reply))) << value;
    }
}

} // namespace
} // namespace studyapp::ipc::pdf

// ---------------------------------------------------------------------------- text (§23)

namespace studyapp::ipc::pdf {
namespace {

TextReply okText() {
    return {.job = someJob(),
            .status = InspectStatus::Ok,
            .detail = 0,
            .pages = {"Chapter 1\r\nEigenvalues", "", "\xCE\xBB = 2 \xE2\x9C\x93"}};
}

bool textRejected(const std::vector<std::byte>& bytes) {
    auto decoded = decodeTextReply(bytes);
    return !decoded && decoded.error().code == ErrorCode::ParseError;
}

TEST(PdfTextReplyTest, ExtractTextRequestsRoundTrip) {
    const InspectRequest request{
        .job = someJob(), .path = kAbsolute, .kind = RequestKind::ExtractText};
    const auto bytes = encode(request);
    EXPECT_EQ(bytes[6], std::byte{2});
    auto decoded = decodeRequest(bytes);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    EXPECT_EQ(*decoded, request);
    EXPECT_EQ(decodeRequest(encode(InspectRequest{.job = someJob(), .path = kAbsolute}))->kind,
              RequestKind::Inspect);
}

TEST(PdfTextReplyTest, RoundTripsWithTheDocumentedLayout) {
    const TextReply ok = okText();
    const auto bytes = encode(ok);
    std::size_t text = 0;
    for (const std::string& page : ok.pages) {
        text += page.size();
    }
    ASSERT_EQ(bytes.size(), kHeaderSize + 4 * ok.pages.size() + text);
    EXPECT_EQ(std::memcmp(bytes.data(), "SBPT", 4), 0);
    auto decoded = decodeTextReply(bytes);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    EXPECT_EQ(*decoded, ok);

    TextReply truncated = okText();
    truncated.detail = 1;
    EXPECT_EQ(*decodeTextReply(encode(truncated)), truncated);
    const std::vector<TextReply> errors{
        {.job = someJob(), .status = InspectStatus::Unreadable, .detail = 1, .pages = {}},
        {.job = someJob(), .status = InspectStatus::Protected, .detail = 0, .pages = {}},
        {.job = someJob(), .status = InspectStatus::NoPages, .detail = 0, .pages = {}},
        {.job = someJob(), .status = InspectStatus::TooManyPages, .detail = 6000, .pages = {}},
        {.job = someJob(), .status = InspectStatus::Internal, .detail = 0, .pages = {}},
    };
    for (const TextReply& error : errors) {
        auto round = decodeTextReply(encode(error));
        ASSERT_TRUE(round.has_value()) << round.error().message;
        EXPECT_EQ(*round, error);
    }
}

TEST(PdfTextReplyTest, AnInspectionReplyIsNotATextReplyNorTheOtherWay) {
    EXPECT_TRUE(textRejected(encode(okReply())));
    EXPECT_TRUE(replyRejected(encode(okText())));
}

TEST(PdfTextReplyTest, MalformedRepliesAreRejected) {
    const auto good = encode(okText());
    EXPECT_TRUE(textRejected({}));
    EXPECT_TRUE(textRejected({good.begin(), good.begin() + 31}));
    auto bytes = good;
    bytes.push_back(std::byte{'x'}); // longer than its page table says
    EXPECT_TRUE(textRejected(bytes));
    bytes = good;
    bytes.pop_back();
    EXPECT_TRUE(textRejected(bytes));
    bytes = good;
    bytes[4] = std::byte{2}; // version
    EXPECT_TRUE(textRejected(bytes));
    bytes = good;
    bytes[6] = std::byte{99}; // status
    EXPECT_TRUE(textRejected(bytes));
    bytes = good;
    for (std::size_t i = 8; i < 24; ++i) {
        bytes[i] = std::byte{0}; // nil job
    }
    EXPECT_TRUE(textRejected(bytes));
    bytes = good;
    put32(bytes, 24, 2); // Ok allows detail 0 or 1 only
    EXPECT_TRUE(textRejected(bytes));
    bytes = encode(TextReply{
        .job = someJob(), .status = InspectStatus::BadPageSize, .detail = 1, .pages = {}});
    EXPECT_TRUE(textRejected(bytes)); // not a text reply status
    bytes = good;
    put32(bytes, 28, 0); // Ok without pages
    EXPECT_TRUE(textRejected(bytes));
    bytes = encode(
        TextReply{.job = someJob(), .status = InspectStatus::NoPages, .detail = 0, .pages = {"x"}});
    EXPECT_TRUE(textRejected(bytes)); // an error status with pages
    bytes = good;
    put32(bytes, 28, kMaxPages + 1);
    EXPECT_TRUE(textRejected(bytes));
    // Text that is not UTF-8, or contains NUL.
    TextReply broken = okText();
    broken.pages[0] = std::string("ok\xFF");
    EXPECT_TRUE(textRejected(encode(broken)));
    broken.pages[0] = std::string("a\0b", 3);
    EXPECT_TRUE(textRejected(encode(broken)));
    broken.pages[0] = "\xED\xA0\x80"; // a surrogate
    EXPECT_TRUE(textRejected(encode(broken)));
}

TEST(PdfTextReplyTest, TextLimitsAreEnforced) {
    TextReply big = okText();
    big.pages = {std::string(kMaxPageTextBytes, 'a')};
    EXPECT_TRUE(decodeTextReply(encode(big)).has_value());
    big.pages = {std::string(kMaxPageTextBytes + 1, 'a')};
    EXPECT_TRUE(textRejected(encode(big)));
    // In all: 8 MiB across pages.
    big.pages.assign(kMaxTextBytes / kMaxPageTextBytes, std::string(kMaxPageTextBytes, 'a'));
    EXPECT_TRUE(decodeTextReply(encode(big)).has_value());
    big.pages.emplace_back("a");
    EXPECT_TRUE(textRejected(encode(big)));
}

TEST(PdfTextReplyTest, Utf8Validation) {
    EXPECT_TRUE(validUtf8(""));
    EXPECT_TRUE(validUtf8("plain \xC3\xBC \xE2\x9C\x93 \xF0\x9F\x98\x80"));
    EXPECT_FALSE(validUtf8(std::string("\0", 1)));
    EXPECT_FALSE(validUtf8("\xC0\xAF"));         // overlong
    EXPECT_FALSE(validUtf8("\xF4\x90\x80\x80")); // above U+10FFFF
    EXPECT_FALSE(validUtf8("\xE2\x9C"));         // cut short
}

} // namespace
} // namespace studyapp::ipc::pdf
