# pipeutil — Python C API ラッパー仕様

## 1. 概要

`_pipeutil` は `pipeutil_core` DLL/SO を Python から利用するための **Python C API** 拡張モジュールです。
`PyObject*` 型システムを通じて C++ クラスを Python 型として公開します。

---

## 2. モジュール構成

```
source/python/
├── _pipeutil_module.cpp      # PyModuleDef, モジュール初期化
├── py_pipe_server.cpp        # PyPipeServer 型定義
├── py_pipe_client.cpp        # PyPipeClient 型定義
├── py_message.cpp            # PyMessage 型定義
├── py_exceptions.cpp         # 例外変換・登録
└── CMakeLists.txt
```

---

## 3. 例外変換

### 3.1 例外クラス階層

Python 側に `pipeutil` 独自例外を登録する。

```
Exception
└── pipeutil.PipeError                (_pipeutil.PipeError)
    ├── pipeutil.TimeoutError
    ├── pipeutil.ConnectionResetError
    ├── pipeutil.BrokenPipeError
    ├── pipeutil.NotConnectedError
    └── pipeutil.InvalidMessageError
```

```cpp
// py_exceptions.cpp
namespace pyutil {

// モジュール初期化時に PyModule_AddObjectRef で登録
extern PyObject* g_PipeError;
extern PyObject* g_TimeoutError;
extern PyObject* g_ConnectionResetError;
extern PyObject* g_BrokenPipeError;
extern PyObject* g_NotConnectedError;
extern PyObject* g_InvalidMessageError;

/// C++ 例外 → Python 例外 への変換
/// catch ブロックで呼び出す: catch (const pipeutil::PipeException& e) { set_python_exception(e); return nullptr; }
void set_python_exception(const pipeutil::PipeException& e) noexcept;

} // namespace pyutil
```

### 3.2 `PipeErrorCode` → Python 例外 マッピング表

| `PipeErrorCode` | Python 例外 |
|----------------|------------|
| `Timeout` | `pipeutil.TimeoutError` |
| `ConnectionReset` | `pipeutil.ConnectionResetError` |
| `BrokenPipe` | `pipeutil.BrokenPipeError` |
| `NotConnected` | `pipeutil.NotConnectedError` |
| `InvalidMessage` | `pipeutil.InvalidMessageError` |
| その他 | `pipeutil.PipeError` |

---

## 4. `PyMessage` 型

### 4.1 Python 型定義

```cpp
typedef struct {
    PyObject_HEAD
    pipeutil::Message* msg;   // C++ オブジェクトへのポインタ（所有権あり）
} PyMessage;
```

### 4.2 公開メソッド・プロパティ

| Python 側 | 型 | 説明 |
|-----------|-----|------|
| `Message(data)` | コンストラクタ | `data`: `bytes` または `str`（UTF-8 エンコード） |
| `.data` | `bytes` プロパティ（読み取り専用）| ペイロードを `bytes` として返す |
| `.text` | `str` プロパティ（読み取り専用）| ペイロードを UTF-8 文字列として返す（デコード失敗時 `UnicodeDecodeError`） |
| `len(msg)` | `int` | ペイロードのバイト数 |
| `bool(msg)` | `bool` | ペイロードが空でなければ `True` |
| `repr(msg)` | `str` | `"Message(size=N)"` |

### 4.3 型変換規則

```cpp
// bytes → Message
PyObject* bytes_obj = ...;
Py_buffer view;
if (PyObject_GetBuffer(bytes_obj, &view, PyBUF_SIMPLE) < 0) return nullptr;
pipeutil::Message msg{std::span<const std::byte>(
    reinterpret_cast<const std::byte*>(view.buf), view.len)};
PyBuffer_Release(&view);

// Message → bytes
PyObject* result = PyBytes_FromStringAndSize(
    reinterpret_cast<const char*>(msg.payload().data()),
    static_cast<Py_ssize_t>(msg.size()));
```

---

## 5. `PyPipeServer` 型

### 5.1 Python 型定義

```cpp
typedef struct {
    PyObject_HEAD
    pipeutil::PipeServer* server;  // nullptr = 未初期化 or closed
} PyPipeServer;
```

### 5.2 コンストラクタ

```python
PipeServer(pipe_name: str, buffer_size: int = 65536)
```

| 引数 | 型 | デフォルト | 説明 |
|------|----|-----------|------|
| `pipe_name` | `str` | 必須 | パイプ識別名 |
| `buffer_size` | `int` | `65536` | 内部バッファサイズ（バイト） |

### 5.3 メソッド一覧

| メソッド | シグネチャ | 説明 |
|---------|-----------|------|
| `listen` | `() -> None` | パイプ作成・接続待機状態に移行 |
| `accept` | `(timeout: float = 0.0) -> None` | クライアント接続を待機。`timeout` は秒単位（`0.0` = 無限） |
| `send` | `(msg: Message \| bytes \| str) -> None` | メッセージ送信 |
| `receive` | `(timeout: float = 0.0) -> Message` | メッセージ受信 |
| `close` | `() -> None` | 接続クローズ |
| `__enter__` | `() -> PipeServer` | コンテキストマネージャ対応 |
| `__exit__` | `(...) -> None` | `close()` 呼び出し |

### 5.4 プロパティ

| プロパティ | 型 | 説明 |
|-----------|-----|------|
| `is_listening` | `bool` | `listen()` 後 `True` |
| `is_connected` | `bool` | `accept()` 成功後 `True` |
| `pipe_name` | `str` | 設定したパイプ名 |

### 5.5 GIL 解放ポイント

ブロッキング I/O 中は GIL を解放する。

```cpp
// accept() の実装例
static PyObject* PyPipeServer_accept(PyPipeServer* self, PyObject* args, PyObject* kwargs) {
    double timeout_sec = 0.0;
    // ... 引数解析 ...

    auto timeout_ms = std::chrono::milliseconds{
        static_cast<int64_t>(timeout_sec * 1000.0)};

    Py_BEGIN_ALLOW_THREADS   // ← GIL 解放
    try {
        self->server->accept(timeout_ms);
    } catch (const pipeutil::PipeException& e) {
        Py_BLOCK_THREADS      // ← GIL 再取得（例外セット前に必要）
        pyutil::set_python_exception(e);
        return nullptr;
    }
    Py_END_ALLOW_THREADS     // ← GIL 再取得

    Py_RETURN_NONE;
}
```

GIL 解放が必要なメソッド: `accept`, `connect`, `send`, `receive`

---

## 6. `PyPipeClient` 型

### 6.1 Python 型定義

```cpp
typedef struct {
    PyObject_HEAD
    pipeutil::PipeClient* client;  // nullptr = 未初期化 or closed
} PyPipeClient;
```

### 6.2 コンストラクタ

```python
PipeClient(pipe_name: str, buffer_size: int = 65536)
```

### 6.3 メソッド一覧

| メソッド | シグネチャ | 説明 |
|---------|-----------|------|
| `connect` | `(timeout: float = 0.0) -> None` | サーバーへの接続 |
| `send` | `(msg: Message \| bytes \| str) -> None` | メッセージ送信 |
| `receive` | `(timeout: float = 0.0) -> Message` | メッセージ受信 |
| `close` | `() -> None` | 接続クローズ |
| `__enter__` | `() -> PipeClient` | コンテキストマネージャ対応 |
| `__exit__` | `(...) -> None` | `close()` 呼び出し |

### 6.4 プロパティ

| プロパティ | 型 | 説明 |
|-----------|-----|------|
| `is_connected` | `bool` | 接続中なら `True` |
| `pipe_name` | `str` | 設定したパイプ名 |

---

## 7. 暗黙的型変換（`send` の引数）

`send(msg)` の引数は以下の型を受け付ける。

| Python 型 | C++ 変換処理 |
|-----------|-------------|
| `Message` | `PyMessage` から直接取得 |
| `bytes` | `PyBytes_AS_STRING` + `PyBytes_GET_SIZE` |
| `bytearray` | `PyByteArray_AS_STRING` + `PyByteArray_GET_SIZE` |
| `str` | `PyUnicode_AsUTF8AndSize` → UTF-8 バイト列 |
| その他 | `TypeError` を送出 |

---

## 8. モジュール初期化 (`_pipeutil_module.cpp`)

```cpp
PyMODINIT_FUNC PyInit__pipeutil(void) {
    // 1. 型オブジェクトの準備
    if (PyType_Ready(&PyMessage_Type)    < 0) return nullptr;
    if (PyType_Ready(&PyPipeServer_Type) < 0) return nullptr;
    if (PyType_Ready(&PyPipeClient_Type) < 0) return nullptr;

    // 2. モジュール作成
    PyObject* m = PyModule_Create(&pipeutil_module);
    if (!m) return nullptr;

    // 3. 型の登録
    // PyModule_AddObjectRef は参照を盗まない（非 steal）ため Py_INCREF 不要（Python 3.10+）
    // 戻り値 0 = 成功, -1 = 失敗（Python 例外セット済み）。失敗時は m を DECREF して nullptr を返す。
#define ADD_OBJECT_OR_FAIL(mod, name, obj)                           \
    do {                                                             \
        if (PyModule_AddObjectRef((mod), (name), (obj)) < 0) {      \
            Py_DECREF(mod);                                          \
            return nullptr;                                          \
        }                                                            \
    } while (0)

    ADD_OBJECT_OR_FAIL(m, "Message",    reinterpret_cast<PyObject*>(&PyMessage_Type));
    ADD_OBJECT_OR_FAIL(m, "PipeServer", reinterpret_cast<PyObject*>(&PyPipeServer_Type));
    ADD_OBJECT_OR_FAIL(m, "PipeClient", reinterpret_cast<PyObject*>(&PyPipeClient_Type));

    // 4. 例外の登録
    pyutil::g_PipeError = PyErr_NewException("_pipeutil.PipeError", PyExc_Exception, nullptr);
    if (!pyutil::g_PipeError) { Py_DECREF(m); return nullptr; }
    ADD_OBJECT_OR_FAIL(m, "PipeError", pyutil::g_PipeError);
    // ... 他の例外も同様 ...

    // 5. バージョン定数
    if (PyModule_AddStringConstant(m, "__version__", "0.1.0") < 0) {
        Py_DECREF(m);
        return nullptr;
    }

#undef ADD_OBJECT_OR_FAIL
    return m;
}
```

---

## 9. Python パッケージ `pipeutil` (`python/pipeutil/`)

```python
# python/pipeutil/__init__.py
from ._pipeutil import (
    Message,
    PipeServer,
    PipeClient,
    PipeError,
    TimeoutError,
    ConnectionResetError,
    BrokenPipeError,
    NotConnectedError,
    InvalidMessageError,
)

__all__ = [
    "Message",
    "PipeServer",
    "PipeClient",
    "PipeError",
    "TimeoutError",
    "ConnectionResetError",
    "BrokenPipeError",
    "NotConnectedError",
    "InvalidMessageError",
]
__version__ = "0.1.0"
```

---

## 10. 型スタブ (`python/pipeutil/__init__.pyi`)

```python
from __future__ import annotations
from types import TracebackType

class Message:
    def __init__(self, data: bytes | str) -> None: ...
    @property
    def data(self) -> bytes: ...
    @property
    def text(self) -> str: ...
    def __len__(self) -> int: ...
    def __bool__(self) -> bool: ...
    def __repr__(self) -> str: ...

class PipeServer:
    def __init__(self, pipe_name: str, buffer_size: int = 65536) -> None: ...
    def listen(self) -> None: ...
    def accept(self, timeout: float = 0.0) -> None: ...
    def send(self, msg: Message | bytes | str) -> None: ...
    def receive(self, timeout: float = 0.0) -> Message: ...
    def close(self) -> None: ...
    def __enter__(self) -> PipeServer: ...
    def __exit__(self, exc_type: type[BaseException] | None,
                 exc_val: BaseException | None,
                 exc_tb: TracebackType | None) -> None: ...
    @property
    def is_listening(self) -> bool: ...
    @property
    def is_connected(self) -> bool: ...
    @property
    def pipe_name(self) -> str: ...

class PipeClient:
    def __init__(self, pipe_name: str, buffer_size: int = 65536) -> None: ...
    def connect(self, timeout: float = 0.0) -> None: ...
    def send(self, msg: Message | bytes | str) -> None: ...
    def receive(self, timeout: float = 0.0) -> Message: ...
    def close(self) -> None: ...
    def __enter__(self) -> PipeClient: ...
    def __exit__(self, exc_type: type[BaseException] | None,
                 exc_val: BaseException | None,
                 exc_tb: TracebackType | None) -> None: ...
    @property
    def is_connected(self) -> bool: ...
    @property
    def pipe_name(self) -> str: ...

# 例外階層
class PipeError(Exception): ...
class TimeoutError(PipeError): ...
class ConnectionResetError(PipeError): ...
class BrokenPipeError(PipeError): ...
class NotConnectedError(PipeError): ...
class InvalidMessageError(PipeError): ...
```

---

## 11. Python 使用例

```python
# === サーバー側 ===
import pipeutil

with pipeutil.PipeServer("my_pipe") as server:
    server.listen()
    server.accept(timeout=30.0)  # 30秒待機

    msg = server.receive(timeout=5.0)
    print(f"Received: {msg.text}")

    server.send("Hello from server!")

# === クライアント側 ===
import pipeutil

with pipeutil.PipeClient("my_pipe") as client:
    client.connect(timeout=10.0)

    client.send("Hello from client!")
    reply = client.receive(timeout=5.0)
    print(f"Reply: {reply.text}")
```

---

## 12. `tp_dealloc` における注意事項

```cpp
static void PyPipeServer_dealloc(PyPipeServer* self) {
    // デストラクタは close() を内包するため、明示的 close 不要
    delete self->server;
    self->server = nullptr;
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}
```

`delete self->server` は C++ デストラクタを呼び、内部で `close()` が実行される。
`dealloc` 内で Python 例外を送出してはならない（`PyErr_Clear()` で握りつぶす）。

---

## 13. 参照カウント管理原則

| 操作 | ルール |
|------|--------|
| 新規 `PyObject*` 生成 | 参照カウント = 1（所有権あり） |
| 関数から返す `PyObject*` | `Py_INCREF` 不要（所有権を呼び出し元に移譲） |
| 例外時に `return nullptr` | `Py_XDECREF` で保有オブジェクトを解放してから返す |
| `PyModule_AddObject` | 参照を**盗む**（steal）ため呼び出し前に `Py_INCREF` 必須。失敗時に参照リークを防ぐには呼び出し後に `Py_XDECREF` が必要。Python 3.13 では **`PyModule_AddObjectRef`（非 steal, Python 3.10+）を使用推奨**。 |
