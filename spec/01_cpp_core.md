# pipeutil — C++ コアライブラリ API 仕様

## 1. 概要

`pipeutil_core` は名前付きパイプ / UNIX ドメインソケットによる IPC を提供する C++20 共有ライブラリです。
公開ヘッダ `pipeutil/pipeutil.hpp` をインクルードし、`pipeutil_core.dll` / `libpipeutil_core.so` にリンクして使用します。

---

## 2. 名前空間

```cpp
namespace pipeutil { ... }          // 公開 API
namespace pipeutil::detail { ... }  // 内部実装（使用禁止）
```

---

## 3. エラー処理

### 3.1 `PipeErrorCode` 列挙体

```cpp
// <pipeutil/pipe_error.hpp>
namespace pipeutil {

enum class PipeErrorCode : int {
    // 成功
    Ok              = 0,

    // システム / OS エラー
    SystemError     = 1,    // errno / GetLastError() 由来
    AccessDenied    = 2,
    NotFound        = 3,

    // 接続状態
    AlreadyConnected = 10,
    NotConnected    = 11,
    ConnectionReset = 12,   // 相手側が切断
    Timeout         = 13,

    // I/O
    BrokenPipe      = 20,
    Overflow        = 21,   // バッファサイズ超過
    InvalidMessage  = 22,   // フレーミングエラー

    // その他
    InvalidArgument = 30,
    NotSupported    = 31,
};

/// PipeErrorCode + std::system_error ラッパー
/// 送出例外型: pipeutil::PipeException
class PipeException : public std::system_error {
public:
    explicit PipeException(PipeErrorCode code, const std::string& what = "");
    PipeErrorCode pipe_code() const noexcept;
};

} // namespace pipeutil
```

### 3.2 エラー処理ポリシー

| 状況 | 挙動 |
|------|------|
| 引数検証エラー | `PipeException(PipeErrorCode::InvalidArgument)` 送出 |
| OS エラー（errno / GetLastError）| `PipeException(PipeErrorCode::SystemError)` 送出（内部 `std::error_code` に OS エラーを保持） |
| タイムアウト | `PipeException(PipeErrorCode::Timeout)` 送出 |
| 相手側切断 | `PipeException(PipeErrorCode::ConnectionReset)` 送出 |

---

## 4. `Message` 構造体

```cpp
// <pipeutil/message.hpp>
namespace pipeutil {

/// ペイロードを保持する不変値型
/// フレームヘッダは送受信時に自動付与・除去される
struct Message {
    using PayloadType = std::vector<std::byte>;

    /// デフォルト構築（空メッセージ）
    Message() = default;

    /// バイト列から構築
    explicit Message(std::span<const std::byte> data);

    /// std::string / std::string_view から構築（UTF-8 バイト列として扱う）
    explicit Message(std::string_view text);

    /// ペイロードへのアクセス
    [[nodiscard]] std::span<const std::byte> payload() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    /// std::string_view として解釈（バイナリの場合は未定義動作なし、ただしヌル終端なし）
    [[nodiscard]] std::string_view as_string_view() const noexcept;

private:
    PayloadType data_;
};

} // namespace pipeutil
```

---

## 5. `IOBuffer` クラス

```cpp
// <pipeutil/io_buffer.hpp>
namespace pipeutil {

/// ゼロコピー I/O のための循環バッファ
/// スレッドセーフ: 単一プロデューサー / 単一コンシューマー
class IOBuffer {
public:
    /// capacity: バッファサイズ (バイト単位、2の累乗推奨)
    explicit IOBuffer(std::size_t capacity = 65536);

    /// コピー・ムーブ不可（所有権明確化）
    IOBuffer(const IOBuffer&)            = delete;
    IOBuffer& operator=(const IOBuffer&) = delete;
    IOBuffer(IOBuffer&&)                 = delete;
    IOBuffer& operator=(IOBuffer&&)      = delete;

    ~IOBuffer() = default;

    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::size_t readable_size() const noexcept;
    [[nodiscard]] std::size_t writable_size() const noexcept;

    /// 書き込み: dst に最大 len バイト書き込む (実際の書き込みバイト数を返す)
    std::size_t write(const std::byte* src, std::size_t len);

    /// 読み出し: src から最大 len バイト読み出す (実際の読み出しバイト数を返す)
    std::size_t read(std::byte* dst, std::size_t len);

    void clear() noexcept;

private:
    std::vector<std::byte> buf_;
    std::atomic<std::size_t> read_pos_{0};
    std::atomic<std::size_t> write_pos_{0};
};

} // namespace pipeutil
```

---

## 6. `PipeServer` クラス

```cpp
// <pipeutil/pipe_server.hpp>
namespace pipeutil {

/// サーバー側: listen → accept → read/write → close の順で使用
class PipeServer {
public:
    /// pipe_name : パイプ識別名（論理名。OS プレフィックスなし、英数字・アンダースコア・ハイフンのみ推奨）
    ///             例: "my_service"
    ///             Windows 実体名: "\\.\pipe\pipeutil_my_service"  ← 内部で "pipeutil_" を自動付与
    ///             Linux  実体名: "/tmp/pipeutil/my_service.sock"
    /// buffer_size : 内部 IOBuffer のサイズ (デフォルト 64 KiB)
    explicit PipeServer(std::string pipe_name,
                        std::size_t buffer_size = 65536);

    /// コピー不可、ムーブ可
    PipeServer(const PipeServer&)            = delete;
    PipeServer& operator=(const PipeServer&) = delete;
    PipeServer(PipeServer&& other) noexcept;
    PipeServer& operator=(PipeServer&& other) noexcept;

    ~PipeServer();

    // ─── ライフサイクル ──────────────────────────────────────────────

    /// パイプを作成して接続待ち状態にする
    /// 例外: PipeException (SystemError / AccessDenied)
    void listen();

    /// クライアントが接続するまでブロックして待機する
    /// timeout_ms == 0  → 無限待機
    /// timeout_ms > 0   → 指定ミリ秒後に Timeout 例外
    /// 例外: PipeException (Timeout / SystemError)
    void accept(std::chrono::milliseconds timeout = std::chrono::milliseconds{0});

    /// 接続を閉じてパイプリソースを解放する
    void close() noexcept;

    // ─── I/O ─────────────────────────────────────────────────────────

    /// フレーム付きメッセージを送信する
    /// 例外: PipeException (BrokenPipe / NotConnected / SystemError)
    void send(const Message& msg);

    /// フレーム付きメッセージを受信する (ブロッキング)
    /// timeout_ms == 0 → 無限待機
    /// 例外: PipeException (Timeout / ConnectionReset / InvalidMessage)
    [[nodiscard]] Message receive(
        std::chrono::milliseconds timeout = std::chrono::milliseconds{0});

    // ─── 状態照会 ─────────────────────────────────────────────────────

    [[nodiscard]] bool is_listening() const noexcept;
    [[nodiscard]] bool is_connected() const noexcept;
    [[nodiscard]] const std::string& pipe_name() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;   // pimpl イディオム (プラットフォーム詳細を隠蔽)
};

} // namespace pipeutil
```

### PipeServer 状態遷移図

```
  [未初期化]
      │ listen()
      ▼
  [LISTENING] ←──────── close()
      │ accept()
      ▼
  [CONNECTED] ─── send() / receive() ───→ [CONNECTED]
      │                                        │
      │ close() / ConnectionReset              │
      ▼                                        │
  [CLOSED] ◄──────────────────────────────────┘
```

---

## 7. `PipeClient` クラス

```cpp
// <pipeutil/pipe_client.hpp>
namespace pipeutil {

/// クライアント側: connect → read/write → close の順で使用
class PipeClient {
public:
    /// pipe_name : PipeServer と同じ識別名を指定する
    /// buffer_size : 内部 IOBuffer のサイズ (デフォルト 64 KiB)
    explicit PipeClient(std::string pipe_name,
                        std::size_t buffer_size = 65536);

    /// コピー不可、ムーブ可
    PipeClient(const PipeClient&)            = delete;
    PipeClient& operator=(const PipeClient&) = delete;
    PipeClient(PipeClient&& other) noexcept;
    PipeClient& operator=(PipeClient&& other) noexcept;

    ~PipeClient();

    // ─── ライフサイクル ──────────────────────────────────────────────

    /// サーバーに接続する
    /// timeout_ms == 0 → 無限待機（サーバーが listen 中になるまでリトライ）
    /// timeout_ms > 0  → 指定ミリ秒後に Timeout 例外
    /// 例外: PipeException (Timeout / NotFound / SystemError)
    void connect(std::chrono::milliseconds timeout = std::chrono::milliseconds{0});

    /// 接続を閉じてリソースを解放する
    void close() noexcept;

    // ─── I/O ─────────────────────────────────────────────────────────

    /// フレーム付きメッセージを送信する
    /// 例外: PipeException (BrokenPipe / NotConnected / SystemError)
    void send(const Message& msg);

    /// フレーム付きメッセージを受信する (ブロッキング)
    /// 例外: PipeException (Timeout / ConnectionReset / InvalidMessage)
    [[nodiscard]] Message receive(
        std::chrono::milliseconds timeout = std::chrono::milliseconds{0});

    // ─── 状態照会 ─────────────────────────────────────────────────────

    [[nodiscard]] bool is_connected() const noexcept;
    [[nodiscard]] const std::string& pipe_name() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pipeutil
```

---

## 8. 公開ヘッダ `pipeutil.hpp`

```cpp
// <pipeutil/pipeutil.hpp>
// pipeutil ライブラリのオールインワンインクルード
#pragma once

#include "pipeutil/pipe_error.hpp"
#include "pipeutil/message.hpp"
#include "pipeutil/io_buffer.hpp"
#include "pipeutil/pipe_server.hpp"
#include "pipeutil/pipe_client.hpp"
```

---

## 9. DLL エクスポートマクロ

```cpp
// <pipeutil/pipeutil_export.hpp>  (CMake generate_export_header で自動生成)
#ifdef _WIN32
#  ifdef PIPEUTIL_CORE_EXPORTS
#    define PIPEUTIL_API __declspec(dllexport)
#  else
#    define PIPEUTIL_API __declspec(dllimport)
#  endif
#else
#  define PIPEUTIL_API __attribute__((visibility("default")))
#endif
```

全ての公開クラスおよび公開関数に `PIPEUTIL_API` を付与すること。

---

## 10. スレッドセーフ要件

| クラス / メソッド | スレッドセーフ |
|------------------|---------------|
| `PipeServer::send()` | ✅ 複数スレッドから同時呼び出し可能（内部ミューテックス保護） |
| `PipeServer::receive()` | ✅ 同上 |
| `PipeServer::listen()` / `accept()` | ❌ 単一スレッドから呼び出すこと |
| `PipeClient::send()` | ✅ |
| `PipeClient::receive()` | ✅ |
| `PipeClient::connect()` | ❌ 単一スレッドから呼び出すこと |
| `IOBuffer` | ✅ SPSC（単一プロデューサー / 単一コンシューマー）安全 |

---

## 11. 使用例（C++）

```cpp
// === サーバー側 ===
#include <pipeutil/pipeutil.hpp>
#include <iostream>

int main() {
    pipeutil::PipeServer server{"my_pipe"};
    server.listen();

    std::cout << "Waiting for client...\n";
    server.accept();

    // 受信
    auto msg = server.receive();
    std::cout << "Received: " << msg.as_string_view() << '\n';

    // 送信
    server.send(pipeutil::Message{"Hello from server!"});

    server.close();
    return 0;
}

// === クライアント側 ===
#include <pipeutil/pipeutil.hpp>

int main() {
    pipeutil::PipeClient client{"my_pipe"};
    client.connect();

    client.send(pipeutil::Message{"Hello from client!"});

    auto reply = client.receive();
    // reply.as_string_view() == "Hello from server!"

    client.close();
    return 0;
}
```

---

## 12. 制約・未対応事項

| 項目 | 詳細 |
|------|------|
| マルチクライアント | 1 サーバー : 1 クライアントのみ（将来拡張でプール対応予定） |
| 非同期 I/O | 現バージョンはブロッキング I/O のみ |
| メッセージ最大サイズ | 4 GiB - 1 バイト（ヘッダの `payload_size` フィールドが `uint32_t`） |
| パイプ名文字種 | 英数字・アンダースコア・ハイフンのみ（スラッシュ・バックスラッシュ禁止） |
