# F-004 詳細設計: asyncio / threading / multiprocessing 対応

**作成日**: 2026-03-07
**対象バージョン**: v0.4.0
**依存**: F-002（RpcPipeClient/Server） — 先行必須

---

## 1. 現状と課題

### 1.1 ブロッキング I/O の問題

現在の全 API がスレッドブロッキング I/O であるため、以下のユースケースで問題が生じる。

| シナリオ | 問題 |
|---|---|
| `asyncio` アプリへの組み込み | `await client.receive()` が書けない。`run_in_executor` の手書きが必要 |
| `asyncio.gather` で並列接続管理 | スレッドプールを枯渇させる恐れ |
| `multiprocessing.spawn` で子プロセスに渡す | C拡張オブジェクトは `pickle` 不可。子プロセスで新規接続を確立できない |
| `concurrent.futures.ThreadPoolExecutor` | I/O 結果を `Future` として扱えない |

### 1.2 スコープ定義

本仕様は以下の 3 領域をカバーする。

| 領域 | 記号 | 概要 |
|---|---|---|
| asyncio 対応 | `[A]` | `AsyncPipeClient/Server` / `AsyncRpcPipeClient/Server` |
| threading 対応 | `[T]` | `concurrent.futures` 連携ラッパー |
| multiprocessing 対応 | `[M]` | spawn/fork 両対応の設計 |

---

## 2. アーキテクチャ概観

```
python/pipeutil/
  __init__.py          ← 既存: PipeClient, PipeServer, RpcPipeClient, RpcPipeServer 公開
  aio.py               ← [A] 新規: AsyncPipeClient, AsyncPipeServer, etc.
  _aio_native.py       ← [A] Phase 2: IOCP/io_uring ネイティブヘルパー (optional import)
  threading_utils.py   ← [T] 新規: ThreadedPipeClient, ThreadedPipeServer
  mp.py                ← [M] 新規: ForkableConn, WorkerPipeClient, spawn_worker_factory
  py_async_pipe.hpp/cpp   ← [A Phase 2] C++: AsyncPlatformPipe (OVERLAPPED + IOCP)
  py_async_module.cpp     ← [A Phase 2] C++: _pipeutil_async 拡張モジュール
```

### 2.1 実装フェーズ

```
Phase 1 (v0.4.0):  aio.py + threading_utils.py + mp.py
                   ─ Python ラッパーのみ、C++ コア変更なし
                   ─ asyncio.to_thread ベース
                   ─ マルチプロセス spawn 対応

Phase 2 (v0.5.0):  py_async_pipe.hpp/cpp + _pipeutil_async モジュール
                   ─ Windows: IOCP ProactorEventLoop ネイティブ統合
                   ─ Linux: io_uring / epoll SelectorEventLoop 統合
                   ─ スループット優先ユースケース向け
```

---

## 3. [A] asyncio 対応

### 3.1 設計原則

1. **Phase 1 はコア変更なし**: `asyncio.to_thread()` でブロッキング操作をスレッドオフロード
2. **キャンセル対応**: `asyncio.CancelledError` を適切に伝播させる
3. **コンテキストマネージャ**: `async with` 構文をサポートし、確実にリソース解放
4. **Phase 2 互換**: Phase 1 と同一 API を Phase 2 でネイティブ実装で置き換え可能にする

### 3.2 クラス設計

#### AsyncPipeClient

```python
class AsyncPipeClient:
    """
    非同期パイプクライアント。
    Phase 1: asyncio.to_thread() によるスレッドオフロード実装。
    """

    def __init__(self, pipe_name: str, buffer_size: int = 65536) -> None: ...

    async def connect(self, timeout_ms: int = 5000) -> None:
        """接続。タイムアウト時は PipeTimeoutError を送出。"""

    async def send(self, msg: Message) -> None:
        """送信。接続断時は PipeBrokenError を送出。"""

    async def receive(self, timeout_ms: int = 5000) -> Message:
        """受信。タイムアウト時は PipeTimeoutError を送出。"""

    async def close(self) -> None:
        """接続をクローズ（冪等）。"""

    # コンテキストマネージャ
    async def __aenter__(self) -> "AsyncPipeClient": ...
    async def __aexit__(self, *args: object) -> None: ...

    @property
    def is_connected(self) -> bool: ...
    @property
    def pipe_name(self) -> str: ...
```

#### AsyncPipeServer

```python
class AsyncPipeServer:
    """
    非同期パイプサーバー。
    """

    def __init__(self, pipe_name: str, buffer_size: int = 65536) -> None: ...

    async def listen(self) -> None:
        """パイプ作成（LISTEN 状態へ移行）。"""

    async def accept(self, timeout_ms: int = 30000) -> None:
        """クライアント接続待機。タイムアウト時は PipeTimeoutError。"""

    async def send(self, msg: Message) -> None: ...
    async def receive(self, timeout_ms: int = 5000) -> Message: ...
    async def close(self) -> None: ...

    async def __aenter__(self) -> "AsyncPipeServer": ...
    async def __aexit__(self, *args: object) -> None: ...

    @property
    def is_connected(self) -> bool: ...
    @property
    def is_listening(self) -> bool: ...
    @property
    def pipe_name(self) -> str: ...
```

#### AsyncRpcPipeClient

```python
class AsyncRpcPipeClient:
    """
    非同期 RPC クライアント。F-002 RpcPipeClient の asyncio ラッパー。
    """

    def __init__(self, pipe_name: str, buffer_size: int = 65536) -> None: ...

    async def connect(self, timeout_ms: int = 5000) -> None: ...

    async def send_request(
        self,
        req: Message,
        timeout_ms: int = 5000,
    ) -> Message:
        """
        リクエスト送信 → 対応 message_id の応答を await で待機。
        複数リクエストを並列 await 可能（asyncio.gather 対応）。
        """

    async def send(self, msg: Message) -> None:
        """通常送信（message_id なし）。"""

    async def receive(self, timeout_ms: int = 5000) -> Message:
        """通常受信（FLAG_REQUEST/RESPONSE でないフレームのみ）。"""

    async def close(self) -> None: ...

    async def __aenter__(self) -> "AsyncRpcPipeClient": ...
    async def __aexit__(self, *args: object) -> None: ...
```

#### AsyncRpcPipeServer

```python
class AsyncRpcPipeServer:
    """
    非同期 RPC サーバー。コールバックを async 関数として登録できる。
    """

    def __init__(self, pipe_name: str, buffer_size: int = 65536) -> None: ...

    async def listen(self) -> None: ...
    async def accept(self, timeout_ms: int = 30000) -> None: ...

    async def serve_requests(
        self,
        handler: Callable[[Message], Awaitable[Message]],
    ) -> None:
        """
        非同期ハンドラでリクエストを処理するサービスループを実行（ブロッキング）。
        run_in_background=True に相当する挙動は asyncio.create_task() で実現する。

        例:
            task = asyncio.create_task(server.serve_requests(my_handler))
            ...
            task.cancel()
            await server.stop()
        """

    async def stop(self) -> None: ...
    async def close(self) -> None: ...

    async def __aenter__(self) -> "AsyncRpcPipeServer": ...
    async def __aexit__(self, *args: object) -> None: ...
```

### 3.3 Phase 1 内部実装方針

```python
# aio.py Phase 1 実装スケルトン
import asyncio
from pipeutil import PipeClient, PipeServer, RpcPipeClient, RpcPipeServer, Message

class AsyncPipeClient:
    def __init__(self, pipe_name: str, buffer_size: int = 65536) -> None:
        self._impl = PipeClient(pipe_name, buffer_size)

    async def connect(self, timeout_ms: int = 5000) -> None:
        # asyncio.to_thread は Python 3.9+、3.8 は run_in_executor にフォールバック
        await asyncio.to_thread(self._impl.connect, timeout_ms)

    async def send(self, msg: Message) -> None:
        await asyncio.to_thread(self._impl.send, msg)

    async def receive(self, timeout_ms: int = 5000) -> Message:
        return await asyncio.to_thread(self._impl.receive, timeout_ms)

    async def close(self) -> None:
        # close() はノンブロッキングなので to_thread 不要
        self._impl.close()

    async def __aenter__(self) -> "AsyncPipeClient":
        return self

    async def __aexit__(self, *args: object) -> None:
        await self.close()
```

#### キャンセル対応

`asyncio.to_thread()` は `CancelledError` を受け取っても内部スレッドは止まらない。
これは Python の制約（`threading.Thread` は中断できない）。
対応策として以下を採用する:

1. **close() による割り込み**: `task.cancel()` 後に `await server.close()` を必ず呼ぶ契約にする
2. **タイムアウトの活用**: `receive(timeout_ms=...)` で短いタイムアウトを設け、キャンセルを定期確認する
3. **Phase 2 での真のキャンセル**: OVERLAPPED I/O の `CancelIoEx` / `io_uring_cancel` で実現

```python
# キャンセル対応パターン（利用側）
async def serve_loop(server: AsyncPipeServer) -> None:
    try:
        while True:
            try:
                msg = await server.receive(timeout_ms=100)  # 短タイムアウトで定期確認
                await server.send(process(msg))
            except PipeTimeoutError:
                continue  # キャンセル確認
    except asyncio.CancelledError:
        await server.close()
        raise
```

### 3.4 Phase 2 内部実装方針（概要）

Phase 2 では Windows IOCP と `asyncio.ProactorEventLoop` を直接接続する。

```
C++ 側:
  AsyncPlatformPipe::async_read(buffer, cb)
      CreateIoCompletionPort + OVERLAPPED
         → GetQueuedCompletionStatus で完了通知
         → Python の asyncio Future.set_result() を呼ぶ

Python 側:
  _pipeutil_async._AsyncPipeClientNative
      .read_into(Future) → C++ async_read を起動
      .write_from(bytes, Future) → C++ async_write を起動
```

Linux では `SelectorEventLoop` に対して `selectors.EpollSelector` 経由で
UNIX ドメインソケットの fd を登録する。

#### Phase 2 C++ インタフェース（概略）

```cpp
// py_async_pipe.hpp (Phase 2 新規)
class PIPEUTIL_API AsyncPlatformPipe {
public:
    explicit AsyncPlatformPipe(std::size_t buf_size);

    // 非同期読み取り: 完了時に cb(bytes_read, error_code) を呼ぶ
    // cb は Python GIL を再取得してから Future.set_result() を呼ぶ必要がある
    void async_read(std::byte* buf, std::size_t len,
                    std::function<void(std::size_t, std::error_code)> cb);

    void async_write(const std::byte* buf, std::size_t len,
                     std::function<void(std::size_t, std::error_code)> cb);

    // Windows: IOCP ハンドルを返す (ProactorEventLoop に登録)
    // Linux:   fd を返す (SelectorEventLoop に登録)
    [[nodiscard]] native_handle_t native_handle() const noexcept;

    void cancel() noexcept;  // CoocelIoEx / io_uring_cancel
    void close() noexcept;
};
```

### 3.5 asyncio ユーティリティ: `serve_connections()`

`MultiPipeServer` の asyncio 版として単一のコルーチンで複数接続を動的に処理する。

```python
async def serve_connections(
    pipe_name: str,
    handler: Callable[[AsyncPipeServer], Awaitable[None]],
    max_connections: int = 8,
    buffer_size: int = 65536,
) -> None:
    """
    接続ごとに asyncio.create_task(handler(conn)) を起動する接続受付ループ。
    stop_event が set されるまで動き続ける。

    例:
        stop = asyncio.Event()
        async def on_connect(conn):
            msg = await conn.receive()
            await conn.send(Message(b'pong'))

        await pipeutil.aio.serve_connections("my_pipe", on_connect)
    """
```

---

## 4. [T] threading 対応

### 4.1 設計原則

1. **既存 API はスレッドセーフ済み**: `PipeClient`/`PipeServer` はミューテックス保護
2. **`concurrent.futures.Future` 連携**: 非同期操作の結果を `Future` で返す
3. **スレッドプール管理**: デフォルトは `ThreadPoolExecutor(max_workers=4)`、差し替え可能

### 4.2 クラス設計

#### ThreadedPipeClient

```python
class ThreadedPipeClient:
    """
    PipeClient の concurrent.futures ラッパー。
    send/receive を Future として非同期実行する。
    """

    def __init__(
        self,
        pipe_name: str,
        buffer_size: int = 65536,
        executor: concurrent.futures.Executor | None = None,
    ) -> None: ...

    def connect(self, timeout_ms: int = 5000) -> None:
        """同期接続（スレッドオフロードなし）。"""

    def send_async(self, msg: Message) -> concurrent.futures.Future[None]:
        """バックグラウンドスレッドで送信。"""

    def receive_async(
        self, timeout_ms: int = 5000
    ) -> concurrent.futures.Future[Message]:
        """バックグラウンドスレッドで受信。"""

    def close(self) -> None: ...

    def __enter__(self) -> "ThreadedPipeClient": ...
    def __exit__(self, *args: object) -> None: ...
```

#### ThreadedPipeServer

```python
class ThreadedPipeServer:
    """
    PipeServer の concurrent.futures ラッパー。
    accept を Future として非同期実行でき、接続ごとにスレッドを振り分ける。
    """

    def __init__(
        self,
        pipe_name: str,
        buffer_size: int = 65536,
        executor: concurrent.futures.Executor | None = None,
    ) -> None: ...

    def listen(self) -> None: ...

    def accept_async(
        self, timeout_ms: int = 30000
    ) -> concurrent.futures.Future[None]:
        """バックグラウンドで accept を実行。完了後 is_connected が True になる。"""

    def send_async(self, msg: Message) -> concurrent.futures.Future[None]: ...
    def receive_async(self, timeout_ms: int = 5000) -> concurrent.futures.Future[Message]: ...
    def close(self) -> None: ...

    def __enter__(self) -> "ThreadedPipeServer": ...
    def __exit__(self, *args: object) -> None: ...
```

### 4.3 内部実装方針

```python
# threading_utils.py の主要な実装スケルトン
import concurrent.futures
from pipeutil import PipeClient, Message

class ThreadedPipeClient:
    def __init__(self, pipe_name, buffer_size=65536, executor=None):
        self._impl = PipeClient(pipe_name, buffer_size)
        self._executor = executor or concurrent.futures.ThreadPoolExecutor(
            max_workers=4,
            thread_name_prefix="pipeutil-thread",
        )
        self._own_executor = executor is None

    def send_async(self, msg: Message) -> concurrent.futures.Future:
        return self._executor.submit(self._impl.send, msg)

    def receive_async(self, timeout_ms: int = 5000) -> concurrent.futures.Future:
        return self._executor.submit(self._impl.receive, timeout_ms)

    def close(self) -> None:
        self._impl.close()
        if self._own_executor:
            self._executor.shutdown(wait=False, cancel_futures=True)
```

### 4.4 `as_completed` パターン

複数サーバーへの並列送信は以下で実現する。

```python
from pipeutil.threading_utils import ThreadedPipeClient
import concurrent.futures

clients = [ThreadedPipeClient(f"pipe_{i}") for i in range(4)]
for c in clients:
    c.connect(timeout_ms=3000)

futures = {c.send_async(msg): c for c in clients}
for fut in concurrent.futures.as_completed(futures):
    client = futures[fut]
    try:
        fut.result()
    except Exception as exc:
        print(f"{client.pipe_name} failed: {exc}")
```

---

## 5. [M] multiprocessing 対応

### 5.1 設計原則と制約

#### spawn 時の問題

Python の `multiprocessing.spawn`（Windows と macOS のデフォルト）では、
親プロセスで作成した C拡張オブジェクト（`PipeClient` 等）は子プロセスに渡せない。

```python
# これは動作しない (spawn コンテキストでは pickle 失敗)
import multiprocessing
c = pipeutil.PipeClient("my_pipe")
c.connect()
p = multiprocessing.Process(target=worker, args=(c,))  # PicklingError!
```

#### fork 時の問題

`fork` コンテキスト（Linux デフォルト）では C拡張オブジェクトが複製されるが、
OS レベルのパイプハンドルが共有される。同一ハンドルから親子両方が読み書きすると競合する。

#### 設計方針

| 問題 | 解決策 |
|---|---|
| spawn: pickle 不可 | `pipe_name`（文字列）のみ渡し、子で新規接続を確立する |
| fork: ハンドル共有 | fork 直前に `close()` +「後継ぎ接続」プロトコル |
| Windows のハンドル継承 | `DuplicateHandle` による明示的な継承（オプション） |

### 5.2 クラス設計

#### WorkerPipeClient

子プロセス内から呼ぶ接続ファクトリ。`pipe_name` を受け取って新規接続を確立する。

```python
class WorkerPipeClient:
    """
    multiprocessing.Process / Pool の worker 関数内で使う接続ファクトリ。
    spawn コンテキストに対応（文字列 pipe_name のみシリアライズ）。

    例:
        def worker(pipe_name: str) -> None:
            with pipeutil.mp.WorkerPipeClient(pipe_name) as client:
                client.connect(timeout_ms=5000)
                msg = client.receive(timeout_ms=5000)
                client.send(pipeutil.Message(b"done"))

        p = multiprocessing.Process(target=worker, args=("my_pipe",))
    """

    def __init__(self, pipe_name: str, buffer_size: int = 65536) -> None: ...

    def connect(self, timeout_ms: int = 5000) -> None: ...
    def send(self, msg: Message) -> None: ...
    def receive(self, timeout_ms: int = 5000) -> Message: ...
    def close(self) -> None: ...

    def __enter__(self) -> "WorkerPipeClient": ...
    def __exit__(self, *args: object) -> None: ...
```

#### ProcessPipeServer

複数の子プロセスを spawn して各プロセスに接続を割り当てるサーバー。

```python
class ProcessPipeServer:
    """
    接続ごとに別プロセスを起動するサーバー（重い処理の CPU バインドな分離に有効）。
    子プロセスは WorkerPipeClient 経由でサーバーパイプに接続する設計にはせず、
    専用の「接続転送」メカニズムを使う。

    内部動作:
      1. メインプロセス: MultiPipeServer で接続を受け付ける
      2. 接続ごとに子プロセスを spawn
      3. 子プロセスに pipe_name を渡し、子プロセスが独自のサーバーパイプを作成する
         （接続を転送する専用ハンドシェイクパイプ経由）

    注意: Windows のハンドル継承 (DuplicateHandle) を用いる上級 API は
          Phase 2 で提供する（§5.4 参照）。
    """

    def __init__(
        self,
        pipe_name: str,
        worker_fn: Callable[[str, int], None],
        max_processes: int = 4,
        buffer_size: int = 65536,
        context: str = "spawn",  # "spawn" | "fork" | "forkserver"
    ) -> None: ...

    def start(self) -> None:
        """接続待機ループをバックグラウンドスレッドで開始。"""

    def stop(self) -> None: ...
    def active_workers(self) -> int: ...
```

#### spawn_worker_factory

`ProcessPoolExecutor` × pipeutil の連携ヘルパー。

```python
def spawn_worker_factory(
    pipe_name: str,
    fn: Callable[[WorkerPipeClient, ...], T],
    *args: object,
    timeout_ms: int = 5000,
    context: str = "spawn",
) -> concurrent.futures.Future[T]:
    """
    spawn した子プロセスで fn(client, *args) を実行し、結果を Future で返す。

    例:
        import pipeutil.mp as pimp
        fut = pimp.spawn_worker_factory(
            "my_pipe",
            lambda c: c.receive(timeout_ms=5000).as_bytes(),
        )
        result: bytes = fut.result(timeout=10)
    """
```

### 5.3 内部実装方針

#### spawn フロー

```
親プロセス:
  MultiPipeServer.listen("my_pipe_worker_N")
  ↓
  接続待機 (accept)
  ↓
  子プロセス spawn("my_pipe_worker_N" を引数)
       ↓
  子プロセス:
    WorkerPipeClient("my_pipe_worker_N")
    connect(timeout_ms=5000)
    ← ハンドシェイク完了 →
    fn(client, ...)
```

#### 接続転送プロトコル（Handshake Pipe パターン）

```
parent                          child
  |                               |
  | ─── listen("pipe_N") ────>    |
  |                               |
  | <──── spawn(pipe_N) ──────    |
  |                               | connect("pipe_N")
  | <──── HELLO frame ──────────  |
  |                               |
  | ─── ACK frame ──────────────> |
  |                               |
  | <──── work() ───────────────  |
```

`HELLO` フレームは `message_id=0xFFFF0001` を使用（予約 ID 範囲内）。

#### fork 時の安全な使い方

```python
# fork コンテキストでの安全なパターン (Linux)
# 親プロセスでの接続は fork 前か fork 後かを意識する

# NG パターン:
client = pipeutil.PipeClient("my_pipe")
client.connect()
pid = os.fork()
if pid == 0:
    client.send(...)  # 危険: 親と同じハンドルを共有している

# OK パターン: fork 前に close し、子プロセスで新規接続
client = pipeutil.PipeClient("my_pipe")
client.connect()
client.close()  # fork 前に明示 close
pid = os.fork()
if pid == 0:
    client2 = pipeutil.PipeClient("my_pipe")
    client2.connect()  # 子で新規接続
```

`mp.py` のドキュメントに上記パターンを明記する。

### 5.4 Phase 2: Windows DuplicateHandle 継承（高度 API）

Windows では `DuplicateHandle` で接続済みパイプハンドルを子プロセスに継承できる。
これにより「接続 → fork → 子プロセスで既存接続を引き継ぐ」が可能になる。

```cpp
// C++ Phase 2: ハンドル継承 API
class PIPEUTIL_API InheritableHandle {
public:
    explicit InheritableHandle(HANDLE h);
    HANDLE duplicate_for(DWORD target_pid) const;
    // Python 側には整数として渡す（HANDLE は intptr_t）
};
```

```python
# Python Phase 2 API (mp.py 追加)
class InheritedPipeClient:
    """
    DuplicateHandle で継承された HANDLE から PipeClient を再構築する。
    Windows 専用。

    例 (親プロセス):
        token = pipeutil.mp.export_handle(client)  # int
        proc = subprocess.Popen(["worker.py", str(token)], ...)

    例 (子プロセス):
        import sys
        token = int(sys.argv[1])
        with pipeutil.mp.InheritedPipeClient(token) as client:
            msg = client.receive(timeout_ms=5000)
    """

    def __init__(self, handle_token: int) -> None: ...
    def send(self, msg: Message) -> None: ...
    def receive(self, timeout_ms: int = 5000) -> Message: ...
    def close(self) -> None: ...

    def __enter__(self) -> "InheritedPipeClient": ...
    def __exit__(self, *args: object) -> None: ...
```

---

## 6. 非機能要件

### 6.1 Python バージョン対応

| 機能 | 最低 Python バージョン | 備考 |
|---|---|---|
| `asyncio.to_thread()` (Phase 1) | 3.9 | 3.8 は `loop.run_in_executor()` にフォールバック |
| `asyncio.TaskGroup` | 3.11 | 非必須（`asyncio.gather` で代替） |
| `concurrent.futures` | 3.2 | 既存サポート範囲内 |
| `multiprocessing.spawn` | 全バージョン | デフォルトはバージョン・OS 依存 |
| Phase 2 IOCP 統合 | 3.9 以上推奨 | `ProactorEventLoop` は 3.8 でも使用可 |

目標: **Python 3.9 以上**（CI は 3.9 / 3.11 / 3.13 で実施）

### 6.2 パフォーマンス目標（Phase 2 以降）

| 指標 | Phase 1 目標 | Phase 2 目標 |
|---|---|---|
| スループット（ローカル） | ≥ 200 MB/s | ≥ 1 GB/s |
| 1000 並列 RPC レイテンシ | < 10 ms (p99) | < 2 ms (p99) |
| asyncio タスク数スケール | スレッドプール上限まで | Unlimited（真の非ブロッキング） |

### 6.3 スレッド安全・プロセス安全

| クラス | スレッド安全 | プロセス安全（fork） | spawn 安全 |
|---|---|---|---|
| `AsyncPipeClient` | ✅（1ループ専任） | ⚠️（close 後に再接続） | ✅（pipe_name 渡し） |
| `ThreadedPipeClient` | ✅ | ⚠️（同上） | ✅ |
| `WorkerPipeClient` | ✅ | ✅（新規接続） | ✅ |
| `InheritedPipeClient` | ✅（単一ハンドル） | ❌（ハンドルは継承不可） | ✅（`Popen` + HANDLE） |

### 6.4 GIL の扱い

- Phase 1: `asyncio.to_thread` は GIL をリリースして I/O スレッドを実行する
- 既存 `recv_frame` / `send_frame` の `Py_BEGIN_ALLOW_THREADS` は維持する
- Phase 2: C++ コールバックから Python Future を set する際は `PyGILState_Ensure` で GIL を取得する

---

## 7. テスト計画

### 7.1 単体テスト（新規追加）

```
tests/python/
  test_aio.py              # AsyncPipeClient/Server の基本 send/receive
  test_aio_rpc.py          # AsyncRpcPipeClient の並列 send_request
  test_aio_cancel.py       # asyncio.CancelledError 伝播
  test_aio_serve.py        # serve_connections() 複数接続
  test_threading_utils.py  # ThreadedPipeClient/Server Future 連携
  test_mp_spawn.py         # WorkerPipeClient spawn テスト
  test_mp_fork.py          # fork コンテキスト (Linux のみ)
  test_mp_process_server.py # ProcessPipeServer
```

### 7.2 非同期テスト構成

```python
# pytest-asyncio を使用
# pyproject.toml に asyncio_mode = "auto" を追加

import asyncio, pytest, pipeutil.aio as aio

@pytest.mark.asyncio
async def test_async_roundtrip():
    pipe = "test_async_rt"
    server_task = asyncio.create_task(_server(pipe))
    await asyncio.sleep(0.05)  # listen 待機

    async with aio.AsyncPipeClient(pipe) as client:
        await client.connect(timeout_ms=3000)
        await client.send(pipeutil.Message(b"ping"))
        resp = await client.receive(timeout_ms=3000)
        assert resp.as_bytes() == b"pong"

    await server_task
```

### 7.3 multiprocessing テスト

```python
# test_mp_spawn.py
import multiprocessing, pipeutil
from pipeutil.mp import WorkerPipeClient

def _worker(pipe_name: str) -> None:
    with WorkerPipeClient(pipe_name) as c:
        c.connect(timeout_ms=5000)
        msg = c.receive(timeout_ms=5000)
        c.send(pipeutil.Message(msg.as_bytes().upper()))

def test_spawn_worker():
    ctx = multiprocessing.get_context("spawn")
    # サーバー側セットアップ
    srv = pipeutil.PipeServer("test_mp_spawn_1")
    srv.listen()
    p = ctx.Process(target=_worker, args=("test_mp_spawn_1",))
    p.start()
    srv.accept(timeout_ms=5000)
    srv.send(pipeutil.Message(b"hello"))
    resp = srv.receive(timeout_ms=5000)
    p.join(timeout=10)
    assert resp.as_bytes() == b"HELLO"
    assert p.exitcode == 0
```

---

## 8. ファイル構成（コード変更一覧）

### Phase 1（C++ 変更なし）

```
python/pipeutil/
  aio.py                   新規: AsyncPipeClient/Server, AsyncRpcPipeClient/Server,
                                  serve_connections()
  threading_utils.py       新規: ThreadedPipeClient, ThreadedPipeServer
  mp.py                    新規: WorkerPipeClient, ProcessPipeServer,
                                  spawn_worker_factory
  __init__.py              更新: aio / threading_utils / mp の public API を re-export
  __init__.pyi             更新: 型スタブ追加
  py.typed                 新規 (既存なければ): PEP 561 マーカー

tests/python/
  test_aio.py              新規
  test_aio_rpc.py          新規
  test_aio_cancel.py       新規
  test_aio_serve.py        新規
  test_threading_utils.py  新規
  test_mp_spawn.py         新規
  test_mp_fork.py          新規 (Linux のみ、pytest.mark.skipif で guard)
  test_mp_process_server.py 新規

pyproject.toml             更新: pytest-asyncio 依存追加、asyncio_mode = "auto"
```

### Phase 2（C++ 追加）

```
source/python/
  py_async_pipe.hpp        新規: AsyncPlatformPipe C++ クラス
  py_async_pipe.cpp        新規: IOCP / io_uring 実装
  py_async_module.cpp      新規: _pipeutil_async Python モジュール

source/core/include/pipeutil/
  async_platform_pipe.hpp  新規: AsyncPlatformPipe 公開ヘッダ（オプション）

source/core/src/
  async_platform_pipe.cpp  新規: Windows IOCP / Linux epoll 実装

python/pipeutil/
  _aio_native.py           新規: Phase 2 ネイティブバックエンドへの切り替えロジック

source/python/CMakeLists.txt 更新: py_async_pipe.cpp を追加
source/core/CMakeLists.txt   更新: async_platform_pipe.cpp を追加
```

---

## 9. 依存・設定追加

### 9.1 pyproject.toml 変更

```toml
[project.optional-dependencies]
async = ["pytest-asyncio>=0.23"]

[tool.pytest.ini_options]
asyncio_mode = "auto"   # @pytest.mark.asyncio を自動付与
```

### 9.2 Python バージョン guard

```python
# aio.py の冒頭
import sys
if sys.version_info < (3, 9):
    raise ImportError("pipeutil.aio requires Python 3.9 or later")

try:
    import asyncio.to_thread  # type: ignore[attr-defined]
    _HAS_TO_THREAD = True
except AttributeError:
    _HAS_TO_THREAD = False
```

---

## 10. 設計上の制約・注意事項

### 10.1 asyncio イベントループの固定

`AsyncPipeClient` は生成時のイベントループに紐づく。
**別スレッドのイベントループで再利用してはならない。**

```python
# NG: loop を切り替えて使いまわす
loop1 = asyncio.new_event_loop()
loop2 = asyncio.new_event_loop()
client = aio.AsyncPipeClient("pipe")
loop1.run_until_complete(client.connect())
loop2.run_until_complete(client.send(...))  # 動作未定義
```

仕様として「1インスタンス = 1イベントループ」を契約にする。

### 10.2 serve_requests と serve_connections の非同時呼び出し

`AsyncRpcPipeServer.serve_requests()` 実行中は `AsyncPipeServer.receive()` を直接呼んではならない
（F-002 spec §7.1 の制約を引き継ぐ）。

### 10.3 Windows の ProactorEventLoop デフォルト

Python 3.8 以降 Windows では `asyncio.ProactorEventLoop` がデフォルト。
Phase 1 は `to_thread` のため `SelectorEventLoop` でも動くが、
Phase 2 IOCP は `ProactorEventLoop` が必須。
Phase 2 モジュールのインポート時に自動チェックする。

```python
# _aio_native.py (Phase 2)
import sys, asyncio
if sys.platform == "win32":
    loop = asyncio.get_event_loop()
    if not isinstance(loop, asyncio.ProactorEventLoop):
        raise RuntimeError(
            "pipeutil Phase 2 async on Windows requires ProactorEventLoop. "
            "Call asyncio.set_event_loop_policy(asyncio.WindowsProactorEventLoopPolicy()) "
            "before importing pipeutil._aio_native."
        )
```

### 10.4 multiprocessing の start method

`mp.py` は spawn コンテキストを推奨し、fork は非推奨（警告を出す）とする。

```python
# mp.py
import multiprocessing, warnings

def _check_start_method(ctx_name: str) -> None:
    if ctx_name == "fork":
        warnings.warn(
            "pipeutil.mp: 'fork' start method may cause pipe handle corruption. "
            "Use 'spawn' (default on Windows/macOS) instead.",
            RuntimeWarning,
            stacklevel=3,
        )
```

---

## 11. 推奨実装順序（Phase 1）

1. `mp.py` — WorkerPipeClient（最シンプル、スレッド・asyncio に依存しない）
2. `threading_utils.py` — ThreadedPipeClient/Server（標準ライブラリのみ）
3. `aio.py` — AsyncPipeClient/Server（asyncio.to_thread ベース）
4. `aio.py` — AsyncRpcPipeClient/Server（F-002 の asyncio ラッパー）
5. `aio.py` — serve_connections()（ユーティリティ）
6. テスト全件 PASS
7. `__init__.py` / `__init__.pyi` 更新、ドキュメント追記

> Phase 2 は Phase 1 の全テスト PASS 後に着手する。
> Phase 2 の C++ 設計は Phase 1 完了後に別途 spec を起こす。
