// win32_pipe.cpp — Windows 名前付きパイプ実装
// 仕様: spec/03_platform.md §3
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include "platform/win32_pipe.hpp"
#include "pipeutil/pipe_error.hpp"

#include <stdexcept>
#include <chrono>
#include <thread>

namespace pipeutil::detail {

// ─── ユーティリティ ────────────────────────────────────────────────

/// UTF-8 文字列 → UTF-16 変換（簡易版: ASCII のみ保証、日本語名は未サポート）
static std::wstring utf8_to_wstring(const std::string& s) {
    if (s.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0,
                                        s.c_str(), static_cast<int>(s.size()),
                                        nullptr, 0);
    if (len <= 0) throw pipeutil::PipeException{pipeutil::PipeErrorCode::InvalidArgument,
                                                "Failed to convert pipe name to wide string"};
    std::wstring ws(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0,
                        s.c_str(), static_cast<int>(s.size()),
                        ws.data(), len);
    return ws;
}

/// GetLastError を PipeErrorCode にマッピングする
static pipeutil::PipeErrorCode map_win32_error(DWORD err) noexcept {
    switch (err) {
        case ERROR_BROKEN_PIPE:
        case ERROR_NO_DATA:          return pipeutil::PipeErrorCode::BrokenPipe;
        case ERROR_PIPE_NOT_CONNECTED: return pipeutil::PipeErrorCode::NotConnected;
        case ERROR_ACCESS_DENIED:    return pipeutil::PipeErrorCode::AccessDenied;
        default:                     return pipeutil::PipeErrorCode::SystemError;
    }
}

// ─── Win32Pipe ────────────────────────────────────────────────────

Win32Pipe::Win32Pipe(std::size_t buf_size) : buf_size_(buf_size) {}

Win32Pipe::~Win32Pipe() {
    // デストラクタは noexcept なのでリソースだけ解放
    server_close();
}

std::wstring Win32Pipe::to_pipe_path(const std::string& name) {
    return L"\\\\.\\pipe\\pipeutil_" + utf8_to_wstring(name);
}

// ─── サーバー操作 ─────────────────────────────────────────────────

void Win32Pipe::server_create(const std::string& pipe_name) {
    if (listening_) {
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::AlreadyConnected,
                                      "Already listening"};
    }

    const std::wstring path = to_pipe_path(pipe_name);
    pipe_name_wstr_ = path;  // server_accept_and_fork で再作成するために保存

    // stop_event_ を作成（manual-reset、初期非シグナル状態）
    // stop_accept() が SetEvent() を呼ぶまで非シグナルのまま。
    if (stop_event_) {
        CloseHandle(stop_event_);
        stop_event_ = nullptr;
    }
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_event_) {
        // hPipe_ はまだ作成前なのでクローズ不要
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::SystemError,
                                      "CreateEventW failed (stop_event)"};
    }

    hPipe_ = CreateNamedPipeW(
        path.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,                 // MultiPipeServer の複数同時接続に対応
        static_cast<DWORD>(buf_size_),            // 出力バッファ
        static_cast<DWORD>(buf_size_),            // 入力バッファ
        0,                                        // デフォルトタイムアウト
        nullptr                                   // セキュリティ属性
    );

    if (hPipe_ == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        throw pipeutil::PipeException{map_win32_error(err), "CreateNamedPipeW failed"};
    }

    // accept 用イベントをここで作成（manual-reset、初期非シグナル）
    if (accept_event_) { CloseHandle(accept_event_); }
    accept_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!accept_event_) {
        CloseHandle(hPipe_); hPipe_ = INVALID_HANDLE_VALUE;
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::SystemError,
                                      "CreateEventW failed (accept_event)"};
    }
    accept_ov_ = {};
    accept_ov_.hEvent = accept_event_;
    listening_ = true;
}

void Win32Pipe::server_accept(int64_t timeout_ms) {
    if (!listening_) {
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::NotConnected,
                                      "server_create() must be called before server_accept()"};
    }

    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr) {
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::SystemError, "CreateEventW failed"};
    }

    const BOOL connected = ConnectNamedPipe(hPipe_, &ov);
    const DWORD err = GetLastError();

    if (connected) {
        // ケース①: 即時成功（まれだが発生する）
        CloseHandle(ov.hEvent);
    } else if (err == ERROR_PIPE_CONNECTED) {
        // ケース②: accept 前にクライアントが接続済み
        CloseHandle(ov.hEvent);
    } else if (err == ERROR_IO_PENDING) {
        // ケース③: OVERLAPPED 非同期操作が開始 → WaitForSingleObject で完了待機
        const DWORD dw_timeout = (timeout_ms == 0)
                                   ? INFINITE
                                   : static_cast<DWORD>(timeout_ms);
        const DWORD result = WaitForSingleObject(ov.hEvent, dw_timeout);
        if (result == WAIT_TIMEOUT) {
            CancelIo(hPipe_);
            CloseHandle(ov.hEvent);
            throw pipeutil::PipeException{pipeutil::PipeErrorCode::Timeout};
        }
        if (result != WAIT_OBJECT_0) {
            CloseHandle(ov.hEvent);
            throw pipeutil::PipeException{pipeutil::PipeErrorCode::SystemError,
                                          "WaitForSingleObject failed"};
        }
        CloseHandle(ov.hEvent);
    } else {
        // ケース④: 即時失敗
        CloseHandle(ov.hEvent);
        throw pipeutil::PipeException{map_win32_error(err), "ConnectNamedPipe failed"};
    }

    connected_ = true;
}

void Win32Pipe::stop_accept() noexcept {
    if (stop_event_) {
        SetEvent(stop_event_);
    }
}

std::unique_ptr<IPlatformPipe> Win32Pipe::server_accept_and_fork(int64_t timeout_ms) {
    if (!listening_) {
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::NotConnected,
                                      "server_create() must be called before server_accept_and_fork()"};
    }
    if (!stop_event_ || !accept_event_) {
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::SystemError,
                                      "events not initialized; call server_create() first"};
    }

    // accept_ov_ / accept_event_ はメンバー変数: lifetime が Win32Pipe と同じなので
    // stop path で GetOverlappedResult を待たずに throw しても安全。
    // I/O は server_close() で hPipe_ を CloseHandle した際に OS が自動キャンセルする。
    ResetEvent(accept_event_);
    accept_ov_ = {};
    accept_ov_.hEvent = accept_event_;

    const BOOL connected_res = ConnectNamedPipe(hPipe_, &accept_ov_);
    const DWORD err = GetLastError();

    if (connected_res || err == ERROR_PIPE_CONNECTED) {
        // 即時成功（クライアントがすでに接続済み）— accept_ov_ は使用済み、何もしない
    } else if (err == ERROR_IO_PENDING) {
        // 非同期: stop_event_ または accept イベントを待つ
        HANDLE handles[2] = {accept_event_, stop_event_};
        const DWORD dw_timeout = (timeout_ms == 0)
                                   ? INFINITE
                                   : static_cast<DWORD>(timeout_ms);
        const DWORD wait_res = WaitForMultipleObjects(2, handles, FALSE, dw_timeout);

        if (wait_res == WAIT_TIMEOUT) {
            // タイムアウト: I/O はメンバー OVERLAPPED なので後始末は server_close() に委ねる
            CancelIoEx(hPipe_, &accept_ov_);
            throw pipeutil::PipeException{pipeutil::PipeErrorCode::Timeout};
        }
        if (wait_res == WAIT_OBJECT_0 + 1) {
            // stop_accept() による中断: GetOverlappedResult を待たずに throw でよい。
            // 未完了の I/O は accept_ov_（メンバー）に結果が書かれ、
            // platform_.reset() → server_close() → CloseHandle(hPipe_) で OS がキャンセルする。
            throw pipeutil::PipeException{pipeutil::PipeErrorCode::Interrupted,
                                          "accept cancelled by stop_accept()"};
        }
        if (wait_res != WAIT_OBJECT_0) {
            throw pipeutil::PipeException{pipeutil::PipeErrorCode::SystemError,
                                          "WaitForMultipleObjects failed"};
        }

        DWORD dummy = 0;
        if (!GetOverlappedResult(hPipe_, &accept_ov_, &dummy, FALSE)) {
            const DWORD err2 = GetLastError();
            throw pipeutil::PipeException{map_win32_error(err2),
                                          "GetOverlappedResult failed (accept_and_fork)"};
        }
    } else {
        throw pipeutil::PipeException{map_win32_error(err), "ConnectNamedPipe failed"};
    }

    // ─── fork: 接続済み HANDLE を新しい Win32Pipe インスタンスに移す ──
    auto forked = std::make_unique<Win32Pipe>(buf_size_);
    forked->hPipe_     = hPipe_;  // 接続済みハンドルを譲渡
    forked->listening_ = false;
    forked->connected_ = true;

    // 自身は次クライアント用の新しいハンドルを作成して listen 状態を維持
    hPipe_ = CreateNamedPipeW(
        pipe_name_wstr_.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        static_cast<DWORD>(buf_size_),
        static_cast<DWORD>(buf_size_),
        0,
        nullptr
    );
    if (hPipe_ == INVALID_HANDLE_VALUE) {
        const DWORD err2 = GetLastError();
        hPipe_     = INVALID_HANDLE_VALUE;
        listening_ = false;
        throw pipeutil::PipeException{map_win32_error(err2),
                                      "CreateNamedPipeW failed in accept_and_fork (next handle)"};
    }

    return forked;
}

void Win32Pipe::server_close() noexcept {
    if (hPipe_ != INVALID_HANDLE_VALUE) {
        if (connected_) {
            // クライアントが未読データを読み切るまで待機してから切断する (R-F001-1)。
            // FlushFileBuffers はデータが全てクライアントに渡るまでブロックする。
            // DisconnectNamedPipe は呼ばない: accepted (forked) 接続では不要であり、
            // 呼ぶとクライアントの pending ReadFile が ERROR_PIPE_NOT_CONNECTED で失敗する。
            // CloseHandle 単独でカーネルが書き込みバッファをフラッシュして接続を閉じる。
            FlushFileBuffers(hPipe_);
        }
        // CloseHandle により pending な ConnectNamedPipe I/O が自動キャンセルされ
        // accept_ov_ に完了通知が書き込まれる（メンバー変数なので安全）。
        CloseHandle(hPipe_);
        hPipe_ = INVALID_HANDLE_VALUE;
    }
    if (accept_event_) {
        // hPipe_ を閉じた後に accept_event_ を閉じる（順序重要）
        CloseHandle(accept_event_);
        accept_event_ = nullptr;
        accept_ov_    = {};
    }
    if (stop_event_) {
        CloseHandle(stop_event_);
        stop_event_ = nullptr;
    }
    pipe_name_wstr_.clear();
    listening_  = false;
    connected_  = false;
}

// ─── クライアント操作 ─────────────────────────────────────────────

void Win32Pipe::client_connect(const std::string& pipe_name, int64_t timeout_ms) {
    if (connected_) {
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::AlreadyConnected,
                                      "Already connected"};
    }

    const std::wstring path = to_pipe_path(pipe_name);
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds{timeout_ms};

    while (true) {
        hPipe_ = CreateFileW(
            path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,          // 共有なし
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            nullptr
        );

        if (hPipe_ != INVALID_HANDLE_VALUE) break;

        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND) {
            // サーバーがまだ listen していない → リトライ
        } else if (err != ERROR_PIPE_BUSY) {
            throw pipeutil::PipeException{map_win32_error(err), "CreateFileW failed"};
        }

        if (timeout_ms > 0 && std::chrono::steady_clock::now() >= deadline) {
            throw pipeutil::PipeException{pipeutil::PipeErrorCode::Timeout};
        }

        // 最大 10ms 待機してリトライ（サーバー起動待ち）
        WaitNamedPipeW(path.c_str(), 10);
    }

    connected_ = true;
}

void Win32Pipe::client_close() noexcept {
    if (hPipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(hPipe_);
        hPipe_ = INVALID_HANDLE_VALUE;
    }
    connected_ = false;
}

// ─── 共通 I/O ─────────────────────────────────────────────────────

void Win32Pipe::write_all(const std::byte* data, std::size_t size) {
    overlapped_write_all(data, size);
}

void Win32Pipe::read_all(std::byte* buf, std::size_t size, int64_t timeout_ms) {
    const DWORD dw_timeout = (timeout_ms == 0)
                               ? INFINITE
                               : static_cast<DWORD>(timeout_ms);
    overlapped_read_all(buf, size, dw_timeout);
}

void Win32Pipe::overlapped_write_all(const std::byte* data, std::size_t size) {
    const std::byte* ptr = data;
    std::size_t remaining = size;

    while (remaining > 0) {
        OVERLAPPED ov = {};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) {
            throw pipeutil::PipeException{pipeutil::PipeErrorCode::SystemError, "CreateEventW failed"};
        }

        DWORD written = 0;
        const BOOL ok = WriteFile(hPipe_, ptr,
                                  static_cast<DWORD>(remaining),
                                  &written, &ov);
        if (!ok) {
            const DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                CloseHandle(ov.hEvent);
                throw pipeutil::PipeException{map_win32_error(err), "WriteFile failed"};
            }
            // OVERLAPPED 完了を待つ
            const DWORD result = WaitForSingleObject(ov.hEvent, INFINITE);
            if (result != WAIT_OBJECT_0) {
                CloseHandle(ov.hEvent);
                throw pipeutil::PipeException{pipeutil::PipeErrorCode::SystemError,
                                              "WaitForSingleObject failed (write)"};
            }
            // GetOverlappedResult の戻り値を検査する (R-013)
            const BOOL got_w = GetOverlappedResult(hPipe_, &ov, &written, FALSE);
            if (!got_w) {
                const DWORD err_w = GetLastError();
                CloseHandle(ov.hEvent);
                throw pipeutil::PipeException{map_win32_error(err_w),
                                              "GetOverlappedResult failed (write)"};
            }
        }
        CloseHandle(ov.hEvent);

        ptr       += written;
        remaining -= written;
    }
}

void Win32Pipe::overlapped_read_all(std::byte* buf, std::size_t size, DWORD dw_timeout) {
    std::byte* ptr = buf;
    std::size_t remaining = size;

    // INFINITE 以外はデッドラインを計算して全チャンク合計に上限を設ける (R-015)
    const bool infinite_timeout = (dw_timeout == INFINITE);
    using Clock = std::chrono::steady_clock;
    const auto deadline = infinite_timeout
        ? Clock::time_point::max()
        : Clock::now() + std::chrono::milliseconds{dw_timeout};

    while (remaining > 0) {
        // チャンクごとの残り待機時間を算出
        DWORD chunk_timeout = dw_timeout;
        if (!infinite_timeout) {
            const auto now  = Clock::now();
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now).count();
            if (left <= 0) {
                throw pipeutil::PipeException{pipeutil::PipeErrorCode::Timeout};
            }
            chunk_timeout = (left > static_cast<long long>(MAXDWORD))
                ? MAXDWORD
                : static_cast<DWORD>(left);
        }

        OVERLAPPED ov = {};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) {
            throw pipeutil::PipeException{pipeutil::PipeErrorCode::SystemError, "CreateEventW failed"};
        }

        DWORD read_bytes = 0;
        const BOOL ok = ReadFile(hPipe_, ptr,
                                 static_cast<DWORD>(remaining),
                                 &read_bytes, &ov);
        if (!ok) {
            const DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA) {
                CloseHandle(ov.hEvent);
                connected_ = false;
                throw pipeutil::PipeException{pipeutil::PipeErrorCode::ConnectionReset,
                                              "Pipe disconnected during read"};
            }
            if (err != ERROR_IO_PENDING) {
                CloseHandle(ov.hEvent);
                throw pipeutil::PipeException{map_win32_error(err), "ReadFile failed"};
            }

            const DWORD result = WaitForSingleObject(ov.hEvent, chunk_timeout);
            if (result == WAIT_TIMEOUT) {
                CancelIo(hPipe_);
                CloseHandle(ov.hEvent);
                throw pipeutil::PipeException{pipeutil::PipeErrorCode::Timeout};
            }
            if (result != WAIT_OBJECT_0) {
                CloseHandle(ov.hEvent);
                throw pipeutil::PipeException{pipeutil::PipeErrorCode::SystemError,
                                              "WaitForSingleObject failed (read)"};
            }

            BOOL got = GetOverlappedResult(hPipe_, &ov, &read_bytes, FALSE);
            if (!got) {
                const DWORD err2 = GetLastError();
                CloseHandle(ov.hEvent);
                if (err2 == ERROR_BROKEN_PIPE) {
                    connected_ = false;
                    throw pipeutil::PipeException{pipeutil::PipeErrorCode::ConnectionReset};
                }
                throw pipeutil::PipeException{map_win32_error(err2), "GetOverlappedResult failed"};
            }
        }
        CloseHandle(ov.hEvent);

        if (read_bytes == 0) {
            // EOF (パイプが閉じられた)
            connected_ = false;
            throw pipeutil::PipeException{pipeutil::PipeErrorCode::ConnectionReset,
                                          "EOF on pipe"};
        }

        ptr       += read_bytes;
        remaining -= read_bytes;
    }
}

// ─── 状態照会 ─────────────────────────────────────────────────────

bool Win32Pipe::is_server_listening() const noexcept { return listening_; }
bool Win32Pipe::is_connected()        const noexcept { return connected_; }

} // namespace pipeutil::detail

#endif // _WIN32
