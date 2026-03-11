// pipe_stats.hpp — 診断・メトリクス API (F-006)
// 仕様: spec/F006_diagnostics_metrics.md §3.1
#pragma once

#include "pipeutil_export.hpp"
#include <cstdint>

namespace pipeutil {

// ─────────────────────────────────────────────────────────────────────────────
// PipeStats — stats() が返す値オブジェクト（コピー可）
//
// 全カウンタ初期値は 0。取得時点のスナップショットであり、
// その後の送受信によって更新されない。
// ─────────────────────────────────────────────────────────────────────────────
struct PIPEUTIL_API PipeStats {
    // ─── 送受信カウンタ ──────────────────────────────────────────────────────
    /// 送信に成功したメッセージ数（例外なしで完了した send() 回数）
    std::uint64_t messages_sent     = 0;
    /// 受信に成功したメッセージ数（例外なしで完了した receive() 回数）
    std::uint64_t messages_received = 0;
    /// 送信したペイロードの総バイト数（フレームヘッダ 20 バイト除く）
    std::uint64_t bytes_sent        = 0;
    /// 受信したペイロードの総バイト数（フレームヘッダ 20 バイト除く）
    std::uint64_t bytes_received    = 0;
    /// PipeException をキャッチした総数（1 send/receive 呼び出し = 最大 1 カウント）
    std::uint64_t errors            = 0;

    // ─── RPC ラウンドトリップ（RpcPipeClient のみ有効） ──────────────────────
    /// send_request() が正常完了した回数（PipeClient / PipeServer では常に 0）
    std::uint64_t rpc_calls         = 0;
    /// send_request() のラウンドトリップ合計時間（ナノ秒）
    std::uint64_t rtt_total_ns      = 0;
    /// 最後の send_request() のラウンドトリップ時間（ナノ秒）
    /// rpc_calls == 0 の場合は 0
    std::uint64_t rtt_last_ns       = 0;

    // ─── 便利メソッド ────────────────────────────────────────────────────────
    /// send_request() のラウンドトリップ平均（ナノ秒）を返す。
    /// rpc_calls == 0 の場合は 0 を返す。
    [[nodiscard]] std::uint64_t avg_round_trip_ns() const noexcept {
        return rpc_calls > 0 ? rtt_total_ns / rpc_calls : 0;
    }

    // ─── 演算子 ──────────────────────────────────────────────────────────────
    /// MultiPipeServer が複数接続分を合算する用途で使用する。
    /// rtt_last_ns は lhs の値を維持する（加算に意味がないため）。
    PipeStats& operator+=(const PipeStats& rhs) noexcept;
};

/// 加算演算子（非メンバー）
PIPEUTIL_API PipeStats operator+(PipeStats lhs, const PipeStats& rhs) noexcept;

} // namespace pipeutil
