#include <studyapp/core/Ids.hpp>

#include <gtest/gtest.h>

#include <type_traits>
#include <unordered_map>

namespace studyapp::core {
namespace {

// Distinct id types must not be interchangeable.
static_assert(!std::is_convertible_v<PageId, ElementId>);
static_assert(!std::is_convertible_v<ElementId, PageId>);
static_assert(!std::is_convertible_v<Uuid, PageId>, "construction from Uuid must be explicit");
static_assert(std::is_constructible_v<PageId, Uuid>);
static_assert(std::is_trivially_copyable_v<PageId>);
static_assert(sizeof(PageId) == sizeof(Uuid));

constexpr Uuid::Bytes kBytes{0x01, 0x8f, 0x3a, 0x4b, 0x5c, 0x6d, 0x7e, 0x8f,
                             0x90, 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x07};

class SequentialIds final : public IdGenerator {
public:
    Uuid next() override {
        Uuid::Bytes bytes{};
        bytes[15] = ++counter_;
        return Uuid{bytes};
    }

private:
    std::uint8_t counter_ = 0;
};

TEST(IdTest, DefaultIsNull) {
    constexpr PageId id;
    EXPECT_TRUE(id.isNull());
    EXPECT_EQ(id.value(), Uuid::nil());
}

TEST(IdTest, WrapsUuid) {
    const PageId id{Uuid{kBytes}};
    EXPECT_FALSE(id.isNull());
    EXPECT_EQ(id.value(), Uuid{kBytes});
    EXPECT_EQ(id.toString(), "018f3a4b-5c6d-7e8f-90a1-b2c3d4e5f607");
}

TEST(IdTest, ParseRoundTrip) {
    const ElementId original{Uuid{kBytes}};
    const auto parsed = ElementId::parse(original.toString());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, original);

    const auto invalid = ElementId::parse("not-a-uuid");
    ASSERT_FALSE(invalid.has_value());
    EXPECT_EQ(invalid.error().code, ErrorCode::ParseError);
}

TEST(IdTest, GenerateUsesInjectedGenerator) {
    SequentialIds ids;
    const TaskId first = TaskId::generate(ids);
    const TaskId second = TaskId::generate(ids);
    EXPECT_NE(first, second);
    EXPECT_LT(first, second);
    EXPECT_EQ(first.value().bytes()[15], 1);
    EXPECT_EQ(second.value().bytes()[15], 2);
}

TEST(IdTest, UsableAsHashMapKey) {
    SequentialIds ids;
    std::unordered_map<NotebookId, int> map;
    const NotebookId a = NotebookId::generate(ids);
    const NotebookId b = NotebookId::generate(ids);
    map[a] = 1;
    map[b] = 2;
    EXPECT_EQ(map.at(a), 1);
    EXPECT_EQ(map.at(b), 2);
    EXPECT_EQ(map.size(), 2U);
}

} // namespace
} // namespace studyapp::core
