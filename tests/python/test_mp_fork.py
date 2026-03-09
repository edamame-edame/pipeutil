# tests/python/test_mp_fork.py
# fork コンテキストのテスト（Linux のみ、仕様 §5.3 fork 安全パターン）
# 仕様: spec/F004 §5.3, §7.1

from __future__ import annotations

import sys

import pytest
import pipeutil
from pipeutil import Message

from conftest import unique_pipe


# fork は Linux / macOS のみ有効
pytestmark = pytest.mark.skipif(
    sys.platform == "win32",
    reason="fork context is not available on Windows",
)


def test_fork_warning() -> None:
    """forK コンテキスト指定時に RuntimeWarning が送出されることを確認する（仕様 §10.4）。"""
    from pipeutil.mp import _check_start_method
    import warnings

    with warnings.catch_warnings(record=True) as w:
        warnings.simplefilter("always")
        _check_start_method("fork")
        assert len(w) == 1
        assert issubclass(w[0].category, RuntimeWarning)
        assert "fork" in str(w[0].message).lower()


@pytest.mark.timeout(30)
def test_fork_safe_pattern() -> None:
    """
    fork 前に close → 子プロセスで新規接続 の安全パターンを確認する（仕様 §5.3）。

    NG パターン（同一ハンドル共有）は回避し、子プロセスで新規 PipeClient を作成する。
    """
    import os
    import multiprocessing

    pipe = unique_pipe("mp_fork_safe")

    srv = pipeutil.PipeServer(pipe)
    srv.listen()

    # fork 前に接続しておいて close するパターン（仕様のOKパターン）
    ctx = multiprocessing.get_context("fork")
    p = ctx.Process(
        target=_fork_worker,
        args=(pipe,),
    )
    p.start()

    srv.accept(5000)
    srv.send(Message(b"fork_test"))
    resp = srv.receive(5000)

    p.join(timeout=15)

    assert p.exitcode == 0, f"Worker exited with code {p.exitcode}"
    assert bytes(resp) == b"FORK_TEST"

    srv.close()


def _fork_worker(pipe_name: str) -> None:
    """fork 後に新規接続を確立するワーカー。"""
    # fork 後に新規 PipeClient を作成（子プロセス専任）
    client = pipeutil.PipeClient(pipe_name)
    client.connect(5000)
    msg = client.receive(5000)
    client.send(Message(bytes(msg).upper()))
    client.close()
