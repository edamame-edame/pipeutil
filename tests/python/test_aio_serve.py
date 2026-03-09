# tests/python/test_aio_serve.py
# serve_connections() の複数接続テスト
# 仕様: spec/F004 §3.5, §7.1

from __future__ import annotations

import asyncio

import pytest
import pipeutil
import pipeutil.aio as aio
from pipeutil import Message
from pipeutil.aio import serve_connections

from conftest import unique_pipe


@pytest.mark.asyncio
async def test_serve_connections_single() -> None:
    """serve_connections が1接続を正しくハンドラに渡すことを確認する。"""
    pipe = unique_pipe("aio_serve_s")
    received: list[bytes] = []
    stop = asyncio.Event()

    async def handler(conn: aio.AsyncPipeServer) -> None:
        msg = await conn.receive(timeout_ms=3000)
        received.append(bytes(msg))
        await conn.send(Message(b"pong"))
        stop.set()

    serve_task = asyncio.create_task(
        serve_connections(pipe, handler, stop_event=stop)
    )
    await asyncio.sleep(0.05)  # serve_connections が最初の listen に達するまで待機

    async with aio.AsyncPipeClient(pipe) as client:
        await client.connect(timeout_ms=3000)
        await client.send(Message(b"ping"))
        resp = await client.receive(timeout_ms=3000)
        assert bytes(resp) == b"pong"

    await asyncio.wait_for(serve_task, timeout=5.0)
    assert received == [b"ping"]


@pytest.mark.asyncio
async def test_serve_connections_stop_event() -> None:
    """stop_event.set() で serve_connections が停止することを確認する。"""
    pipe = unique_pipe("aio_serve_stop")
    stop = asyncio.Event()

    async def handler(conn: aio.AsyncPipeServer) -> None:
        await conn.receive(timeout_ms=3000)

    serve_task = asyncio.create_task(
        serve_connections(pipe, handler, stop_event=stop)
    )
    await asyncio.sleep(0.05)

    # stop_event を set することでループを終了させる
    stop.set()
    await asyncio.wait_for(serve_task, timeout=5.0)


@pytest.mark.asyncio
async def test_serve_connections_cancel() -> None:
    """task.cancel() で serve_connections が CancelledError で終了することを確認する。"""
    pipe = unique_pipe("aio_serve_cancel")

    async def handler(conn: aio.AsyncPipeServer) -> None:
        await conn.receive(timeout_ms=3000)

    serve_task = asyncio.create_task(
        serve_connections(pipe, handler)
    )
    await asyncio.sleep(0.05)

    serve_task.cancel()
    try:
        await asyncio.wait_for(serve_task, timeout=3.0)
    except asyncio.CancelledError:
        pass
    except asyncio.TimeoutError:
        pytest.fail("serve_connections did not stop after cancel")
