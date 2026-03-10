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
| `send()` / `receive()` の自動再接続（同期版） | ✅ 必須 |
| 最大リトライ回数制限（`0` = 無限） | ✅ 必須 |
| 再接続成功コールバック（`on_reconnect`） | ✅ 必須 |
| スレッドセーフ（同期版） | ✅ `threading.Lock` で保護 |
| asyncio 統合（`AsyncReconnectingPipeClient`） | ✅ F-004 実装済みのため本 spec で対応 |
| RPC `send_request()` の**接続再確立**（方針 A） | ✅ `ReconnectingRpcPipeClient` として提供 |
| RPC `send_request()` の**自動再送** | ❌ 冪等性を保証できないため提供しない |

---

## 2. アーキテクチャ概観

```
python/pipeutil/
  reconnecting_client.py     ← 新規: ReconnectingPipeClient / AsyncReconnectingPipeClient
                                        / ReconnectingRpcPipeClient
                                        / MaxRetriesExceededError
  __init__.py                ← 更新: re-export を追加
  __init__.pyi               ← 更新: 型スタブを追加

tests/python/
  test_reconnecting.py       ← 新規: 12 件（同期）+ 8 件（非同期）+ 5 件（RPC）= 25 件
```

### 2.1 クラス関係

```
[同期版]
ReconnectingPipeClient
  ├─ _impl: PipeClient          ← 実際の I/O を担当
  ├─ _lock: threading.Lock      ← 再接続クリティカルセクション保護
  ├─ _closed: bool              ← close() 後の操作ガード
  └─ _retry_count: int          ← 今セッション通算の再接続成功回数

[非同期版]
AsyncReconnectingPipeClient
  ├─ _impl: AsyncPipeClient     ← 実際の非同期 I/O を担当（aio.py）
  ├─ _lock: asyncio.Lock        ← 再接続クリティカルセクション保護
  ├─ _closed: bool              ← close() 後の操作ガード
  └─ _retry_count: int          ← 今セッション通算の再接続成功回数

[RPC版（同期）]
ReconnectingRpcPipeClient
  ├─ _impl: RpcPipeClient       ← 実際の I/O・RPC・バックグラウンド受信スレッドを担当
  ├─ _lock: threading.Lock      ← 再接続クリティカルセクション保護
  ├─ _closed: bool              ← close() 後の操作ガード
  └─ _retry_count: int          ← 今セッション通算の再接続成功回数
```

`PipeClient` / `AsyncPipeClient` / `RpcPipeClient` 以外の C 拡張型には依存しない。
`_impl` は `__init__` 内で生成し、クラスのライフサイクルを通じて同一インスタンスを再利用する。

`asyncio.Lock` はイベントループにバインドされるため、`AsyncReconnectingPipeClient` は
**1 インスタンス = 1 イベントループ**（`AsyncPipeClient` と同じ制約）。

---

## 3. API 設計

本 spec では 3 つのクラスを定義する。

| クラス | 用途 | I/O モデル |
|---|---|---|
| `ReconnectingPipeClient` | スレッドブロッキング用 | `PipeClient`（同期 I/O）|
| `AsyncReconnectingPipeClient` | asyncio コルーチン用 | `AsyncPipeClient`（非同期 I/O）|
| `ReconnectingRpcPipeClient` | RPC ブロッキング用 | `RpcPipeClient`（同期 I/O + バックグラインド受信スレッド）|

`ReconnectingPipeClient` / `AsyncReconnectingPipeClient` はコンストラクタシグネチャ・プロパティ・例外クラスを共有する。
`ReconnectingRpcPipeClient` も同じコンストラクタシグネチャを持つが、`send_request()` を追加する。

---

### 3.1 コンストラクタ（共通パラメータ）

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

class AsyncReconnectingPipeClient:
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

`AsyncReconnectingPipeClient` のコンストラクタは同期関数。
`asyncio.Lock` の初期化はコンストラクタ内で行う（Python 3.10+ で非推奨の引数レス `asyncio.Lock()` が 3.8–3.9 では唯一の方法であり互換性あり）。

| パラメータ | 型 | デフォルト | 説明 |
|---|---|:---:|---|
| `pipe_name` | `str` | — | 接続先パイプ名（`PipeClient` と同一規約）|
| `retry_interval_ms` | `int` | `500` | 再接続試行間の待機時間（ms）|
| `max_retries` | `int` | `0` | 最大再接続試行回数（`0` = 無限）|
| `connect_timeout_ms` | `int` | `5000` | 個々の接続試行タイムアウト（ms、`0` = 無限待機）|
| `on_reconnect` | `Callable[[], None] \| None` | `None` | 再接続成功後に呼ばれるコールバック。`AsyncReconnectingPipeClient` では同期関数を渡す（コルーチン不可）|
| `buffer_size` | `int` | `65536` | 内部バッファサイズ（`PipeClient` / `AsyncPipeClient` に委譲）|

**引数検証**

- `retry_interval_ms < 0` → `ValueError`
- `max_retries < 0` → `ValueError`
- `connect_timeout_ms < 0` → `ValueError`

### 3.2 メソッド一覧（`ReconnectingPipeClient`）

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

def send(self, msg: Message) -> None:
    """フレーム化メッセージを送信する。

    切断を検知した場合は自動再接続後に同一メッセージを 1 回のみ再送信する
    （at-most-once セマンティクス）。再送後の送信失敗は再試行しない。

    Raises
    ------
    MaxRetriesExceededError
        再接続試行がすべて失敗した場合。
    NotConnectedError
        close() 済みのインスタンスに対して呼んだ場合。
    PipeError
        再接続後の送信が失敗した場合など、その他のパイプエラー。
    """

def receive(self, timeout: float = 0.0) -> Message:
    """フレーム化メッセージを受信する。

    切断を検知した場合は自動再接続後に次のメッセージを待機する。
    切断前に受信途中だったメッセージは失われる。

    Parameters
    ----------
    timeout:
        受信タイムアウト (秒)。0.0 = 無限待機。

    Raises
    ------
    MaxRetriesExceededError
        再接続試行がすべて失敗した場合。
    TimeoutError
        timeout 秒以内にメッセージが届かなかった場合（再接続は行わない）。
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

**コンテキストマネージャ（同期版）**

`__enter__` は `self` を返す。`__exit__` は例外有無を問わず `close()` を呼ぶ。

---

### 3.3 メソッド一覧（`AsyncReconnectingPipeClient`）

`ReconnectingPipeClient` と同じプロパティ / パラメータ / 例外ルールを持つ。
各 I/O メソッドがコルーチンになる点のみ異なる。

```python
async def connect(self, timeout_ms: int = 0) -> None:
    """サーバーへ初回接続する（コルーチン）。

    Raises
    ------
    PipeError
        接続に失敗した場合。
    NotConnectedError
        close() 済みのインスタンスに対して呼んだ場合。
    """

async def send(self, msg: Message) -> None:
    """フレーム化メッセージを送信する（コルーチン）。

    切断を検知した場合は自動再接続後に同一メッセージを 1 回のみ再送信する。

    Raises
    ------
    MaxRetriesExceededError
        再接続試行がすべて失敗した場合。
    NotConnectedError
        close() 済みのインスタンスに対して呼んだ場合。
    """

async def receive(self, timeout_ms: int = 5000) -> Message:
    """フレーム化メッセージを受信する（コルーチン）。

    Raises
    ------
    MaxRetriesExceededError
        再接続試行がすべて失敗した場合。
    TimeoutError
        timeout_ms 内にメッセージが届かなかった場合（再接続は行わない）。
    NotConnectedError
        close() 済みのインスタンスに対して呼んだ場合。
    """

async def close(self) -> None:
    """接続を閉じ、以降の操作を無効化する（コルーチン / 冪等）。

    AsyncPipeClient.close() を await する。ロックは取得しない（デッドロック防止）。
    """

@property
def pipe_name(self) -> str: ...

@property
def is_connected(self) -> bool: ...

@property
def retry_count(self) -> int: ...
```

**非同期コンテキストマネージャ**

`async with AsyncReconnectingPipeClient(...) as client:` で使用する。
`__aenter__` は `self` を返す。`__aexit__` は `await self.close()` を呼ぶ。

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

### 3.4 メソッド一覧（`ReconnectingRpcPipeClient`）

`ReconnectingPipeClient` と同じコンストラクタ・プロパティ・`connect()` / `close()` / `__enter__` / `__exit__` を持つ。
以下は差分（追加・変更されるメソッド）のみ示す。

```python
def send(self, msg: Message) -> None:
    """通常フレームを送信する（message_id=0）。
    切断を検知した場合は自動再接続後に同一メッセージを 1 回のみ再送信する。
    """

def receive(self, timeout: float = 0.0) -> Message:
    """通常受信キューからメッセージを取り出す（message_id=0 のフレームのみ）。
    切断を検知した場合は自動再接続後に次のメッセージを待機する。
    timeout は秒単位（RpcPipeClient.receive と同一単位）。0.0 = 無限待機。
    """

def send_request(self, msg: Message, timeout: float = 5.0) -> Message:
    """RPC リクエストを送信し、対応するレスポンスを返す（方針 A: 再送なし）。

    切断を検知した場合は _reconnect_with_retry() で接続を再確立する。
    ただし in-flight のリクエストは再送しない。接続再確立後に
    ConnectionResetError / BrokenPipeError を上位に伝播するので、
    ユーザーが必要に応じて再呼び出しを判断する。

    Parameters
    ----------
    msg:
        送信するリクエストメッセージ。
    timeout:
        秒単位のタイムアウト（RpcPipeClient.send_request と同一単位）。0.0 = 無限待機。

    Raises
    ------
    ConnectionResetError / BrokenPipeError
        送信中または応答待機中に切断が発生し、再接続後も例外が伝播した場合。
    MaxRetriesExceededError
        再接続試行がすべて失敗した場合。
    TimeoutError
        timeout 内にレスポンスが届かなかった場合（再接続は行わない）。
    NotConnectedError
        close() 済みのインスタンスに対して呼んだ場合。
    """
```

**`send_request()` のフロー（方針 A）**

```
send_request(msg, timeout)
│
├─ _closed? → raise NotConnectedError
│
├─ _impl.send_request(msg, timeout)  ─── 成功 → return
│
└─ ConnectionResetError / BrokenPipeError:
     │
     ├─ _reconnect_with_retry()   # _lock で保護
     │     ├─ 成功 → ConnectionResetError / BrokenPipeError を上位に伝播
     │     │         （再接続は完了したが in-flight 分は再送しない）
     │     └─ MaxRetriesExceededError → 上位に伝播
     │
     └─ その他例外 → 上位に伝播
```

`TimeoutError` は接続切断ではないため `_reconnect_with_retry()` を呼ばずにそのまま送出する。

---

## 4. 再接続アルゴリズム

§4.1〜§4.3 は `ReconnectingPipeClient`（同期版）の実装を示す。
§4.4〜§4.6 は `AsyncReconnectingPipeClient`（非同期版）の実装を示す。

### 4.1 `send()` のフロー

```
send(msg)
│
├─ _closed? → raise NotConnectedError
│
├─ _impl.send(msg)  ─── 成功 → return
│
└─ ConnectionResetError / BrokenPipeError:
     │
     ├─ _reconnect_with_retry()          # _lock で保護
     │     ├─ 成功 → _impl.send(msg)  # 1 回のみ再試行
     │     │         成功 → return
     │     │         失敗 → 例外を上位に伝播（再帰的再接続は行わない）
     │     └─ MaxRetriesExceededError → 上位に伝播
     │
     └─ その他例外 → 上位に伝播
```

### 4.2 `receive()` のフロー

```
receive(timeout)
│
├─ _closed? → raise NotConnectedError
│
├─ _impl.receive(timeout)  ─── 成功 → return
│
└─ ConnectionResetError / BrokenPipeError:
     │
     ├─ _reconnect_with_retry()          # _lock で保護
     │     ├─ 成功 → _impl.receive(timeout)  # 再接続後に次のメッセージを待機
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

### 4.4 `connect()` のフロー（同期版）

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

### 4.5 `AsyncReconnectingPipeClient._reconnect_with_retry()` の内部ロジック

同期版の `_reconnect_with_retry()` と同一のアルゴリズムだが、次の点が異なる。

| 同期版 | 非同期版 |
|---|---|
| `threading.Lock` | `asyncio.Lock`（`async with self._lock:`）|
| `time.sleep(interval / 1000.0)` | `await asyncio.sleep(interval / 1000.0)` |
| `self._impl.close()` | `await self._impl.close()` |
| `self._impl.connect(timeout_sec)` | `await self._impl.connect(timeout_ms)` |

```python
async def _reconnect_with_retry(self) -> None:
    """
    asyncio.Lock 取得後に再接続を試みる。
    別タスクが既に再接続を完了していた場合は何もせず返る。
    """
    async with self._lock:
        # 二重再接続防止: 別タスクが先に再接続済みであればスキップ
        if self._impl.is_connected:
            return

        attempts = 0
        last_exc: Exception | None = None

        while self._max_retries == 0 or attempts < self._max_retries:
            attempts += 1
            try:
                await self._impl.close()
                await asyncio.sleep(self._retry_interval_ms / 1000.0)
                await self._impl.connect(self._connect_timeout_ms)
                # 成功
                self._retry_count += 1
                if self._on_reconnect is not None:
                    self._on_reconnect()   # 同期コールバック
                return
            except PipeError as e:
                last_exc = e

        raise MaxRetriesExceededError(
            attempts=attempts,
            last_exception=last_exc,  # type: ignore[arg-type]
        )
```

**タイムアウト引数の違い**: `AsyncPipeClient.connect(timeout_ms: int)` はミリ秒単位（`aio.py` の実装に合わせる）。
同期版 `PipeClient.connect(timeout: float)` は秒単位なので `/1000.0` 変換が必要。

### 4.6 非同期版フロー（`send()` / `receive()`）

```
await send(msg)
│
├─ _closed? → raise NotConnectedError
├─ await _impl.send(msg)  ─── 成功 → return
└─ ConnectionResetError / BrokenPipeError:
     ├─ await _reconnect_with_retry()   # asyncio.Lock で保護
     │     ├─ 成功 → await _impl.send(msg)  # 1 回のみ再試行
     │     └─ MaxRetriesExceededError → 上位に伝播
     └─ その他例外 → 上位に伝播

await receive(timeout_ms)
│
├─ _closed? → raise NotConnectedError
├─ await _impl.receive(timeout_ms)  ─── 成功 → return
└─ ConnectionResetError / BrokenPipeError:
     ├─ await _reconnect_with_retry()   # asyncio.Lock で保護
     │     ├─ 成功 → await _impl.receive(timeout_ms)  # 再接続後に次メッセージ待機
     │     └─ MaxRetriesExceededError → 上位に伝播
     └─ その他例外 → 上位に伝播
```

`TimeoutError` は接続切断ではないため、`_reconnect_with_retry()` を呼ばずにそのまま送出する。

---

## 5. スレッド安全性 / タスク安全性

### 5.1 同期版（`ReconnectingPipeClient`）

| 操作 | 保護方式 | 理由 |
|---|---|---|
| `_reconnect_with_retry()` | `threading.Lock` | 複数スレッドの同時再接続を防ぐ |
| `_impl` への I/O | `PipeClient` 内部ミューテックス | C 拡張レイヤーで保護済み |
| `_closed` フラグの読み書き | GIL 保護 | Python の `bool` 代入は GIL 保護下で原子的 |
| `_retry_count` のインクリメント | `_lock` 内で実施 | `_reconnect_with_retry` の Lock 内でのみ更新 |

**二重再接続防止の仕組み（同期版）**

スレッド A・B が同時に `ConnectionResetError` を検知した場合:

1. A が `_lock` を取得 → `is_connected` が `False` → 再接続処理を開始
2. B は `_lock` 待ちでブロック
3. A の再接続が成功 → `is_connected` が `True` に変わる → `_lock` を解放
4. B が `_lock` を取得 → `is_connected` が `True` → 処理をスキップして返る

### 5.2 非同期版（`AsyncReconnectingPipeClient`）

| 操作 | 保護方式 | 理由 |
|---|---|---|
| `_reconnect_with_retry()` | `asyncio.Lock` | 複数タスクの同時再接続を防ぐ |
| `_impl` への I/O | `AsyncPipeClient` 内部排他 | `aio.py` の実装に依存 |
| `_closed` フラグの読み書き | イベントループシングルスレッド | await しない判定は原子的 |
| `_retry_count` のインクリメント | `_lock` 内で実施 | `_reconnect_with_retry` の Lock 内でのみ更新 |

`asyncio.Lock` はスレッドセーフではない。
複数のスレッドから同じ `AsyncReconnectingPipeClient` を共有することは禁止する。
1 インスタンス = 1 イベントループの制約を守ること。

**二重再接続防止の仕組み（非同期版）**

タスク A・B が同時に `ConnectionResetError` を検知した場合:

1. A が `asyncio.Lock` を取得（`async with`）→ `is_connected` が `False` → 再接続を開始
2. B は `asyncio.Lock` で `await` → イベントループに制御を返す（ブロックしない）
3. A の `await _impl.connect(...)` 完了 → `is_connected` が `True` → Lock 解放
4. B が Lock を取得 → `is_connected` が `True` → スキップして返る

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

### 7.1 基本使用（同期版）

```python
import pipeutil

client = pipeutil.ReconnectingPipeClient(
    "my_pipe",
    retry_interval_ms=500,
    max_retries=10,
    on_reconnect=lambda: print("reconnected"),
)

with client:
    client.connect()  # 初回接続（connect_timeout_ms=0 でサーバー起動を待つ）
    while True:
        # サーバーが再起動しても自動復旧
        client.send(pipeutil.Message(b"ping"))
        resp = client.receive(timeout=5.0)
        print(resp.text)
```

### 7.2 基本使用（asyncio 版）

```python
import asyncio
import pipeutil

async def main() -> None:
    client = pipeutil.AsyncReconnectingPipeClient(
        "my_pipe",
        retry_interval_ms=500,
        max_retries=10,
        on_reconnect=lambda: print("reconnected"),
    )

    async with client:
        await client.connect()  # 初回接続
        while True:
            await client.send(pipeutil.Message(b"ping"))
            resp = await client.receive(timeout_ms=5000)
            print(resp.text)

asyncio.run(main())
```

### 7.3 無限リトライ + ロギング

```python
import logging
import pipeutil

def on_reconnect() -> None:
    logging.warning("Reconnected to server")

# 同期版
client = pipeutil.ReconnectingPipeClient(
    "my_pipe",
    max_retries=0,          # 無限リトライ
    retry_interval_ms=1000,
    on_reconnect=on_reconnect,
)

# 非同期版
async_client = pipeutil.AsyncReconnectingPipeClient(
    "my_pipe",
    max_retries=0,
    retry_interval_ms=1000,
    on_reconnect=on_reconnect,  # 同期コールバックを渡す
)
```

### 7.4 接続状態の確認

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

  ReconnectingPipeClient                          ← 同期版
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

  AsyncReconnectingPipeClient                     ← 非同期版
    __init__(pipe_name, *, retry_interval_ms, max_retries, connect_timeout_ms,
             on_reconnect, buffer_size)
    async connect(timeout_ms) → None
    async send(msg) → None
    async receive(timeout_ms) → Message
    async close() → None
    __aenter__() / __aexit__()
    pipe_name: str            [property]
    is_connected: bool        [property]
    retry_count: int          [property]
    async _reconnect_with_retry()   [private]

  ReconnectingRpcPipeClient                       ← RPC版（標準 + send_request)
    __init__(pipe_name, *, retry_interval_ms, max_retries, connect_timeout_ms,
             on_reconnect, buffer_size)
    connect(timeout_ms) → None
    send(msg) → None
    receive(timeout) → Message
    send_request(msg, timeout) → Message   [方針 A: 再送なし]
    close() → None
    __enter__() / __exit__()
    pipe_name: str            [property]
    is_connected: bool        [property]
    retry_count: int          [property]
    _reconnect_with_retry()   [private]
```

依存（同期版）: `time`, `threading`, `._pipeutil.PipeError`, `._pipeutil.PipeClient`,
       `._pipeutil.Message`, `._pipeutil.ConnectionResetError`, `._pipeutil.BrokenPipeError`,
       `._pipeutil.NotConnectedError`

依存（非同期版）: `asyncio`, `._pipeutil.PipeError`, `._pipeutil.Message`,
       `._pipeutil.ConnectionResetError`, `._pipeutil.BrokenPipeError`,
       `._pipeutil.NotConnectedError`, `.aio.AsyncPipeClient`

依存（RPC版）: `time`, `threading`, `._pipeutil.PipeError`, `._pipeutil.RpcPipeClient`,
       `._pipeutil.Message`, `._pipeutil.ConnectionResetError`, `._pipeutil.BrokenPipeError`,
       `._pipeutil.NotConnectedError`

### 8.2 更新ファイル: `python/pipeutil/__init__.py`

```python
from .reconnecting_client import (   # noqa: F401
    ReconnectingPipeClient,
    AsyncReconnectingPipeClient,
    ReconnectingRpcPipeClient,
    MaxRetriesExceededError,
)
```

`__all__` に `"ReconnectingPipeClient"`, `"AsyncReconnectingPipeClient"`, `"ReconnectingRpcPipeClient"`, `"MaxRetriesExceededError"` を追加。

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

    def send(self, msg: Message) -> None:
        """フレーム化メッセージを送信する。切断時は自動再接続後に再送信する。

        Raises
        ------
        MaxRetriesExceededError
            再接続試行がすべて失敗した場合。
        NotConnectedError
            close() 済みのインスタンスに対して呼んだ場合。
        """
        ...

    def receive(self, timeout: float = 0.0) -> Message:
        """フレーム化メッセージを受信する。切断時は自動再接続後に次のメッセージを待機する。

        Raises
        ------
        MaxRetriesExceededError
            再接続試行がすべて失敗した場合。
        TimeoutError
            timeout 秒以内にメッセージが届かなかった場合。
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

# ─── AsyncReconnectingPipeClient ────────────────────────────────────
class AsyncReconnectingPipeClient:
    """AsyncPipeClient の自動再接続ラッパー（asyncio コルーチン版）。

    send / receive で ConnectionResetError / BrokenPipeError を受けると
    自動的に再接続してからオペレーションを再試行する。

    注意: 1インスタンス = 1イベントループ固定（AsyncPipeClient と同じ制約）。

    ライフサイクル: ``__init__()`` → ``await connect()`` → ``await send()`` / ``await receive()`` → ``await close()``
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

    async def connect(self, timeout_ms: int = 0) -> None:
        """サーバーへ初回接続する（コルーチン）。

        Parameters
        ----------
        timeout_ms:
            接続試行タイムアウト (ms)。0 = 無限待機。
        """
        ...

    async def send(self, msg: Message, timeout_ms: int = 0) -> None:
        """フレーム化メッセージを送信する（コルーチン）。切断時は自動再接続後に再送信する。

        Raises
        ------
        MaxRetriesExceededError
            再接続試行がすべて失敗した場合。
        NotConnectedError
            close() 済みのインスタンスに対して呼んだ場合。
        """
        ...

    async def receive(self, timeout_ms: int = 5000) -> Message:
        """フレーム化メッセージを受信する（コルーチン）。切断時は自動再接続後に次のメッセージを待機する。

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

    async def close(self) -> None:
        """接続を閉じ、以降の操作を無効化する（コルーチン / 冪等）。"""
        ...

    async def __aenter__(self) -> AsyncReconnectingPipeClient: ...

    async def __aexit__(
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

# ─── ReconnectingRpcPipeClient ────────────────────────────────────

class ReconnectingRpcPipeClient:
    """RpcPipeClient の自動再接続ラッパー。

    切断を検知した場合に接続を再確立する。
    send_request() は in-flight のリクエストを再送しない（方針 A）。

    ライフサイクル: ``__init__()`` → ``connect()`` → ``send_request()`` / ``send()`` / ``receive()`` → ``close()``
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
        """サーバーへ初回接続する。"""
        ...

    def send(self, msg: Message) -> None:
        """通常フレームを送信する（message_id=0）。切断時は自動再接続後に再送信する。"""
        ...

    def receive(self, timeout: float = 0.0) -> Message:
        """通常受信キューからメッセージを取り出す。切断時は自動再接続後に次のメッセージを待機する。"""
        ...

    def send_request(self, msg: Message, timeout: float = 5.0) -> Message:
        """RPC リクエストを送信しレスポンスを返す。切断時は接続を再確立するが、in-flight のリクエストは再送しない（方針 A）。

        Raises
        ------
        ConnectionResetError / BrokenPipeError
            送信中または応答待機中に切断が発生した場合。再接続後も上位に伝播される。
        MaxRetriesExceededError
            再接続試行がすべて失敗した場合。
        TimeoutError
            timeout 内にレスポンスが届かなかった場合（再接続は行わない）。
        NotConnectedError
            close() 済みのインスタンスに対して呼んだ場合。
        """
        ...

    def close(self) -> None:
        """接続を閉じ、以降の操作を無効化する（冪等）。"""
        ...

    def __enter__(self) -> ReconnectingRpcPipeClient: ...

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc_val: BaseException | None,
        exc_tb: TracebackType | None,
    ) -> None: ...

    @property
    def pipe_name(self) -> str: ...

    @property
    def is_connected(self) -> bool: ...

    @property
    def retry_count(self) -> int: ...
```

---

## 10. テスト設計

テストファイル: `tests/python/test_reconnecting.py`

### 10.1 同期版テスト（`ReconnectingPipeClient`）

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

### 10.2 非同期版テスト（`AsyncReconnectingPipeClient`）

| # | テスト名 | 概要 |
|---|---|---|
| T13 | `test_async_basic_send_receive` | 正常接続・非同期送受信ラウンドトリップが成功すること |
| T14 | `test_async_context_manager` | `async with` ブロック脱出後に `close()` が呼ばれ `NotConnectedError` になること |
| T15 | `test_async_reconnect_on_connection_reset` | `ConnectionResetError` 後に再接続して非同期送信成功すること |
| T16 | `test_async_max_retries_exceeded` | 再接続失敗 N 回で `MaxRetriesExceededError` が送出されること |
| T17 | `test_async_on_reconnect_callback` | 再接続成功後に同期コールバックが 1 回呼ばれること |
| T18 | `test_async_retry_count_property` | `retry_count` が再接続成功ごとにインクリメントされること |
| T19 | `test_async_close_prevents_reconnect` | `close()` 後の `send()` が `NotConnectedError`（再接続しない）こと |
| T20 | `test_async_task_safe_reconnect` | 複数タスクが同時に切断を検知しても 1 回だけ再接続すること |

非同期テストはすべて `pytest-asyncio` を使用し、`@pytest.mark.asyncio` デコレータを付与する。
モック方法は同期版と同様。`AsyncPipeClient.send` / `receive` / `connect` / `close` / `is_connected` をモック化する。

### 10.3 RPC 版テスト（`ReconnectingRpcPipeClient`）

| # | テスト名 | 概要 |
|---|---|---|
| T21 | `test_rpc_basic_send_request` | 正常接続で `send_request()` が成功すること |
| T22 | `test_rpc_reconnect_on_connection_reset` | 切断後に接続を再確立し、`ConnectionResetError` を上位に伝播すること |
| T23 | `test_rpc_no_auto_resend` | 切断後に in-flight リクエストが自動再送されないこと（方針 A 検証） |
| T24 | `test_rpc_timeout_not_retried` | `TimeoutError` では再接続しないこと |
| T25 | `test_rpc_max_retries_exceeded` | 再接続失敗 N 回で `MaxRetriesExceededError` が送出されること |

`RpcPipeClient.send_request` / `connect` / `close` / `is_connected` をモック化し、T22 では `send_request` が `ConnectionResetError` を送出 → 再接続後も同じ例外を上位に伝播することを確認する。 T23 では切断後に `_impl.send_request` の呼び出し回数が 1 回のみであること（再呼び出ししない）をアサートする。

---

## 11. 制約・注意事項

1. **at-most-once セマンティクス**: `send()` は再接続後に同一メッセージを 1 回のみ再送信する。
   再送後の送信失敗（例: 2 回目の接続もすぐ切れた場合）は再試行せずに例外を伝播する。

2. **`receive()` のメッセージ消失**: パイプ切断時に受信途中だったメッセージは失われる。
   アプリケーション層での確認応答（ACK）が必要な場合、このクラスだけでは不十分。

3. **`send_request()` は再送しない（方針 A）**: `ReconnectingRpcPipeClient` は切断を検知したとき
   接続を再確立するが、in-flight の `send_request()` を自動再送しない。

   **理由**: `RpcPipeClient` 内部の `receiver_loop()` が切断を検知すると
   `notify_all_pending()` が走り、`pending_map_` 内の全 `promise` に例外を伝播してクリアされる。
   この時点でリクエストがサーバーに届いたかどうかは不明であり、再送すると
   サーバーが同じ処理を二重実行する可能性がある（`send_request()` に冪等性保証はない）。

   **方針 A の動作**: 切断 → `_reconnect_with_retry()` で接続再確立 → `send_request()` の
   `future.get()` から `ConnectionResetError` / `BrokenPipeError` が上位に伝播する。
   ユーザーが再送の要否を判断し、必要なら明示的に `send_request()` を再呼び出しする。

   **自動再送（方針 B）を提供しない理由**: `idempotent=True` でオプトインする設計も検討したが、
   冪等性の保証責任をライブラリが引き取れないため本 spec では対象外とする。

4. **`AsyncReconnectingPipeClient` の asyncio コールバック制約**: `on_reconnect` は同期コールバックのみ受け付ける。
   コルーチン関数を渡してもそのまま呼ばれるだけで await されない（意図しない動作になる）。
   asyncio 上での非同期コールバックが必要な場合は、`on_reconnect` から `asyncio.ensure_future()` を使用すること（ただし Lock 保持中のため注意）。

5. **`on_reconnect` コールバックの注意**: コールバックは `_lock` 保持中に呼ばれる。
   コールバック内でこのクラスの `send()` / `receive()` を呼ぶとデッドロックする。

6. **`connect_timeout_ms` と `PipeClient.connect()` の単位変換**:
   `PipeClient.connect(timeout: float)` は **秒単位**（`__init__.pyi` 準拠）。
   `_reconnect_with_retry()` 内では `connect_timeout_ms / 1000.0` に変換して渡すこと。

7. **`max_retries=0`（無限リトライ）の使用**: サーバーが永久に起動しない場合は
   プロセスが永久にブロックする。`KeyboardInterrupt` / `SIGTERM` によるプロセス終了で
   中断できることを確認して使用すること。
