// crc32c.cpp — CRC-32C ソフトウェア実装
// ポリノミアル: 0x1EDC6F41 (Castagnoli)
// アーキテクチャ固有命令（SSE4.2 crc32 等）は使用しない。
// 将来的にハードウェアアクセラレーションを追加する場合は compute_crc32c の実装を差し替える。

#include "detail/crc32c.hpp"
#include <array>

namespace pipeutil::detail {

namespace {

/// CRC-32C 用ルックアップテーブル（256 エントリ × 4 バイト）
/// コンパイル時生成。
consteval std::array<uint32_t, 256> make_table() noexcept {
    constexpr uint32_t POLY = 0x82F63B78u; // reflected form of 0x1EDC6F41
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^ ((crc & 1u) ? POLY : 0u);
        }
        t[i] = crc;
    }
    return t;
}

constexpr auto CRC32C_TABLE = make_table();

} // anonymous namespace

uint32_t compute_crc32c(std::span<const std::byte> data) noexcept {
    uint32_t crc = 0xFFFFFFFFu;
    for (const std::byte b : data) {
        const uint8_t idx = static_cast<uint8_t>(
            (crc ^ static_cast<uint32_t>(b)) & 0xFFu);
        crc = (crc >> 8) ^ CRC32C_TABLE[idx];
    }
    return crc ^ 0xFFFFFFFFu;
}

} // namespace pipeutil::detail
