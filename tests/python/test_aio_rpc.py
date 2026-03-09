# tests/python/test_aio_rpc.py
# AsyncRpcPipeClient / AsyncRpcPipeServer の RPC テスト
# 仕様: spec/F004 §3.2, §7.2

from __future__ import annotations

import asyncio
import threading

import pytest
import pipeutil
import pipeutil.aio as aio
from pipeutil import Message

from conftest import unique_pipe


# ─── ヘルパー: 同期サーバー（バックグラウンドスレッド）─────────────────

def _sync_rpc_echo_server(pipe_name: str, ready: threading.Event) -> None:
    """既存 RpcPipeServer を使い、リクエストをエコーするサーバー（同期版）。"""
    srv = pipeutil.RpcPipeServer(pipe_name)
    srv.listen()
    ready.set()
    try:
        srv.accept(timeout=10.0)
        srv.serve_requests(lambda msg: msg)  # エコー
    finally:
        srv.stop()
        srv.close()


def _make_sync_rpc_server(pipe_name: str) -> threading.Thread:
    """同期 RPC エコーサーバーをバックグラウンドスレッドで起動する。"""
    ready = threading.Event()
    t = threading.Thread(
        target=_sync_rpc_echo_server,
        args=(pipe_name, ready),
        daemon=True,
    )
    t.start()
    assert ready.wait(timeout=5.0), "RPC server did not start in time"
    return t


# ─── テスト ───────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_async_rpc_roundtrip() -> None:
    """AsyncRpcPipeClient の基本 send_request → response 確認。"""
    pipe = unique_pipe("aio_rpc_rt")
    srv_thread = _make_sync_rpc_server(pipe)

    async with aio.AsyncRpcPipeClient(pipe) as client:
        await client.connect(timeout_ms=3000)
        resp = await client.send_request(Message(b"hello_rpc"), timeout_ms=3000)
        assert bytes(resp) == b"hello_rpc"

    srv_thread.join(timeout=5.0)


@pytest.mark.asyncio
async def test_async_rpc_parallel_requests() -> None:
    """asyncio.gather で複数リクエストを並列 await できることを確認する。"""
    pipe = unique_pipe("aio_rpc_par")
    srv_thread = _make_sync_rpc_server(pipe)

    async with aio.AsyncRpcPipeClient(pipe) as client:
        await client.connect(timeout_ms=3000)

        payloads = [f"req_{i}".encode() for i in range(4)]
        results = await asyncio.gather(
            *[client.send_request(Message(p), timeout_ms=5000) for p in payloads]
        )
        for payload, result in zip(payloads, results):
            assert bytes(result) == payload

    srv_thread.join(timeout=5.0)


@pytest.mark.asyncio
async def test_async_rpc_server_async_handler() -> None:
    """AsyncRpcPipeServer が非同期ハンドラで RPC を処理できることを確認する。"""
    pipe = unique_pipe("aio_rpc_srv")

    async def handler(msg: Message) -> Message:
        # 非同期コンテキストでの処理（小さい遅延をシミュレート）
        await asyncio.sleep(0.001)
        return Message(bytes(msg).upper())

    server = aio.AsyncRpcPipeServer(pipe)
    await server.listen()

    serve_task: asyncio.Task[None] | None = None

    async def _accept_and_serve() -> None:
        await server.accept(timeout_ms=5000)
        nonlocal serve_task
        serve_task = asyncio.create_task(server.serve_requests(handler))
        await serve_task

    accept_task = asyncio.create_task(_accept_and_serve())
    await asyncio.sleep(0.05)  # listen/accept に達するまで待機

    async with aio.AsyncRpcPipeClient(pipe) as client:
        await client.connect(timeout_ms=3000)
        resp = await client.send_request(Message(b"hello"), timeout_ms=3000)
        assert bytes(resp) == b"HELLO"

    # サービスループを停止
    await server.stop()
    if serve_task is not None:
        serve_task.cancel()
        try:
            await serve_task
        except (asyncio.CancelledError, Exception):
            pass
    accept_task.cancel()
    try:
        await accept_task
    except (asyncio.CancelledError, Exception):
        pass
    await server.close()
