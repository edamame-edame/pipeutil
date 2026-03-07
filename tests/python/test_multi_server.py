# tests/python/test_multi_server.py — MultiPipeServer 統合テスト

import threading
import time
import pytest
import pipeutil
from conftest import unique_pipe


class TestMultiPipeServerBasic:
    """MultiPipeServer の基本動作テスト。"""

    def test_start_stop(self):
        """serve() が起動し、stop() で正常終了すること。"""
        pipe_name = unique_pipe("mps_start")
        srv = pipeutil.MultiPipeServer(pipe_name, max_connections=2)

        t = threading.Thread(
            target=srv.serve,
            args=[lambda conn: None],
            daemon=True,
        )
        t.start()
        time.sleep(0.1)  # 起動待ち

        assert srv.is_serving

        srv.stop()
        t.join(timeout=3.0)
        assert not t.is_alive(), "serve() thread did not terminate"
        assert not srv.is_serving

    def test_single_client_echo(self):
        """1 クライアントのエコー通信が正常に完了すること。"""
        pipe_name = unique_pipe("mps_echo")
        srv = pipeutil.MultiPipeServer(pipe_name, max_connections=2)
        received: list[bytes] = []

        def handler(conn: pipeutil.PipeServer) -> None:
            msg = conn.receive(3000)
            conn.send(msg)
            received.append(bytes(msg))

        t = threading.Thread(target=srv.serve, args=[handler], daemon=True)
        t.start()
        time.sleep(0.1)

        cli = pipeutil.PipeClient(pipe_name)
        cli.connect(3000)
        cli.send(pipeutil.Message(b"hello_multi"))
        reply = cli.receive(3000)
        cli.close()

        # ハンドラ完了まで待つ
        for _ in range(50):
            if received:
                break
            time.sleep(0.02)

        srv.stop()
        t.join(timeout=3.0)

        assert received == [b"hello_multi"]
        assert bytes(reply) == b"hello_multi"

    def test_multiple_clients_parallel(self):
        """複数クライアントが同時接続してそれぞれ独立したエコーを受け取ること。"""
        pipe_name = unique_pipe("mps_parallel")
        N = 3
        srv = pipeutil.MultiPipeServer(pipe_name, max_connections=N)
        results: dict[str, str] = {}
        lock = threading.Lock()

        def handler(conn: pipeutil.PipeServer) -> None:
            msg = conn.receive(3000)
            payload = msg.text  # str として取得（.text プロパティ = UTF-8 decode）
            conn.send(msg)
            with lock:
                results[payload] = payload

        t = threading.Thread(target=srv.serve, args=[handler], daemon=True)
        t.start()
        time.sleep(0.1)

        def client_task(idx: int) -> str:
            cli = pipeutil.PipeClient(pipe_name)
            cli.connect(3000)
            payload = f"msg{idx}"
            cli.send(pipeutil.Message(payload.encode()))
            reply = cli.receive(3000)
            cli.close()
            return bytes(reply).decode()

        with_threads = [
            threading.Thread(target=lambda i=i: client_task(i))
            for i in range(N)
        ]
        for th in with_threads:
            th.start()
        for th in with_threads:
            th.join(timeout=5.0)

        # 全ハンドラ完了待ち
        for _ in range(50):
            with lock:
                if len(results) >= N:
                    break
            time.sleep(0.02)

        srv.stop()
        t.join(timeout=3.0)

        assert len(results) == N
