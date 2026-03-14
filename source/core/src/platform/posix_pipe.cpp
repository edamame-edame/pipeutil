// posix_pipe.cpp — POSIX UNIX ドメインソケット実装
// 仕様: spec/03_platform.md §4
#ifndef _WIN32

#include "platform/posix_pipe.hpp"
#include "pipeutil/pipe_error.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cerrno>
#include <climits>
#include <chrono>
#include <thread>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace pipeutil::detail {

// ─── ユーティリティ ────────────────────────────────────────────────

/// errno を PipeErrorCode にマッピングする
static pipeutil::PipeErrorCode map_errno(int e) noexcept {
    switch (e) {
        case EPIPE:
        case ECONNRESET:  return pipeutil::PipeErrorCode::BrokenPipe;
        case ENOENT:
        case ECONNREFUSED: return pipeutil::PipeErrorCode::NotFound;
        case EACCES:       return pipeutil::PipeErrorCode::AccessDenied;
        case ETIMEDOUT:    return pipeutil::PipeErrorCode::Timeout;
        default:           return pipeutil::PipeErrorCode::SystemError;
    }
}

// ─── PosixPipe ────────────────────────────────────────────────────

PosixPipe::PosixPipe(std::size_t buf_size) : buf_size_(buf_size) {}

PosixPipe::~PosixPipe() {
    server_close();
    client_close();
}

std::string PosixPipe::to_sock_path(const std::string& name) {
    // sun_path 最大108バイト制約チェック（プレフィックス含む）
    const std::string path = "/tmp/pipeutil/" + name + ".sock";
    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::InvalidArgument,
                                      "Pipe name too long for UNIX domain socket path"};
    }
    return path;
}

void PosixPipe::ensure_dir() {
    // /tmp/pipeutil/ が存在しなければ作成（モード 0700）
    if (mkdir("/tmp/pipeutil", 0700) != 0) {
        if (errno != EEXIST) {
            throw pipeutil::PipeException{pipeutil::PipeErrorCode::SystemError,
                                          "Failed to create /tmp/pipeutil/"};
        }
        // 既存ディレクトリの場合: 所有者・モードを検証し、不正権限なら拒否する (R-073)
        // lstat() を使いシンボリックリンクを追わない。S_ISDIR で実体型を確認する (R-074)
        struct stat st{};
        if (lstat("/tmp/pipeutil", &st) != 0) {
            throw pipeutil::PipeException{pipeutil::PipeErrorCode::SystemError,
                                          "lstat() failed on /tmp/pipeutil/"};
        }
        // シンボリックリンクや非ディレクトリ（通常ファイル等）は即拒否 (R-074)
        if (!S_ISDIR(st.st_mode)) {
            throw pipeutil::PipeException{pipeutil::PipeErrorCode::AccessDenied,
                                          "/tmp/pipeutil exists but is not a directory"};
        }
        // 所有者が自プロセスの UID と一致しない場合は拒否（他ユーザーが先にディレクトリを作成した）
        if (st.st_uid != ::getuid()) {
            throw pipeutil::PipeException{pipeutil::PipeErrorCode::AccessDenied,
                                          "/tmp/pipeutil/ is owned by a different user"};
        }
        // パーミッションが 0700 より広い場合は拒否（グループ/その他への読み書き実行ビットを禁止）
        if ((st.st_mode & 0777) != 0700) {
            // 矯正: 権限を 0700 に絞る（失敗した場合は AccessDenied で終了）
            if (chmod("/tmp/pipeutil", 0700) != 0) {
                throw pipeutil::PipeException{pipeutil::PipeErrorCode::AccessDenied,
                                              "/tmp/pipeutil/ has unsafe permissions and chmod failed"};
            }
        }
    }
}

// ─── サーバー操作 ─────────────────────────────────────────────────

void PosixPipe::server_create(const std::string& pipe_name,
                               pipeutil::PipeAcl acl,
                               const std::string& /*custom_sddl*/) {
    // Linux では PipeAcl::Custom（SDDL 文字列）は非対応
    if (acl == pipeutil::PipeAcl::Custom) {
        throw pipeutil::PipeException{
            pipeutil::PipeErrorCode::InvalidArgument,
            "PipeAcl::Custom is not supported on Linux; use Default/LocalSystem/Everyone"};
    }

    if (listening_) {
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::AlreadyConnected,
                                      "Already listening"};
    }

    ensure_dir();
    sock_path_ = to_sock_path(pipe_name);

    // 1. ソケット作成
    server_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server_fd_ < 0) {
        throw pipeutil::PipeException{map_errno(errno), "socket() failed"};
    }

    // 2. 既存ソケットファイル削除（前回クラッシュ残留対策）
    unlink(sock_path_.c_str());

    // 3. バインド
    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sock_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(server_fd_, reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) != 0) {
        const int e = errno;
        close(server_fd_);
        server_fd_ = -1;
        throw pipeutil::PipeException{map_errno(e), "bind() failed"};
    }

    // 4. ACL に応じたソケットファイルパーミッション設定（Default: umask のまま変更しない）
    if (acl != pipeutil::PipeAcl::Default) {
        const mode_t mode = (acl == pipeutil::PipeAcl::Everyone) ? 0666 : 0600;
        if (chmod(sock_path_.c_str(), mode) != 0) {
            const int e = errno;
            close(server_fd_);
            server_fd_ = -1;
            throw pipeutil::PipeException{map_errno(e), "chmod failed on socket file"};
        }
    }

    // 5. listen（バックログ 1）
    if (listen(server_fd_, 1) != 0) {
        const int e = errno;
        close(server_fd_);
        server_fd_ = -1;
        throw pipeutil::PipeException{map_errno(e), "listen() failed"};
    }

    listening_ = true;
}

void PosixPipe::server_accept(int64_t timeout_ms) {
    if (!listening_) {
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::NotConnected,
                                      "server_create() must be called before server_accept()"};
    }

    // poll でタイムアウト制御
    pollfd pfd{};
    pfd.fd     = server_fd_;
    pfd.events = POLLIN;
    const int timeout_int = (timeout_ms == 0)
                              ? -1
                              : static_cast<int>(timeout_ms);
    const int ret = poll(&pfd, 1, timeout_int);
    if (ret == 0) throw pipeutil::PipeException{pipeutil::PipeErrorCode::Timeout};
    if (ret < 0)  throw pipeutil::PipeException{map_errno(errno), "poll() failed"};

    client_fd_ = accept4(server_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (client_fd_ < 0) {
        throw pipeutil::PipeException{map_errno(errno), "accept4() failed"};
    }
    connected_ = true;
}

void PosixPipe::stop_accept() noexcept {
    if (stop_fd_[1] >= 0) {
        // stop_accept_and_fork() の poll を起床させるためにバイトを書き込む
        const char c = 'x';
        while (write(stop_fd_[1], &c, 1) < 0 && errno == EINTR) {}  // EINTR リトライ
    }
}

std::unique_ptr<IPlatformPipe> PosixPipe::server_accept_and_fork(int64_t timeout_ms) {
    if (!listening_) {
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::NotConnected,
                                      "server_create() must be called before server_accept_and_fork()"};
    }

    // stop_fd_ pair を初回作成（O_CLOEXEC 付き）
    if (stop_fd_[0] < 0) {
        if (pipe(stop_fd_) != 0) {
            throw pipeutil::PipeException{map_errno(errno), "pipe() failed (stop_fd)"};
        }
        // O_CLOEXEC を設定して exec 時に自動クローズ
        for (int fd : stop_fd_) {
            fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
    }

    pollfd pfds[2]{};
    pfds[0].fd     = server_fd_;
    pfds[0].events = POLLIN;
    pfds[1].fd     = stop_fd_[0];
    pfds[1].events = POLLIN;

    const int timeout_int = (timeout_ms == 0)
                              ? -1
                              : static_cast<int>(timeout_ms);
    const int ret = poll(pfds, 2, timeout_int);
    if (ret == 0) throw pipeutil::PipeException{pipeutil::PipeErrorCode::Timeout};
    if (ret < 0)  throw pipeutil::PipeException{map_errno(errno), "poll() failed"};

    // stop_accept() によって stop_fd_[0] に書き込みがあった場合
    if (pfds[1].revents & POLLIN) {
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::Interrupted,
                                      "accept cancelled by stop_accept()"};
    }

    const int client_fd = accept4(server_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (client_fd < 0) {
        throw pipeutil::PipeException{map_errno(errno), "accept4() failed"};
    }

    // fork: 新しい PosixPipe インスタンスに接続済み fd のみを設定して返す
    auto forked = std::make_unique<PosixPipe>(buf_size_);
    forked->client_fd_ = client_fd;
    forked->connected_ = true;

    return forked;
}

void PosixPipe::server_close() noexcept {
    if (client_fd_ >= 0) {
        // 接続中のクライアント fd に対しても shutdown でブロック中スレッドをアンブロック
        shutdown(client_fd_, SHUT_RDWR);
        close(client_fd_);
        client_fd_ = -1;
    }
    if (server_fd_ >= 0) {
        close(server_fd_);
        server_fd_ = -1;
        // ソケットファイルを削除してクリーンアップ
        if (!sock_path_.empty()) {
            unlink(sock_path_.c_str());
            sock_path_.clear();
        }
    }
    // stop_accept 用パイプを閉じる
    for (int& fd : stop_fd_) {
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    }
    listening_ = false;
    connected_ = false;
}

// ─── クライアント操作 ─────────────────────────────────────────────

void PosixPipe::client_connect(const std::string& pipe_name, int64_t timeout_ms) {
    if (connected_) {
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::AlreadyConnected,
                                      "Already connected"};
    }

    const std::string path = to_sock_path(pipe_name);

    client_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (client_fd_ < 0) {
        throw pipeutil::PipeException{map_errno(errno), "socket() failed"};
    }

    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds{timeout_ms};

    while (true) {
        if (connect(client_fd_,
                    reinterpret_cast<sockaddr*>(&addr),
                    sizeof(addr)) == 0) {
            break; // 接続成功
        }

        const int e = errno;
        if (e == ENOENT || e == ECONNREFUSED) {
            // サーバーが未起動 / ソケットファイルが存在しない → リトライ
            if (timeout_ms > 0 && std::chrono::steady_clock::now() >= deadline) {
                close(client_fd_);
                client_fd_ = -1;
                throw pipeutil::PipeException{pipeutil::PipeErrorCode::Timeout};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
            continue;
        }

        close(client_fd_);
        client_fd_ = -1;
        throw pipeutil::PipeException{map_errno(e), "connect() failed"};
    }

    connected_ = true;
}

void PosixPipe::client_close() noexcept {
    if (client_fd_ >= 0) {
        // shutdown(SHUT_RDWR) で他スレッドが poll()/recv() でブロックしている場合に
        // EOF を通知してアンブロックしてから close する。
        // Linux では close(fd) だけでは他スレッドの poll() は解除されない（POSIX 未定義）。
        shutdown(client_fd_, SHUT_RDWR);
        close(client_fd_);
        client_fd_ = -1;
    }
    connected_ = false;
}

// ─── 共通 I/O ─────────────────────────────────────────────────────

void PosixPipe::write_all(const std::byte* data, std::size_t size) {
    const std::byte* ptr = data;
    std::size_t remaining = size;
    const int fd = (client_fd_ >= 0) ? client_fd_ : -1;
    if (fd < 0) {
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::NotConnected,
                                      "Not connected"};
    }

    while (remaining > 0) {
        const ssize_t n = send(fd, ptr, remaining, MSG_NOSIGNAL);
        if (n < 0) {
            const int e = errno;
            if (e == EINTR) continue;
            if (e == EPIPE) {
                connected_ = false;
                throw pipeutil::PipeException{pipeutil::PipeErrorCode::BrokenPipe};
            }
            throw pipeutil::PipeException{map_errno(e), "send() failed"};
        }
        ptr       += n;
        remaining -= static_cast<std::size_t>(n);
    }
}

void PosixPipe::read_all(std::byte* buf, std::size_t size, int64_t timeout_ms) {
    timed_read_all(buf, size,
                   (timeout_ms == 0) ? -1 : static_cast<int>(timeout_ms));
}

void PosixPipe::timed_read_all(std::byte* buf, std::size_t size, int timeout_ms_int) {
    std::byte* ptr = buf;
    std::size_t remaining = size;
    const int fd = (client_fd_ >= 0) ? client_fd_ : -1;
    if (fd < 0) {
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::NotConnected,
                                      "Not connected"};
    }

    // -1 は無限待機。それ以外はデッドラインを計算して全チャンク合計に上限を設ける (R-015)
    const bool infinite = (timeout_ms_int < 0);
    using Clock = std::chrono::steady_clock;
    const auto deadline = infinite
        ? Clock::time_point::max()
        : Clock::now() + std::chrono::milliseconds{timeout_ms_int};

    while (remaining > 0) {
        // チャンクごとの残り待機時間を算出
        int chunk_timeout_ms = timeout_ms_int;
        if (!infinite) {
            const auto now  = Clock::now();
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now).count();
            if (left <= 0) {
                throw pipeutil::PipeException{pipeutil::PipeErrorCode::Timeout};
            }
            chunk_timeout_ms = (left > static_cast<long long>(INT_MAX))
                ? INT_MAX
                : static_cast<int>(left);
        }

        // poll でタイムアウト付き待機
        pollfd pfd{};
        pfd.fd     = fd;
        pfd.events = POLLIN;
        const int ret = poll(&pfd, 1, chunk_timeout_ms);
        if (ret == 0) throw pipeutil::PipeException{pipeutil::PipeErrorCode::Timeout};
        if (ret < 0) {
            const int e = errno;
            if (e == EINTR) continue;
            throw pipeutil::PipeException{map_errno(e), "poll() failed"};
        }

        const ssize_t n = recv(fd, ptr, remaining, 0);
        if (n < 0) {
            const int e = errno;
            if (e == EINTR) continue;
            connected_ = false;
            throw pipeutil::PipeException{map_errno(e), "recv() failed"};
        }
        if (n == 0) {
            // EOF
            connected_ = false;
            throw pipeutil::PipeException{pipeutil::PipeErrorCode::ConnectionReset,
                                          "EOF on socket"};
        }
        ptr       += n;
        remaining -= static_cast<std::size_t>(n);
    }
}

// ─── 状態照会 ─────────────────────────────────────────────────────

bool PosixPipe::is_server_listening() const noexcept { return listening_; }
bool PosixPipe::is_connected()        const noexcept { return connected_; }

} // namespace pipeutil::detail

#endif // !_WIN32
