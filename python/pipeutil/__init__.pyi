# python/pipeutil/__init__.pyi
# 型スタブ（PEP 484）
# 仕様: spec/04_python_wrapper.md §10

from __future__ import annotations

from types import TracebackType

# ─── Message ──────────────────────────────────────────────────────────

class Message:
    """ペイロードを保持する不変値型。"""

    def __init__(self, data: bytes | bytearray | str) -> None: ...

    @property
    def data(self) -> bytes:
        """ペイロードを bytes として返す。"""
        ...

    @property
    def text(self) -> str:
        """ペイロードを UTF-8 文字列として返す。デコード失敗時 UnicodeDecodeError。"""
        ...

    def __len__(self) -> int: ...
    def __bool__(self) -> bool: ...
    def __repr__(self) -> str: ...

# ─── PipeServer ───────────────────────────────────────────────────────

class PipeServer:
    """Named pipe / UNIX domain socket サーバー。

    ライフサイクル: ``listen()`` → ``accept()`` → ``send()`` / ``receive()`` → ``close()``
    """

    def __init__(
        self,
        pipe_name: str,
        buffer_size: int = 65536,
    ) -> None: ...

    def listen(self) -> None:
        """パイプを作成して接続待ち状態に移行する。"""
        ...

    def accept(self, timeout: float = 0.0) -> None:
        """クライアントが接続するまでブロックする。

        Parameters
        ----------
        timeout:
            秒単位のタイムアウト。0.0 = 無限待機。
        """
        ...

    def send(self, msg: Message | bytes | bytearray | str) -> None:
        """フレーム化メッセージを送信する。"""
        ...

    def receive(self, timeout: float = 0.0) -> Message:
        """フレーム化メッセージを受信する。

        Parameters
        ----------
        timeout:
            秒単位のタイムアウト。0.0 = 無限待機。
        """
        ...

    def close(self) -> None:
        """接続を閉じてリソースを解放する。"""
        ...

    def __enter__(self) -> PipeServer: ...

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc_val: BaseException | None,
        exc_tb: TracebackType | None,
    ) -> None: ...

    @property
    def is_listening(self) -> bool:
        """``listen()`` 呼び出し後 ``True``。"""
        ...

    @property
    def is_connected(self) -> bool:
        """``accept()`` 成功後 ``True``。"""
        ...

    @property
    def pipe_name(self) -> str:
        """設定したパイプ識別名。"""
        ...

# ─── PipeClient ───────────────────────────────────────────────────────

class PipeClient:
    """Named pipe / UNIX domain socket クライアント。

    ライフサイクル: ``connect()`` → ``send()`` / ``receive()`` → ``close()``
    """

    def __init__(
        self,
        pipe_name: str,
        buffer_size: int = 65536,
    ) -> None: ...

    def connect(self, timeout: float = 0.0) -> None:
        """サーバーに接続する。

        Parameters
        ----------
        timeout:
            秒単位のタイムアウト。0.0 = 無限待機（サーバー起動待ちを含む）。
        """
        ...

    def send(self, msg: Message | bytes | bytearray | str) -> None:
        """フレーム化メッセージを送信する。"""
        ...

    def receive(self, timeout: float = 0.0) -> Message:
        """フレーム化メッセージを受信する。"""
        ...

    def close(self) -> None:
        """接続を閉じてリソースを解放する。"""
        ...

    def __enter__(self) -> PipeClient: ...

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc_val: BaseException | None,
        exc_tb: TracebackType | None,
    ) -> None: ...

    @property
    def is_connected(self) -> bool:
        """接続中なら ``True``。"""
        ...

    @property
    def pipe_name(self) -> str:
        """設定したパイプ識別名。"""
        ...

# ─── RpcPipeClient ───────────────────────────────────────────────────

from collections.abc import Callable

class RpcPipeClient:
    """RPC 対応パイプクライアント。send_request() でリクエスト/レスポンス型通信を行う。

    ライフサイクル: ``connect()`` → ``send_request()`` / ``send()`` / ``receive()`` → ``close()``
    """

    def __init__(self, pipe_name: str, buffer_size: int = 65536) -> None: ...

    def connect(self, timeout: float = 0.0) -> None:
        """サーバーに接続し、背景受信スレッドを起動する。"""
        ...

    def close(self) -> None:
        """背景スレッドを停止し接続を閉じる。"""
        ...

    def send(self, msg: Message) -> None:
        """通常フレームを送信する（message_id=0, flags=0）。"""
        ...

    def receive(self, timeout: float = 0.0) -> Message:
        """通常受信キューからメッセージを取り出す。"""
        ...

    def send_request(self, message: Message, timeout: float = 5.0) -> Message:
        """RPC リクエストを送信し、レスポンスを返す。

        Parameters
        ----------
        message:
            送信するリクエストメッセージ。
        timeout:
            秒単位のタイムアウト。0.0 = 無限待機。

        Raises
        ------
        TimeoutError
            レスポンスが timeout 以内に届かなかった。
        ConnectionResetError
            待機中に接続が切断された。
        """
        ...

    def is_connected(self) -> bool:
        """接続中で受信スレッドが稼働中なら ``True``。"""
        ...

    def pipe_name(self) -> str:
        """設定したパイプ識別名。"""
        ...


# ─── RpcPipeServer ───────────────────────────────────────────────────

class RpcPipeServer:
    """RPC 対応パイプサーバー。serve_requests(handler) でリクエスト/レスポンス型通信を行う。

    ライフサイクル: ``listen()`` → ``accept()`` → ``serve_requests()`` → ``stop()`` → ``close()``
    """

    def __init__(self, pipe_name: str, buffer_size: int = 65536) -> None: ...

    def listen(self) -> None: ...
    def accept(self, timeout: float = 0.0) -> None: ...
    def close(self) -> None: ...
    def stop(self) -> None:
        """serve_requests の背景スレッドを停止する。"""
        ...

    def send(self, msg: Message) -> None: ...
    def receive(self, timeout: float = 0.0) -> Message: ...

    def serve_requests(
        self,
        handler: Callable[[Message], Message],
        run_in_background: bool = False,
    ) -> None:
        """受信ループを開始する。

        Parameters
        ----------
        handler:
            リクエストを受け取りレスポンスを返す callable。
        run_in_background:
            ``True`` なら背景スレッドで実行（即時返却）。
            ``False`` なら呼び出し元スレッドで実行（ブロッキング）。
        """
        ...

    def is_listening(self) -> bool: ...
    def is_connected(self) -> bool: ...
    def is_serving(self) -> bool: ...
    def pipe_name(self) -> str: ...


# ─── 例外階層 ─────────────────────────────────────────────────────────

class PipeError(Exception):
    """pipeutil の基底例外クラス。"""
    ...

class TimeoutError(PipeError):
    """操作がタイムアウトした。"""
    ...

class ConnectionResetError(PipeError):
    """相手側が接続をリセットした。"""
    ...

class BrokenPipeError(PipeError):
    """パイプが壊れた（書き込み先がない）。"""
    ...

class NotConnectedError(PipeError):
    """接続されていない状態で I/O が試みられた。"""
    ...

class InvalidMessageError(PipeError):
    """フレームのマジック / バージョン / CRC が不正。"""
    ...

# ─── Codec ────────────────────────────────────────────────────────────
# CodecError は PipeError 定義後に配置する（R-036 対応）

class CodecError(PipeError):
    """エンコード / デコード処理の失敗。"""

    def __init__(
        self,
        message: str,
        *,
        codec: str,
        original: BaseException | None = ...,
    ) -> None: ...

    @property
    def codec(self) -> str: ...

    @property
    def original(self) -> BaseException | None: ...

# ─── パッケージ定数 ───────────────────────────────────────────────────

__version__: str

# ─── asyncio ラッパー [A]（pipeutil.aio から re-export） ──────────────

from .aio import (
    AsyncPipeClient as AsyncPipeClient,
    AsyncPipeServer as AsyncPipeServer,
    AsyncRpcPipeClient as AsyncRpcPipeClient,
    AsyncRpcPipeServer as AsyncRpcPipeServer,
    serve_connections as serve_connections,
)

# ─── threading ラッパー [T]（pipeutil.threading_utils から re-export）─

from .threading_utils import (
    ThreadedPipeClient as ThreadedPipeClient,
    ThreadedPipeServer as ThreadedPipeServer,
)

# ─── multiprocessing ラッパー [M]（pipeutil.mp から re-export）────────

from .mp import (
    WorkerPipeClient as WorkerPipeClient,
    ProcessPipeServer as ProcessPipeServer,
    spawn_worker_factory as spawn_worker_factory,
)

# ─── コーデックユーティリティ [C]（pipeutil.message_utils から re-export）─────
# CodecError は上の例外階層セクションで定義済み（R-036 対応）

from typing import Any

def encode_json(
    data: Any,
    *,
    encoding: str = ...,
    ensure_ascii: bool = ...,
    **json_kwargs: Any,
) -> Message: ...

def decode_json(
    msg: Message,
    *,
    encoding: str = ...,
    **json_kwargs: Any,
) -> Any: ...

def encode_msgpack(
    data: Any,
    *,
    use_bin_type: bool = ...,
    **pack_kwargs: Any,
) -> Message: ...

def decode_msgpack(
    msg: Message,
    *,
    raw: bool = ...,
    **unpack_kwargs: Any,
) -> Any: ...

# ─── 自動再接続クライアント [R]（pipeutil.reconnecting_client から re-export）─

from .reconnecting_client import (
    MaxRetriesExceededError as MaxRetriesExceededError,
    ReconnectingPipeClient as ReconnectingPipeClient,
    AsyncReconnectingPipeClient as AsyncReconnectingPipeClient,
    ReconnectingRpcPipeClient as ReconnectingRpcPipeClient,
)
