# python/pipeutil/threading_utils.py
# [T] threading / concurrent.futures 対応ラッパー
# 仕様: spec/F004_async_threading_multiprocessing.md §4
#
# 既存 PipeClient / PipeServer はミューテックス保護済みのため
# 複数スレッドから直接呼び出し可能。本モジュールは send/receive を
# concurrent.futures.Future として返すラッパーを提供する。

from __future__ import annotations

import concurrent.futures
from typing import Optional

from . import Message, PipeClient, PipeServer


# ─── ThreadedPipeClient ──────────────────────────────────────────────

class ThreadedPipeClient:
    """
    PipeClient の concurrent.futures ラッパー。
    send / receive を Future として非同期実行する。

    仕様: spec/F004 §4.2

    例::

        client = ThreadedPipeClient("my_pipe")
        client.connect(timeout_ms=3000)
        fut = client.send_async(pipeutil.Message(b"hello"))
        fut.result()  # 送信完了を待機
    """

    def __init__(
        self,
        pipe_name: str,
        buffer_size: int = 65536,
        executor: Optional[concurrent.futures.Executor] = None,
    ) -> None:
        """
        Parameters
        ----------
        pipe_name:
            接続先パイプ名。
        buffer_size:
            パイプバッファサイズ（バイト）。
        executor:
            使用するスレッドプール。None の場合は内部で ThreadPoolExecutor を生成し、
            close() 時にシャットダウンする。
        """
        self._impl = PipeClient(pipe_name, buffer_size)
        self._own_executor = executor is None
        self._executor: concurrent.futures.Executor = (
            executor
            if executor is not None
            else concurrent.futures.ThreadPoolExecutor(
                max_workers=4,
                thread_name_prefix="pipeutil-thread",
            )
        )

    # ─── 同期操作 ────────────────────────────────────────────────────

    def connect(self, timeout_ms: int = 5000) -> None:
        """同期接続（スレッドオフロードなし）。タイムアウト時は PipeTimeoutError。"""
        self._impl.connect(timeout_ms)

    def close(self) -> None:
        """接続をクローズし、自前の executor を shutdown する（冪等）。"""
        self._impl.close()
        if self._own_executor:
            self._executor.shutdown(wait=False)

    @property
    def pipe_name(self) -> str:
        """パイプ名を返す。"""
        return self._impl.pipe_name

    # ─── 非同期操作（Future 返し）──────────────────────────────────────

    def send_async(self, msg: Message) -> "concurrent.futures.Future[None]":
        """バックグラウンドスレッドでメッセージを送信する。"""
        return self._executor.submit(self._impl.send, msg)

    def receive_async(
        self, timeout_ms: int = 5000
    ) -> "concurrent.futures.Future[Message]":
        """バックグラウンドスレッドでメッセージを受信する。"""
        return self._executor.submit(self._impl.receive, timeout_ms)

    # ─── コンテキストマネージャ（同期 with 構文）──────────────────────

    def __enter__(self) -> "ThreadedPipeClient":
        return self

    def __exit__(self, *args: object) -> None:
        self.close()


# ─── ThreadedPipeServer ──────────────────────────────────────────────

class ThreadedPipeServer:
    """
    PipeServer の concurrent.futures ラッパー。
    accept を Future として非同期実行でき、接続ごとにスレッドを振り分ける。

    仕様: spec/F004 §4.2

    例::

        server = ThreadedPipeServer("my_pipe")
        server.listen()
        accept_fut = server.accept_async(timeout_ms=10000)
        accept_fut.result()  # 接続完了を待機

        recv_fut = server.receive_async(timeout_ms=5000)
        msg = recv_fut.result()
    """

    def __init__(
        self,
        pipe_name: str,
        buffer_size: int = 65536,
        executor: Optional[concurrent.futures.Executor] = None,
    ) -> None:
        """
        Parameters
        ----------
        pipe_name:
            サーバーが公開するパイプ名。
        buffer_size:
            パイプバッファサイズ（バイト）。
        executor:
            使用するスレッドプール。None の場合は内部で生成・管理する。
        """
        self._impl = PipeServer(pipe_name, buffer_size)
        self._own_executor = executor is None
        self._executor: concurrent.futures.Executor = (
            executor
            if executor is not None
            else concurrent.futures.ThreadPoolExecutor(
                max_workers=4,
                thread_name_prefix="pipeutil-server-thread",
            )
        )

    # ─── 同期操作 ────────────────────────────────────────────────────

    def listen(self) -> None:
        """パイプを作成して接続待ち状態に移行する。"""
        self._impl.listen()

    def close(self) -> None:
        """接続をクローズし、自前の executor を shutdown する（冪等）。"""
        self._impl.close()
        if self._own_executor:
            self._executor.shutdown(wait=False)

    @property
    def pipe_name(self) -> str:
        """パイプ名を返す。"""
        return self._impl.pipe_name

    # ─── 非同期操作（Future 返し）──────────────────────────────────────

    def accept_async(
        self, timeout_ms: int = 30000
    ) -> "concurrent.futures.Future[None]":
        """バックグラウンドでクライアント接続を待機する。完了後 is_connected が True。"""
        return self._executor.submit(self._impl.accept, timeout_ms)

    def send_async(self, msg: Message) -> "concurrent.futures.Future[None]":
        """バックグラウンドスレッドでメッセージを送信する。"""
        return self._executor.submit(self._impl.send, msg)

    def receive_async(
        self, timeout_ms: int = 5000
    ) -> "concurrent.futures.Future[Message]":
        """バックグラウンドスレッドでメッセージを受信する。"""
        return self._executor.submit(self._impl.receive, timeout_ms)

    # ─── コンテキストマネージャ（同期 with 構文）──────────────────────

    def __enter__(self) -> "ThreadedPipeServer":
        return self

    def __exit__(self, *args: object) -> None:
        self.close()
