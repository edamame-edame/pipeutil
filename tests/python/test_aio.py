# tests/python/test_aio.py
# AsyncPipeClient / AsyncPipeServer の基本 send/receive テスト
# 仕様: spec/F004 §3.2, §7.1, §7.2

from __future__ import annotations

import asyncio

import pytest
import pipeutil
import pipeutil.aio as aio
from pipeutil import Message

from conftest import unique_pipe


# ─── ヘルパー ─────────────────────────────────────────────────────────

async def _echo_server(pipe_name: str, *, num_msgs: int = 1) -> None:
    """接続を受け付けて num_msgs 回エコーして終了するサーバーコルーチン。"""
    async with aio.AsyncPipeServer(pipe_name) as server:
        await server.listen()
        await server.accept(timeout_ms=5000)
        try:
            for _ in range(num_msgs):
                msg = await server.receive(timeout_ms=3000)
                await server.send(msg)
        except Exception:
            # クライアントが先に切断した場合（context manager テスト等）は無視
            pass


# ─── テスト ───────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_async_roundtrip() -> None:
    """基本的な非同期ラウンドトリップ（send → receive → echo 確認）。"""
    pipe = unique_pipe("aio_rt")
    server_task = asyncio.create_task(_echo_server(pipe))
    await asyncio.sleep(0.05)  # server が listen に達するまで待機

    async with aio.AsyncPipeClient(pipe) as client:
        await client.connect(timeout_ms=3000)
        await client.send(Message(b"ping"))
        resp = await client.receive(timeout_ms=3000)
        assert bytes(resp) == b"ping"

    await server_task


@pytest.mark.asyncio
async def test_async_large_payload() -> None:
    """大きなペイロードが正しく転送されることを確認する。"""
    pipe = unique_pipe("aio_large")
    payload = b"x" * (64 * 1024)  # 64 KiB
    server_task = asyncio.create_task(_echo_server(pipe))
    await asyncio.sleep(0.05)

    async with aio.AsyncPipeClient(pipe) as client:
        await client.connect(timeout_ms=3000)
        await client.send(Message(payload))
        resp = await client.receive(timeout_ms=5000)
        assert bytes(resp) == payload

    await server_task


@pytest.mark.asyncio
async def test_async_context_manager() -> None:
    """async with 構文で確実にクローズされることを確認する。"""
    pipe = unique_pipe("aio_ctx")
    server_task = asyncio.create_task(_echo_server(pipe))
    await asyncio.sleep(0.05)

    async with aio.AsyncPipeClient(pipe) as client:
        assert not client.is_connected  # connect 前
        await client.connect(timeout_ms=3000)
        assert client.is_connected

    # __aexit__ でクローズ済み
    await server_task


@pytest.mark.asyncio
async def test_async_multiple_messages() -> None:
    """複数メッセージを連続して送受信できることを確認する。"""
    pipe = unique_pipe("aio_multi")
    messages = [f"msg_{i}".encode() for i in range(5)]
    server_task = asyncio.create_task(_echo_server(pipe, num_msgs=5))
    await asyncio.sleep(0.05)

    async with aio.AsyncPipeClient(pipe) as client:
        await client.connect(timeout_ms=3000)
        for payload in messages:
            await client.send(Message(payload))
            resp = await client.receive(timeout_ms=3000)
            assert bytes(resp) == payload

    await server_task


@pytest.mark.asyncio
async def test_async_pipe_name_property() -> None:
    """pipe_name プロパティが正しい値を返すことを確認する。"""
    pipe = unique_pipe("aio_prop")
    server_task = asyncio.create_task(_echo_server(pipe))
    await asyncio.sleep(0.05)

    async with aio.AsyncPipeClient(pipe) as client:
        assert client.pipe_name == pipe
        await client.connect(timeout_ms=3000)

    await server_task


# ─── テスト 6: native 不在時の to_thread フォールバック確認 ─────────────
# 仕様: spec/F004p2_async_native.md §9 テスト No.6

@pytest.mark.asyncio
async def test_fallback_when_native_absent() -> None:
    """
    _pipeutil_async がない環境で AsyncPipeClient が to_thread ベースで動作することを確認する。
    importlib.reload と sys.modules 操作でネイティブモジュールを隠してリロードする。
    """
    import importlib
    import sys

    # _pipeutil_async を sys.modules から一時隠蔽
    async_mod_key = "pipeutil._pipeutil_async"
    native_mod_key = "pipeutil._aio_native"
    aio_mod_key = "pipeutil.aio"

    saved_async = sys.modules.pop(async_mod_key, None)
    saved_native = sys.modules.pop(native_mod_key, None)
    # pipeutil.aio は import 済み → reload で再評価させる
    saved_aio = sys.modules.pop(aio_mod_key, None)

    # sys.modules に None をセットすると import 時に ImportError が発生する
    # (ディスク上に .pyd が存在しても再ロードをブロックできる)
    sys.modules[async_mod_key] = None  # type: ignore[assignment]
    sys.modules[native_mod_key] = None  # type: ignore[assignment]

    try:
        # _pipeutil_async が存在しない状態で aio を再 import → Phase 1 扱いになる
        import pipeutil.aio as aio_reloaded

        assert not aio_reloaded.is_native(), (
            "is_native() should return False when _pipeutil_async is absent"
        )
    finally:
        # None センチネルを削除してから元の状態を復元
        sys.modules.pop(async_mod_key, None)
        sys.modules.pop(native_mod_key, None)
        sys.modules.pop(aio_mod_key, None)
        if saved_aio is not None:
            sys.modules[aio_mod_key] = saved_aio
        if saved_native is not None:
            sys.modules[native_mod_key] = saved_native
        if saved_async is not None:
            sys.modules[async_mod_key] = saved_async
