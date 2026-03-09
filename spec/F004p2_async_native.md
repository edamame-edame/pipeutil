# F-004 Phase 2 詳細設計: IOCP / epoll ネイティブ非同期 I/O

**作成日**: 2026-03-10
**対象バージョン**: v0.5.0
**依存**: F-004 Phase 1（aio.py 実装済み） / F-002（FrameHeader v0.02）

---

## 1. 背景と目的

### 1.1 Phase 1 の限界

Phase 1 の `AsyncPipeClient/Server` は `asyncio.to_thread()` でブロッキング I/O を
スレッドプールにオフロードする。これは以下の問題を抱える。

| 問題 | 詳細 |
|---|---|
| スレッドオーバーヘッド | 受信 1 件ごとに TPE ワーカースレッドを占有。高接続数で枯渇する |
| 疑似的キャンセル | `CancelledError` を受けてもスレッド内のブロッキング I/O は即座に中断できない |
| レイテンシ | スレッドコンテキストスイッチが 1 往復追加される |

### 1.2 Phase 2 の目的

| 項目 | 目標 |
|---|---|
| 真のノンブロッキング I/O | Windows: IOCP / Linux: epoll（UNIX ソケット fd 登録）で I/O スレッドゼロ |
| 真のキャンセル | `CancelIoEx`（Windows）/ `read()` 非同期破棄（Linux）で即座に中断 |
| API 互換 | Phase 1 との公開 API は完全同一。内部実装のみ置き換え |
| opt-in ビルド | CMake `-DPIPEUTIL_WITH_ASYNC=ON` でのみ `_pipeutil_async` を生成 |

### 1.3 スコープ

| 項目 | Phase 2 対象 |
|---|---|
| `AsyncPipeClient` / `AsyncPipeServer` の内部置き換え | ✅ |
| `AsyncRpcPipeClient` / `AsyncRpcPipeServer` の内部置き換え | ✅ |
| `serve_connections()` の最適化 | ✅（Phase 1 と同 API、内部は native） |
| `ThreadedPipeClient` / `ProcessPipeServer` | ❌（Phase 2 対象外） |
| Linux io_uring サポート | ❌ 対象外（epoll のみ。io_uring は v0.6.0 候補） |

---

## 2. アーキテクチャ概観

```
┌───────────────────────────────────────────────────────────────────┐
│  Python 公開 API (pipeutil.aio)  ← Phase 1/2 で完全に同一         │
│  AsyncPipeClient / AsyncPipeServer / serve_connections()          │
└──────────────────┬────────────────────────────────────────────────┘
                   │ Phase 1 fallback (to_thread)
                   │ Phase 2 native backend  ←── 自動切り替え
┌──────────────────▼────────────────────────────────────────────────┐
│  pipeutil/_aio_native.py   ← Python グルー                        │
│  _NativeAsyncPipeHandle: connect / read_frame / write_frame       │
└──────────────────┬────────────────────────────────────────────────┘
                   │ import _pipeutil_async (C 拡張)
┌──────────────────▼────────────────────────────────────────────────┐
│  _pipeutil_async (C 拡張モジュール)                                 │
│  AsyncPipeHandle: 接続・非同期 I/O・キャンセルの Python 型          │
└──────────────────┬────────────────────────────────────────────────┘
                   │
┌──────────────────▼────────────────────────────────────────────────┐
│  source/python/py_async_pipe.hpp/.cpp (C++)                       │
│  AsyncPlatformPipe: OS 非同期 I/O 抽象レイヤー                      │
│  Windows: IOCP + dispatch thread                                   │
│  Linux:   non-blocking fd + SelectorEventLoop                      │
└───────────────────────────────────────────────────────────────────┘
```

### 2.1 ファイル構成

```
追加・変更ファイル（Phase 2）:

source/python/
  py_async_pipe.hpp      ← 新規: AsyncPlatformPipe C++ クラス宣言
  py_async_pipe.cpp      ← 新規: AsyncPlatformPipe 実装 (Win32/POSIX)
  py_async_module.cpp    ← 新規: _pipeutil_async 拡張モジュール定義

python/pipeutil/
  _aio_native.py         ← 新規: C 拡張 → aio.py グルーレイヤー
  aio.py                 ← 更新: native backend の自動検出・切り替え

source/
  CMakeLists.txt         ← 更新: PIPEUTIL_WITH_ASYNC オプション追加
  python/CMakeLists.txt  ← 更新: _pipeutil_async ターゲット追加

tests/python/
  test_aio_native.py     ← 新規: native backend 専用テスト (requires native)
```

---

## 3. C++ レイヤー設計 (`AsyncPlatformPipe`)

### 3.1 設計原則

1. **ディスパッチスレッドモデル（Windows）**：C++ 側が専用 IOCP + バックグラウンドスレッドを管理する。
   Python の ProactorEventLoop の内部 IOCP とは独立させる（プライベート API 非依存）。
   完了通知は `loop.call_soon_threadsafe()` で asyncio ループへ安全に注入する。

2. **fd 登録モデル（Linux）**：UNIX ドメインソケットを `O_NONBLOCK` で開き、
   `loop.add_reader/add_writer(fd, callback)` で SelectorEventLoop に登録する。
   コールバックは asyncio スレッド上で呼ばれるため GIL 取得不要。

3. **フレームプロトコル維持**：Phase 1 と同一の 20 バイト `FrameHeader`（magic / version / payload_size / checksum/CRC-32C / message_id）を非同期 I/O でも正確に実装する。

4. **GIL 安全**：ディスパッチスレッド（Windows）でのコールバック内では必ず
   `Py_BEGIN_ALLOW_THREADS` / `Py_END_ALLOW_THREADS` を適切に扱う。

### 3.2 `AsyncPlatformPipe` クラス

```cpp
// source/python/py_async_pipe.hpp
#pragma once

#include "pipeutil/pipeutil_export.hpp"
#include "pipeutil/detail/frame_header.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <system_error>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace pipeutil::async {

// 非同期 I/O 完了コールバック型
using ReadCallback  = std::function<void(std::error_code, std::vector<std::byte>)>;
using WriteCallback = std::function<void(std::error_code)>;

// ──────────────────────────────────────────────────────────────────────────────
// AsyncPlatformPipe — OS 非同期 I/O 抽象レイヤー
//
// インスタンス生成 → connect/server_create → async_read_frame/async_write_frame → close
// スレッドセーフ: connect/close は呼び出し元スレッドから。
// async_read_frame / async_write_frame は asyncio スレッドから呼ぶこと（同時に
// 1 件ずつ。並列呼び出し不可）。
// ──────────────────────────────────────────────────────────────────────────────
class PIPEUTIL_API AsyncPlatformPipe {
public:
    explicit AsyncPlatformPipe(std::size_t buf_size = 65536);
    ~AsyncPlatformPipe();

    // コピー・ムーブ禁止（HANDLE / fd 管理を単純化）
    AsyncPlatformPipe(const AsyncPlatformPipe&)            = delete;
    AsyncPlatformPipe& operator=(const AsyncPlatformPipe&) = delete;
    AsyncPlatformPipe(AsyncPlatformPipe&&)                 = delete;
    AsyncPlatformPipe& operator=(AsyncPlatformPipe&&)      = delete;

    // ─── 接続（同期: 呼び出し元スレッドでブロック） ──────────────────

    /// クライアントとしてサーバーに接続する
    /// 例外: PipeException (Timeout / NotFound / SystemError)
    void client_connect(const std::string& pipe_name, int64_t timeout_ms);

    /// サーバーとしてパイプを作成して接続を受け入れる（fork した接続済みインスタンスを返す）
    /// 例外: PipeException (Timeout / SystemError)
    std::unique_ptr<AsyncPlatformPipe> server_create_and_accept(
        const std::string& pipe_name, int64_t timeout_ms);

    // ─── 非同期 I/O ──────────────────────────────────────────────────

    /// FrameHeader + payload を非同期に読み取る。
    /// 完了時 cb(error_code, decoded_payload_bytes) が呼ばれる。
    /// cb は loop.call_soon_threadsafe 経由で asyncio スレッドに注入される（Windows）。
    /// または asyncio スレッド上で直接呼ばれる（Linux）。
    void async_read_frame(ReadCallback cb);

    /// FrameHeader + payload を非同期に送信する。
    /// message_id == 0: 通常メッセージ (FLAG_REQUEST / RESPONSE は 0)
    void async_write_frame(std::span<const std::byte> payload,
                           uint32_t message_id,
                           WriteCallback cb);

    // ─── キャンセル / クローズ ────────────────────────────────────────

    /// 実行中の非同期 I/O をキャンセルする（noexcept）
    /// Windows: CancelIoEx / Linux: close + reopen / shutdown
    void cancel() noexcept;

    /// ハンドル / fd を閉じる（noexcept）
    void close() noexcept;

    // ─── OS ハンドル照会 ─────────────────────────────────────────────

    /// Windows: 関連付けた HANDLE を返す（Python 側で ProactorEventLoop 統合に使用可能）
    /// Linux:   非ブロッキング fd を返す（loop.add_reader/writer 登録に使用）
    [[nodiscard]] std::intptr_t native_handle() const noexcept;

    [[nodiscard]] bool is_connected() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pipeutil::async
```

### 3.3 Windows IOCP ディスパッチスレッドモデル

#### ディスパッチスレッドのライフサイクル（R-038 対応）

| タイミング | 操作 |
|---|---|
| `client_connect()` 成功後 | `hIocp_` 作成 → `dispatch_thread_` 開始 |
| `server_create_and_accept()` 返却後 | フォーク済みインスタンスの `dispatch_thread_` 開始 |
| `close()` / `cancel()` | `running_.store(false)` + `PostQueuedCompletionStatus(0, CLOSE_KEY, nullptr)` で thread を停止 → `dispatch_thread_.join()` |

`dispatch_thread_.join()` は `close()` 内で必ず呼び、スレッドリークを防ぐこと。

```
┌──────────────────────────────────────────────────────────────────┐
│ AsyncPlatformPipe::Impl (Windows)                                 │
│                                                                   │
│  HANDLE hPipe_      → CreateFile(..., FILE_FLAG_OVERLAPPED)       │
│  HANDLE hIocp_      → CreateIoCompletionPort(hPipe_, ...)         │
│  std::thread dispatch_thread_  ← connect/accept 時に開始          │
│  std::atomic<bool>  running_   ← close() で false にして join     │                                       │
│                                                                   │
│  dispatch_thread_ ループ:                                          │
│    GetQueuedCompletionStatus(hIocp_, &bytes, &key, &pOv, INFINITE) │
│    → 完了キー別に ReadCallback / WriteCallback を取り出す          │
│    → loop_.call_soon_threadsafe(callback, result)                  │
│    → ループが閉じていた場合（RuntimeError）はログのみで無視（R-043）  │
│                                                                   │
│  async_read_frame():                                               │
│    ① ReadFileEx(hPipe_, hdr_buf, 20, &ov_read_, nullptr)           │
│    ② dispatch_thread_ が hdr 完了を受信                            │
│    ③ magic/version/payload_size 検証後、ReadFileEx(payload_buf, N) │
│    ④ dispatch_thread_ が payload 完了を受信 → CRC-32C 検証         │
│    ⑤ loop_.call_soon_threadsafe(cb, error_code, payload_bytes)     │
│                                                                   │
│  cancel():                                                         │
│    CancelIoEx(hPipe_, nullptr) → dispatch_thread_ に ERROR_OPERATION_ABORTED が返る │
│    → cb(make_error_code(errc::operation_canceled), {}) を注入      │
└──────────────────────────────────────────────────────────────────┘
```

**注意事項（R-039 / R-043 対応）**:
- `async_read_frame` は同一ハンドルで同時に 1 件のみ許容（並列呼び出し → `std::logic_error`）
- IOCP は `hPipe_` ごとに 1 つ作成。1 パイプ = 1 dispatch thread。v0.5.0 の 1 接続上限は **64 接続**（R-044 対応、§12.1 参照）
- dispatch_thread_ 内では Python GIL を**保持しない**。`PyObject*` の使用は `call_soon_threadsafe` に渡す直前まで行わない
- `async_read_frame` / `async_write_frame` 呼び出し時に `Py_INCREF(loop_obj)` / `Py_INCREF(future_obj)` を行い、
  コールバック完了後（またはキャンセル時）に dispatch_thread_ ではなく asyncio スレッド上で `Py_DECREF` すること（R-039 対応）
- `call_soon_threadsafe` が `RuntimeError`（ループクローズ）を送出した場合は `PyErr_Clear()` でエラーをクリアし、
  `PIPELOG` でデバッグログを出力して無視する（R-043 対応）

### 3.4 Linux epoll モデル

> **注記（R-042 対応）**: `add_reader` / `add_writer` は `asyncio.SelectorEventLoop` 専用の API。
> Windows のデフォルトループ `ProactorEventLoop` では `NotImplementedError` が送出される。
> Linux epoll モデルは Linux 環境のみ有効。Windows では §3.3 の IOCP モデルのみ使用する。
> `_aio_native.py` の `NativeAsyncPipe` は実行環境を判定せず、`AsyncPipeHandle` の
> プラットフォーム実装（`#ifdef _WIN32`）が自動的に適切なモデルを選択する。

```
┌──────────────────────────────────────────────────────────────────┐
│ AsyncPlatformPipe::Impl (Linux)                                   │
│                                                                   │
│  int sock_fd_       → UNIX ドメインソケット（O_NONBLOCK）           │
│                                                                   │
│  async_read_frame():                                               │
│    ① ReadState に cb を保存                                         │
│    ② loop.add_reader(sock_fd_, _on_readable_cb)                    │
│         _on_readable_cb はヘッダ→ペイロードの段階読み取り            │
│         段階完了後: loop.remove_reader(sock_fd_); cb(ec, data)      │
│                                                                   │
│  async_write_frame():                                             │
│    ① フレームを送信バッファに積む                                    │
│    ② writev() で非ブロッキング送信試行                              │
│    ③ EAGAIN なら loop.add_writer(sock_fd_, _on_writable_cb)        │
│    ④ 完全送信後: loop.remove_writer(sock_fd_); cb(ec)              │
│                                                                   │
│  cancel():                                                         │
│    loop.remove_reader(sock_fd_); loop.remove_writer(sock_fd_)      │
│    cb(errc::operation_canceled, {}) を直接呼ぶ                     │
└──────────────────────────────────────────────────────────────────┘
```

---

## 4. Python 拡張モジュール設計 (`_pipeutil_async`)

### 4.1 公開型

#### `server_create_and_accept` の Python ラップ方針（R-040 対応）

`server_create_and_accept()` は C++ では `std::unique_ptr<AsyncPlatformPipe>` を返す。
Python 拡張側では `py_async_module.cpp` 内で次の手順でラップする。

1. `PyObject* new_handle = PyAsyncPipeHandle_Type.tp_alloc(&PyAsyncPipeHandle_Type, 0);`
2. `((PyAsyncPipeHandle*)new_handle)->pipe = released_ptr.release();` （所有権移転）
3. `return new_handle;`

失敗時（`tp_alloc` が `nullptr`）は `released_ptr` がスコープ脱出時に解放されるため
元の `unique_ptr` を保持したままにしておくこと（`Py_DECREF` 不要）。

```python
# _pipeutil_async — C 拡張（source/python/py_async_module.cpp 生成）

class AsyncPipeHandle:
    """
    接続済み名前付きパイプ / UNIX ソケットの非同期 I/O ハンドル。
    asyncio のイベントループと統合して GIL 解放状態でI/O を実行する。

    使用上の前提:
      - connect() / server_create_and_accept() 後のみ async_read_frame / async_write_frame 可
      - 同一インスタンスで複数の async_read_frame を同時に発行してはならない
      - イベントループをまたいで使用してはならない
    """

    def __init__(self, buf_size: int = 65536) -> None: ...

    # ─── 接続 ────────────────────────────────────────────────────────

    def connect(self, pipe_name: str, timeout_ms: int = 5000) -> None:
        """
        クライアントとして接続（同期）。
        GIL を解放して接続待機する。
        例外: pipeutil.TimeoutError / pipeutil.PipeError
        """

    def server_create_and_accept(
        self, pipe_name: str, timeout_ms: int = 0
    ) -> "AsyncPipeHandle":
        """
        サーバーとして受付・接続済みインスタンス返却（同期）。
        自身は次クライアントを受け入れられる状態を維持する。
        """

    # ─── 非同期 I/O ──────────────────────────────────────────────────

    def async_read_frame(
        self, loop: asyncio.AbstractEventLoop, future: asyncio.Future
    ) -> None:
        """
        FrameHeader + payload を非同期に読み取る。
        完了時 future.set_result(bytes) または future.set_exception(exc) をスケジュールする。
        """

    def async_write_frame(
        self,
        payload: bytes | bytearray | memoryview,
        message_id: int,
        loop: asyncio.AbstractEventLoop,
        future: asyncio.Future,
    ) -> None:
        """
        FrameHeader + payload を非同期に送信する。
        完了時 future.set_result(None) または future.set_exception(exc) をスケジュールする。
        """

    # ─── キャンセル・クローズ ─────────────────────────────────────────

    def cancel(self) -> None:
        """実行中の非同期 I/O をキャンセルする（noexcept 相当）。"""

    def close(self) -> None:
        """ハンドルを閉じリソースを解放する（べきとう）。"""

    # ─── コンテキストマネージャ ───────────────────────────────────────

    def __enter__(self) -> "AsyncPipeHandle": ...
    def __exit__(self, *args: object) -> None: ...

    @property
    def is_connected(self) -> bool: ...
```

### 4.2 拡張モジュール定義スケルトン

```cpp
// source/python/py_async_module.cpp

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "py_async_pipe.hpp"
#include "pipeutil/pipe_error.hpp"
#include "py_debug_log.hpp"

// ─── AsyncPipeHandle Python 型 ──────────────────────────────────────

struct PyAsyncPipeHandle {
    PyObject_HEAD
    pipeutil::async::AsyncPlatformPipe* pipe;  // owned
};

// tp_new / tp_dealloc / tp_methods ...

static PyMethodDef AsyncPipeHandle_methods[] = {
    {"connect",                  PyAsyncPipeHandle_connect,                  METH_VARARGS | METH_KEYWORDS, nullptr},
    {"server_create_and_accept", PyAsyncPipeHandle_server_create_and_accept, METH_VARARGS | METH_KEYWORDS, nullptr},
    {"async_read_frame",         PyAsyncPipeHandle_async_read_frame,         METH_VARARGS | METH_KEYWORDS, nullptr},
    {"async_write_frame",        PyAsyncPipeHandle_async_write_frame,        METH_VARARGS | METH_KEYWORDS, nullptr},
    {"cancel",                   PyAsyncPipeHandle_cancel,                   METH_NOARGS,                  nullptr},
    {"close",                    PyAsyncPipeHandle_close,                    METH_NOARGS,                  nullptr},
    {nullptr, nullptr, 0, nullptr},
};
```

---

## 5. Python グルーレイヤー設計 (`_aio_native.py`)

### 5.1 役割

`_pipeutil_async.AsyncPipeHandle` の `async_read_frame` / `async_write_frame` は
`asyncio.Future` を受け取るが、Python 利用者には `await conn.receive()` として使わせたい。
`_aio_native.py` は Future 生成・キャンセルハンドリングを担うグルーレイヤー。

### 5.2 実装スケルトン

```python
# python/pipeutil/_aio_native.py
# Phase 2 native backend のグルーレイヤー
# aio.py からのみ import される（直接公開 API ではない）
from __future__ import annotations

import asyncio
from typing import Any

from ._pipeutil_async import AsyncPipeHandle
from ._pipeutil import Message, PipeError


class NativeAsyncPipe:
    """AsyncPipeHandle を asyncio コルーチンインタフェースで包むラッパー。"""

    def __init__(self, buf_size: int = 65536) -> None:
        self._handle = AsyncPipeHandle(buf_size)

    async def connect(self, pipe_name: str, timeout_ms: int = 5000) -> None:
        """接続（GIL を解放して待機）。"""
        loop = asyncio.get_running_loop()
        # connect は同期だが GIL 解放済みなので to_thread 不要
        await loop.run_in_executor(None, self._handle.connect, pipe_name, timeout_ms)

    async def read_frame(self) -> Message:
        """FrameHeader + payload を非同期受信。CancelledError 対応。"""
        loop = asyncio.get_running_loop()
        future: asyncio.Future[bytes] = loop.create_future()

        self._handle.async_read_frame(loop, future)

        try:
            payload = await future
        except asyncio.CancelledError:
            self._handle.cancel()
            raise
        return Message(payload)

    async def write_frame(self, msg: Message, message_id: int = 0) -> None:
        """FrameHeader + payload を非同期送信。"""
        loop = asyncio.get_running_loop()
        future: asyncio.Future[None] = loop.create_future()

        self._handle.async_write_frame(bytes(msg), message_id, loop, future)

        try:
            await future
        except asyncio.CancelledError:
            self._handle.cancel()
            raise

    def cancel(self) -> None:
        """実行中の I/O をキャンセルする。"""
        self._handle.cancel()

    def close(self) -> None:
        self._handle.close()

    async def __aenter__(self) -> "NativeAsyncPipe":
        return self

    async def __aexit__(self, *args: Any) -> None:
        self.close()

    @property
    def is_connected(self) -> bool:
        return self._handle.is_connected
```

---

## 6. `aio.py` 自動切り替え設計

### 6.1 バックエンド検出

`aio.py` の先頭で `_pipeutil_async` の import を試み、成功した場合に限り native backend を使使用する。
`_pipeutil_async` が存在しない場合は Phase 1 の `to_thread` ベースにフォールバックする。

```python
# aio.py の先頭（抜粋）

try:
    from ._aio_native import NativeAsyncPipe as _NativeAsyncPipe
    _NATIVE_BACKEND: bool = True
except ImportError:
    _NATIVE_BACKEND = False

def _is_native() -> bool:
    """ネイティブバックエンドが利用可能かを返す（デバッグ/テスト用）。"""
    return _NATIVE_BACKEND
```

### 6.2 `AsyncPipeClient` 内部切り替え

```python
class AsyncPipeClient:
    """
    非同期パイプクライアント。
    Phase 1 (__NATIVE_BACKEND=False): asyncio.to_thread() ベース
    Phase 2 (_NATIVE_BACKEND=True):   _pipeutil_async ネイティブバックエンド
    """

    def __init__(self, pipe_name: str, buffer_size: int = 65536) -> None:
        self._pipe_name = pipe_name
        self._buffer_size = buffer_size
        if _NATIVE_BACKEND:
            self._native = _NativeAsyncPipe(buffer_size)
            self._impl   = None
        else:
            self._native = None
            self._impl   = PipeClient(pipe_name, buffer_size)  # Phase 1

    async def receive(self, timeout_ms: int = 5000) -> Message:
        if _NATIVE_BACKEND:
            return await self._native.read_frame()  # type: ignore[union-attr]
        else:
            # Phase 1 fallback
            return await asyncio.to_thread(self._impl.receive, timeout_ms)  # type: ignore[union-attr]
    ...
```

### 6.3 `pipeutil.aio.is_native()` 関数の公開

```python
# aio.py に追加
def is_native() -> bool:
    """
    True なら IOCP/epoll ネイティブバックエンドが有効。
    False なら Phase 1 (asyncio.to_thread) ベース。
    `pip install pipeutil` であれば常に True（v0.5.0 以降）。
    ソースビルドで PIPEUTIL_WITH_ASYNC=OFF の場合は False。
    """
    return _NATIVE_BACKEND
```

---

## 7. CMake 変更

### 7.1 ビルドオプション追加

```cmake
# source/CMakeLists.txt への追加

option(PIPEUTIL_WITH_ASYNC
    "Build _pipeutil_async native async extension (requires Python dev headers)"
    OFF   # デフォルト OFF。v0.5.0 リリース手順で ON に切り替え
)

if(PIPEUTIL_WITH_ASYNC)
    add_subdirectory(python_async)  # または python/CMakeLists.txt へ統合
endif()
```

### 7.2 `_pipeutil_async` ターゲット

```cmake
# source/python/CMakeLists.txt への追加(抜粋)

if(PIPEUTIL_WITH_ASYNC)
    Python3_add_library(_pipeutil_async MODULE
        py_async_pipe.cpp
        py_async_module.cpp
    )
    target_link_libraries(_pipeutil_async PRIVATE
        pipeutil_core      # FrameHeader / PipeException 等
    )
    target_compile_features(_pipeutil_async PRIVATE cxx_std_20)
    set_target_properties(_pipeutil_async PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/python/pipeutil"
    )
    if(WIN32)
        target_link_libraries(_pipeutil_async PRIVATE kernel32)
    endif()
endif()
```

---

## 8. フレームプロトコル実装（非同期版）

Phase 2 の async_read_frame は 2 段階で 20 バイトヘッダ → N バイトペイロードを非同期に読む。

### CRC-32C の計算主体（R-041 対応）

| 操作 | CRC-32C の扱い |
|---|---|
| `async_write_frame` | **C++ 実装が** payload の CRC-32C を計算し `FrameHeader.checksum` に書き込んで送信 |
| `async_read_frame` | 受信ペイロードの CRC-32C を計算し `FrameHeader.checksum` と照合。不一致 → `InvalidMessage` |

CRC-32C 実装は既存 `source/core/src/detail/` のヘルパーを再利用すること。

### 8.1 状態遷移

```
ReadState: IDLE → READ_HEADER → PARSE_HEADER → READ_PAYLOAD → DONE / ERROR
```

| 状態 | 処理 |
|---|---|
| `READ_HEADER` | 20 バイト非同期読み取り開始 |
| `PARSE_HEADER` | magic/version 検証・payload_size 取得 |
| `READ_PAYLOAD` | payload_size バイト非同期読み取り開始 |
| `DONE` | CRC-32C 検証・ReadCallback 呼び出し |
| `ERROR` | PipeException → ReadCallback に error_code を渡す |

### 8.2 ヘッダ検証チェックリスト

1. `magic` == `{0x50, 0x49, 0x50, 0x45}` (`"PIPE"`)
2. `version` == `PROTOCOL_VERSION` (0x02)
3. `payload_size` <= 運用上限（現行: 2 GiB − 1 = `0x7FFFFFFF`）
4. `checksum` == `crc32c(payload)`（payload 受信後）

検証失敗時は `PipeErrorCode::InvalidMessage` を error_code として ReadCallback に渡す。

---

## 9. テスト設計

| # | テスト名 | ファイル | 内容 | 備考 |
|---|---|---|---|---|
| 1 | `test_native_available` | `test_aio_native.py` | `pipeutil.aio.is_native()` が True | `_pipeutil_async` ビルド済み環境のみ |
| 2 | `test_native_roundtrip` | `test_aio_native.py` | 接続・送受信（1往復）の動作確認 | native backend 強制 |
| 3 | `test_native_cancel_read` | `test_aio_native.py` | 受信中 `task.cancel()` → `CancelledError` が返る | True cancel テスト |
| 4 | `test_native_large_payload` | `test_aio_native.py` | 1 MiB ペイロードの送受信 | チャンク分割の確認 |
| 5 | `test_native_concurrent` | `test_aio_native.py` | 10 並列タスクで同一サーバーへ接続（serve_connections） | スループットテスト |
| 6 | `test_fallback_when_native_absent` | `test_aio.py` | `_pipeutil_async` なし環境で AsyncPipeClient が to_thread で動作する | `importlib.reload` でモック |
| 7 | `test_api_compatibility_native` | `test_aio_native.py` | Phase 1 と Phase 2 で同一 API テストが通る | 互換性確認 |

### 9.1 skipif条件

```python
import pytest
import pipeutil.aio as _aio

pytestmark = pytest.mark.skipif(
    not _aio.is_native(),
    reason="_pipeutil_async not built (run cmake -DPIPEUTIL_WITH_ASYNC=ON)"
)
```

---

## 10. ファイル変更サマリー

| ファイル | 操作 | 影響 |
|---|---|---|
| `source/python/py_async_pipe.hpp` | 新規 | `AsyncPlatformPipe` クラス宣言 |
| `source/python/py_async_pipe.cpp` | 新規 | Win32/POSIX 実装（#ifdef 切り替え） |
| `source/python/py_async_module.cpp` | 新規 | `_pipeutil_async` Python 拡張モジュール |
| `source/CMakeLists.txt` | 更新 | `PIPEUTIL_WITH_ASYNC` オプション追加 |
| `source/python/CMakeLists.txt` | 更新 | `_pipeutil_async` ターゲット追加 |
| `python/pipeutil/_aio_native.py` | 新規 | C 拡張 → aio.py グルーレイヤー |
| `python/pipeutil/aio.py` | 更新 | `_NATIVE_BACKEND` 切り替え・`is_native()` 追加 |
| `python/pipeutil/__init__.py` | 更新 | `pipeutil.aio.is_native` の再 export（optional） |
| `tests/python/test_aio_native.py` | 新規 | native 専用テスト 7 件 |

---

## 11. 実装タスク順序

| # | タスク | 見積 | 依存 |
|---|---|---|---|
| T1 | `py_async_pipe.hpp` クラス設計・宣言 | 小 | なし |
| T2 | `py_async_pipe.cpp` Windows IOCP 実装 | 大 | T1 |
| T3 | `py_async_pipe.cpp` Linux epoll 実装 | 中 | T1 |
| T4 | `py_async_module.cpp` C 拡張定義 | 中 | T1 |
| T5 | CMakeLists.txt 更新 | 小 | T4 |
| T6 | `_aio_native.py` グルーレイヤー | 小 | T4 |
| T7 | `aio.py` 自動切り替え実装 | 小 | T6 |
| T8 | `test_aio_native.py` テスト作成 | 小 | T7 |
| T9 | ビルド確認・テスト全通過 | 中 | T8 |

**合計見積**: 2〜3日（T2 の IOCP 実装が難度最大）

---

## 12. 設計上の注意事項・リスク

### 12.1 IOCP dispatch thread の本数管理（R-044 対応）

v0.5.0 では **1 接続 = 1 dispatch thread** とする。
上限は `AsyncPlatformPipe::Impl` の生成時に接続数カウンタ（`std::atomic<int>` グローバル）で管理し、
**64 接続超過時は `PipeException{TooManyConnections}` を送出**する。
（`PipeErrorCode` に `TooManyConnections = 30` を追加する。）
v0.6.0 で共有 IOCP プール（スレッド数上限設定可能）へ移行することで この制限を撤廃予定。

### 12.2 Python ProactorEventLoop の IOCP との独立性

Python 3.8+ の `ProactorEventLoop` は内部で `_winapi.CreateIoCompletionPort` を使っている。
本設計では **独自 IOCP** を作成するため ProactorEventLoop の内部 IOCP とは別物になる。
これにより asyncio のプライベート API に依存しない安定した設計となる。

### 12.3 Linux の `SelectorEventLoop` との制約

`add_reader(fd, cb)` は fd が **同一イベントループで 1 件のみ登録可能**。
`async_read_frame` 中に 2 回目を呼ぶと `ValueError` が発生するため、
`NativeAsyncPipe` が直列呼び出しを保証しなければならない（コルーチン内の `await` により自然に直列化される）。

### 12.4 `call_soon_threadsafe` の GIL 安全性

Windows dispatch thread から `loop.call_soon_threadsafe(future.set_result, data)` を
呼ぶ際、`future` オブジェクトの参照カウントを保持しておく必要がある。
C++ 側で `PyObject*` として保持する場合は `Py_INCREF` / `Py_DECREF` の対称性を必ず守ること。

### 12.5 フォールバック互換性の永続的担保

`_aio_native.py` が `ImportError` を送出した場合、`aio.py` は必ず Phase 1 にフォールバックする。
CI マトリクスに「`PIPEUTIL_WITH_ASYNC=OFF` 環境での全テスト通過」を含めることで
フォールバックパスの回帰を防ぐ（`review/parent_reviewer_proposal_ci_matrix_20260308.md` 参照）。
