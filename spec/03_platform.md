# pipeutil — プラットフォーム実装仕様

## 1. 概要

`pipeutil_core` は `PlatformPipe` 抽象化レイヤーを通じて OS 差異を吸収します。
公開 API (`PipeServer` / `PipeClient`) は OS を意識することなく使用できます。

プラットフォーム実装は **pimpl イディオム** を用いて `source/core/src/platform/` 配下に配置します。

---

## 2. 実装クラス構成

```
pipeutil::detail::IPlatformPipe     (純粋仮想インタフェース)
    │
    ├── Win32Pipe                   (Windows 専用, win32_pipe.cpp)
    └── PosixPipe                   (Linux 専用,   posix_pipe.cpp)
```

### `IPlatformPipe` インタフェース

```cpp
// <pipeutil/detail/platform_pipe.hpp>
namespace pipeutil::detail {

class IPlatformPipe {
public:
    virtual ~IPlatformPipe() = default;

    // サーバー操作
    virtual void   server_create(const std::string& pipe_name) = 0;
    virtual void   server_accept(int64_t timeout_ms)           = 0;
    virtual void   server_close()  noexcept                    = 0;

    // クライアント操作
    virtual void   client_connect(const std::string& pipe_name,
                                  int64_t timeout_ms)           = 0;
    virtual void   client_close()  noexcept                    = 0;

    // 共通 I/O（ブロッキング、全バイト保証）
    virtual void   write_all(const std::byte* data,
                             std::size_t size)                  = 0;
    virtual void   read_all(std::byte* buf,
                            std::size_t size,
                            int64_t timeout_ms)                 = 0;

    // 状態照会
    virtual bool   is_server_listening()  const noexcept        = 0;
    virtual bool   is_connected()         const noexcept        = 0;
};

} // namespace pipeutil::detail
```

---

## 3. Windows 実装（`Win32Pipe`）

### 3.1 使用 API

| 用途 | Win32 API |
|------|-----------|
| パイプ作成（サーバー） | `CreateNamedPipeW` |
| 接続待機 | `ConnectNamedPipe` / `WaitForSingleObjectEx` |
| 接続（クライアント） | `WaitNamedPipeW` + `CreateFileW` |
| 読み取り | `ReadFile` |
| 書き込み | `WriteFile` |
| クローズ | `DisconnectNamedPipe` + `CloseHandle` |
| タイムアウト | `WaitForSingleObject` + `OVERLAPPED` |

### 3.2 パイプ名変換

```
ユーザー指定名: "my_service"
Windows展開後: L"\\.\pipe\pipeutil_my_service"
```

プレフィックス `pipeutil_` を付与して衝突を避ける。

### 3.3 `CreateNamedPipeW` パラメータ

```cpp
HANDLE hPipe = CreateNamedPipeW(
    wide_name,
    PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,   // 非同期モード（タイムアウト実装のため）
    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
    1,                          // 最大インスタンス数（現バージョン: 1）
    static_cast<DWORD>(buf_size),   // 出力バッファサイズ
    static_cast<DWORD>(buf_size),   // 入力バッファサイズ
    0,                          // デフォルトタイムアウト
    nullptr                     // セキュリティ属性（デフォルト: 呼び出しプロセスと同一）
);
```

### 3.4 接続待機（OVERLAPPED + タイムアウト）

```cpp
// ConnectNamedPipe を OVERLAPPED で呼び出す
OVERLAPPED ov = {};
ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
BOOL connected = ConnectNamedPipe(hPipe_, &ov);
DWORD err = GetLastError();

if (connected) {
    // ケース①: 即時成功（まれだが発生する）→ 正常接続扱い
    CloseHandle(ov.hEvent);
    // 接続確立済みとして続行
} else if (err == ERROR_PIPE_CONNECTED) {
    // ケース②: CreateNamedPipe 後 accept 前にクライアントが接続済み → 正常接続扱い
    CloseHandle(ov.hEvent);
    // 接続確立済みとして続行
} else if (err == ERROR_IO_PENDING) {
    // ケース③: OVERLAPPED 非同期操作が開始された → WaitForSingleObject で完了待機
    DWORD result = WaitForSingleObject(ov.hEvent, timeout_ms == 0
                                                  ? INFINITE
                                                  : static_cast<DWORD>(timeout_ms));
    if (result == WAIT_TIMEOUT) {
        CancelIo(hPipe_);
        CloseHandle(ov.hEvent);
        throw PipeException{PipeErrorCode::Timeout};
    }
    // WAIT_FAILED や WAIT_ABANDONED は SystemError へ
    if (result != WAIT_OBJECT_0) {
        CloseHandle(ov.hEvent);
        throw PipeException{PipeErrorCode::SystemError};
    }
    CloseHandle(ov.hEvent);
} else {
    // ケース④: 即時失敗（ERROR_INVALID_HANDLE 等） → SystemError を即時送出
    CloseHandle(ov.hEvent);
    throw PipeException{PipeErrorCode::SystemError};
}
```

### 3.5 クライアント接続

```cpp
// サーバーが listen 中でない場合はリトライ
HANDLE hPipe = INVALID_HANDLE_VALUE;
auto deadline = std::chrono::steady_clock::now()
              + std::chrono::milliseconds{timeout_ms};

while (true) {
    hPipe = CreateFileW(wide_name, GENERIC_READ | GENERIC_WRITE,
                        0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (hPipe != INVALID_HANDLE_VALUE) break;

    DWORD err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND) {
        // サーバーがまだ listen していない → 10ms スリープしてリトライ
    } else if (err != ERROR_PIPE_BUSY) {
        throw PipeException{PipeErrorCode::SystemError, ...};
    }

    if (timeout_ms > 0 && std::chrono::steady_clock::now() >= deadline)
        throw PipeException{PipeErrorCode::Timeout};

    WaitNamedPipeW(wide_name, 10);  // 最大 10ms 待機
}
```

### 3.6 完全書き込み / 完全読み取り

`WriteFile` / `ReadFile` は OVERLAPPED を使用し、指定バイト数が完了するまでループする。
部分転送が発生した場合は残りバイト数で再呼び出しする。

### 3.7 エラー変換

| `GetLastError()` | `PipeErrorCode` |
|-----------------|-----------------|
| `ERROR_BROKEN_PIPE` | `BrokenPipe` |
| `ERROR_NO_DATA` | `BrokenPipe` |
| `ERROR_PIPE_NOT_CONNECTED` | `NotConnected` |
| `ERROR_ACCESS_DENIED` | `AccessDenied` |
| その他 | `SystemError` |

---

## 4. Linux 実装（`PosixPipe`）

### 4.1 使用 API

UNIX ドメインソケット（`AF_UNIX`, `SOCK_STREAM`）を使用する。
`AF_UNIX` はカーネルバッファ内で転送されるため、ループバック TCP より高速。

| 用途 | POSIX API |
|------|-----------|
| ソケット作成 | `socket(AF_UNIX, SOCK_STREAM, 0)` |
| バインド（サーバー） | `bind` |
| 接続待機 | `listen` |
| 接続受諾 | `accept` |
| 接続（クライアント） | `connect` |
| 読み取り | `recv` / `read` |
| 書き込み | `send` / `write` |
| タイムアウト | `poll` / `select` |
| クローズ | `close` + `unlink`（ソケットファイル削除） |

### 4.2 ソケットパス変換

```
ユーザー指定名: "my_service"
Linux 展開後:  "/tmp/pipeutil/my_service.sock"
```

ディレクトリ `/tmp/pipeutil/` が存在しない場合は `mkdir` で作成する（モード `0700`）。

`sockaddr_un.sun_path` の最大長は **108 バイト**（`<sys/un.h>` 参照）。
パイプ名が長すぎる場合は `InvalidArgument` 例外を送出する。

### 4.3 サーバー初期化シーケンス

```cpp
// 1. ソケット作成
server_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

// 2. 既存ソケットファイル削除（前回クラッシュ残留対策）
unlink(sock_path_.c_str());

// 3. バインド
sockaddr_un addr = {};
addr.sun_family = AF_UNIX;
strncpy(addr.sun_path, sock_path_.c_str(), sizeof(addr.sun_path) - 1);
bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

// 4. listen (バックログ 1)
listen(server_fd_, 1);
```

### 4.4 accept（タイムアウト付き）

```cpp
// poll でタイムアウト制御
pollfd pfd = {.fd = server_fd_, .events = POLLIN};
int ret = poll(&pfd, 1, timeout_ms == 0 ? -1 : static_cast<int>(timeout_ms));
if (ret == 0) throw PipeException{PipeErrorCode::Timeout};
if (ret < 0)  throw PipeException{PipeErrorCode::SystemError, ...};

client_fd_ = accept4(server_fd_, nullptr, nullptr, SOCK_CLOEXEC);
```

### 4.5 クライアント接続

```cpp
client_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

auto deadline = std::chrono::steady_clock::now()
              + std::chrono::milliseconds{timeout_ms};

while (true) {
    if (connect(client_fd_, ...) == 0) break;

    if (errno == ENOENT || errno == ECONNREFUSED) {
        // ソケットファイルが存在しない / サーバーが未起動 → リトライ
        if (timeout_ms > 0 && std::chrono::steady_clock::now() >= deadline)
            throw PipeException{PipeErrorCode::Timeout};
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        continue;
    }
    throw PipeException{PipeErrorCode::SystemError, ...};
}
```

### 4.6 完全書き込み / 完全読み取り

```cpp
// write_all: EINTR に対応しつつ全バイト送信
while (remaining > 0) {
    ssize_t n = send(fd_, ptr, remaining, MSG_NOSIGNAL);
    if (n < 0) {
        if (errno == EINTR) continue;
        if (errno == EPIPE) throw PipeException{PipeErrorCode::BrokenPipe};
        throw PipeException{PipeErrorCode::SystemError, ...};
    }
    ptr += n;
    remaining -= static_cast<std::size_t>(n);
}

// read_all: poll + recv でタイムアウト対応しつつ全バイト受信
```

### 4.7 エラー変換

| `errno` | `PipeErrorCode` |
|---------|-----------------|
| `EPIPE` / `ECONNRESET` | `BrokenPipe` / `ConnectionReset` |
| `ENOENT` / `ECONNREFUSED` | `NotFound` |
| `EACCES` | `AccessDenied` |
| `ETIMEDOUT` | `Timeout` |
| その他 | `SystemError` |

### 4.8 クローズ時のクリーンアップ

サーバーがクローズされる際は `unlink(sock_path_)` でソケットファイルを削除する。
クライアントは `close(client_fd_)` のみで良い（ファイル作成しないため）。

---

## 5. プリプロセッサによる分岐方針

```cpp
// platform_factory.hpp
#ifdef _WIN32
#  include "pipeutil/detail/win32_pipe.hpp"
   using PlatformPipeImpl = pipeutil::detail::Win32Pipe;
#else
#  include "pipeutil/detail/posix_pipe.hpp"
   using PlatformPipeImpl = pipeutil::detail::PosixPipe;
#endif
```

CMake の `target_compile_definitions` で `_WIN32` は自動定義されるため、
追加定義は不要。

---

## 6. コンパイルターゲット要件

| プラットフォーム | コンパイラ | 最低バージョン | 備考 |
|----------------|-----------|-------------|------|
| Windows x64 | MSVC | VS 2022 (v17.x) | `/std:c++20` |
| Windows x64 | Clang-cl | 17+ | `/std:c++20` |
| Linux x86_64 | GCC | 13+ | `-std=c++20` |
| Linux x86_64 | Clang | 17+ | `-std=c++20` |
| Linux ARM64 | GCC | 13+ | `-std=c++20` |

---

## 7. シグナルハンドリング（Linux）

`SIGPIPE` シグナルは `MSG_NOSIGNAL` フラグで抑制する（`send()` 呼び出し時）。
アプリケーション側で `signal(SIGPIPE, SIG_IGN)` を設定する必要はない。

---

## 8. ソケットファイルのパーミッション

デフォルトのソケットファイルパーミッション: `0600`（所有者のみ読み書き可）
セキュリティ要件が異なる場合は `PipeServer` のコンストラクタに `mode_t` オプション引数を追加すること（将来拡張）。

---

## 9. WSL（Windows Subsystem for Linux）での動作

WSL2 環境では Linux 実装（POSIX / UNIX ドメインソケット）が使用される。
WSL2 ↔ Windows ネイティブプロセス間通信は本ライブラリのスコープ外とする。
