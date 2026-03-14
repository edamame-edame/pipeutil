# tests/python/test_rpc.py — RpcPipeClient / RpcPipeServer 統合テスト
# 仕様: spec/F002_rpc_message_id.md §10

import threading
import time
import pytest
import pipeutil
from conftest import unique_pipe


# ─── RPC サーバーヘルパー ─────────────────────────────────────────────────────

class RpcServerThread(threading.Thread):
    """
    背景スレッド上で RpcPipeServer を起動するヘルパー。

    usage:
        srv = RpcServerThread(pipe_name, handler=my_handler)
        srv.start()
        srv.wait_ready()    # accept 完了まで待機
        # ... クライアント操作 ...
        srv.shutdown()
        srv.join()
        srv.reraise()
    """

    def __init__(self, pipe_name: str, handler=None,
                 accept_timeout: float = 5.0,
                 serve_duration: float = 5.0):
        super().__init__(daemon=True)
        self.pipe_name = pipe_name
        self.handler = handler or (lambda req: req)  # デフォルト: エコー
        self.accept_timeout = accept_timeout
        self.serve_duration  = serve_duration
        self._ready = threading.Event()
        self._stop_ev = threading.Event()  # '_stop' は Python 3.8 Thread の内部メソッドと衝突するため回避
        self._exc: BaseException | None = None
        self._srv: pipeutil.RpcPipeServer | None = None

    def run(self):
        srv = pipeutil.RpcPipeServer(self.pipe_name)
        self._srv = srv
        try:
            srv.listen()
            self._ready.set()              # listen 後に set → クライアントが接続可能になる
            srv.accept(self.accept_timeout)
            srv.serve_requests(self.handler, run_in_background=True)
            # serve_duration 待ってから stop
            self._stop_ev.wait(self.serve_duration)
            srv.stop()
        except Exception as e:
            self._exc = e
            self._ready.set()  # エラーでもブロックしない
        finally:
            srv.close()

    def wait_ready(self, timeout: float = 5.0):
        assert self._ready.wait(timeout), "RpcServer did not start in time"

    def shutdown(self):
        self._stop_ev.set()

    def reraise(self):
        if self._exc:
            raise self._exc


# ─── テストクラス ─────────────────────────────────────────────────────────────

class TestRpc:

    # ─── TC-F002-PY-01: 単発 RPC ラウンドトリップ ─────────────────────────────
    def test_single_roundtrip(self):
        pipe_name = unique_pipe("rpc_single")

        def handler(req: pipeutil.Message) -> pipeutil.Message:
            return pipeutil.Message(b"pong:" + bytes(req))

        srv = RpcServerThread(pipe_name, handler=handler)
        srv.start()
        srv.wait_ready()

        cli = pipeutil.RpcPipeClient(pipe_name)
        try:
            cli.connect(3.0)
            resp = cli.send_request(pipeutil.Message(b"ping"), timeout=3.0)
            assert bytes(resp) == b"pong:ping"
        finally:
            cli.close()
            srv.shutdown()
            srv.join(timeout=5.0)
            srv.reraise()

    # ─── TC-F002-PY-02: 複数スレッドからの並行 send_request ──────────────────
    def test_concurrent_requests(self):
        pipe_name = unique_pipe("rpc_concurrent")
        N_THREADS  = 3
        N_REQUESTS = 5

        def handler(req: pipeutil.Message) -> pipeutil.Message:
            # "req:T:I" → "resp:T:I"
            s = bytes(req).decode()
            return pipeutil.Message(s.replace("req:", "resp:", 1).encode())

        srv = RpcServerThread(pipe_name, handler=handler, serve_duration=10.0)
        srv.start()
        srv.wait_ready()

        cli = pipeutil.RpcPipeClient(pipe_name)
        errors: list[str] = []
        lock = threading.Lock()

        def worker(t: int):
            for i in range(N_REQUESTS):
                req_b  = f"req:{t}:{i}".encode()
                exp_b  = f"resp:{t}:{i}".encode()
                resp = cli.send_request(pipeutil.Message(req_b), timeout=5.0)
                if bytes(resp) != exp_b:
                    with lock:
                        errors.append(
                            f"t={t} i={i}: got {bytes(resp)!r}, want {exp_b!r}")

        cli.connect(3.0)
        try:
            threads = [threading.Thread(target=worker, args=(t,)) for t in range(N_THREADS)]
            for th in threads: th.start()
            for th in threads: th.join(timeout=10.0)
        finally:
            cli.close()
            srv.shutdown()
            srv.join(timeout=5.0)
            srv.reraise()

        assert not errors, "\n".join(errors)

    # ─── TC-F002-PY-03: タイムアウト例外 ─────────────────────────────────────
    def test_timeout_raises(self):
        pipe_name = unique_pipe("rpc_timeout")

        # ハンドラが応答しない（time.sleep で遅延）
        def slow_handler(req: pipeutil.Message) -> pipeutil.Message:
            time.sleep(2.0)  # クライアントのタイムアウト (0.3s) より長いが合理的な範囲
            return pipeutil.Message(b"too_late")

        srv = RpcServerThread(pipe_name, handler=slow_handler, serve_duration=5.0)
        srv.start()
        srv.wait_ready()

        cli = pipeutil.RpcPipeClient(pipe_name)
        cli.connect(3.0)
        try:
            with pytest.raises(pipeutil.TimeoutError):
                cli.send_request(pipeutil.Message(b"fast_req"), timeout=0.3)
        finally:
            cli.close()
            srv.shutdown()
            srv.join(timeout=5.0)
            # サーバー側の例外は無視（slow_handler がブロックしている可能性）
