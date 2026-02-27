// test_io_buffer.cpp — pipeutil::IOBuffer 単体テスト
// 対象: source/core/include/pipeutil/io_buffer.hpp

#include <gtest/gtest.h>
#include "pipeutil/io_buffer.hpp"
#include <array>
#include <cstring>
#include <numeric>
#include <thread>
#include <vector>

using namespace pipeutil;

// ─── 初期状態の検証 ───────────────────────────────────────────────────

TEST(IOBufferTest, InitialState) {
    IOBuffer buf{256};
    EXPECT_EQ(buf.capacity(), 256u);
    EXPECT_EQ(buf.readable_size(), 0u);
    EXPECT_EQ(buf.writable_size(), 256u);
}

// ─── 基本的な Write / Read ────────────────────────────────────────────

TEST(IOBufferTest, WriteRead_Basic) {
    IOBuffer buf{256};
    const std::byte src[] = {std::byte{1}, std::byte{2}, std::byte{3}};
    ASSERT_EQ(buf.write(src, 3), 3u);
    EXPECT_EQ(buf.readable_size(), 3u);

    std::byte dst[3]{};
    ASSERT_EQ(buf.read(dst, 3), 3u);
    EXPECT_EQ(std::memcmp(src, dst, 3), 0);
    EXPECT_EQ(buf.readable_size(), 0u);
}

// ─── 容量ちょうどまで書き込み ─────────────────────────────────────────

TEST(IOBufferTest, WriteExactCapacity) {
    IOBuffer buf{8};
    std::array<std::byte, 8> src{};
    std::iota(reinterpret_cast<uint8_t*>(src.data()),
               reinterpret_cast<uint8_t*>(src.data()) + 8, uint8_t{0});
    ASSERT_EQ(buf.write(src.data(), 8), 8u);
    EXPECT_EQ(buf.writable_size(), 0u);
}

// ─── 容量超過は書き込み可能分だけ書く ────────────────────────────────

TEST(IOBufferTest, WriteOverCapacity_PartialWrite) {
    IOBuffer buf{4};
    std::array<std::byte, 8> src{};
    const std::size_t written = buf.write(src.data(), 8);
    EXPECT_LE(written, 4u);
}

// ─── 読み出し可能バイト数分しか読まない ──────────────────────────────

TEST(IOBufferTest, ReadMoreThanAvailable_PartialRead) {
    IOBuffer buf{256};
    const std::byte src[] = {std::byte{0xAA}};
    buf.write(src, 1);

    std::array<std::byte, 4> dst{};
    const std::size_t n = buf.read(dst.data(), 4);
    EXPECT_EQ(n, 1u);
    EXPECT_EQ(dst[0], std::byte{0xAA});
}

// ─── clear() ─────────────────────────────────────────────────────────

TEST(IOBufferTest, Clear_ResetsBuffer) {
    IOBuffer buf{256};
    const std::byte src[] = {std::byte{1}, std::byte{2}};
    buf.write(src, 2);
    buf.clear();
    EXPECT_EQ(buf.readable_size(), 0u);
    EXPECT_EQ(buf.writable_size(), 256u);
}

// ─── ラップアラウンド（循環バッファの折り返し）──────────────────────

TEST(IOBufferTest, WrapAround) {
    // 小容量バッファでラップアラウンドが正しく機能するか
    IOBuffer buf{8};
    std::array<std::byte, 6> src{};
    for (int i = 0; i < 6; ++i) src[i] = std::byte(i + 1);

    // 6 バイト書き込み → 4 バイト読み出し（読み位置が中央付近に）
    buf.write(src.data(), 6);
    std::array<std::byte, 4> tmp{};
    buf.read(tmp.data(), 4);

    // さらに 6 バイト書き込む（バッファ末尾を超えてラップアラウンド）
    std::array<std::byte, 6> src2{};
    for (int i = 0; i < 6; ++i) src2[i] = std::byte(i + 10);
    buf.write(src2.data(), 6);

    // 残り 2 + 6 = 8 バイトが読める
    EXPECT_EQ(buf.readable_size(), 8u);

    std::array<std::byte, 8> dst{};
    ASSERT_EQ(buf.read(dst.data(), 8), 8u);
    // 最初の 2 バイトは src の後半、次の 6 バイトは src2
    EXPECT_EQ(dst[0], std::byte{5});
    EXPECT_EQ(dst[1], std::byte{6});
    EXPECT_EQ(dst[2], std::byte{10});
}

// ─── SPSC スレッドセーフ性 ───────────────────────────────────────────

TEST(IOBufferTest, SPSC_ThreadSafe) {
    // 1 プロデューサー / 1 コンシューマーで大量転送しデータ破損がないことを確認
    constexpr std::size_t kBufSize  = 4096;
    constexpr std::size_t kTotalMsg = 10000;
    IOBuffer buf{kBufSize};

    std::atomic<bool>     done{false};
    std::vector<uint8_t>  received;
    received.reserve(kTotalMsg);

    // コンシューマー: 読み出しループ
    std::thread consumer([&] {
        std::byte tmp{};
        while (!done.load(std::memory_order_relaxed) || buf.readable_size() > 0) {
            if (buf.read(&tmp, 1) == 1) {
                received.push_back(static_cast<uint8_t>(tmp));
            }
        }
    });

    // プロデューサー: 0〜255 をループして書き込み
    for (std::size_t i = 0; i < kTotalMsg; ++i) {
        const std::byte b{static_cast<uint8_t>(i & 0xFF)};
        while (buf.write(&b, 1) == 0) {
            std::this_thread::yield();  // バッファ満杯時はビジーウェイト
        }
    }
    done.store(true, std::memory_order_relaxed);
    consumer.join();

    ASSERT_EQ(received.size(), kTotalMsg);
    for (std::size_t i = 0; i < kTotalMsg; ++i) {
        EXPECT_EQ(received[i], static_cast<uint8_t>(i & 0xFF)) << "mismatch at i=" << i;
    }
}
