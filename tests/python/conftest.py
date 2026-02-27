# tests/python/conftest.py — pytest 共通フィクスチャ

import threading
import time
import pytest
import pipeutil

# ─── パイプ名のユニーク生成 ───────────────────────────────────────────

_counter = 0
_counter_lock = threading.Lock()

def unique_pipe(prefix: str = "test") -> str:
    """並列テスト実行時の競合を防ぐためスレッドセーフな連番を付与する。"""
    global _counter
    with _counter_lock:
        _counter += 1
        return f"pipeutil_pytest_{prefix}_{_counter}"


# ─── サーバー起動ヘルパー ─────────────────────────────────────────────

class ServerThread(threading.Thread):
    """
    バックグラウンドでサーバーを動かすヘルパースレッド。

    usage:
        srv = ServerThread(pipe_name)
        srv.start()
        srv.wait_listening()   # サーバーが listen 状態になるまで待機
        # ... クライアント操作 ...
        srv.join()
        srv.reraise()          # サーバー側の例外を再送出
    """

    def __init__(self, pipe_name: str, handler=None, timeout_ms: int = 5000):
        super().__init__(daemon=True)
        self.pipe_name = pipe_name
        self.handler = handler  # callable(server) or None → シンプルなエコー
        self.timeout_ms = timeout_ms
        self._ready = threading.Event()
        self._exc: BaseException | None = None

    def run(self):
        srv = pipeutil.PipeServer(self.pipe_name)
        try:
            srv.listen()
            self._ready.set()
            srv.accept(self.timeout_ms)
            if self.handler:
                self.handler(srv)
            else:
                # デフォルト: 1 メッセージ受信してエコー返送
                msg = srv.receive(self.timeout_ms)
                srv.send(msg)
        except Exception as e:
            self._exc = e
        finally:
            srv.close()

    def wait_listening(self, timeout: float = 3.0):
        assert self._ready.wait(timeout), "Server did not start in time"

    def reraise(self):
        if self._exc:
            raise self._exc


@pytest.fixture
def make_server():
    """ServerThread ファクトリフィクスチャ。テスト後に join する。"""
    threads: list[ServerThread] = []

    def factory(pipe_name: str, handler=None, timeout_ms: int = 5000) -> ServerThread:
        t = ServerThread(pipe_name, handler=handler, timeout_ms=timeout_ms)
        threads.append(t)
        t.start()
        t.wait_listening()
        return t

    yield factory

    for t in threads:
        t.join(timeout=5.0)
