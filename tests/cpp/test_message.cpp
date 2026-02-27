// test_message.cpp — pipeutil::Message 単体テスト
// 対象: source/core/include/pipeutil/message.hpp, src/message.cpp

#include <gtest/gtest.h>
#include "pipeutil/message.hpp"

using namespace pipeutil;

// ─── デフォルト構築 ───────────────────────────────────────────────────

TEST(MessageTest, DefaultConstruct_IsEmpty) {
    Message msg;
    EXPECT_TRUE(msg.empty());
    EXPECT_EQ(msg.size(), 0u);
    EXPECT_EQ(msg.payload().size(), 0u);
}

// ─── 文字列ビューからの構築 ───────────────────────────────────────────

TEST(MessageTest, FromStringView_RoundTrip) {
    Message msg{std::string_view{"hello"}};
    EXPECT_FALSE(msg.empty());
    EXPECT_EQ(msg.size(), 5u);
    EXPECT_EQ(msg.as_string_view(), "hello");
}

TEST(MessageTest, FromStringView_Empty) {
    Message msg{std::string_view{""}};
    EXPECT_TRUE(msg.empty());
}

TEST(MessageTest, FromStringView_NullBytesPreserved) {
    // null バイトを含む文字列がそのままペイロードに入ること（R-002: バイナリ安全性）
    using namespace std::string_view_literals;
    const auto sv = "ab\0cd"sv;  // length = 5
    Message msg{sv};
    EXPECT_EQ(msg.size(), 5u);
    EXPECT_EQ(msg.as_string_view(), sv);
}

// ─── バイト列スパンからの構築 ─────────────────────────────────────────

TEST(MessageTest, FromSpan_RoundTrip) {
    const std::byte data[] = {std::byte{0x01}, std::byte{0xFF}, std::byte{0x00}};
    Message msg{std::span<const std::byte>(data)};
    EXPECT_EQ(msg.size(), 3u);
    EXPECT_EQ(msg.payload()[0], std::byte{0x01});
    EXPECT_EQ(msg.payload()[1], std::byte{0xFF});
    EXPECT_EQ(msg.payload()[2], std::byte{0x00});
}

// ─── コピーセマンティクス ─────────────────────────────────────────────

TEST(MessageTest, Copy_IsDeepCopy) {
    Message a{std::string_view{"original"}};
    Message b = a;
    // コピー後に元を変えても b に影響しないことを確認するためムーブ元を破棄
    a = Message{std::string_view{"changed"}};
    EXPECT_EQ(b.as_string_view(), "original");
    EXPECT_EQ(a.as_string_view(), "changed");
}

// ─── ムーブセマンティクス ─────────────────────────────────────────────

TEST(MessageTest, Move_LeavesSourceEmpty) {
    Message src{std::string_view{"data"}};
    Message dst = std::move(src);
    EXPECT_EQ(dst.as_string_view(), "data");
    // ムーブ後のオブジェクトは有効（空または任意の状態）であること
    // size() が 0 になることを標準仕様として検証可能
    EXPECT_EQ(src.size(), 0u);
}

// ─── 境界値 ───────────────────────────────────────────────────────────

TEST(MessageTest, SingleByte) {
    const std::byte b{0xAB};
    Message msg{std::span<const std::byte>(&b, 1)};
    EXPECT_EQ(msg.size(), 1u);
    EXPECT_EQ(msg.payload()[0], std::byte{0xAB});
}

TEST(MessageTest, LargePayload_64KiB) {
    // 64 KiB ペイロードが問題なく構築・参照できること
    constexpr std::size_t kSize = 65536;
    std::vector<std::byte> buf(kSize, std::byte{0x5A});
    Message msg{std::span<const std::byte>(buf)};
    EXPECT_EQ(msg.size(), kSize);
    EXPECT_EQ(msg.payload()[0],       std::byte{0x5A});
    EXPECT_EQ(msg.payload()[kSize-1], std::byte{0x5A});
}
