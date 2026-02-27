// posix_pipe.hpp — POSIX UNIX ドメインソケット実装 (internal)
#pragma once

#ifndef _WIN32

#include "pipeutil/detail/platform_pipe.hpp"
#include <string>

namespace pipeutil::detail {

class PosixPipe final : public IPlatformPipe {
public:
    explicit PosixPipe(std::size_t buf_size);
    ~PosixPipe() override;

    PosixPipe(const PosixPipe&)            = delete;
    PosixPipe& operator=(const PosixPipe&) = delete;
    PosixPipe(PosixPipe&&)                 = delete;
    PosixPipe& operator=(PosixPipe&&)      = delete;

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
    int         server_fd_ = -1;   // listen ソケット
    int         client_fd_ = -1;   // accept/connect 後の I/O ソケット
    bool        listening_ = false;
    bool        connected_ = false;
    std::size_t buf_size_;
    std::string sock_path_;        // バインドしたソケットファイルパス（クローズ時に unlink）

    /// 論理名 → UNIX ドメインソケットパス変換
    static std::string to_sock_path(const std::string& name);

    /// /tmp/pipeutil/ ディレクトリが存在しなければ作成する
    static void ensure_dir();

    /// poll + recv による完全読み取り
    void timed_read_all(std::byte* buf, std::size_t size, int timeout_ms_int);
};

} // namespace pipeutil::detail

#endif // !_WIN32
