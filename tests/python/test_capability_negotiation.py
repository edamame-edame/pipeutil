# tests/python/test_capability_negotiation.py — A-001 Capability Negotiation Python テスト
# 仕様: spec/A001_capability_negotiation.md, docs/design_change_spec_v1.1.0.md §11.2
#
# T-PY-CN-001 test_capability_bitmap_values    — CapabilityBitmap 各値が仕様通り
# T-PY-CN-002 test_negotiated_capabilities_has — has() の動作確認
# T-PY-CN-003 test_negotiated_capabilities_legacy — is_legacy_v1 の動作確認
# T-PY-CN-004 test_hello_config_defaults        — HelloConfig() のデフォルト値確認
# T-PY-CN-005 test_server_client_negotiation    — PipeServer ↔ PipeClient で HELLO 往来
# T-PY-CN-006 test_negotiation_skip_mode        — 双方 Skip → 通常メッセージ正常
# T-PY-CN-007 test_on_hello_complete_callback   — コールバックが accept 後に呼ばれる
# T-PY-CN-008 test_queue_full_error_is_pipe_error — QueueFullError が PipeError サブクラス
# T-PY-CN-009 test_hello_config_custom_timeout  — hello_timeout_ms が C++ 側に渡る

import threading
import time

import pytest
import pipeutil
from pipeutil import (
    HelloMode,
    HelloConfig,
    CapabilityBitmap,
    NegotiatedCapabilities,
    PipeServer,
    PipeClient,
    PipeError,
    ConnectionRejectedError,
    QueueFullError,
)
from conftest import unique_pipe


# ─── T-PY-CN-001 CapabilityBitmap 値の検証 ───────────────────────────

class TestCapabilityBitmapValues:
    """T-PY-CN-001 — CapabilityBitmap 各値が仕様通りであること。"""

    def test_proto_v2(self):
        assert CapabilityBitmap.ProtoV2 == 0x01

    def test_concurrent_rpc(self):
        assert CapabilityBitmap.ConcurrentRpc == 0x02

    def test_streaming(self):
        assert CapabilityBitmap.Streaming == 0x04

    def test_heartbeat(self):
        assert CapabilityBitmap.Heartbeat == 0x08

    def test_priority_queue(self):
        assert CapabilityBitmap.PriorityQueue == 0x10

    def test_no_overlap(self):
        """全 bits が互いに重複しないこと。"""
        all_flags = [
            CapabilityBitmap.ProtoV2,
            CapabilityBitmap.ConcurrentRpc,
            CapabilityBitmap.Streaming,
            CapabilityBitmap.Heartbeat,
            CapabilityBitmap.PriorityQueue,
        ]
        combined = 0
        for flag in all_flags:
            assert combined & int(flag) == 0, f"Flag {flag!r} overlaps with a previous value"
            combined |= int(flag)


# ─── T-PY-CN-002 NegotiatedCapabilities.has() ───────────────────────

class TestNegotiatedCapabilitiesHas:
    """T-PY-CN-002 — NegotiatedCapabilities.has()"""

    def test_has_matching_bit(self):
        """bitmap=0x05 → has(ProtoV2) と has(Streaming) が True。"""
        caps = NegotiatedCapabilities(0x05)
        assert caps.has(int(CapabilityBitmap.ProtoV2))    # 0x01 ⊂ 0x05
        assert caps.has(int(CapabilityBitmap.Streaming))  # 0x04 ⊂ 0x05

    def test_has_unset_bit(self):
        """bitmap=0x05 → has(ConcurrentRpc) が False。"""
        caps = NegotiatedCapabilities(0x05)
        assert not caps.has(int(CapabilityBitmap.ConcurrentRpc))  # 0x02 ⊄ 0x05

    def test_has_zero_bitmap(self):
        caps = NegotiatedCapabilities(0)
        assert not caps.has(int(CapabilityBitmap.ProtoV2))

    def test_bitmap_property(self):
        caps = NegotiatedCapabilities(0x0F)
        assert caps.bitmap == 0x0F


# ─── T-PY-CN-003 NegotiatedCapabilities 状態プロパティ ───────────────

class TestNegotiatedCapabilitiesLegacy:
    """T-PY-CN-003 — is_legacy_v1 / is_v1_compat プロパティ。"""

    def test_default_is_legacy_v1(self):
        """bitmap=0, v1_compat=False → is_legacy_v1 == True。"""
        caps = NegotiatedCapabilities(0)         # デフォルト: v1_compat=False
        assert caps.is_legacy_v1

    def test_nonzero_bitmap_not_legacy(self):
        caps = NegotiatedCapabilities(0x01)
        assert not caps.is_legacy_v1

    def test_v1_compat_is_not_legacy(self):
        """v1_compat=True かつ bitmap=0 なら is_legacy_v1 は False。"""
        caps = NegotiatedCapabilities(bitmap=0, v1_compat=True)
        assert not caps.is_legacy_v1

    def test_v1_compat_property_true(self):
        caps = NegotiatedCapabilities(bitmap=0, v1_compat=True)
        assert caps.is_v1_compat

    def test_v1_compat_property(self):
        """デフォルト構築では v1_compat=False。"""
        caps = NegotiatedCapabilities(0)
        assert not caps.v1_compat


# ─── T-PY-CN-004 HelloConfig デフォルト値 ────────────────────────────

class TestHelloConfigDefaults:
    """T-PY-CN-004 — HelloConfig() のデフォルト値が仕様通り。"""

    def test_default_mode(self):
        cfg = HelloConfig()
        assert cfg.mode == HelloMode.Compat

    def test_default_timeout(self):
        cfg = HelloConfig()
        assert cfg.hello_timeout_ms == 500

    def test_default_advertised_capabilities(self):
        cfg = HelloConfig()
        assert cfg.advertised_capabilities == 0

    def test_custom_values(self):
        cfg = HelloConfig(
            mode=HelloMode.Strict,
            hello_timeout_ms=100,
            advertised_capabilities=int(CapabilityBitmap.ProtoV2),
        )
        assert cfg.mode == HelloMode.Strict
        assert cfg.hello_timeout_ms == 100
        assert cfg.advertised_capabilities == 0x01

    def test_hello_mode_values(self):
        assert HelloMode.Compat == 0
        assert HelloMode.Strict == 1
        assert HelloMode.Skip   == 2


# ─── T-PY-CN-005 PipeServer ↔ PipeClient HELLO ネゴシエーション ──────

class TestServerClientNegotiation:
    """T-PY-CN-005 — Python PipeServer ↔ PipeClient で HELLO 往来。"""

    def test_negotiated_capabilities_after_connect(self):
        pipe_name = unique_pipe("py_neg")
        srv_caps: list[NegotiatedCapabilities] = []
        cli_caps: list[NegotiatedCapabilities] = []
        ready = threading.Event()

        def server_thread():
            srv = PipeServer(pipe_name, hello_config=HelloConfig())
            srv.listen()
            ready.set()
            srv.accept(5000)
            srv_caps.append(srv.negotiated_capabilities)
            srv.send(pipeutil.Message(b"hello"))
            srv.close()

        t = threading.Thread(target=server_thread, daemon=True)
        t.start()
        assert ready.wait(3.0), "Server did not start"

        cli = PipeClient(pipe_name, hello_config=HelloConfig())
        cli.connect(3000)
        cli_caps.append(cli.negotiated_capabilities)
        msg = cli.receive(3000)
        cli.close()
        t.join(timeout=5.0)

        assert bytes(msg) == b"hello"
        assert len(srv_caps) == 1
        assert len(cli_caps) == 1
        # 双方 bitmap=0 → is_legacy_v1
        assert srv_caps[0].is_legacy_v1
        assert cli_caps[0].is_legacy_v1

    def test_bit_and_negotiation(self):
        """server_bitmap=0x0F, client_bitmap=0x05 → negotiated=0x05。"""
        pipe_name = unique_pipe("py_bitand")
        srv_caps: list[NegotiatedCapabilities] = []
        ready = threading.Event()

        def server_thread():
            cfg = HelloConfig(advertised_capabilities=0x0F)
            srv = PipeServer(pipe_name, hello_config=cfg)
            srv.listen()
            ready.set()
            srv.accept(5000)
            srv_caps.append(srv.negotiated_capabilities)
            srv.send(pipeutil.Message(b"go"))
            srv.close()

        t = threading.Thread(target=server_thread, daemon=True)
        t.start()
        assert ready.wait(3.0)

        cfg = HelloConfig(advertised_capabilities=0x05)
        cli = PipeClient(pipe_name, hello_config=cfg)
        cli.connect(3000)
        cli_caps = cli.negotiated_capabilities
        cli.receive(3000)
        cli.close()
        t.join(timeout=5.0)

        assert len(srv_caps) == 1
        assert srv_caps[0].bitmap == 0x05
        assert cli_caps.bitmap == 0x05


# ─── T-PY-CN-006 Skip モード ─────────────────────────────────────────

class TestNegotiationSkipMode:
    """T-PY-CN-006 — 双方 Skip → HELLO 交換なし、通常メッセージ正常。"""

    def test_skip_both_sides(self):
        pipe_name = unique_pipe("py_skip")
        received: list[bytes] = []
        ready = threading.Event()

        def server_thread():
            cfg = HelloConfig(mode=HelloMode.Skip)
            srv = PipeServer(pipe_name, hello_config=cfg)
            srv.listen()
            ready.set()
            srv.accept(5000)
            msg = srv.receive(3000)
            received.append(bytes(msg))
            srv.close()

        t = threading.Thread(target=server_thread, daemon=True)
        t.start()
        assert ready.wait(3.0)

        cfg = HelloConfig(mode=HelloMode.Skip)
        cli = PipeClient(pipe_name, hello_config=cfg)
        cli.connect(3000)
        cli.send(pipeutil.Message(b"skip_test"))
        cli.close()
        t.join(timeout=5.0)

        assert received == [b"skip_test"]


# ─── T-PY-CN-007 on_hello_complete コールバック ─────────────────────

class TestOnHelloCompleteCallback:
    """T-PY-CN-007 — on_hello_complete が accept() 後に呼ばれる。"""

    def test_callback_is_called(self):
        pipe_name = unique_pipe("py_cb")
        callback_results: list[NegotiatedCapabilities] = []
        ready = threading.Event()

        def server_thread():
            srv = PipeServer(pipe_name, hello_config=HelloConfig())
            srv.on_hello_complete = callback_results.append
            srv.listen()
            ready.set()
            srv.accept(5000)
            srv.send(pipeutil.Message(b"done"))
            srv.close()

        t = threading.Thread(target=server_thread, daemon=True)
        t.start()
        assert ready.wait(3.0)

        cli = PipeClient(pipe_name, hello_config=HelloConfig())
        cli.connect(3000)
        cli.receive(3000)
        cli.close()
        t.join(timeout=5.0)

        assert len(callback_results) == 1, "on_hello_complete should be called once"
        assert isinstance(callback_results[0], NegotiatedCapabilities)

    def test_callback_setter_accepts_none(self):
        """on_hello_complete に None を設定できること。"""
        srv = PipeServer("dummy_pipe_name_not_used")
        srv.on_hello_complete = None
        assert srv.on_hello_complete is None

    def test_callback_raises_typeerror_for_non_callable(self):
        """on_hello_complete に非 callable を設定すると TypeError。"""
        srv = PipeServer("dummy_pipe_name_not_used")
        with pytest.raises(TypeError):
            srv.on_hello_complete = 42  # type: ignore[assignment]


# ─── T-PY-CN-008 QueueFullError / ConnectionRejectedError 継承 ───────

class TestQueueFullErrorIsPipeError:
    """T-PY-CN-008 — 新例外が PipeError のサブクラスであること。"""

    def test_queue_full_inherits_pipe_error(self):
        assert issubclass(QueueFullError, PipeError)

    def test_connection_rejected_inherits_pipe_error(self):
        assert issubclass(ConnectionRejectedError, PipeError)

    def test_queue_full_is_exception(self):
        assert issubclass(QueueFullError, Exception)

    def test_connection_rejected_is_exception(self):
        assert issubclass(ConnectionRejectedError, Exception)


# ─── T-PY-CN-009 hello_timeout_ms が C++ 側に伝わること ─────────────

class TestHelloConfigCustomTimeout:
    """T-PY-CN-009 — hello_timeout_ms=100 をサーバーに渡したとき、
    Strict + Skip クライアントで ConnectionRejectedError が送出されること。"""

    def test_strict_rejects_skip_client(self):
        """Strict + 50ms timeout + Skip クライアント → ConnectionRejectedError。"""
        pipe_name = unique_pipe("py_strict_to")
        server_error: list[Exception] = []
        ready = threading.Event()

        def server_thread():
            cfg = HelloConfig(mode=HelloMode.Strict, hello_timeout_ms=50)
            srv = PipeServer(pipe_name, hello_config=cfg)
            srv.listen()
            ready.set()
            try:
                srv.accept(5000)
            except ConnectionRejectedError as e:
                server_error.append(e)
            except Exception as e:
                server_error.append(e)
            finally:
                srv.close()

        t = threading.Thread(target=server_thread, daemon=True)
        t.start()
        assert ready.wait(3.0)

        # Skip クライアント: HELLO を送らない
        cfg = HelloConfig(mode=HelloMode.Skip)
        cli = PipeClient(pipe_name, hello_config=cfg)
        cli.connect(3000)
        time.sleep(0.3)  # サーバーがタイムアウトするまで待機
        cli.close()
        t.join(timeout=5.0)

        assert len(server_error) == 1, "Server should have raised an error"
        assert isinstance(server_error[0], ConnectionRejectedError), (
            f"Expected ConnectionRejectedError, got {type(server_error[0]).__name__}: {server_error[0]}"
        )
