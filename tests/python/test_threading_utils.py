# tests/python/test_threading_utils.py
# ThreadedPipeClient / ThreadedPipeServer の concurrent.futures テスト
# 仕様: spec/F004 §4.2, §7.1

from __future__ import annotations

import concurrent.futures
import threading
import time

import pytest
import pipeutil
from pipeutil import Message
from pipeutil.threading_utils import ThreadedPipeClient, ThreadedPipeServer

from conftest import unique_pipe


# ─── テスト ───────────────────────────────────────────────────────────

# NOTE: クラスにまとめず関数ベースのテストにする（pytest-asyncio との干渉回避）

def test_threaded_client_send_receive() -> None:
    """ThreadedPipeClient の send_async / receive_async が Future を返すことを確認する。"""
    pipe = unique_pipe("thr_cli_rt")

    # バックグラウンドエコーサーバーを立てる
    server_ready = threading.Event()
    received_payload: list[bytes] = []

    def _server() -> None:
        srv = pipeutil.PipeServer(pipe)
        srv.listen()
        server_ready.set()
        srv.accept(5000)
        msg = srv.receive(3000)
        received_payload.append(bytes(msg))
        srv.send(msg)  # エコー
        srv.close()

    t = threading.Thread(target=_server, daemon=True)
    t.start()
    assert server_ready.wait(timeout=5.0)

    with ThreadedPipeClient(pipe) as client:
        client.connect(timeout_ms=3000)
        send_fut = client.send_async(Message(b"threaded"))
        assert send_fut.result(timeout=5.0) is None

        recv_fut = client.receive_async(timeout_ms=3000)
        msg = recv_fut.result(timeout=5.0)
        assert bytes(msg) == b"threaded"

    t.join(timeout=5.0)
    assert received_payload == [b"threaded"]


def test_threaded_client_as_completed_pattern() -> None:
    """as_completed パターンで複数 send_async を使えることを確認する。（仕様 §4.4）"""
    pipe = unique_pipe("thr_cli_ac")

    server_ready = threading.Event()

    def _echo_server() -> None:
        srv = pipeutil.PipeServer(pipe)
        srv.listen()
        server_ready.set()
        srv.accept(5000)
        # 3メッセージを順番にエコー
        for _ in range(3):
            msg = srv.receive(3000)
            srv.send(msg)
        srv.close()

    t = threading.Thread(target=_echo_server, daemon=True)
    t.start()
    assert server_ready.wait(timeout=5.0)

    with ThreadedPipeClient(pipe) as client:
        client.connect(timeout_ms=3000)
        payloads = [b"a", b"b", b"c"]
        send_futures = [client.send_async(Message(p)) for p in payloads]
        # 送信完了を確認
        for fut in concurrent.futures.as_completed(send_futures, timeout=5.0):
            assert fut.result() is None

        # 受信は1件ずつ（同時に複数の receive は非推奨）
        received = []
        for _ in range(3):
            recv_fut = client.receive_async(timeout_ms=3000)
            received.append(bytes(recv_fut.result(timeout=5.0)))
        assert sorted(received) == sorted(payloads)

    t.join(timeout=5.0)


def test_threaded_client_pipe_name_property() -> None:
    """pipe_name プロパティが正しい値を返すことを確認する。"""
    pipe = unique_pipe("thr_prop")
    client = ThreadedPipeClient(pipe)
    assert client.pipe_name == pipe
    client.close()


def test_threaded_client_context_manager_closes() -> None:
    """with 構文でクローズされることを確認する（例外なし）。"""
    pipe = unique_pipe("thr_ctx")
    with ThreadedPipeClient(pipe):
        pass  # close() が呼ばれることを確認


def test_threaded_server_accept_future() -> None:
    """ThreadedPipeServer の accept_async が Future を返すことを確認する。"""
    pipe = unique_pipe("thr_srv_acc")

    with ThreadedPipeServer(pipe) as server:
        server.listen()
        accept_fut = server.accept_async(timeout_ms=5000)

        # バックグラウンドでクライアント接続
        def _connect() -> None:
            time.sleep(0.05)
            c = pipeutil.PipeClient(pipe)
            c.connect(3000)
            c.close()

        t = threading.Thread(target=_connect, daemon=True)
        t.start()

        assert accept_fut.result(timeout=5.0) is None  # 接続完了

        t.join(timeout=5.0)


def test_threaded_server_io() -> None:
    """ThreadedPipeServer の send_async / receive_async が Future を返すことを確認する。"""
    pipe = unique_pipe("thr_srv_io")
    server_ready = threading.Event()

    with ThreadedPipeServer(pipe) as server:
        server.listen()
        server_ready.set()

        def _client() -> None:
            server_ready.wait(timeout=5.0)
            c = pipeutil.PipeClient(pipe)
            c.connect(3000)
            c.send(Message(b"from_client"))
            msg = c.receive(3000)
            assert bytes(msg) == b"from_server"
            c.close()

        t = threading.Thread(target=_client, daemon=True)
        t.start()

        accept_fut = server.accept_async(timeout_ms=5000)
        accept_fut.result(timeout=5.0)

        recv_fut = server.receive_async(timeout_ms=3000)
        msg = recv_fut.result(timeout=5.0)
        assert bytes(msg) == b"from_client"

        send_fut = server.send_async(Message(b"from_server"))
        send_fut.result(timeout=5.0)

        t.join(timeout=5.0)
