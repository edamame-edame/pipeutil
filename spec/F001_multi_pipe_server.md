# F-001 詳細設計: MultiPipeServer（多重クライアント対応）

**作成日**: 2026-02-28
**対象バージョン**: v0.2.0
**依存**: F-007（テストスイート） — 先行必須

---

## 1. 現状と課題

`PipeServer` は `listen → accept → 1対1通信` のモデルで、1 本の接続しか扱えない。
Windows は `CreateNamedPipe` で生成した 1 HANDLE = 1 クライアント対応。
複数クライアントを扱うには、accept 後に _次のクライアント用 HANDLE を別途作成_ しなければならない。

---

## 2. 設計方針

### 2.1 スレッドモデル

```
MultiPipeServer::serve()
│
├─ [acceptor_thread]  (1本, 内部管理)
│     └─ ループ: server_accept_and_fork() で接続受理 → fork 済みパイプをハンドラスレッドへ渡す
│
├─ [handler_thread-1] (接続ごとに生成, max_connections 上限)
│     └─ ユーザー Handler(PipeServer) を実行
│
├─ [handler_thread-2]
└─ ...
```

- acceptor スレッドは `stop()` まで永続稼働する
- handler スレッドはハンドラが return したら自動終了する
- 同時接続数が `max_connections` に達したら acceptor はスロットが空くまでブロックする

### 2.2 内部クラス構成

```
MultiPipeServer          ← 公開 API
  └─ Impl
       ├─ acceptor_thread_     (std::thread)
       ├─ active_count_        (std::atomic<std::size_t>)
       ├─ done_cv_             (std::condition_variable) ← 全ハンドラ終了通知
       ├─ done_mutex_          (std::mutex)
       ├─ sem_                 (セマフォ: スロット管理)
       └─ stop_flag_           (std::atomic<bool>)
```

`handler_threads_` は **保持しない**。  
ハンドラスレッドは起動後に `detach()` し、完了を `active_count_` + `done_cv_` で追跡する。  
これにより長時間運用でも終了済みスレッドオブジェクトが蓄積しない。  
`stop()` では `done_cv_` で `active_count_ == 0` になるまで待機して全ハンドラ終了を保証する。

### 2.3 各接続の表現

新型 `PipeConnection` は **不要**。  
接続済みの `PipeServer` インスタンスをハンドラに渡す。これにより既存の `PipeServer` API がそのまま使える。

実現のため `PipeServer::Impl` に内部コンストラクタを追加する:

```cpp
// pimpl 内部: 既に accept 済みのプラットフォームパイプから直接構築
// （MultiPipeServer のみ使用, 公開 API なし）
Impl(std::string name, std::unique_ptr<detail::IPlatformPipe> accepted_platform);
```

---

## 3. 公開 API 設計

```cpp
// multi_pipe_server.hpp
namespace pipeutil {

class PIPEUTIL_API MultiPipeServer {
public:
    using Handler = std::function<void(PipeServer)>;

    /// pipe_name       : 論理名 (PipeServer と同じ規則)
    /// max_connections : 同時ハンドラスレッド数の上限 (1〜64)
    /// buffer_size     : 各接続の内部バッファサイズ (デフォルト 64 KiB)
    explicit MultiPipeServer(std::string pipe_name,
                             std::size_t max_connections = 8,
                             std::size_t buffer_size    = 65536);

    MultiPipeServer(const MultiPipeServer&)            = delete;
    MultiPipeServer& operator=(const MultiPipeServer&) = delete;
    MultiPipeServer(MultiPipeServer&&)                 noexcept;
    MultiPipeServer& operator=(MultiPipeServer&&)      noexcept;

    ~MultiPipeServer();

    /// acceptor ループを開始する (ブロッキング or 別スレッド)
    ///
    /// run_in_background == false (デフォルト):
    ///   stop() が呼ばれるまでブロックする。
    ///
    /// run_in_background == true:
    ///   acceptor ループを内部スレッドで起動し即座に返る。
    ///   stop() が呼ばれるまでサーバーは稼働し続ける。
    ///
    /// 例外: PipeException (SystemError / AccessDenied)
    void serve(Handler on_connect,
               bool    run_in_background = false);

    /// acceptor ループを停止し、全ハンドラスレッドの終了を待つ
    /// noexcept: 呼び出し元は例外を受け取らない
    void stop() noexcept;

    /// 現在アクティブなハンドラスレッド数
    [[nodiscard]] std::size_t active_connections() const noexcept;

    /// acceptor が稼働中かどうか
    [[nodiscard]] bool is_serving() const noexcept;

    [[nodiscard]] const std::string& pipe_name() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pipeutil
```

---

## 4. プラットフォーム実装

### 4.1 Windows (Win32Pipe)

Windows 名前付きパイプは「1 HANDLE = 1 クライアント」。
多重対応のため以下の変更が必要:

**現状** (`Win32Pipe::server_accept`):
```
CreateNamedPipe → ConnectNamedPipe → 接続済み
```

**必要な変更**:  
accept 後、次のクライアント用に新しい HANDLE を作る機能を IPlatformPipe に追加する。

```cpp
// IPlatformPipe に追加
virtual std::unique_ptr<IPlatformPipe> server_accept_and_fork(
    int64_t timeout_ms) = 0;
```

実装:
```
1. ConnectNamedPipe (現 HANDLE) → 接続成立
2. 新しい CreateNamedPipe (同じ名前, PIPE_UNLIMITED_INSTANCES) → next_handle
3. 現 HANDLE を接続済みとして返す (PipeServer::Impl に渡す)
4. next_handle を acceptor の次回ループで使う
```

### 4.2 POSIX (PosixPipe)

POSIX は `server_fd_` (listen) と `client_fd_` (I/O) が分離しているため、
`accept()` を繰り返すだけで多重化できる。

`server_accept_and_fork` の POSIX 実装:
```
1. accept(server_fd_) → new_client_fd
2. new_client_fd を持つ新 PosixPipe インスタンスを返す
3. server_fd_ は次ループでも使い続ける
```

### 4.3 セマフォによるスロット制御

```cpp
// std::counting_semaphore (C++20)
std::counting_semaphore<64> sem_{max_connections};

// acceptor ループ内
sem_.acquire();           // スロット確保 (max 到達時でブロック)
auto conn_pipe = platform_->server_accept_and_fork(0);
active_count_.fetch_add(1, std::memory_order_relaxed);

std::thread([this, pipe = std::move(conn_pipe)] mutable {
    // RAII guard: 例外発生時も含めて、スロット解放とカウンタ減算を必ず保証
    struct SlotGuard {
        Impl& self;
        ~SlotGuard() {
            self.sem_.release();
            if (self.active_count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                // 最後のハンドラが完了: stop() の待機を起こす
                std::lock_guard lk{self.done_mutex_};
                self.done_cv_.notify_all();
            }
        }
    } guard{*this};

    try {
        handler_(PipeServer{name_, std::move(pipe)});
    } catch (...) {
        // ハンドラ例外はサーバー継続のため飲み込む (必要に応じてログ出力)
    }
    // guard デストラクタで sem_.release() と active_count_ 減算が保証される
}).detach();  // detach: 終了済みスレッドオブジェクトの蓄積なし
```

---

## 5. stop() の実装

`stop()` では acceptor のブロッキング `server_accept_and_fork` を中断させる必要がある。

### Windows
- Dummy クライアントを自分自身に接続して `ConnectNamedPipe` のブロックを解除:
  ```cpp
  CreateFile(pipe_path, ...);  // acceptor を起床させる
  stop_flag_ = true;
  ```

### POSIX
- `stop_fd_` (eventfd / pipe) に書き込み、`poll` で検出して `accept` ループを抜け出す

### 全ハンドラ終了の待機
`stop()` は acceptor を停止後、`done_cv_` で `active_count_ == 0` になるまでブロックする:
```cpp
void stop() noexcept {
    stop_flag_ = true;
    /* acceptor を起床させる (Windows: ダミー接続, POSIX: stop_fd_ 書き込み) */
    if (acceptor_thread_.joinable()) acceptor_thread_.join();
    // 全ハンドラの完了を待つ
    std::unique_lock lk{done_mutex_};
    done_cv_.wait(lk, [this] { return active_count_.load(std::memory_order_acquire) == 0; });
}
```

---

## 6. Python バインディング

```python
# Python API
server = pipeutil.MultiPipeServer("my_pipe", max_connections=4)

def handle(conn: pipeutil.PipeServer) -> None:
    msg = conn.receive(timeout_ms=5000)
    conn.send(pipeutil.Message(b"pong"))
    conn.close()

server.serve(handle, run_in_background=True)
# ...
server.stop()
```

### Python スレッドと GIL

ハンドラが Python callable の場合、各スレッドで `PyGILState_Ensure` / `PyGILState_Release` が必要。  
`py_multi_pipe_server.cpp` で C API レベルで GIL を適切に管理する。

---

## 7. 変更ファイル一覧

| ファイル | 変更内容 |
|---|---|
| `source/core/include/pipeutil/detail/platform_pipe.hpp` | `server_accept_and_fork()` 追加 |
| `source/core/src/platform/win32_pipe.hpp/cpp` | `server_accept_and_fork()` 実装 |
| `source/core/src/platform/posix_pipe.hpp/cpp` | `server_accept_and_fork()` 実装 |
| `source/core/include/pipeutil/pipe_server.hpp` | 内部コンストラクタ追加 |
| `source/core/src/pipe_server.cpp` | `Impl` 内部コンストラクタ追加 |
| `source/core/include/pipeutil/multi_pipe_server.hpp` | **新規** |
| `source/core/src/multi_pipe_server.cpp` | **新規** |
| `source/core/CMakeLists.txt` | `multi_pipe_server.cpp` 追加 |
| `source/python/py_multi_pipe_server.hpp/cpp` | **新規** |
| `source/python/_pipeutil_module.cpp` | `MultiPipeServer` 型の登録 |
| `tests/cpp/test_multi_pipe_server.cpp` | **新規** |
| `tests/cpp/CMakeLists.txt` | テストターゲット追加 |
| `tests/python/test_multi_server.py` | **新規** |

---

## 8. テスト計画

### C++ テスト (`tests/cpp/test_multi_pipe_server.cpp`)

| テスト名 | 検証内容 |
|---|---|
| `ThreeClients_Concurrent` | 3 クライアントが同時に接続・送受信できること |
| `MaxConnections_FourthBlocks` | max_connections=2 時、3件目は空きが出るまでブロックされること |
| `StopWhileServing` | `stop()` が全ハンドラ終了後に返ること |
| `HandlerException_DoesNotKillServer` | ハンドラが例外を投げても acceptor は継続すること |
| `BackgroundMode_ServeAndStop` | `run_in_background=true` で非ブロッキング起動できること |

### Python テスト (`tests/python/test_multi_server.py`)

| テスト名 | 検証内容 |
|---|---|
| `TestMultiServer::test_3_clients_concurrent` | 3 クライアント同時接続 |
| `TestMultiServer::test_stop_cleanly` | `stop()` でサーバーが正常終了 |
| `TestMultiServer::test_background_serve` | `run_in_background=True` |

---

## 9. 既知リスク・注意点

| リスク | 対策 |
|---|---|
| Windows: `PIPE_UNLIMITED_INSTANCES` 指定でも OS 上限あり | max_connections ≤ 64 の制約を文書化 |
| stop() のダミー接続が handler に到達してしまう | stop_flag_ を確認してハンドラに渡さない |
| Python GIL: ハンドラスレッドがクラッシュ | C++ 側で例外キャッチ、Python 側に伝達する仕組みが必要 |
| handle スレッド数が大量になるとスレッドスラッシング | max_connections ≤ 64 を上限として文書化 |
