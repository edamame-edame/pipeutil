// detail/platform_pipe.hpp — プラットフォーム抽象化インタフェース
#pragma once

#include "pipeutil/pipe_acl.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace pipeutil::detail {

// ──────────────────────────────────────────────────────────────────────────────
// IPlatformPipe — OS 固有パイプ/ソケット操作の純粋仮想インタフェース
//
// 実装クラス:
//   * Win32Pipe  (Windows: 名前付きパイプ, win32_pipe.cpp)
//   * PosixPipe  (Linux: UNIX ドメインソケット, posix_pipe.cpp)
//
// PipeServer / PipeClient の pimpl は IPlatformPipe を所有する。
// ──────────────────────────────────────────────────────────────────────────────
class IPlatformPipe {
public:
    virtual ~IPlatformPipe() = default;

    // ─── サーバー操作 ─────────────────────────────────────────────────

    /// パイプ/ソケットを作成し接続受付可能状態にする
    /// acl        : アクセス制御レベル（PipeAcl::Default で後方互換）
    /// custom_sddl: PipeAcl::Custom のときのみ参照される SDDL 文字列（Linux では無視）
    /// 例外: PipeException (SystemError / AccessDenied / InvalidArgument)
    virtual void server_create(const std::string& pipe_name,
                               PipeAcl acl,
                               const std::string& custom_sddl) = 0;

    /// クライアント接続を待機する（ブロッキング）
    /// timeout_ms == 0 → 無限待機
    /// 例外: PipeException (Timeout / SystemError)
    virtual void server_accept(int64_t timeout_ms) = 0;

    /// クライアント接続を待機し、接続済みパイプを別インスタンスとして返す（fork）。
    /// 自身は次クライアントを受け付けられる状態を維持する。
    /// timeout_ms == 0 → 無限待機
    /// 例外: PipeException (Timeout / Interrupted / SystemError)
    virtual std::unique_ptr<IPlatformPipe> server_accept_and_fork(int64_t timeout_ms) = 0;

    /// 進行中の server_accept_and_fork() を非同期的に中断させる（noexcept）。
    /// 呼び出し後、実行中の server_accept_and_fork() は Interrupted を送出して返る。
    virtual void stop_accept() noexcept = 0;

    /// サーバー側リソースを解放する（noexcept）
    virtual void server_close() noexcept = 0;

    // ─── クライアント操作 ─────────────────────────────────────────────

    /// サーバーに接続する（ブロッキング）
    /// 例外: PipeException (Timeout / NotFound / SystemError)
    virtual void client_connect(const std::string& pipe_name,
                                int64_t timeout_ms) = 0;

    /// クライアント側リソースを解放する（noexcept）
    virtual void client_close() noexcept = 0;

    // ─── 共通 I/O（全バイト完了を保証するブロッキング操作）────────────

    /// 指定バイト数を全て送信する
    /// 例外: PipeException (BrokenPipe / NotConnected / SystemError)
    virtual void write_all(const std::byte* data, std::size_t size) = 0;

    /// 指定バイト数を全て受信する
    /// timeout_ms == 0 → 無限待機
    /// 例外: PipeException (Timeout / ConnectionReset / SystemError)
    virtual void read_all(std::byte* buf,
                          std::size_t size,
                          int64_t timeout_ms) = 0;

    // ─── 状態照会 ─────────────────────────────────────────────────────

    [[nodiscard]] virtual bool is_server_listening() const noexcept = 0;
    [[nodiscard]] virtual bool is_connected()        const noexcept = 0;
};

} // namespace pipeutil::detail
