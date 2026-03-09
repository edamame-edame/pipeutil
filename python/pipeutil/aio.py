# python/pipeutil/aio.py
# [A] asyncio 対応ラッパー（Phase 1: asyncio.to_thread / run_in_executor ベース）
# 仕様: spec/F004_async_threading_multiprocessing.md §3
#
# Python バージョン互換性:
#   asyncio.to_thread は 3.9+。3.8 は loop.run_in_executor() にフォールバック。
#   _to_thread() ヘルパーで統一的に扱う（仕様 §9.2）。

from __future__ import annotations

import asyncio
import sys
from typing import Awaitable, Callable, Optional

from . import Message, PipeClient, PipeServer, RpcPipeClient, RpcPipeServer

# ─── バージョン互換ヘルパー ──────────────────────────────────────────

if sys.version_info < (3, 8):
    raise ImportError("pipeutil.aio requires Python 3.8 or later")

# asyncio.to_thread は Python 3.9+ で利用可能（仕様 §9.2 / §6.1）
_HAS_TO_THREAD: bool = hasattr(asyncio, "to_thread")


async def _to_thread(func: Callable[..., object], *args: object) -> object:
    """
    ブロッキング関数をスレッドオフロードで実行する互換ヘルパー。
    Python 3.9+: asyncio.to_thread()
    Python 3.8:  loop.run_in_executor(None, ...) にフォールバック
    """
    if _HAS_TO_THREAD:
        return await asyncio.to_thread(func, *args)  # type: ignore[attr-defined]
    else:
        loop = asyncio.get_event_loop()
        return await loop.run_in_executor(None, func, *args)


# ─── AsyncPipeClient ─────────────────────────────────────────────────

class AsyncPipeClient:
    """
    非同期パイプクライアント。
    Phase 1: asyncio.to_thread() によるスレッドオフロード実装。

    仕様: spec/F004 §3.2

    注意: 1インスタンス = 1イベントループ（仕様 §10.1）。
    """

    def __init__(self, pipe_name: str, buffer_size: int = 65536) -> None:
        self._impl = PipeClient(pipe_name, buffer_size)

    # ─── 接続 / 通信 ─────────────────────────────────────────────────

    async def connect(self, timeout_ms: int = 5000) -> None:
        """接続する。タイムアウト時は PipeTimeoutError を送出。"""
        await _to_thread(self._impl.connect, timeout_ms)

    async def send(self, msg: Message) -> None:
        """メッセージを送信する。接続断時は PipeBrokenError を送出。"""
        await _to_thread(self._impl.send, msg)

    async def receive(self, timeout_ms: int = 5000) -> Message:
        """メッセージを受信する。タイムアウト時は PipeTimeoutError を送出。"""
        return await _to_thread(self._impl.receive, timeout_ms)  # type: ignore[return-value]

    async def close(self) -> None:
        """接続をクローズする（冪等）。close() はノンブロッキングなので to_thread 不要。"""
        self._impl.close()

    # ─── プロパティ ───────────────────────────────────────────────────

    @property
    def is_connected(self) -> bool:
        return self._impl.is_connected

    @property
    def pipe_name(self) -> str:
        return self._impl.pipe_name

    # ─── コンテキストマネージャ ───────────────────────────────────────

    async def __aenter__(self) -> "AsyncPipeClient":
        return self

    async def __aexit__(self, *args: object) -> None:
        await self.close()


# ─── AsyncPipeServer ─────────────────────────────────────────────────

class AsyncPipeServer:
    """
    非同期パイプサーバー。
    Phase 1: asyncio.to_thread() によるスレッドオフロード実装。

    仕様: spec/F004 §3.2
    """

    def __init__(self, pipe_name: str, buffer_size: int = 65536) -> None:
        self._impl = PipeServer(pipe_name, buffer_size)

    # ─── ライフサイクル ───────────────────────────────────────────────

    async def listen(self) -> None:
        """パイプを作成して LISTEN 状態へ移行する。"""
        # listen() は即時操作（ファイル作成相当）なので to_thread 不要
        self._impl.listen()

    async def accept(self, timeout_ms: int = 30000) -> None:
        """クライアント接続を待機する。タイムアウト時は PipeTimeoutError。"""
        await _to_thread(self._impl.accept, timeout_ms)

    async def send(self, msg: Message) -> None:
        """メッセージを送信する。"""
        await _to_thread(self._impl.send, msg)

    async def receive(self, timeout_ms: int = 5000) -> Message:
        """メッセージを受信する。タイムアウト時は PipeTimeoutError。"""
        return await _to_thread(self._impl.receive, timeout_ms)  # type: ignore[return-value]

    async def close(self) -> None:
        """接続をクローズする（冪等）。"""
        self._impl.close()

    # ─── プロパティ ───────────────────────────────────────────────────

    @property
    def is_connected(self) -> bool:
        return self._impl.is_connected

    @property
    def is_listening(self) -> bool:
        return self._impl.is_listening

    @property
    def pipe_name(self) -> str:
        return self._impl.pipe_name

    # ─── コンテキストマネージャ ───────────────────────────────────────

    async def __aenter__(self) -> "AsyncPipeServer":
        return self

    async def __aexit__(self, *args: object) -> None:
        await self.close()


# ─── AsyncRpcPipeClient ──────────────────────────────────────────────

class AsyncRpcPipeClient:
    """
    非同期 RPC クライアント。F-002 RpcPipeClient の asyncio ラッパー。
    複数リクエストを asyncio.gather で並列 await 可能。

    仕様: spec/F004 §3.2
    """

    def __init__(self, pipe_name: str, buffer_size: int = 65536) -> None:
        self._impl = RpcPipeClient(pipe_name, buffer_size)

    # ─── 接続 ────────────────────────────────────────────────────────

    async def connect(self, timeout_ms: int = 5000) -> None:
        """サーバーに接続し、背景受信スレッドを起動する。"""
        # RpcPipeClient.connect の timeout は秒単位（__init__.pyi 参照）
        timeout_sec = timeout_ms / 1000.0
        await _to_thread(self._impl.connect, timeout_sec)

    async def close(self) -> None:
        """背景スレッドを停止し接続をクローズする。"""
        self._impl.close()

    # ─── RPC 通信 ────────────────────────────────────────────────────

    async def send_request(
        self,
        req: Message,
        timeout_ms: int = 5000,
    ) -> Message:
        """
        リクエストを送信し、対応する応答を await で待機する。
        複数リクエストを asyncio.gather で並列 await 可能（仕様 §3.2）。
        """
        timeout_sec = timeout_ms / 1000.0
        return await _to_thread(self._impl.send_request, req, timeout_sec)  # type: ignore[return-value]

    async def send(self, msg: Message) -> None:
        """通常送信（RPC でない通常フレーム）。"""
        await _to_thread(self._impl.send, msg)

    async def receive(self, timeout_ms: int = 5000) -> Message:
        """通常受信（通常フレームのみ）。"""
        timeout_sec = timeout_ms / 1000.0
        return await _to_thread(self._impl.receive, timeout_sec)  # type: ignore[return-value]

    # ─── プロパティ ───────────────────────────────────────────────────

    @property
    def is_connected(self) -> bool:
        return self._impl.is_connected

    @property
    def pipe_name(self) -> str:
        return self._impl.pipe_name

    # ─── コンテキストマネージャ ───────────────────────────────────────

    async def __aenter__(self) -> "AsyncRpcPipeClient":
        return self

    async def __aexit__(self, *args: object) -> None:
        await self.close()


# ─── AsyncRpcPipeServer ──────────────────────────────────────────────

class AsyncRpcPipeServer:
    """
    非同期 RPC サーバー。コールバックを async 関数として登録できる。
    仕様: spec/F004 §3.2

    例::

        async def my_handler(msg: Message) -> Message:
            return Message(msg.as_bytes().upper())

        async with AsyncRpcPipeServer("my_pipe") as server:
            await server.listen()
            await server.accept(timeout_ms=10000)
            task = asyncio.create_task(server.serve_requests(my_handler))
            # ... 後で task.cancel(); await server.stop()
    """

    def __init__(self, pipe_name: str, buffer_size: int = 65536) -> None:
        self._impl = RpcPipeServer(pipe_name, buffer_size)
        self._stop_event = asyncio.Event() if _HAS_TO_THREAD else None
        self._serving = False

    # ─── ライフサイクル ───────────────────────────────────────────────

    async def listen(self) -> None:
        """パイプを作成して LISTEN 状態へ移行する。"""
        self._impl.listen()

    async def accept(self, timeout_ms: int = 30000) -> None:
        """クライアント接続を待機する。タイムアウト時は PipeTimeoutError。"""
        timeout_sec = timeout_ms / 1000.0
        await _to_thread(self._impl.accept, timeout_sec)

    async def stop(self) -> None:
        """serve_requests ループを停止する（シグナル送信のみ、完了は await server.close()）。"""
        self._impl.stop()

    async def close(self) -> None:
        """接続をクローズする（冪等）。stop() も内包する。"""
        self._impl.stop()
        self._impl.close()

    # ─── RPC サービスループ ───────────────────────────────────────────

    async def serve_requests(
        self,
        handler: Callable[[Message], Awaitable[Message]],
    ) -> None:
        """
        非同期ハンドラでリクエストを処理するサービスループ（ブロッキング）。
        asyncio.create_task() でバックグラウンド実行させることを想定する（仕様 §3.2）。

        内部実装: 同期 RpcPipeServer.serve_requests() をスレッドオフロードし、
        ハンドラ呼び出しは asyncio.run_coroutine_threadsafe() で
        呼び出し元イベントループへ委譲する（GIL・スレッド安全性を確保）。

        注意: このメソッド実行中に receive() / send() を直接呼んではならない（仕様 §10.2）。

        例::

            task = asyncio.create_task(server.serve_requests(my_handler))
            ...
            task.cancel()
            await server.stop()
        """
        loop = asyncio.get_event_loop()

        def sync_handler(msg: Message) -> Message:
            """
            バックグラウンドスレッドから asyncio ハンドラを呼ぶブリッジ関数。
            run_coroutine_threadsafe で呼び出し元ループへ委譲する。
            """
            fut = asyncio.run_coroutine_threadsafe(handler(msg), loop)
            return fut.result(timeout=30.0)

        self._serving = True
        try:
            # run_in_background=False: ブロッキング実行をスレッドオフロード
            await _to_thread(self._impl.serve_requests, sync_handler, False)
        except asyncio.CancelledError:
            raise
        finally:
            self._serving = False

    # ─── プロパティ ───────────────────────────────────────────────────

    @property
    def is_listening(self) -> bool:
        return self._impl.is_listening

    @property
    def is_connected(self) -> bool:
        return self._impl.is_connected

    @property
    def pipe_name(self) -> str:
        return self._impl.pipe_name

    # ─── コンテキストマネージャ ───────────────────────────────────────

    async def __aenter__(self) -> "AsyncRpcPipeServer":
        return self

    async def __aexit__(self, *args: object) -> None:
        await self.close()


# ─── serve_connections ───────────────────────────────────────────────

async def serve_connections(
    pipe_name: str,
    handler: Callable[["AsyncPipeServer"], Awaitable[None]],
    max_connections: int = 8,
    buffer_size: int = 65536,
    stop_event: Optional["asyncio.Event"] = None,
) -> None:
    """
    接続ごとに asyncio.create_task(handler(conn)) を起動する接続受付ループ。
    stop_event が set されるまで動き続ける（None の場合は task.cancel() で停止）。

    仕様: spec/F004 §3.5

    例::

        stop = asyncio.Event()

        async def on_connect(conn: AsyncPipeServer) -> None:
            msg = await conn.receive()
            await conn.send(Message(b"pong"))

        await pipeutil.aio.serve_connections(
            "my_pipe", on_connect, stop_event=stop
        )
    """
    active_tasks: set[asyncio.Task[None]] = set()

    async def _handle_conn(conn: AsyncPipeServer) -> None:
        """接続ハンドラを実行し、完了後に接続を確実にクローズする。"""
        try:
            await handler(conn)
        finally:
            await conn.close()

    def _on_task_done(task: "asyncio.Task[None]") -> None:
        """タスク完了時にアクティブセットから削除する。"""
        active_tasks.discard(task)

    try:
        while True:
            # stop_event が set されていたら受付を終了
            if stop_event is not None and stop_event.is_set():
                break

            # 同時接続数が上限に達したら少し待機
            if len(active_tasks) >= max_connections:
                await asyncio.sleep(0.01)
                continue

            conn = AsyncPipeServer(pipe_name, buffer_size)
            await conn.listen()

            try:
                # 短いタイムアウトで accept し、stop_event を定期確認（仕様 §3.3参照）
                await conn.accept(timeout_ms=200)
            except Exception as exc:
                await conn.close()
                exc_type = type(exc).__name__
                if exc_type in ("TimeoutError", "PipeTimeoutError"):
                    # タイムアウトは正常: ループを継続して stop_event を再確認
                    continue
                raise

            task: asyncio.Task[None] = asyncio.create_task(_handle_conn(conn))
            active_tasks.add(task)
            task.add_done_callback(_on_task_done)

    except asyncio.CancelledError:
        raise
    finally:
        # 残存タスクのキャンセル
        for task in list(active_tasks):
            task.cancel()
        if active_tasks:
            await asyncio.gather(*active_tasks, return_exceptions=True)
