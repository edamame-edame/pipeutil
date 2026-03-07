// win32_pipe.hpp — Windows 名前付きパイプ実装 (internal)
#pragma once

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include "pipeutil/detail/platform_pipe.hpp"
#include <memory>
#include <string>

namespace pipeutil::detail {

class Win32Pipe final : public IPlatformPipe {
public:
    explicit Win32Pipe(std::size_t buf_size);
    ~Win32Pipe() override;

    // コピー・ムーブ禁止（HANDLE 管理を単純化）
    Win32Pipe(const Win32Pipe&)            = delete;
    Win32Pipe& operator=(const Win32Pipe&) = delete;
    Win32Pipe(Win32Pipe&&)                 = delete;
    Win32Pipe& operator=(Win32Pipe&&)      = delete;

    // サーバー操作
    void server_create(const std::string& pipe_name) override;
    void server_accept(int64_t timeout_ms)            override;
    std::unique_ptr<IPlatformPipe> server_accept_and_fork(int64_t timeout_ms) override;
    void stop_accept() noexcept                       override;
    void server_close() noexcept                      override;

    // クライアント操作
    void client_connect(const std::string& pipe_name,
                        int64_t timeout_ms)            override;
    void client_close() noexcept                      override;

    // 共通 I/O
    void write_all(const std::byte* data, std::size_t size)           override;
    void read_all (std::byte* buf,        std::size_t size,
                   int64_t timeout_ms)                                 override;

    // 状態照会
    [[nodiscard]] bool is_server_listening() const noexcept override;
    [[nodiscard]] bool is_connected()        const noexcept override;

private:
    HANDLE       hPipe_          = INVALID_HANDLE_VALUE; // サーバー/クライアント共用 HANDLE
    HANDLE       stop_event_     = nullptr;              // manual-reset イベント（stop_accept 用）
    // accept 用 OVERLAPPED をメンバーとして保持（スタック上に置くと lifetime 問題が起きる）
    // stop path では GetOverlappedResult を呼ばず投げてよい: I/O は server_close で hPipe_ を
    // 閉じた際に自動キャンセルされ、accept_ov_ が書き換えられても安全。
    OVERLAPPED   accept_ov_      = {};                   // ConnectNamedPipe 用 OVERLAPPED
    HANDLE       accept_event_   = nullptr;              // accept_ov_.hEvent
    bool         listening_      = false;
    bool         connected_      = false;
    std::size_t  buf_size_;
    std::wstring pipe_name_wstr_;  // server_accept_and_fork で再作成するために保存

    /// 論理名 → Windows 名前付きパイプパス変換
    static std::wstring to_pipe_path(const std::string& name);

    /// OVERLAPPED を使用した完全書き込み（ループ）
    void overlapped_write_all(const std::byte* data, std::size_t size);

    /// OVERLAPPED を使用した完全読み取り（ループ + タイムアウト）
    void overlapped_read_all(std::byte* buf, std::size_t size, DWORD timeout_ms_dw);
};

} // namespace pipeutil::detail

#endif // _WIN32
