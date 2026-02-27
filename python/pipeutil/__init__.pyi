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

# ─── パッケージ定数 ───────────────────────────────────────────────────

__version__: str
