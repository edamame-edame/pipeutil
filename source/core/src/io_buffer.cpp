// io_buffer.cpp — SPSC 循環バッファ実装
#include "pipeutil/io_buffer.hpp"
#include <algorithm>
#include <cassert>

namespace pipeutil {

IOBuffer::IOBuffer(std::size_t capacity)
    : buf_(capacity)
{}

std::size_t IOBuffer::capacity() const noexcept {
    return buf_.size();
}

std::size_t IOBuffer::readable_size() const noexcept {
    const std::size_t w = write_pos_.load(std::memory_order_acquire);
    const std::size_t r = read_pos_.load(std::memory_order_relaxed);
    return w - r;  // アンダーフローしないよう write >= read を不変条件とする
}

std::size_t IOBuffer::writable_size() const noexcept {
    return buf_.size() - readable_size();
}

std::size_t IOBuffer::write(const std::byte* src, std::size_t len) {
    const std::size_t cap   = buf_.size();
    const std::size_t avail = writable_size();
    const std::size_t n     = std::min(len, avail);
    if (n == 0) return 0;

    // 書き込み位置（循環インデックス）
    const std::size_t w_idx = write_pos_.load(std::memory_order_relaxed) % cap;

    // バッファ末尾で折り返す場合は 2 回に分けてコピー
    const std::size_t first = std::min(n, cap - w_idx);
    std::memcpy(buf_.data() + w_idx, src, first);
    if (first < n) {
        std::memcpy(buf_.data(), src + first, n - first);
    }

    // write_pos を更新（コンシューマーから見えるようにする）
    write_pos_.fetch_add(n, std::memory_order_release);
    return n;
}

std::size_t IOBuffer::read(std::byte* dst, std::size_t len) {
    const std::size_t cap  = buf_.size();
    const std::size_t avail = readable_size();
    const std::size_t n    = std::min(len, avail);
    if (n == 0) return 0;

    // 読み出し位置（循環インデックス）
    const std::size_t r_idx = read_pos_.load(std::memory_order_relaxed) % cap;

    const std::size_t first = std::min(n, cap - r_idx);
    std::memcpy(dst, buf_.data() + r_idx, first);
    if (first < n) {
        std::memcpy(dst + first, buf_.data(), n - first);
    }

    read_pos_.fetch_add(n, std::memory_order_release);
    return n;
}

void IOBuffer::clear() noexcept {
    read_pos_.store(0,  std::memory_order_relaxed);
    write_pos_.store(0, std::memory_order_relaxed);
}

} // namespace pipeutil
