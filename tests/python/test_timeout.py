# tests/python/test_timeout.py — タイムアウト契約のテスト（R-015 回帰）

import threading
import time
import pytest
import pipeutil
from conftest import unique_pipe


class TestReceiveTimeout:
    """receive() のタイムアウト動作の検証。"""

    def test_receive_timeout_raises_timeout_error(self, make_server):
        """サーバーが何も送らない状態で receive() するとタイムアウト例外が発生する。"""
        pipe_name = unique_pipe("rx_timeout")

        def handler(srv: pipeutil.PipeServer):
            # 何も送らず待機
            time.sleep(2.0)

        make_server(pipe_name, handler=handler)

        cli = pipeutil.PipeClient(pipe_name)
        cli.connect(3000)

        start = time.monotonic()
        with pytest.raises(pipeutil.TimeoutError):
            cli.receive(200)  # 200 ms タイムアウト
        elapsed = time.monotonic() - start

        cli.close()
        # タイムアウト時間前後（余裕を持って 1 秒以内）に例外が発生すること（R-015）
        assert elapsed < 1.0, f"Timeout took too long: {elapsed:.2f}s"

    def test_receive_timeout_is_not_instant(self, make_server):
        """タイムアウトは即座に発生せず、指定時間まで待機するべき。"""
        pipe_name = unique_pipe("rx_not_instant")

        def handler(srv: pipeutil.PipeServer):
            time.sleep(2.0)

        make_server(pipe_name, handler=handler)

        cli = pipeutil.PipeClient(pipe_name)
        cli.connect(3000)

        start = time.monotonic()
        with pytest.raises(pipeutil.TimeoutError):
            cli.receive(300)  # 300 ms 待つ
        elapsed = time.monotonic() - start

        cli.close()
        # 最低でも 200 ms 待ったことを確認（即座に例外にならないこと）
        assert elapsed >= 0.2, f"Timeout resolved too early: {elapsed:.2f}s"


class TestConnectTimeout:
    """connect() のタイムアウト動作の検証。"""

    def test_connect_to_nonexistent_pipe_raises_timeout(self):
        """存在しないパイプへの connect() はタイムアウトで失敗すること。"""
        cli = pipeutil.PipeClient(unique_pipe("nonexistent_9999"))
        with pytest.raises(pipeutil.PipeError):
            cli.connect(200)  # 200 ms タイムアウト


class TestAcceptTimeout:
    """accept() のタイムアウト動作の検証。"""

    def test_accept_with_no_client_raises_timeout(self):
        """クライアントが来ない状態で accept() するとタイムアウト例外が発生する。"""
        pipe_name = unique_pipe("accept_timeout")
        srv = pipeutil.PipeServer(pipe_name)
        srv.listen()

        with pytest.raises(pipeutil.TimeoutError):
            srv.accept(200)  # 200 ms タイムアウト

        srv.close()
