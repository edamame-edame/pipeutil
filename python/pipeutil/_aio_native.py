# python/pipeutil/_aio_native.py
# Phase 2 ネイティブバックエンドのグルーレイヤー
# aio.py からのみ import される（直接公開 API ではない）
# 仕様: spec/F004p2_async_native.md §5
from __future__ import annotations

import asyncio
from typing import Any, Optional

from ._pipeutil_async import AsyncPipeHandle  # type: ignore[import]
from ._pipeutil import Message, TimeoutError as _PipeTimeoutError  # type: ignore[import]


class NativeAsyncPipe:
    """
    AsyncPipeHandle を asyncio コルーチンインタフェースで包むラッパー。

    クライアント側: connect → read_frame / write_frame → close
    サーバー側:    server_accept → read_frame / write_frame → close
    """

    def __init__(
        self,
        buf_size: int = 65536,
        *,
        _handle: Optional[AsyncPipeHandle] = None,
    ) -> None:
        self._handle: AsyncPipeHandle = (
            _handle if _handle is not None else AsyncPipeHandle(buf_size)
        )
        self._buf_size = buf_size

    # ─── 接続（クライアント側） ───────────────────────────────────────

    async def connect(self, pipe_name: str, timeout_ms: int = 5000) -> None:
        """クライアントとして接続する（GIL を解放して待機）。"""
        loop = asyncio.get_running_loop()
        await loop.run_in_executor(None, self._handle.connect, pipe_name, timeout_ms)

    # ─── 接続待機（サーバー側） ───────────────────────────────────────

    async def server_accept(
        self, pipe_name: str, timeout_ms: int = 30000
    ) -> "NativeAsyncPipe":
        """
        サーバーとして接続を 1 件受け付ける。
        接続済みの NativeAsyncPipe を返し、自身は次の accept に備える。
        GIL を解放して待機する。
        """
        loop = asyncio.get_running_loop()
        connected_handle: AsyncPipeHandle = await loop.run_in_executor(
            None,
            self._handle.server_create_and_accept,
            pipe_name,
            timeout_ms,
        )
        return NativeAsyncPipe(self._buf_size, _handle=connected_handle)

    # ─── 非同期 I/O ──────────────────────────────────────────────────

    async def read_frame(self, timeout_ms: int = 0) -> Message:
        """
        FrameHeader + payload を非同期受信する。

        Args:
            timeout_ms: タイムアウト（ミリ秒）。0 は無制限待機。
        Raises:
            pipeutil.TimeoutError: timeout_ms > 0 で時間内に受信できなかった場合。
            asyncio.CancelledError: タスクがキャンセルされた場合（I/O もキャンセルする）。
        """
        loop = asyncio.get_running_loop()
        future: asyncio.Future[bytes] = loop.create_future()

        self._handle.async_read_frame(loop, future)

        try:
            if timeout_ms > 0:
                payload = await asyncio.wait_for(future, timeout=timeout_ms / 1000.0)
            else:
                payload = await future
        except asyncio.TimeoutError:
            self._handle.cancel()
            raise _PipeTimeoutError(
                f"read_frame timed out after {timeout_ms} ms"
            ) from None
        except asyncio.CancelledError:
            self._handle.cancel()
            raise

        return Message(payload)

    async def write_frame(self, msg: Message, message_id: int = 0) -> None:
        """
        FrameHeader + payload を非同期送信する。

        Raises:
            asyncio.CancelledError: タスクがキャンセルされた場合（I/O もキャンセルする）。
        """
        loop = asyncio.get_running_loop()
        future: asyncio.Future[None] = loop.create_future()

        self._handle.async_write_frame(bytes(msg), message_id, loop, future)

        try:
            await future
        except asyncio.CancelledError:
            self._handle.cancel()
            raise

    # ─── キャンセル / クローズ ────────────────────────────────────────

    def cancel(self) -> None:
        """実行中の非同期 I/O をすべてキャンセルする。"""
        self._handle.cancel()

    def close(self) -> None:
        """ハンドルを閉じリソースを解放する（べきとう）。"""
        self._handle.close()

    # ─── コンテキストマネージャ ───────────────────────────────────────

    async def __aenter__(self) -> "NativeAsyncPipe":
        return self

    async def __aexit__(self, *args: Any) -> None:
        self.close()

    # ─── プロパティ ───────────────────────────────────────────────────

    @property
    def is_connected(self) -> bool:
        return self._handle.is_connected
