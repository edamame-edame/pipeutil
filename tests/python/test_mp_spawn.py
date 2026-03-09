# tests/python/test_mp_spawn.py
# WorkerPipeClient + multiprocessing.spawn テスト
# 仕様: spec/F004 §5.2, §5.3, §7.3

from __future__ import annotations

import multiprocessing
import sys

import pytest
import pipeutil
from pipeutil import Message
from pipeutil.mp import WorkerPipeClient

from conftest import unique_pipe


# ─── ワーカー関数（トップレベルでないと spawn で pickle できない）─────

def _echo_worker(pipe_name: str) -> None:
    """受信したメッセージをエコーして終了するワーカー。"""
    with WorkerPipeClient(pipe_name) as client:
        client.connect(timeout_ms=5000)
        msg = client.receive(timeout_ms=5000)
        client.send(Message(bytes(msg)))


def _uppercase_worker(pipe_name: str) -> None:
    """受信したメッセージを大文字にして返すワーカー。"""
    with WorkerPipeClient(pipe_name) as client:
        client.connect(timeout_ms=5000)
        msg = client.receive(timeout_ms=5000)
        client.send(Message(bytes(msg).upper()))


# ─── テスト ───────────────────────────────────────────────────────────

@pytest.mark.timeout(30)
def test_spawn_worker_echo() -> None:
    """spawn した子プロセスで WorkerPipeClient がメッセージを受信してエコーすることを確認する。"""
    pipe = unique_pipe("mp_spawn_echo")
    ctx = multiprocessing.get_context("spawn")

    srv = pipeutil.PipeServer(pipe)
    srv.listen()

    p = ctx.Process(target=_echo_worker, args=(pipe,))
    p.start()

    srv.accept(10000)
    srv.send(Message(b"hello_spawn"))
    resp = srv.receive(10000)

    p.join(timeout=15)

    assert p.exitcode == 0
    assert bytes(resp) == b"hello_spawn"

    srv.close()


@pytest.mark.timeout(30)
def test_spawn_worker_transform() -> None:
    """spawn した子プロセスでメッセージを変換して返すことを確認する（仕様 §7.3）。"""
    pipe = unique_pipe("mp_spawn_upper")
    ctx = multiprocessing.get_context("spawn")

    srv = pipeutil.PipeServer(pipe)
    srv.listen()

    p = ctx.Process(target=_uppercase_worker, args=(pipe,))
    p.start()

    srv.accept(10000)
    srv.send(Message(b"hello"))
    resp = srv.receive(10000)

    p.join(timeout=15)

    assert p.exitcode == 0
    assert bytes(resp) == b"HELLO"

    srv.close()


@pytest.mark.timeout(30)
def test_worker_pipe_client_context_manager() -> None:
    """WorkerPipeClient のコンテキストマネージャが確実にクローズすることを確認する。"""
    pipe = unique_pipe("mp_ctx")
    ctx = multiprocessing.get_context("spawn")

    srv = pipeutil.PipeServer(pipe)
    srv.listen()

    p = ctx.Process(target=_echo_worker, args=(pipe,))
    p.start()

    srv.accept(10000)
    srv.send(Message(b"ctx_test"))
    resp = srv.receive(10000)

    p.join(timeout=15)

    assert p.exitcode == 0, f"Worker exited with code {p.exitcode}"
    assert bytes(resp) == b"ctx_test"

    srv.close()
