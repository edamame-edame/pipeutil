// pipe_client.hpp — IPC パイプクライアント側 API
#pragma once

#include "pipeutil_export.hpp"
#include "message.hpp"
#include <chrono>
#include <memory>
#include <string>

namespace pipeutil {

// ──────────────────────────────────────────────────────────────────────────────
// PipeClient — クライアント側: connect → send/receive → close の順で使用
//
// pipe_name は PipeServer に渡した識別名と同一を指定する。
// OS 実体名への変換はライブラリが自動的に行う。
//
// スレッドセーフ: send() / receive() は複数スレッドから安全に利用可。
//                connect() は単一スレッドから呼び出すこと。
// ──────────────────────────────────────────────────────────────────────────────
class PIPEUTIL_API PipeClient {
public:
    explicit PipeClient(std::string pipe_name,
                        std::size_t buffer_size = 65536);

    PipeClient(const PipeClient&)            = delete;
    PipeClient& operator=(const PipeClient&) = delete;
    PipeClient(PipeClient&&)                 noexcept;
    PipeClient& operator=(PipeClient&&)      noexcept;

    ~PipeClient();

    // ─── ライフサイクル ──────────────────────────────────────────────

    /// サーバーに接続する（サーバーが listen 中になるまでリトライ）
    /// timeout == 0 → 無限待機
    /// timeout > 0  → タイムアウト後に PipeException(Timeout) を送出
    /// 例外: PipeException (Timeout / NotFound / SystemError)
    void connect(std::chrono::milliseconds timeout = std::chrono::milliseconds{0});

    /// 接続を閉じてリソースを解放する（noexcept）
    void close() noexcept;

    // ─── I/O ─────────────────────────────────────────────────────────

    /// フレーム付きメッセージを送信する（ブロッキング）
    /// 例外: PipeException (BrokenPipe / NotConnected / SystemError)
    void send(const Message& msg);

    /// フレーム付きメッセージを受信する（ブロッキング）
    /// timeout == 0 → 無限待機
    /// 例外: PipeException (Timeout / ConnectionReset / InvalidMessage)
    [[nodiscard]] Message receive(
        std::chrono::milliseconds timeout = std::chrono::milliseconds{0});

    // ─── 状態照会 ─────────────────────────────────────────────────────

    [[nodiscard]] bool               is_connected() const noexcept;
    [[nodiscard]] const std::string& pipe_name()    const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pipeutil
