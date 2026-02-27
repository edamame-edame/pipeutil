// pipe_server.hpp — IPC パイプサーバー側 API
#pragma once

#include "pipeutil_export.hpp"
#include "message.hpp"
#include <chrono>
#include <memory>
#include <string>

namespace pipeutil {

// ──────────────────────────────────────────────────────────────────────────────
// PipeServer — サーバー側: listen → accept → send/receive → close の順で使用
//
// パイプ識別名（論理名）の規則:
//   * 英数字・アンダースコア・ハイフンのみ使用推奨（スラッシュ・バックスラッシュ禁止）
//   * OS 実体名はライブラリが内部で自動生成する:
//       Windows: "\\.\pipe\pipeutil_<name>"  ← "pipeutil_" を自動付与
//       Linux:   "/tmp/pipeutil/<name>.sock"
//
// スレッドセーフ: send() / receive() はミューテックス保護で複数スレッドから安全に利用可。
//                listen() / accept() は単一スレッドから呼び出すこと。
// ──────────────────────────────────────────────────────────────────────────────
class PIPEUTIL_API PipeServer {
public:
    /// pipe_name : パイプ識別名（論理名。OS プレフィックスなし）
    /// buffer_size: 内部 IOBuffer のサイズ（デフォルト 64 KiB）
    explicit PipeServer(std::string pipe_name,
                        std::size_t buffer_size = 65536);

    PipeServer(const PipeServer&)            = delete;
    PipeServer& operator=(const PipeServer&) = delete;
    PipeServer(PipeServer&&)                 noexcept;
    PipeServer& operator=(PipeServer&&)      noexcept;

    ~PipeServer();

    // ─── ライフサイクル ──────────────────────────────────────────────

    /// パイプを作成して接続待ち状態にする
    /// 例外: PipeException (SystemError / AccessDenied)
    void listen();

    /// クライアントが接続するまでブロックして待機する
    /// timeout == 0  → 無限待機
    /// timeout > 0   → タイムアウト後に PipeException(Timeout) を送出
    void accept(std::chrono::milliseconds timeout = std::chrono::milliseconds{0});

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

    [[nodiscard]] bool               is_listening() const noexcept;
    [[nodiscard]] bool               is_connected() const noexcept;
    [[nodiscard]] const std::string& pipe_name()    const noexcept;

private:
    // pimpl イディオム: プラットフォーム依存実装を .cpp に隠蔽
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pipeutil
