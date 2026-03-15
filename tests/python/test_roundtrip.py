# tests/python/test_roundtrip.py — PipeServer / PipeClient 統合テスト

import threading
import pytest
import pipeutil
from conftest import unique_pipe


class TestBasicRoundTrip:
    """クライアント → サーバー → クライアントの基本ラウンドトリップ。"""

    def test_client_to_server(self, make_server):
        pipe_name = unique_pipe("c2s")
        received: list[bytes] = []
        done = threading.Event()

        def handler(srv: pipeutil.PipeServer):
            msg = srv.receive(3000)
            received.append(bytes(msg))
            done.set()

        make_server(pipe_name, handler=handler)

        cli = pipeutil.PipeClient(pipe_name)
        cli.connect(3000)
        cli.send(pipeutil.Message(b"hello"))
        cli.close()

        assert done.wait(3.0), "server handler did not complete"
        assert received == [b"hello"]

    def test_server_to_client(self, make_server):
        pipe_name = unique_pipe("s2c")

        def handler(srv: pipeutil.PipeServer):
            srv.send(pipeutil.Message(b"world"))

        make_server(pipe_name, handler=handler)

        cli = pipeutil.PipeClient(pipe_name)
        cli.connect(3000)
        msg = cli.receive(3000)
        cli.close()

        assert bytes(msg) == b"world"

    def test_echo_server(self, make_server):
        """デフォルトのエコーサーバーで送受信が一致すること。"""
        pipe_name = unique_pipe("echo")
        make_server(pipe_name)  # handler=None → エコー動作

        cli = pipeutil.PipeClient(pipe_name)
        cli.connect(3000)
        cli.send(pipeutil.Message(b"echo_test"))
        msg = cli.receive(3000)
        cli.close()

        assert bytes(msg) == b"echo_test"


class TestMultipleMessages:
    """複数メッセージの連続送受信。"""

    def test_sequential_50_messages(self, make_server):
        pipe_name = unique_pipe("multi50")
        count = 50
        received: list[bytes] = []
        done = threading.Event()

        def handler(srv: pipeutil.PipeServer):
            for _ in range(count):
                msg = srv.receive(3000)
                received.append(bytes(msg))
            done.set()

        make_server(pipe_name, handler=handler)

        cli = pipeutil.PipeClient(pipe_name)
        cli.connect(3000)
        for i in range(count):
            cli.send(pipeutil.Message(f"msg_{i}".encode()))
        cli.close()

        assert done.wait(5.0), "server handler did not complete"
        assert len(received) == count
        for i, data in enumerate(received):
            assert data == f"msg_{i}".encode()


class TestLargePayload:
    """大きなペイロードの転送。"""

    def test_1mib_payload(self, make_server):
        pipe_name = unique_pipe("large1m")
        payload = bytes([0xCD] * (1024 * 1024))  # 1 MiB
        received: list[bytes] = []
        done = threading.Event()

        def handler(srv: pipeutil.PipeServer):
            msg = srv.receive(10000)
            received.append(bytes(msg))
            done.set()

        make_server(pipe_name, handler=handler)

        cli = pipeutil.PipeClient(pipe_name)
        cli.connect(3000)
        cli.send(pipeutil.Message(payload))
        cli.close()

        assert done.wait(10.0), "server handler did not complete"
        assert len(received) == 1
        assert received[0] == payload


class TestConnectionErrors:
    """接続前の操作はエラーになること。"""

    def test_send_without_connect_raises(self):
        cli = pipeutil.PipeClient(unique_pipe("noconn_send"))
        with pytest.raises(pipeutil.PipeError):
            cli.send(pipeutil.Message(b"x"))

    def test_receive_without_connect_raises(self):
        cli = pipeutil.PipeClient(unique_pipe("noconn_recv"))
        with pytest.raises(pipeutil.PipeError):
            cli.receive(100)
