// detail/endian.hpp — エンディアン変換ユーティリティ（C++20 準拠）
// R-008 対応: GCC/Clang 固有マクロを排除し std::endian (<bit>) を使用する。
#pragma once

#include <bit>      // std::endian（C++20）
#include <cstdint>

namespace pipeutil::detail {

// ──────────────────────────────────────────────────────────────────────────────
// to_le32 / from_le32 — ホストバイトオーダー ↔ リトルエンディアン変換
//
// * リトルエンディアン環境（x86/x64/ARM LE）ではゼロコスト（no-op）。
// * ビッグエンディアン環境（PowerPC/SPARC 等）ではバイトスワップを行う。
// * constexpr のため、定数式でも使用可能。
// * MSVC / GCC / Clang 全て std::endian に対応（C++20 以降）。
// ──────────────────────────────────────────────────────────────────────────────

constexpr uint32_t to_le32(uint32_t v) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return v;  // ネイティブが LE → 変換不要
    } else {
        // ビッグエンディアン環境用バイトスワップ
        // コンパイラは BSWAP 命令等に最適化する。
        return ((v & 0x000000FFu) << 24u)
             | ((v & 0x0000FF00u) <<  8u)
             | ((v & 0x00FF0000u) >>  8u)
             | ((v & 0xFF000000u) >> 24u);
    }
}

constexpr uint32_t from_le32(uint32_t v) noexcept {
    return to_le32(v);  // バイトスワップは対称操作
}

} // namespace pipeutil::detail
