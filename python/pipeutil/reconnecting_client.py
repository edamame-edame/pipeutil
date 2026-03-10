# python/pipeutil/reconnecting_client.py
# F-003: 自動再接続クライアント群
# 仕様: spec/F003_reconnecting_pipe_client.md

from __future__ import annotations

import asyncio
import threading
import time
from typing import Callable, Optional

from ._pipeutil import (
    BrokenPipeError,
    ConnectionResetError,
    Message,
    NotConnectedError,
    PipeClient,
    PipeError,
    RpcPipeClient,
)
from .aio import AsyncPipeClient

# 切断を意味する例外。これに該当した場合のみ再接続を試みる。
_RECONNECT_ERRORS = (ConnectionResetError, BrokenPipeError)


# ─── 例外クラス ───────────────────────────────────────────────────────

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


# ─── 共通引数検証 ─────────────────────────────────────────────────────

def _validate_reconnect_args(
    retry_interval_ms: int,
    max_retries: int,
    connect_timeout_ms: int,
) -> None:
    """コンストラクタ共通の引数検証（仕様 §3.1）。"""
    if retry_interval_ms < 0:
        raise ValueError(f"retry_interval_ms must be >= 0, got {retry_interval_ms}")
    if max_retries < 0:
        raise ValueError(f"max_retries must be >= 0, got {max_retries}")
    if connect_timeout_ms < 0:
        raise ValueError(f"connect_timeout_ms must be >= 0, got {connect_timeout_ms}")


# ─── ReconnectingPipeClient ───────────────────────────────────────────

class ReconnectingPipeClient:
    """PipeClient の自動再接続ラッパー（同期版）。

    send / receive で ConnectionResetError / BrokenPipeError を受けると
    自動的に再接続してからオペレーションを再試行する（at-most-once セマンティクス）。

    スレッド安全性: _lock（threading.Lock）で再接続クリティカルセクションを保護する。

    ライフサイクル:
        __init__() → connect() → send() / receive() → close()
    """

    def __init__(
        self,
        pipe_name: str,
        *,
        retry_interval_ms: int = 500,
        max_retries: int = 0,
        connect_timeout_ms: int = 5000,
        on_reconnect: Optional[Callable[[], None]] = None,
        buffer_size: int = 65536,
    ) -> None:
        _validate_reconnect_args(retry_interval_ms, max_retries, connect_timeout_ms)
        self._pipe_name = pipe_name
        self._retry_interval_ms = retry_interval_ms
        self._max_retries = max_retries
        self._connect_timeout_ms = connect_timeout_ms
        self._on_reconnect = on_reconnect
        self._impl = PipeClient(pipe_name, buffer_size)
        self._lock = threading.Lock()
        self._closed = False
        self._retry_count = 0

    # ─── 公開メソッド ─────────────────────────────────────────────────

    def connect(self, timeout_ms: int = 0) -> None:
        """サーバーへ初回接続する。

        Parameters
        ----------
        timeout_ms:
            接続試行タイムアウト (ms)。0 = 無限待機。
            コンストラクタの connect_timeout_ms より優先する。

        Raises
        ------
        PipeError
            接続に失敗した場合。
        NotConnectedError
            close() 済みのインスタンスに対して呼んだ場合。
        """
        if self._closed:
            raise NotConnectedError("Cannot connect: client is closed")
        # PipeClient.connect は秒単位（仕様 §11.6）
        self._impl.connect(timeout_ms / 1000.0)

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
        if self._closed:
            raise NotConnectedError("Cannot send: client is closed")
        try:
            self._impl.send(msg)
        except _RECONNECT_ERRORS:
            self._reconnect_with_retry()
            self._impl.send(msg)  # 1 回のみ再試行（再帰しない）

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
        if self._closed:
            raise NotConnectedError("Cannot receive: client is closed")
        try:
            return self._impl.receive(timeout)
        except _RECONNECT_ERRORS:
            self._reconnect_with_retry()
            return self._impl.receive(timeout)

    def close(self) -> None:
        """接続を閉じ、以降の send/receive/connect を無効化する（冪等）。

        ロックを取得しない（デッドロック防止）。
        close() 後のすべての操作は NotConnectedError を送出する。
        """
        self._closed = True
        self._impl.close()

    # ─── プロパティ ───────────────────────────────────────────────────

    @property
    def pipe_name(self) -> str:
        """接続先パイプ名を返す（read-only）。"""
        return self._pipe_name

    @property
    def is_connected(self) -> bool:
        """現在接続中なら True。"""
        return self._impl.is_connected

    @property
    def retry_count(self) -> int:
        """インスタンス生成からの累計再接続成功回数。close() でリセットされない。"""
        return self._retry_count

    # ─── コンテキストマネージャ ───────────────────────────────────────

    def __enter__(self) -> "ReconnectingPipeClient":
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.close()

    # ─── 内部メソッド ─────────────────────────────────────────────────

    def _reconnect_with_retry(self) -> None:
        """Lock 取得後に再接続を試みる。

        別スレッドが既に再接続を完了していた場合（is_connected=True）は
        何もせず返る（二重再接続防止）。
        """
        with self._lock:
            # 二重再接続防止: 別スレッドが先に再接続済みであればスキップ
            if self._impl.is_connected:
                return

            attempts = 0
            last_exc: Optional[Exception] = None
            # PipeClient.connect は秒単位（仕様 §11.6）
            timeout_sec = self._connect_timeout_ms / 1000.0

            while self._max_retries == 0 or attempts < self._max_retries:
                attempts += 1
                try:
                    self._impl.close()
                    time.sleep(self._retry_interval_ms / 1000.0)
                    self._impl.connect(timeout_sec)
                    # 接続成功
                    self._retry_count += 1
                    if self._on_reconnect is not None:
                        self._on_reconnect()
                    return
                except PipeError as e:
                    last_exc = e

            raise MaxRetriesExceededError(
                attempts=attempts,
                last_exception=last_exc,  # type: ignore[arg-type]
            )


# ─── AsyncReconnectingPipeClient ─────────────────────────────────────

class AsyncReconnectingPipeClient:
    """AsyncPipeClient の自動再接続ラッパー（asyncio コルーチン版）。

    send / receive で ConnectionResetError / BrokenPipeError を受けると
    自動的に再接続してからオペレーションを再試行する。

    注意:
        1インスタンス = 1イベントループ固定（AsyncPipeClient と同じ制約）。
        on_reconnect には同期コールバックのみ渡すこと（コルーチン不可）。

    ライフサイクル:
        __init__() → await connect() → await send() / await receive() → await close()
    """

    def __init__(
        self,
        pipe_name: str,
        *,
        retry_interval_ms: int = 500,
        max_retries: int = 0,
        connect_timeout_ms: int = 5000,
        on_reconnect: Optional[Callable[[], None]] = None,
        buffer_size: int = 65536,
    ) -> None:
        _validate_reconnect_args(retry_interval_ms, max_retries, connect_timeout_ms)
        self._pipe_name = pipe_name
        self._retry_interval_ms = retry_interval_ms
        self._max_retries = max_retries
        self._connect_timeout_ms = connect_timeout_ms
        self._on_reconnect = on_reconnect
        self._impl = AsyncPipeClient(pipe_name, buffer_size)
        # asyncio.Lock はイベントループにバインドされる（Python 3.8 互換）
        self._lock = asyncio.Lock()
        self._closed = False
        self._retry_count = 0

    # ─── 公開メソッド（コルーチン） ───────────────────────────────────

    async def connect(self, timeout_ms: int = 0) -> None:
        """サーバーへ初回接続する（コルーチン）。

        Parameters
        ----------
        timeout_ms:
            接続試行タイムアウト (ms)。0 = 無限待機。
            AsyncPipeClient.connect はミリ秒単位を受け付ける。
        """
        if self._closed:
            raise NotConnectedError("Cannot connect: client is closed")
        await self._impl.connect(timeout_ms)

    async def send(self, msg: Message) -> None:
        """フレーム化メッセージを送信する（コルーチン）。

        切断を検知した場合は自動再接続後に同一メッセージを 1 回のみ再送信する。
        """
        if self._closed:
            raise NotConnectedError("Cannot send: client is closed")
        try:
            await self._impl.send(msg)
        except _RECONNECT_ERRORS:
            await self._reconnect_with_retry()
            await self._impl.send(msg)  # 1 回のみ再試行

    async def receive(self, timeout_ms: int = 5000) -> Message:
        """フレーム化メッセージを受信する（コルーチン）。

        切断を検知した場合は自動再接続後に次のメッセージを待機する。
        """
        if self._closed:
            raise NotConnectedError("Cannot receive: client is closed")
        try:
            return await self._impl.receive(timeout_ms)
        except _RECONNECT_ERRORS:
            await self._reconnect_with_retry()
            return await self._impl.receive(timeout_ms)

    async def close(self) -> None:
        """接続を閉じ、以降の操作を無効化する（コルーチン / 冪等）。

        ロックを取得しない（デッドロック防止）。
        """
        self._closed = True
        await self._impl.close()

    # ─── プロパティ ───────────────────────────────────────────────────

    @property
    def pipe_name(self) -> str:
        return self._pipe_name

    @property
    def is_connected(self) -> bool:
        return self._impl.is_connected

    @property
    def retry_count(self) -> int:
        return self._retry_count

    # ─── 非同期コンテキストマネージャ ─────────────────────────────────

    async def __aenter__(self) -> "AsyncReconnectingPipeClient":
        return self

    async def __aexit__(self, exc_type, exc_val, exc_tb) -> None:
        await self.close()

    # ─── 内部コルーチン ───────────────────────────────────────────────

    async def _reconnect_with_retry(self) -> None:
        """asyncio.Lock 取得後に再接続を試みる。

        別タスクが既に再接続を完了していた場合（is_connected=True）は
        何もせず返る（二重再接続防止）。
        """
        async with self._lock:
            # 二重再接続防止: 別タスクが先に再接続済みであればスキップ
            if self._impl.is_connected:
                return

            attempts = 0
            last_exc: Optional[Exception] = None

            while self._max_retries == 0 or attempts < self._max_retries:
                attempts += 1
                try:
                    await self._impl.close()
                    await asyncio.sleep(self._retry_interval_ms / 1000.0)
                    # AsyncPipeClient.connect はミリ秒単位（仕様 §4.5）
                    await self._impl.connect(self._connect_timeout_ms)
                    # 接続成功
                    self._retry_count += 1
                    if self._on_reconnect is not None:
                        self._on_reconnect()  # 同期コールバック（await しない）
                    return
                except PipeError as e:
                    last_exc = e

            raise MaxRetriesExceededError(
                attempts=attempts,
                last_exception=last_exc,  # type: ignore[arg-type]
            )


# ─── ReconnectingRpcPipeClient ────────────────────────────────────────

class ReconnectingRpcPipeClient:
    """RpcPipeClient の自動再接続ラッパー（同期版）。

    切断を検知した場合に接続を再確立する。
    send_request() は in-flight のリクエストを再送しない（方針 A）。

    ライフサイクル:
        __init__() → connect() → send_request() / send() / receive() → close()
    """

    def __init__(
        self,
        pipe_name: str,
        *,
        retry_interval_ms: int = 500,
        max_retries: int = 0,
        connect_timeout_ms: int = 5000,
        on_reconnect: Optional[Callable[[], None]] = None,
        buffer_size: int = 65536,
    ) -> None:
        _validate_reconnect_args(retry_interval_ms, max_retries, connect_timeout_ms)
        self._pipe_name = pipe_name
        self._retry_interval_ms = retry_interval_ms
        self._max_retries = max_retries
        self._connect_timeout_ms = connect_timeout_ms
        self._on_reconnect = on_reconnect
        self._impl = RpcPipeClient(pipe_name, buffer_size)
        self._lock = threading.Lock()
        self._closed = False
        self._retry_count = 0

    # ─── 公開メソッド ─────────────────────────────────────────────────

    def connect(self, timeout_ms: int = 0) -> None:
        """サーバーへ初回接続する。"""
        if self._closed:
            raise NotConnectedError("Cannot connect: client is closed")
        # RpcPipeClient.connect は秒単位（仕様 §11.6）
        self._impl.connect(timeout_ms / 1000.0)

    def send(self, msg: Message) -> None:
        """通常フレームを送信する（message_id=0）。

        切断を検知した場合は自動再接続後に同一メッセージを 1 回のみ再送信する。
        """
        if self._closed:
            raise NotConnectedError("Cannot send: client is closed")
        try:
            self._impl.send(msg)
        except _RECONNECT_ERRORS:
            self._reconnect_with_retry()
            self._impl.send(msg)

    def receive(self, timeout: float = 0.0) -> Message:
        """通常受信キューからメッセージを取り出す（message_id=0 のフレームのみ）。

        切断を検知した場合は自動再接続後に次のメッセージを待機する。
        timeout は秒単位（RpcPipeClient.receive と同一単位）。0.0 = 無限待機。
        """
        if self._closed:
            raise NotConnectedError("Cannot receive: client is closed")
        try:
            return self._impl.receive(timeout)
        except _RECONNECT_ERRORS:
            self._reconnect_with_retry()
            return self._impl.receive(timeout)

    def send_request(self, msg: Message, timeout: float = 5.0) -> Message:
        """RPC リクエストを送信し、対応するレスポンスを返す（方針 A: 再送なし）。

        切断を検知した場合は _reconnect_with_retry() で接続を再確立する。
        in-flight のリクエストは再送しないため、ConnectionResetError /
        BrokenPipeError がそのまま上位に伝播する（ユーザーが再送の要否を判断する）。

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
        if self._closed:
            raise NotConnectedError("Cannot send_request: client is closed")
        try:
            return self._impl.send_request(msg, timeout)
        except _RECONNECT_ERRORS:
            # 接続を再確立してから in-flight の例外を上位に伝播（方針 A）
            self._reconnect_with_retry()
            raise

    def close(self) -> None:
        """接続を閉じ、以降の操作を無効化する（冪等）。"""
        self._closed = True
        self._impl.close()

    # ─── プロパティ ───────────────────────────────────────────────────

    @property
    def pipe_name(self) -> str:
        return self._pipe_name

    @property
    def is_connected(self) -> bool:
        return self._impl.is_connected

    @property
    def retry_count(self) -> int:
        return self._retry_count

    # ─── コンテキストマネージャ ───────────────────────────────────────

    def __enter__(self) -> "ReconnectingRpcPipeClient":
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.close()

    # ─── 内部メソッド ─────────────────────────────────────────────────

    def _reconnect_with_retry(self) -> None:
        """Lock 取得後に再接続を試みる。

        別スレッドが既に再接続を完了していた場合は何もせず返る（二重再接続防止）。
        """
        with self._lock:
            if self._impl.is_connected:
                return

            attempts = 0
            last_exc: Optional[Exception] = None
            timeout_sec = self._connect_timeout_ms / 1000.0

            while self._max_retries == 0 or attempts < self._max_retries:
                attempts += 1
                try:
                    self._impl.close()
                    time.sleep(self._retry_interval_ms / 1000.0)
                    self._impl.connect(timeout_sec)
                    # 接続成功
                    self._retry_count += 1
                    if self._on_reconnect is not None:
                        self._on_reconnect()
                    return
                except PipeError as e:
                    last_exc = e

            raise MaxRetriesExceededError(
                attempts=attempts,
                last_exception=last_exc,  # type: ignore[arg-type]
            )
