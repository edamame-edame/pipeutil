# tests/python/test_mp_process_server.py
# ProcessPipeServer のテスト
# 仕様: spec/F004 §5.2, §5.3

from __future__ import annotations

import time

import pytest
import pipeutil
from pipeutil import Message
from pipeutil.mp import ProcessPipeServer, WorkerPipeClient

from conftest import unique_pipe


# ─── ワーカー関数（トップレベル必須）────────────────────────────────

def _echo_process_worker(pipe_name: str) -> None:
    """ProcessPipeServer から spawn されるワーカー（エコー）。"""
    with WorkerPipeClient(pipe_name) as client:
        client.connect(timeout_ms=5000)
        msg = client.receive(timeout_ms=5000)
        client.send(Message(bytes(msg)))


def _set_event_worker(pipe_name: str) -> None:
    """
    ProcessPipeServer から spawn されるワーカー。
    接続をハンドシェイクして即終了する（active_workers > 0 を確認するためのマーカー）。
    """
    with WorkerPipeClient(pipe_name) as client:
        client.connect(timeout_ms=10000)


# ─── テスト ───────────────────────────────────────────────────────────

@pytest.mark.timeout(30)
def test_process_server_start_stop() -> None:
    """ProcessPipeServer の start() / stop() が正常に動作することを確認する。"""
    pipe = unique_pipe("pps_ss")

    server = ProcessPipeServer(
        pipe,
        worker_fn=_echo_process_worker,
        max_processes=2,
    )
    server.start()
    time.sleep(0.1)  # accept_loop が起動するまで待機
    # active_workers は 0 か 1（最初の spawn が既に開始している可能性がある）
    assert server.active_workers() >= 0
    server.stop()


@pytest.mark.timeout(60)
def test_process_server_spawns_worker() -> None:
    """
    ProcessPipeServer が子プロセスを spawn して接続を処理することを確認する。

    ハンドシェイクパイプパターン（仕様 §5.2 / §5.3）:
    1. ProcessPipeServer が "{pipe_base}_1" を listen
    2. 子プロセス（WorkerPipeClient）が接続
    3. accept が完了して active_workers が 1 以上になる
    """
    pipe_base = unique_pipe("pps_spawn")

    server = ProcessPipeServer(
        pipe_base,
        worker_fn=_set_event_worker,
        max_processes=2,
    )
    server.start()

    # accept が完了するまでポーリング（最大 20 秒）
    deadline = time.monotonic() + 20.0
    spawned = False
    while time.monotonic() < deadline:
        if server.active_workers() >= 1:
            spawned = True
            break
        time.sleep(0.1)

    server.stop()

    assert spawned, "ProcessPipeServer が 20 秒以内にワーカーを spawn できなかった"


@pytest.mark.timeout(30)
def test_check_start_method_warning() -> None:
    """fork コンテキスト指定時に RuntimeWarning が送出されることを確認する。"""
    import warnings
    import sys

    if sys.platform == "win32":
        pytest.skip("fork is not available on Windows")

    with warnings.catch_warnings(record=True) as w:
        warnings.simplefilter("always")
        ProcessPipeServer(
            unique_pipe("pps_warn"),
            worker_fn=_echo_process_worker,
            context="fork",
        )
    assert any(issubclass(x.category, RuntimeWarning) for x in w)
