// rpc_pipe_client.hpp — RpcPipeClient 公開 API
// 仕様: spec/F002_rpc_message_id.md §4, §6
#pragma once

#include "pipeutil_export.hpp"
#include "message.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace pipeutil {

// ─────────────────────────────────────────────────────────────────────────────
// RpcPipeClient — message_id 付き RPC 送受信をサポートするパイプクライアント
//
// 機能:
//   - 通常の同期 send() / receive() (message_id = 0)
//   - RPC リクエスト: send_request() でメッセージ ID を付与して送信し、
//     対応する応答が届くまで待機する（背景受信スレッドが ID で振り分け）
//
// スレッド安全性:
//   - send() / send_request() は内部 io_mutex_ で保護されており、複数スレッドから呼べる
//   - 背景受信スレッドは connect() 後に 1 本起動し、close() で停止する
// ─────────────────────────────────────────────────────────────────────────────
class PIPEUTIL_API RpcPipeClient {
public:
    explicit RpcPipeClient(std::string pipe_name,
                           std::size_t buffer_size = 65536);

    RpcPipeClient(const RpcPipeClient&)            = delete;
    RpcPipeClient& operator=(const RpcPipeClient&) = delete;
    RpcPipeClient(RpcPipeClient&&)                 noexcept;
    RpcPipeClient& operator=(RpcPipeClient&&)      noexcept;

    ~RpcPipeClient();

    // ─── ライフサイクル ──────────────────────────────────────────────

    /// サーバーに接続して背景受信スレッドを起動する
    /// timeout = 0 → 無限待機
    /// 例外: PipeException (Timeout / NotFound / SystemError)
    void connect(std::chrono::milliseconds timeout = std::chrono::milliseconds{0});

    /// 接続を閉じ、背景スレッドを停止する (noexcept)
    void close() noexcept;

    // ─── 通常の同期送受信 (message_id = 0) ───────────────────────────

    /// フレームを送信する (FLAG_REQUEST なし、message_id = 0)
    void send(const Message& msg);

    /// フレームを受信する (message_id = 0 のキューから取り出す)
    /// timeout = 0 → 無限待機
    [[nodiscard]] Message receive(
        std::chrono::milliseconds timeout = std::chrono::milliseconds{0});

    // ─── RPC: リクエスト–レスポンス ──────────────────────────────────

    /// フレームに message_id と FLAG_REQUEST を付与して送信し、
    /// 対応する応答フレーム（FLAG_RESPONSE）が届くまで待機する。
    ///
    /// timeout = 0 → 無限待機
    /// 例外: PipeException (Timeout / ConnectionReset / BrokenPipe)
    [[nodiscard]] Message send_request(
        const Message&            request,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});

    // ─── 状態照会 ─────────────────────────────────────────────────────

    [[nodiscard]] bool               is_connected() const noexcept;
    [[nodiscard]] const std::string& pipe_name()    const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pipeutil
