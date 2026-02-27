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
    HANDLE       hPipe_     = INVALID_HANDLE_VALUE; // サーバー: pipe HANDLE / クライアント: 同一
    bool         listening_ = false;
    bool         connected_ = false;
    std::size_t  buf_size_;

    /// 論理名 → Windows 名前付きパイプパス変換
    static std::wstring to_pipe_path(const std::string& name);

    /// OVERLAPPED を使用した完全書き込み（ループ）
    void overlapped_write_all(const std::byte* data, std::size_t size);

    /// OVERLAPPED を使用した完全読み取り（ループ + タイムアウト）
    void overlapped_read_all(std::byte* buf, std::size_t size, DWORD timeout_ms_dw);
};

} // namespace pipeutil::detail

#endif // _WIN32
