// io_buffer.hpp — ゼロコピー志向の単方向循環バッファ
#pragma once

#include "pipeutil_export.hpp"
#include <atomic>
#include <cstddef>
#include <vector>

namespace pipeutil {

// ──────────────────────────────────────────────────────────────────────────────
// IOBuffer — ロックフリー SPSC（単一プロデューサー / 単一コンシューマー）循環バッファ
//
// スレッドモデル:
//   * write()  を呼ぶスレッドを「プロデューサー」と呼ぶ。
//   * read()   を呼ぶスレッドを「コンシューマー」と呼ぶ。
//   * 同一スレッドから両方を呼ぶ単一スレッド利用も可。
//   * 複数プロデューサー / 複数コンシューマーは非対応。
//
// コピー・ムーブ不可（所有権の一意性を保証）。
// ──────────────────────────────────────────────────────────────────────────────
class PIPEUTIL_API IOBuffer {
public:
    /// capacity: バッファサイズ (バイト単位)
    /// 2 の累乗を推奨（モジュロ演算をビット AND で最適化できる場合あり）
    explicit IOBuffer(std::size_t capacity = 65536);

    IOBuffer(const IOBuffer&)            = delete;
    IOBuffer& operator=(const IOBuffer&) = delete;
    IOBuffer(IOBuffer&&)                 = delete;
    IOBuffer& operator=(IOBuffer&&)      = delete;

    ~IOBuffer() = default;

    // ─── 状態照会 ─────────────────────────────────────────────────────

    [[nodiscard]] std::size_t capacity()      const noexcept;
    [[nodiscard]] std::size_t readable_size() const noexcept;
    [[nodiscard]] std::size_t writable_size() const noexcept;

    // ─── I/O ─────────────────────────────────────────────────────────

    /// src から最大 len バイト書き込む。実際の書き込みバイト数を返す。
    std::size_t write(const std::byte* src, std::size_t len);

    /// dst へ最大 len バイト読み出す。実際の読み出しバイト数を返す。
    std::size_t read(std::byte* dst, std::size_t len);

    /// バッファを空にする（read_pos = write_pos = 0）
    void clear() noexcept;

private:
    std::vector<std::byte>  buf_;
    std::atomic<std::size_t> read_pos_{0};
    std::atomic<std::size_t> write_pos_{0};
};

} // namespace pipeutil
