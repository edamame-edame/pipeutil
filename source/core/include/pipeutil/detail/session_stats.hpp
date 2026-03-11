// detail/session_stats.hpp — MultiPipeServer セッション単位の統計バッファ（内部実装専用）
// PipeServer::Impl が send/receive 時にここへ書き込み、MultiPipeServer::Impl が集約する。
// 公開 API ではない。
#pragma once

#include "pipeutil/pipe_stats.hpp"

#include <atomic>
#include <cstdint>

namespace pipeutil::detail {

/// MultiPipeServer が各接続スレッドの送受信数を収集する内部バッファ。
/// std::atomic フィールドにより PipeServer の send/receive スレッドからロックなしで書き込み可。
/// snapshot() は memory_order_relaxed でスナップショットを取得する（診断用途で十分）。
struct SessionStats {
    std::atomic<uint64_t> messages_sent{0};
    std::atomic<uint64_t> messages_recv{0};
    std::atomic<uint64_t> bytes_sent   {0};
    std::atomic<uint64_t> bytes_recv   {0};
    std::atomic<uint64_t> errors       {0};

    [[nodiscard]] pipeutil::PipeStats snapshot() const noexcept {
        pipeutil::PipeStats s;
        s.messages_sent     = messages_sent.load(std::memory_order_relaxed);
        s.messages_received = messages_recv.load(std::memory_order_relaxed);
        s.bytes_sent        = bytes_sent.load(std::memory_order_relaxed);
        s.bytes_received    = bytes_recv.load(std::memory_order_relaxed);
        s.errors            = errors.load(std::memory_order_relaxed);
        return s;
    }

    void reset() noexcept {
        messages_sent.store(0, std::memory_order_relaxed);
        messages_recv.store(0, std::memory_order_relaxed);
        bytes_sent.store(0, std::memory_order_relaxed);
        bytes_recv.store(0, std::memory_order_relaxed);
        errors.store(0, std::memory_order_relaxed);
    }
};

} // namespace pipeutil::detail
