// rpc_pipe_server.hpp — RpcPipeServer 公開 API
// 仕様: spec/F002_rpc_message_id.md §5, §7
#pragma once

#include "pipeutil_export.hpp"
#include "message.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace pipeutil {

// ─────────────────────────────────────────────────────────────────────────────
// RpcPipeServer — message_id 付き RPC リクエスト処理をサポートするパイプサーバー
//
// 機能:
//   - 通常の同期 send() / receive() (listen → accept → send/receive)
//   - RPC サーバー: serve_requests(handler) で背景スレッドが受信ループを回し、
//     FLAG_REQUEST フレームを handler に渡してレスポンスを自動返送する
//
// 制約:
//   - serve_requests() 呼び出し後は receive() / send() の直接呼び出し禁止
//     （受信主体が背景スレッドに移行するため）
// ─────────────────────────────────────────────────────────────────────────────
class PIPEUTIL_API RpcPipeServer {
public:
    explicit RpcPipeServer(std::string pipe_name,
                           std::size_t buffer_size = 65536);

    RpcPipeServer(const RpcPipeServer&)            = delete;
    RpcPipeServer& operator=(const RpcPipeServer&) = delete;
    RpcPipeServer(RpcPipeServer&&)                 noexcept;
    RpcPipeServer& operator=(RpcPipeServer&&)      noexcept;

    ~RpcPipeServer();

    // ─── ライフサイクル ──────────────────────────────────────────────

    void listen();
    void accept(std::chrono::milliseconds timeout = std::chrono::milliseconds{0});

    /// 接続を閉じる (noexcept)
    void close() noexcept;

    // ─── RPC サーバーループ ───────────────────────────────────────────

    using RequestHandler = std::function<Message(const Message& request)>;

    /// 背景スレッドで受信ループを開始し、FLAG_REQUEST フレームを handler に渡す。
    /// handler の戻り値を FLAG_RESPONSE として自動送信する。
    ///
    /// run_in_background = false : stop() が呼ばれるまでブロック
    /// run_in_background = true  : 即座に返る（背景スレッドで動作継続）
    ///
    /// 注意: serve_requests() 呼び出し後は send() / receive() 直接呼び出し禁止
    void serve_requests(RequestHandler handler,
                        bool           run_in_background = false);

    /// serve_requests のループを停止して背景スレッドの終了を待つ (noexcept)
    void stop() noexcept;

    // ─── 通常の同期送受信 (serve_requests の前にのみ使用可) ──────────

    void                send(const Message& msg);
    [[nodiscard]] Message receive(
        std::chrono::milliseconds timeout = std::chrono::milliseconds{0});

    // ─── 状態照会 ─────────────────────────────────────────────────────

    [[nodiscard]] bool               is_listening()  const noexcept;
    [[nodiscard]] bool               is_connected()  const noexcept;
    [[nodiscard]] bool               is_serving()    const noexcept;
    [[nodiscard]] const std::string& pipe_name()     const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pipeutil
