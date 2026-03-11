# tests/python/test_stats.py — F-006 診断・メトリクス API Python テスト
# 仕様: spec/F006_diagnostics_metrics.md §10.2
#
# T1  test_stats_returns_pipe_stats       — stats() が PipeStats 型を返す
# T2  test_stats_initial                  — 接続直後の全フィールドが 0
# T3  test_messages_sent_increments       — send() 後に messages_sent が 1 増える
# T4  test_bytes_sent_increments          — 100 バイト送信後に bytes_sent >= 100
# T5  test_messages_received_increments   — receive() 後に messages_received が 1 増える
# T6  test_errors_on_broken_pipe          — 失敗時に errors が 1 増える
# T7  test_reset_stats                    — reset_stats() 後に全フィールドが 0
# T8  test_rpc_calls_increment            — send_request() 後に rpc_calls が 1 増える
# T9  test_rtt_last_ns_nonzero            — send_request() 後に rtt_last_ns > 0
# T10 test_avg_round_trip_zero_when_no_calls — rpc_calls == 0 なら avg_round_trip_ns == 0
# T11 test_avg_round_trip_equals_total_div_calls — avg == total // calls
# T12 test_pipe_client_rpc_fields_always_zero    — PipeClient の RPC フィールドは常に 0
# T13 test_stats_snapshot_independence    — stats() 取得後の送信はスナップショットに影響しない
# T14 test_pipe_stats_repr               — repr(stats) が "PipeStats(" で始まる
# T15 test_server_stats                  — PipeServer.stats()/reset_stats() が呼び出せる

import threading
import time
import pytest
import pipeutil
from conftest import unique_pipe, ServerThread


# ─── RPC サーバーヘルパー ─────────────────────────────────────────────

class EchoRpcServer(threading.Thread):
    def __init__(self, pipe_name: str):
        super().__init__(daemon=True)
        self.pipe_name = pipe_name
        self._ready = threading.Event()
        self._stop = threading.Event()
        self._srv: pipeutil.RpcPipeServer | None = None

    def run(self):
        srv = pipeutil.RpcPipeServer(self.pipe_name)
        self._srv = srv
        srv.listen()
        self._ready.set()
        srv.accept(5.0)
        srv.serve_requests(lambda req: req, run_in_background=True)
        self._stop.wait(10.0)
        srv.stop()
        srv.close()

    def wait_ready(self, timeout: float = 3.0):
        assert self._ready.wait(timeout), "RpcPipeServer did not start"

    def shutdown(self):
        self._stop.set()
        self.join(timeout=5.0)


# ─── T1: stats() が PipeStats 型を返す ────────────────────────────────

class TestStatsType:
    def test_stats_returns_pipe_stats(self):
        cli = pipeutil.PipeClient(unique_pipe("type"))
        s = cli.stats()
        assert isinstance(s, pipeutil.PipeStats)


# ─── T2: 接続直後の全フィールドが 0 ─────────────────────────────────

class TestStatsInitial:
    def test_stats_initial(self):
        cli = pipeutil.PipeClient(unique_pipe("init"))
        s = cli.stats()
        assert s.messages_sent     == 0
        assert s.messages_received == 0
        assert s.bytes_sent        == 0
        assert s.bytes_received    == 0
        assert s.errors            == 0
        assert s.rpc_calls         == 0
        assert s.rtt_total_ns      == 0
        assert s.rtt_last_ns       == 0
        assert s.avg_round_trip_ns == 0


# ─── T3: send() 後に messages_sent が 1 増える ────────────────────────

class TestStatsSend:
    def test_messages_sent_increments(self, make_server):
        pipe = unique_pipe("sent_inc")
        make_server(pipe)

        cli = pipeutil.PipeClient(pipe)
        cli.connect(3.0)
        cli.send(pipeutil.Message(b"hello"))

        s = cli.stats()
        assert s.messages_sent == 1
        cli.close()


# ─── T4: 100 バイト送信後に bytes_sent >= 100 ────────────────────────

class TestBytesSent:
    def test_bytes_sent_increments(self, make_server):
        pipe = unique_pipe("bytes_sent")
        payload = b"x" * 100
        make_server(pipe)

        cli = pipeutil.PipeClient(pipe)
        cli.connect(3.0)
        cli.send(pipeutil.Message(payload))

        s = cli.stats()
        assert s.bytes_sent >= 100
        cli.close()


# ─── T5: receive() 後に messages_received が 1 増える ────────────────

class TestStatsReceive:
    def test_messages_received_increments(self):
        pipe = unique_pipe("recv_inc")
        done = threading.Event()

        def handler(srv: pipeutil.PipeServer):
            srv.send(pipeutil.Message(b"data"))
            done.wait(5.0)

        t = ServerThread(pipe, handler=handler)
        t.start()
        t.wait_listening()

        cli = pipeutil.PipeClient(pipe)
        cli.connect(3.0)
        cli.receive(3.0)

        s = cli.stats()
        assert s.messages_received == 1
        done.set()
        cli.close()
        t.join(timeout=3.0)


# ─── T6: 送信失敗時に errors が 1 増える ─────────────────────────────

class TestStatsErrors:
    def test_errors_on_broken_pipe(self):
        pipe = unique_pipe("errors")
        cli = pipeutil.PipeClient(pipe)
        # 接続なしで send() → PipeError
        with pytest.raises(pipeutil.PipeError):
            cli.send(pipeutil.Message(b"x"))
        s = cli.stats()
        assert s.errors >= 1


# ─── T7: reset_stats() 後に全フィールドが 0 ─────────────────────────

class TestStatsReset:
    def test_reset_stats(self, make_server):
        pipe = unique_pipe("reset")
        make_server(pipe)

        cli = pipeutil.PipeClient(pipe)
        cli.connect(3.0)
        cli.send(pipeutil.Message(b"stuff"))
        cli.reset_stats()

        s = cli.stats()
        assert s.messages_sent     == 0
        assert s.messages_received == 0
        assert s.bytes_sent        == 0
        assert s.bytes_received    == 0
        assert s.errors            == 0
        cli.close()


# ─── T8: send_request() 後に rpc_calls が 1 増える ───────────────────

class TestRpcCallsIncrement:
    def test_rpc_calls_increment(self):
        pipe = unique_pipe("rpc_calls")
        srv = EchoRpcServer(pipe)
        srv.start()
        srv.wait_ready()

        cli = pipeutil.RpcPipeClient(pipe)
        cli.connect(3.0)
        cli.send_request(pipeutil.Message(b"ping"))

        s = cli.stats()
        assert s.rpc_calls == 1
        cli.close()
        srv.shutdown()


# ─── T9: send_request() 後に rtt_last_ns > 0 ─────────────────────────

class TestRttNonzero:
    def test_rtt_last_ns_nonzero(self):
        pipe = unique_pipe("rtt_ns")
        srv = EchoRpcServer(pipe)
        srv.start()
        srv.wait_ready()

        cli = pipeutil.RpcPipeClient(pipe)
        cli.connect(3.0)
        cli.send_request(pipeutil.Message(b"ping"))

        s = cli.stats()
        assert s.rtt_last_ns > 0
        cli.close()
        srv.shutdown()


# ─── T10: rpc_calls == 0 の場合 avg_round_trip_ns == 0 ───────────────

class TestAvgRttZeroWhenNoCalls:
    def test_avg_round_trip_zero_when_no_calls(self):
        cli = pipeutil.RpcPipeClient(unique_pipe("avg_zero"))
        s = cli.stats()
        assert s.rpc_calls         == 0
        assert s.avg_round_trip_ns == 0


# ─── T11: avg_round_trip_ns == rtt_total_ns // rpc_calls ─────────────

class TestAvgRttFormula:
    def test_avg_round_trip_equals_total_div_calls(self):
        pipe = unique_pipe("avg_formula")
        srv = EchoRpcServer(pipe)
        srv.start()
        srv.wait_ready()

        cli = pipeutil.RpcPipeClient(pipe)
        cli.connect(3.0)
        for _ in range(3):
            cli.send_request(pipeutil.Message(b"x"))

        s = cli.stats()
        assert s.rpc_calls > 0
        expected_avg = s.rtt_total_ns // s.rpc_calls
        assert s.avg_round_trip_ns == expected_avg
        cli.close()
        srv.shutdown()


# ─── T12: PipeClient の RPC フィールドは常に 0 ────────────────────────

class TestPipeClientRpcFieldsZero:
    def test_pipe_client_rpc_fields_always_zero(self, make_server):
        pipe = unique_pipe("rpc_zero")
        make_server(pipe)

        cli = pipeutil.PipeClient(pipe)
        cli.connect(3.0)
        cli.send(pipeutil.Message(b"hi"))

        s = cli.stats()
        assert s.rpc_calls    == 0
        assert s.rtt_total_ns == 0
        assert s.rtt_last_ns  == 0
        cli.close()


# ─── T13: stats() スナップショットの独立性 ────────────────────────────

class TestStatsSnapshotIndependence:
    def test_stats_snapshot_independence(self, make_server):
        pipe = unique_pipe("snapshot")
        # サーバー: 2 メッセージ受信
        received = []

        def handler(srv: pipeutil.PipeServer):
            received.append(srv.receive(3.0))
            received.append(srv.receive(3.0))

        make_server(pipe, handler=handler)

        cli = pipeutil.PipeClient(pipe)
        cli.connect(3.0)
        cli.send(pipeutil.Message(b"first"))

        snap = cli.stats()  # スナップショット取得
        assert snap.messages_sent == 1

        cli.send(pipeutil.Message(b"second"))  # 追加送信

        # スナップショットは影響を受けない
        assert snap.messages_sent == 1

        # 最新の stats は 2 になっている
        current = cli.stats()
        assert current.messages_sent == 2

        cli.close()


# ─── T14: repr(stats) が "PipeStats(" で始まる ────────────────────────

class TestPipeStatsRepr:
    def test_pipe_stats_repr(self):
        cli = pipeutil.PipeClient(unique_pipe("repr"))
        s = cli.stats()
        r = repr(s)
        assert r.startswith("PipeStats(")


# ─── T15: PipeServer.stats()/reset_stats() が呼び出せる ───────────────

class TestServerStats:
    def test_server_stats(self):
        pipe = unique_pipe("srv_stats")
        srv = pipeutil.PipeServer(pipe)
        # 接続前でも呼び出せること
        s = srv.stats()
        assert isinstance(s, pipeutil.PipeStats)
        assert s.messages_sent == 0
        srv.reset_stats()
        s2 = srv.stats()
        assert s2.messages_sent == 0
