// multi_pipe_server.hpp — 複数クライアント同時接続サーバー API
// 仕様: spec/F001_multi_pipe_server.md
#pragma once

#include "pipeutil_export.hpp"
#include "pipe_acl.hpp"
#include "pipe_server.hpp"
#include "pipe_stats.hpp"
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace pipeutil {

// ──────────────────────────────────────────────────────────────────────────────
// MultiPipeServer — 複数クライアントを並行処理するサーバー
//
// 使用方法:
//   MultiPipeServer srv{"my_pipe", 8};      // 最大 8 同時接続
//   srv.serve([](PipeServer conn) {
//       auto msg = conn.receive();
//       conn.send(msg);                      // エコー
//   });                                      // ハンドラが返るまでスレッドをブロック
//   // 別スレッドから:
//   srv.stop();
//
// スレッドモデル:
//   * acceptor スレッド × 1: server_accept_and_fork() ループ
//   * handler スレッド × N: detach() した std::thread（スロット上限: max_connections）
//   * stop() は acceptor スレッドを join し、全ハンドラの完了を待機する。
//
// スレッドセーフ: is_serving()/active_connections() は任意のスレッドから安全に呼べる。
//                serve()/stop() は同じ（または外部制御）スレッドから呼ぶこと。
// ──────────────────────────────────────────────────────────────────────────────
class PIPEUTIL_API MultiPipeServer {
public:
    /// pipe_name      : パイプ識別名（PipeServer の論理名と同じ書式）
    /// max_connections: 同時接続数の上限（1 以上 64 以下）
    /// buffer_size    : 各接続の内部バッファサイズ（デフォルト 64 KiB）
    /// acl            : アクセス制御レベル（デフォルト: OS デフォルト，後方互換）
    /// custom_sddl    : PipeAcl::Custom 指定時の SDDL 文字列（Windows のみ有効）
    explicit MultiPipeServer(std::string pipe_name,
                             std::size_t max_connections = 8,
                             std::size_t buffer_size     = 65536,
                             PipeAcl     acl             = PipeAcl::Default,
                             std::string custom_sddl     = "");

    // コピー・ムーブ禁止（acceptor スレッドが self を参照するため）
    MultiPipeServer(const MultiPipeServer&)            = delete;
    MultiPipeServer& operator=(const MultiPipeServer&) = delete;
    MultiPipeServer(MultiPipeServer&&)                 = delete;
    MultiPipeServer& operator=(MultiPipeServer&&)      = delete;

    ~MultiPipeServer();

    // ─── ライフサイクル ──────────────────────────────────────────────

    /// acceptor スレッドを起動して接続を受け付けを開始する（ブロッキング）。
    /// handler は接続ごとに別スレッドで呼び出される。
    /// stop() が呼ばれるまでこの関数は返らない。
    /// 例外: PipeException (SystemError / AccessDenied)
    void serve(std::function<void(PipeServer)> handler);

    /// 実行中の serve() を停止させる（スレッドセーフ・noexcept）。
    /// acceptor スレッドを終了させ、全 handler スレッドの完了を待って返る。
    void stop() noexcept;

    // ─── 状態照会 ─────────────────────────────────────────────────────

    [[nodiscard]] bool        is_serving()         const noexcept;
    [[nodiscard]] std::size_t active_connections() const noexcept;
    [[nodiscard]] const std::string& pipe_name()  const noexcept;

    // ─── 診断・メトリクス (F-006) ─────────────────────────────────────

    /// 全接続分を合算したスナップショットを返す（noexcept）
    [[nodiscard]] PipeStats stats() const noexcept;
    /// アクティブ・終了済み全接続の stats を 0 にリセットする（noexcept）
    void reset_stats() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pipeutil
