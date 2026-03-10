# tests/python/test_aio_native.py
# _pipeutil_async ネイティブバックエンド専用テスト（テスト 1〜5, 7）
# 仕様: spec/F004p2_async_native.md §9

from __future__ import annotations

import asyncio

import pytest
import pipeutil.aio as _aio
from pipeutil import Message

from conftest import unique_pipe


# ─── skipif 条件: native バックエンドが未ビルドの場合はスキップ ───────
pytestmark = pytest.mark.skipif(
    not _aio.is_native(),
    reason="_pipeutil_async not built (run cmake -DPIPEUTIL_WITH_ASYNC=ON)",
)


# ─── ヘルパー ─────────────────────────────────────────────────────────

async def _echo_server(pipe_name: str, *, num_msgs: int = 1) -> None:
    """接続を受け付けて num_msgs 回エコーして終了するサーバーコルーチン。"""
    async with _aio.AsyncPipeServer(pipe_name) as server:
        await server.listen()
        await server.accept(timeout_ms=5000)
        try:
            for _ in range(num_msgs):
                msg = await server.receive(timeout_ms=5000)
                await server.send(msg)
        except Exception:
            # クライアント側が先に切断した場合等は無視
            pass


# ─── テスト 1: native backend 有効確認 ─────────────────────────────────

@pytest.mark.asyncio
async def test_native_available() -> None:
    """ネイティブバックエンドが有効であることを確認する。"""
    assert _aio.is_native(), "Native backend should be available when _pipeutil_async is built"


# ─── テスト 2: 基本ラウンドトリップ ────────────────────────────────────

@pytest.mark.asyncio
async def test_native_roundtrip() -> None:
    """native backend で接続・送受信（1往復）が正常に動作することを確認する。"""
    pipe = unique_pipe("n_rt")
    server_task = asyncio.create_task(_echo_server(pipe))
    await asyncio.sleep(0.05)

    async with _aio.AsyncPipeClient(pipe) as client:
        await client.connect(timeout_ms=3000)
        await client.send(Message(b"hello"))
        resp = await client.receive(timeout_ms=3000)
        assert bytes(resp) == b"hello"

    await server_task


# ─── テスト 3: receive 中の task.cancel() ──────────────────────────────

@pytest.mark.asyncio
async def test_native_cancel_read() -> None:
    """
    受信待ち中に task.cancel() すると CancelledError が返ることを確認する。
    native backend の真のキャンセル（CancelIoEx / remove_reader）を検証する。
    """
    pipe = unique_pipe("n_cancel")

    async with _aio.AsyncPipeServer(pipe) as server:
        await server.listen()
        accept_task = asyncio.create_task(server.accept(timeout_ms=5000))
        await asyncio.sleep(0.05)

        async with _aio.AsyncPipeClient(pipe) as client:
            await client.connect(timeout_ms=3000)
            await accept_task  # accept 完了を待つ

            # サーバーは何も送らない → クライアントはデフォルトタイムアウトまで待機
            receive_task = asyncio.create_task(client.receive(timeout_ms=5000))
            await asyncio.sleep(0.05)
            receive_task.cancel()

            with pytest.raises(asyncio.CancelledError):
                await receive_task


# ─── テスト 4: 大きなペイロード（1 MiB）──────────────────────────────

@pytest.mark.asyncio
async def test_native_large_payload() -> None:
    """1 MiB ペイロードが正しく送受信できることを確認する（チャンク分割対応）。"""
    pipe = unique_pipe("n_large")
    payload = b"z" * (1024 * 1024)  # 1 MiB
    server_task = asyncio.create_task(_echo_server(pipe))
    await asyncio.sleep(0.05)

    async with _aio.AsyncPipeClient(pipe) as client:
        await client.connect(timeout_ms=5000)
        await client.send(Message(payload))
        resp = await client.receive(timeout_ms=10000)
        assert bytes(resp) == payload

    await server_task


# ─── テスト 5: 並列 10 接続 ─────────────────────────────────────────

@pytest.mark.asyncio
async def test_native_concurrent() -> None:
    """10 並列クライアントから同一 serve_connections へ接続し全て正常応答を確認する。"""
    pipe = unique_pipe("n_concurrent")
    num_clients = 10
    results: list[bool] = []

    async def client_task(i: int) -> None:
        async with _aio.AsyncPipeClient(pipe) as client:
            await client.connect(timeout_ms=5000)
            payload = f"msg_{i}".encode()
            await client.send(Message(payload))
            resp = await client.receive(timeout_ms=5000)
            results.append(bytes(resp) == payload)

    async def on_connect(conn: _aio.AsyncPipeServer) -> None:
        msg = await conn.receive(timeout_ms=5000)
        await conn.send(msg)

    serve_task = asyncio.create_task(
        _aio.serve_connections(pipe, on_connect, max_connections=num_clients)
    )
    await asyncio.sleep(0.05)

    await asyncio.gather(*[client_task(i) for i in range(num_clients)])

    serve_task.cancel()
    try:
        await serve_task
    except asyncio.CancelledError:
        pass

    assert len(results) == num_clients
    assert all(results)


# ─── テスト 7: Phase 1 と同一 API の互換性確認 ─────────────────────────

@pytest.mark.asyncio
async def test_api_compatibility_native() -> None:
    """native backend で Phase 1 と完全に同一の公開 API が使えることを確認する。"""
    pipe = unique_pipe("n_compat")
    server_task = asyncio.create_task(_echo_server(pipe))
    await asyncio.sleep(0.05)

    # Phase 1 と完全同一の操作手順
    client = _aio.AsyncPipeClient(pipe)
    try:
        assert not client.is_connected
        assert client.pipe_name == pipe
        await client.connect(timeout_ms=3000)
        assert client.is_connected
        await client.send(Message(b"compat"))
        resp = await client.receive(timeout_ms=3000)
        assert bytes(resp) == b"compat"
    finally:
        await client.close()

    await server_task
