# F-003 詳細設計: 自動再接続（ReconnectingPipeClient）

**作成日**: 2026-03-10
**対象バージョン**: v0.3.0
**依存**: なし（C++ コア変更不要）

---

## 1. 現状と課題

### 1.1 問題

サーバー再起動時にクライアントプロセスを再起動せず自動復旧したい。
現行の `PipeClient` はサーバー側が切断すると `ConnectionResetError` / `BrokenPipeError` を送出する。
再接続ロジックをユーザーが毎回実装する必要があり、コードが冗長になる。

```python
# 現状 — 毎回この再接続ボイラープレートが必要
import time
import pipeutil

while True:
    try:
        client.send(pipeutil.Message(b"ping"))
        break
    except (pipeutil.ConnectionResetError, pipeutil.BrokenPipeError):
        client.close()
        time.sleep(0.5)
        client.connect(timeout=5.0)
```

### 1.2 スコープ定義

| 項目 | 対応 |
|---|---|
| Python レイヤーのみ実装 | ✅ 必須 |
| C++ コア変更 | ❌ 不要 |
| `send()` / `receive()` の自動再接続 | ✅ 必須 |
| 最大リトライ回数制限（`0` = 無限） | ✅ 必須 |
| 再接続成功コールバック（`on_reconnect`） | ✅ 必須 |
| スレッドセーフ | ✅ `threading.Lock` で保護 |
| RPC（`send_request()`） 対応 | ❌ 対象外（v0.3.0 では提供しない） |
| asyncio 統合 | ❌ 対象外（F-004 `AsyncPipeClient` で別途検討） |

---

## 2. アーキテクチャ概観

```
python/pipeutil/
  reconnecting_client.py     ← 新規: ReconnectingPipeClient / MaxRetriesExceededError
  __init__.py                ← 更新: re-export を追加
  __init__.pyi               ← 更新: 型スタブを追加

tests/python/
  test_reconnecting.py       ← 新規: 12 件
```

### 2.1 クラス関係

```
ReconnectingPipeClient
  ├─ _impl: PipeClient          ← 実際の I/O を担当
  ├─ _lock: threading.Lock      ← 再接続クリティカルセクション保護
  ├─ _closed: bool              ← close() 後の操作ガード
  └─ _retry_count: int          ← 今セッション通算の再接続成功回数
```

`PipeClient` 以外の C 拡張型には依存しない。
`_impl` は `__init__` 内で生成し、クラスのライフサイクルを通じて同一インスタンスを再利用する。

---

## 3. API 設計

### 3.1 コンストラクタ

```python
class ReconnectingPipeClient:
    def __init__(
        self,
        pipe_name: str,
        *,
        retry_interval_ms: int = 500,
        max_retries: int = 0,
        connect_timeout_ms: int = 5000,
        on_reconnect: Callable[[], None] | None = None,
        buffer_size: int = 65536,
    ) -> None: ...
```

| パラメータ | 型 | デフォルト | 説明 |
|---|---|:---:|---|
| `pipe_name` | `str` | — | 接続先パイプ名（`PipeClient` と同一規約）|
| `retry_interval_ms` | `int` | `500` | 再接続試行間の待機時間（ms）|
| `max_retries` | `int` | `0` | 最大再接続試行回数（`0` = 無限）|
| `connect_timeout_ms` | `int` | `5000` | 個々の接続試行タイムアウト（ms、`0` = 無限待機）|
| `on_reconnect` | `Callable[[], None] \| None` | `None` | 再接続成功後に呼ばれるコールバック |
| `buffer_size` | `int` | `65536` | 内部バッファサイズ（`PipeClient` に委譲）|

**引数検証**

- `retry_interval_ms < 0` → `ValueError`
- `max_retries < 0` → `ValueError`
- `connect_timeout_ms < 0` → `ValueError`

### 3.2 メソッド一覧

```python
def connect(self, timeout_ms: int = 0) -> None:
    """サーバーへ初回接続する。

    Parameters
    ----------
    timeout_ms:
        接続試行タイムアウト (ms)。0 = 無限待機（サーバー起動を待つ場合に使用）。
        コンストラクタの connect_timeout_ms より優先する（このメソッド呼び出し側で上書き）。

    Raises
    ------
    PipeError
        接続に失敗した場合。
    NotConnectedError
        close() 済みのインスタンスに対して呼んだ場合。
    """

def send(self, msg: Message, timeout_ms: int = 0) -> None:
    """フレーム化メッセージを送信する。

    切断を検知した場合は自動再接続後に同一メッセージを 1 回のみ再送信する
    （at-most-once セマンティクス）。再送後の送信失敗は再試行しない。

    Parameters
    ----------
    timeout_ms:
        送信タイムアウト (ms)。0 = 無限待機。

    Raises
    ------
    MaxRetriesExceededError
        再接続試行がすべて失敗した場合。
    NotConnectedError
        close() 済みのインスタンスに対して呼んだ場合。
    PipeError
        再接続後の送信が失敗した場合など、その他のパイプエラー。
    """

def receive(self, timeout_ms: int = 5000) -> Message:
    """フレーム化メッセージを受信する。

    切断を検知した場合は自動再接続後に次のメッセージを待機する。
    切断前に受信途中だったメッセージは失われる。

    Parameters
    ----------
    timeout_ms:
        受信タイムアウト (ms)。0 = 無限待機。

    Raises
    ------
    MaxRetriesExceededError
        再接続試行がすべて失敗した場合。
    TimeoutError
        timeout_ms 内にメッセージが届かなかった場合（再接続は行わない）。
    NotConnectedError
        close() 済みのインスタンスに対して呼んだ場合。
    """

def close(self) -> None:
    """接続を閉じ、以降の send/receive/connect を無効化する（冪等）。

    close() 後のすべての操作は NotConnectedError を送出する。
    このメソッド自体はロックを取得しない（デッドロック防止）。
    """

@property
def pipe_name(self) -> str:
    """接続先パイプ名を返す（read-only）。"""

@property
def is_connected(self) -> bool:
    """現在接続中なら True。"""

@property
def retry_count(self) -> int:
    """インスタンス生成からの累計再接続成功回数。close() でリセットされない。"""
```

**コンテキストマネージャ**

`__enter__` は `self` を返す。`__exit__` は例外有無を問わず `close()` を呼ぶ。

### 3.3 例外クラス

```python
class MaxRetriesExceededError(PipeError):
    """max_retries 回の再接続試行がすべて失敗した際に送出される。

    Attributes
    ----------
    attempts : int
        実施した再接続試行回数。
    last_exception : Exception
        最後の試行で発生した例外。
    """
    def __init__(self, attempts: int, last_exception: Exception) -> None:
        super().__init__(
            f"Reconnection failed after {attempts} attempt(s): {last_exception}"
        )
        self.attempts = attempts
        self.last_exception = last_exception
```

`MaxRetriesExceededError` は `pipeutil.PipeError` のサブクラスとして定義し、
`from .reconnecting_client import MaxRetriesExceededError` で `pipeutil` 直下に公開する。

---

## 4. 再接続アルゴリズム

### 4.1 `send()` のフロー

```
send(msg, timeout_ms)
│
├─ _closed? → raise NotConnectedError
│
├─ _impl.send(msg, timeout_ms)  ─── 成功 → return
│
└─ ConnectionResetError / BrokenPipeError:
     │
     ├─ _reconnect_with_retry()          # _lock で保護
     │     ├─ 成功 → _impl.send(msg, timeout_ms)  # 1 回のみ再試行
     │     │         成功 → return
     │     │         失敗 → 例外を上位に伝播（再帰的再接続は行わない）
     │     └─ MaxRetriesExceededError → 上位に伝播
     │
     └─ その他例外 → 上位に伝播
```

### 4.2 `receive()` のフロー

```
receive(timeout_ms)
│
├─ _closed? → raise NotConnectedError
│
├─ _impl.receive(timeout_ms)  ─── 成功 → return
│
└─ ConnectionResetError / BrokenPipeError:
     │
     ├─ _reconnect_with_retry()          # _lock で保護
     │     ├─ 成功 → _impl.receive(timeout_ms)  # 再接続後に次のメッセージを待機
     │     └─ MaxRetriesExceededError → 上位に伝播
     │
     └─ その他例外 → 上位に伝播
```

`TimeoutError` は接続切断ではないため、`_reconnect_with_retry()` を呼ばずにそのまま送出する。

### 4.3 `_reconnect_with_retry()` の内部ロジック

```python
def _reconnect_with_retry(self) -> None:
    """
    Lock 取得後に再接続を試みる。
    別スレッドが既に再接続を完了していた場合は何もせず返る。
    """
    with self._lock:
        # 二重再接続防止: 別スレッドが先に再接続済みであればスキップ
        if self._impl.is_connected:
            return

        attempts = 0
        last_exc: Exception | None = None
        timeout_sec = self._connect_timeout_ms / 1000.0

        while self._max_retries == 0 or attempts < self._max_retries:
            attempts += 1
            try:
                self._impl.close()
                time.sleep(self._retry_interval_ms / 1000.0)
                self._impl.connect(timeout_sec)
                # 成功
                self._retry_count += 1
                if self._on_reconnect is not None:
                    self._on_reconnect()
                return
            except PipeError as e:
                last_exc = e
                # 次の試行へ

        raise MaxRetriesExceededError(
            attempts=attempts,
            last_exception=last_exc,  # type: ignore[arg-type]
        )
```

**注意事項**

- `_closed == True` のときは `_reconnect_with_retry()` を呼ばない（呼び出し元で事前チェック）
- 再接続中に別スレッドから `close()` された場合、次の `connect()` は `PipeError` で失敗し
  最終的に `MaxRetriesExceededError` となる（または `NotConnectedError`）
- `on_reconnect` コールバックは GIL を保持したまま Lock 内で呼ぶ。
  コールバック内でブロッキング操作を行うと Lock を長時間保持するため、ユーザーが注意する必要がある

### 4.4 `connect()` のフロー

```
connect(timeout_ms)
│
├─ _closed? → raise NotConnectedError
└─ _impl.connect(timeout_ms / 1000.0)   # PipeClient.connect は秒単位
     ├─ 成功 → return
     └─ 失敗 → PipeError を上位に伝播（リトライは行わない）
```

初回接続でのリトライは `PipeClient.connect(timeout=0)` の無限待機（サーバー起動待ち）で代替できる。

---

## 5. スレッド安全性

| 操作 | 保護方式 | 理由 |
|---|---|---|
| `_reconnect_with_retry()` | `threading.Lock` | 複数スレッドの同時再接続を防ぐ |
| `_impl` への I/O | `PipeClient` 内部ミューテックス | C 拡張レイヤーで保護済み |
| `_closed` フラグの読み書き | GIL 保護 | Python の `bool` 代入は GIL 保護下で原子的 |
| `_retry_count` のインクリメント | `_lock` 内で実施 | `_reconnect_with_retry` の Lock 内でのみ更新 |

**二重再接続防止の仕組み**

スレッド A・B が同時に `ConnectionResetError` を検知した場合:

1. A が `_lock` を取得 → `is_connected` が `False` → 再接続処理を開始
2. B は `_lock` 待ちでブロック
3. A の再接続が成功 → `is_connected` が `True` に変わる → `_lock` を解放
4. B が `_lock` を取得 → `is_connected` が `True` → 処理をスキップして返る

---

## 6. エラーハンドリング設計

| 例外クラス | トリガー条件 | 再接続試行 |
|---|---|:---:|
| `ConnectionResetError` | サーバー側がパイプを閉じた | ✅ |
| `BrokenPipeError` | 書き込み中にパイプが壊れた | ✅ |
| `TimeoutError` | `receive()` タイムアウト | ❌ |
| `InvalidMessageError` | フレーム破損 | ❌ |
| `PipeError`（その他） | OS 系エラー、未定義エラー | ❌ |
| `MaxRetriesExceededError` | 全再接続試行が失敗 | — |
| `NotConnectedError` | `close()` 済みインスタンスへの操作 | — |

---

## 7. 使用例

### 7.1 基本使用

```python
import pipeutil

client = pipeutil.ReconnectingPipeClient(
    "my_pipe",
    retry_interval_ms=500,
    max_retries=10,
    on_reconnect=lambda: print("reconnected"),
)

with client:
    client.connect()  # 初回接続（timeout_ms=0 でサーバー起動を待つ）
    while True:
        # サーバーが再起動しても自動復旧
        client.send(pipeutil.Message(b"ping"))
        resp = client.receive(timeout_ms=5000)
        print(resp.text)
```

### 7.2 無限リトライ + ロギング

```python
import logging
import pipeutil

def on_reconnect() -> None:
    logging.warning("Reconnected to server")

client = pipeutil.ReconnectingPipeClient(
    "my_pipe",
    max_retries=0,          # 無限リトライ
    retry_interval_ms=1000,
    on_reconnect=on_reconnect,
)
```

### 7.3 接続状態の確認

```python
client = pipeutil.ReconnectingPipeClient("my_pipe")
client.connect()

print(client.is_connected)   # True
print(client.retry_count)    # 0（まだ再接続していない）
print(client.pipe_name)      # "my_pipe"
```

---

## 8. ファイル構成と実装概要

### 8.1 新規ファイル: `python/pipeutil/reconnecting_client.py`

```
python/pipeutil/reconnecting_client.py
  MaxRetriesExceededError(PipeError)
  ReconnectingPipeClient
    __init__(pipe_name, *, retry_interval_ms, max_retries, connect_timeout_ms,
             on_reconnect, buffer_size)
    connect(timeout_ms) → None
    send(msg, timeout_ms) → None
    receive(timeout_ms) → Message
    close() → None
    __enter__() / __exit__()
    pipe_name: str            [property]
    is_connected: bool        [property]
    retry_count: int          [property]
    _reconnect_with_retry()   [private]
```

依存: `time`, `threading`, `._pipeutil.PipeError`, `._pipeutil.PipeClient`,
       `._pipeutil.Message`, `._pipeutil.ConnectionResetError`, `._pipeutil.BrokenPipeError`,
       `._pipeutil.NotConnectedError`

### 8.2 更新ファイル: `python/pipeutil/__init__.py`

```python
from .reconnecting_client import (   # noqa: F401
    ReconnectingPipeClient,
    MaxRetriesExceededError,
)
```

`__all__` に `"ReconnectingPipeClient"`, `"MaxRetriesExceededError"` を追加。

### 8.3 更新ファイル: `python/pipeutil/__init__.pyi`

型スタブを追加（§9 参照）。

---

## 9. 型スタブ（`__init__.pyi` 追記分）

```python
# ─── MaxRetriesExceededError ──────────────────────────────────────────

class MaxRetriesExceededError(PipeError):
    """max_retries 回の再接続試行がすべて失敗した際に送出される。"""
    attempts: int
    last_exception: Exception
    def __init__(self, attempts: int, last_exception: Exception) -> None: ...

# ─── ReconnectingPipeClient ──────────────────────────────────────────

from collections.abc import Callable

class ReconnectingPipeClient:
    """PipeClient の自動再接続ラッパー。

    send / receive で ConnectionResetError / BrokenPipeError を受けると
    自動的に再接続してからオペレーションを再試行する。

    ライフサイクル: ``__init__()`` → ``connect()`` → ``send()`` / ``receive()`` → ``close()``
    """

    def __init__(
        self,
        pipe_name: str,
        *,
        retry_interval_ms: int = 500,
        max_retries: int = 0,
        connect_timeout_ms: int = 5000,
        on_reconnect: Callable[[], None] | None = None,
        buffer_size: int = 65536,
    ) -> None: ...

    def connect(self, timeout_ms: int = 0) -> None:
        """サーバーへ初回接続する。

        Parameters
        ----------
        timeout_ms:
            接続試行タイムアウト (ms)。0 = 無限待機。
        """
        ...

    def send(self, msg: Message, timeout_ms: int = 0) -> None:
        """フレーム化メッセージを送信する。切断時は自動再接続後に再送信する。

        Raises
        ------
        MaxRetriesExceededError
            再接続試行がすべて失敗した場合。
        NotConnectedError
            close() 済みのインスタンスに対して呼んだ場合。
        """
        ...

    def receive(self, timeout_ms: int = 5000) -> Message:
        """フレーム化メッセージを受信する。切断時は自動再接続後に次のメッセージを待機する。

        Raises
        ------
        MaxRetriesExceededError
            再接続試行がすべて失敗した場合。
        TimeoutError
            timeout_ms 内にメッセージが届かなかった場合。
        NotConnectedError
            close() 済みのインスタンスに対して呼んだ場合。
        """
        ...

    def close(self) -> None:
        """接続を閉じ、以降の操作を無効化する（冪等）。"""
        ...

    def __enter__(self) -> ReconnectingPipeClient: ...

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc_val: BaseException | None,
        exc_tb: TracebackType | None,
    ) -> None: ...

    @property
    def pipe_name(self) -> str:
        """接続先パイプ名。"""
        ...

    @property
    def is_connected(self) -> bool:
        """現在接続中なら True。"""
        ...

    @property
    def retry_count(self) -> int:
        """累計再接続成功回数（close() でリセットされない）。"""
        ...
```

---

## 10. テスト設計

テストファイル: `tests/python/test_reconnecting.py`

| # | テスト名 | 概要 |
|---|---|---|
| T1 | `test_basic_send_receive` | 正常接続・送受信ラウンドトリップが成功すること |
| T2 | `test_context_manager` | `with` ブロック脱出後に `close()` が呼ばれ `NotConnectedError` になること |
| T3 | `test_reconnect_on_connection_reset` | `ConnectionResetError` 後に再接続して送信成功すること |
| T4 | `test_reconnect_on_broken_pipe` | `BrokenPipeError` 後に再接続して送信成功すること |
| T5 | `test_max_retries_exceeded` | 再接続失敗 N 回で `MaxRetriesExceededError` が送出されること |
| T6 | `test_max_retries_exceeded_attributes` | `MaxRetriesExceededError.attempts` と `last_exception` が正しいこと |
| T7 | `test_on_reconnect_callback` | 再接続成功後にコールバックが 1 回呼ばれること |
| T8 | `test_retry_count_property` | `retry_count` が再接続成功ごとにインクリメントされること |
| T9 | `test_close_prevents_reconnect` | `close()` 後の `send()` が `NotConnectedError`（再接続しない）こと |
| T10 | `test_infinite_retry` | `max_retries=0` でサーバーが n 回目で起動した場合に成功すること |
| T11 | `test_timeout_not_retried` | `receive()` の `TimeoutError` では再接続しないこと |
| T12 | `test_thread_safe_reconnect` | 複数スレッドが同時に切断を検知しても 1 回だけ再接続すること |

テストにおける「サーバー再起動」のシミュレーション方法:
- `unittest.mock.patch` で `PipeClient.send` / `receive` を差し替え、
  1 回目の呼び出しで `ConnectionResetError` を送出 → 2 回目は正常動作させる
- `PipeClient.connect` / `close` / `is_connected` もモック化する

---

## 11. 制約・注意事項

1. **at-most-once セマンティクス**: `send()` は再接続後に同一メッセージを 1 回のみ再送信する。
   再送後の送信失敗（例: 2 回目の接続もすぐ切れた場合）は再試行せずに例外を伝播する。

2. **`receive()` のメッセージ消失**: パイプ切断時に受信途中だったメッセージは失われる。
   アプリケーション層での確認応答（ACK）が必要な場合、このクラスだけでは不十分。

3. **RPC 非対応**: `send_request()` は v0.3.0 では提供しない。
   RPC + 自動再接続が必要な場合は F-002 との統合として別途検討する。

4. **asyncio 非対応**: asyncio 用途は `AsyncPipeClient`（F-004）を使用すること。
   このクラスはブロッキング I/O 専用である。

5. **`on_reconnect` コールバックの注意**: コールバックは `_lock` 保持中に呼ばれる。
   コールバック内でこのクラスの `send()` / `receive()` を呼ぶとデッドロックする。

6. **`connect_timeout_ms` と `PipeClient.connect()` の単位変換**:
   `PipeClient.connect(timeout: float)` は **秒単位**（`__init__.pyi` 準拠）。
   `_reconnect_with_retry()` 内では `connect_timeout_ms / 1000.0` に変換して渡すこと。

7. **`max_retries=0`（無限リトライ）の使用**: サーバーが永久に起動しない場合は
   プロセスが永久にブロックする。`KeyboardInterrupt` / `SIGTERM` によるプロセス終了で
   中断できることを確認して使用すること。
