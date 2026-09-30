// ipc::FileChannel: the C++ adapter over IPCFileLab. The C library's own tests cover the
// protocol, recovery and multi-process behaviour; these cover the adapter: ownership,
// conversions (paths, timeouts, errors) and a waiting exchange between two channels.

#include <studyapp/ipc/FileChannel.hpp>

#include <studyapp/testing/ResultMacros.hpp>
#include <studyapp/testing/TempDirectory.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace studyapp::ipc {
namespace {

using namespace std::chrono_literals;

std::vector<std::byte> bytes(const std::string& text) {
    std::vector<std::byte> result;
    for (const char c : text) {
        result.push_back(static_cast<std::byte>(c));
    }
    return result;
}

struct FileChannelTest : ::testing::Test {
    testing::TempDirectory dir; // its name holds non-ASCII letters
    std::filesystem::path file = dir / "channel.ipc";
};

TEST_F(FileChannelTest, CarriesMessagesIncludingBinaryAndEmptyOnes) {
    auto channel = FileChannel::open(file);
    ASSERT_OK(channel);
    std::vector<std::byte> binary;
    for (int value = 0; value < 256; ++value) {
        binary.push_back(static_cast<std::byte>(value));
    }

    ASSERT_OK(channel->send(binary, 1s));
    auto received = channel->receive(1s);
    ASSERT_OK(received);
    ASSERT_TRUE(received->has_value());
    EXPECT_EQ(**received, binary);

    ASSERT_OK(channel->send({}, 1s));
    received = channel->receive(1s);
    ASSERT_OK(received);
    ASSERT_TRUE(received->has_value());
    EXPECT_TRUE((*received)->empty());
}

TEST_F(FileChannelTest, ReceiveReturnsNothingWhenTheTimeRunsOut) {
    auto channel = FileChannel::open(file);
    ASSERT_OK(channel);
    const auto start = std::chrono::steady_clock::now();

    const auto received = channel->receive(100ms);

    ASSERT_OK(received);
    EXPECT_FALSE(received->has_value());
    EXPECT_GE(std::chrono::steady_clock::now() - start, 80ms);
}

TEST_F(FileChannelTest, SendIsAConflictWhileAnUnreadMessageWaits) {
    auto channel = FileChannel::open(file);
    ASSERT_OK(channel);
    ASSERT_OK(channel->send(bytes("first"), 1s));

    const auto second = channel->send(bytes("second"), 0ms);

    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code, core::ErrorCode::Conflict);
    const auto received = channel->receive(1s);
    ASSERT_OK(received);
    EXPECT_EQ(**received, bytes("first"));
}

TEST_F(FileChannelTest, RefusesOversizedMessagesAndNegativeTimeouts) {
    auto channel = FileChannel::open(file);
    ASSERT_OK(channel);
    const std::vector<std::byte> tooLarge(FileChannel::kMaxMessageSize + 1);

    const auto sent = channel->send(tooLarge, 0ms);
    ASSERT_FALSE(sent.has_value());
    EXPECT_EQ(sent.error().code, core::ErrorCode::InvalidArgument);

    const auto received = channel->receive(-5ms);
    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error().code, core::ErrorCode::InvalidArgument);
}

TEST_F(FileChannelTest, ReportsMalformedFilesAndMissingDirectories) {
    std::ofstream(file, std::ios::binary) << "not a channel";
    const auto malformed = FileChannel::open(file);
    ASSERT_FALSE(malformed.has_value());
    EXPECT_EQ(malformed.error().code, core::ErrorCode::ParseError);

    const auto missing = FileChannel::open(dir / "missing" / "channel.ipc");
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code, core::ErrorCode::IoError);
    EXPECT_NE(missing.error().message.find("IPC channel"), std::string::npos);
}

TEST_F(FileChannelTest, AMessageOutlivesItsSendersChannel) {
    {
        auto sender = FileChannel::open(file);
        ASSERT_OK(sender);
        ASSERT_OK(sender->send(bytes("kept"), 1s));
    }
    auto receiver = FileChannel::open(file);
    ASSERT_OK(receiver);
    const auto received = receiver->receive(1s);
    ASSERT_OK(received);
    EXPECT_EQ(**received, bytes("kept"));
}

TEST_F(FileChannelTest, MovingTransfersTheChannel) {
    auto opened = FileChannel::open(file);
    ASSERT_OK(opened);
    FileChannel first = std::move(*opened);
    FileChannel second = std::move(first);
    auto other = FileChannel::open(dir / "other.ipc");
    ASSERT_OK(other);
    *other = std::move(second); // closes the other channel, takes this one

    ASSERT_OK(other->send(bytes("moved"), 1s));
    const auto received = other->receive(1s);
    ASSERT_OK(received);
    EXPECT_EQ(**received, bytes("moved"));
}

TEST_F(FileChannelTest, AReceiverWaitsForASenderOnAnotherThread) {
    auto receiver = FileChannel::open(file);
    auto sender = FileChannel::open(file);
    ASSERT_OK(receiver);
    ASSERT_OK(sender);
    auto received = std::async(std::launch::async, [&receiver] { return receiver->receive(5s); });

    std::this_thread::sleep_for(50ms);
    ASSERT_OK(sender->send(bytes("across threads"), 1s));

    ASSERT_EQ(received.wait_for(5s), std::future_status::ready);
    const auto result = received.get();
    ASSERT_OK(result);
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(**result, bytes("across threads"));
}

} // namespace
} // namespace studyapp::ipc
