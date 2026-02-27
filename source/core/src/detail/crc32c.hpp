// crc32c.hpp — CRC-32C (Castagnoli) 内部ヘルパー
// 外部依存なし・ソフトウェア実装
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace pipeutil::detail {

/// ペイロードバイト列に対して CRC-32C を計算する
/// \param data  対象バイト列
/// \return CRC-32C 値（ホストバイトオーダー。送信時に to_le32 で変換すること）
[[nodiscard]] uint32_t compute_crc32c(std::span<const std::byte> data) noexcept;

} // namespace pipeutil::detail
