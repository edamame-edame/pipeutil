# python/pipeutil/__init__.py
# pipeutil パッケージ公開 API
# 相対インポートを使用（spec/04_python_wrapper.md §9, R-005 対応）

from ._pipeutil import (       # noqa: F401 — re-export
    Message,
    PipeAcl,
    PipeStats,
    PipeServer,
    PipeClient,
    MultiPipeServer,
    RpcPipeClient,
    RpcPipeServer,
    NegotiatedCapabilities,
    PipeError,
    TimeoutError,
    ConnectionResetError,
    BrokenPipeError,
    NotConnectedError,
    InvalidMessageError,
    ConnectionRejectedError,
    QueueFullError,
)

# [A] asyncio ラッパー（Phase 1: to_thread ベース）
from .aio import (             # noqa: F401
    AsyncPipeClient,
    AsyncPipeServer,
    AsyncRpcPipeClient,
    AsyncRpcPipeServer,
    serve_connections,
)

# [T] threading / concurrent.futures ラッパー
from .threading_utils import ( # noqa: F401
    ThreadedPipeClient,
    ThreadedPipeServer,
)

# [M] multiprocessing ラッパー
from .mp import (              # noqa: F401
    WorkerPipeClient,
    ProcessPipeServer,
    spawn_worker_factory,
)

# [C] コーデックユーティリティ（JSON / msgpack）
from .message_utils import (   # noqa: F401
    encode_json,
    decode_json,
    encode_msgpack,
    decode_msgpack,
    CodecError,
)

# [R] 自動再接続クライアント群（F-003）
from .reconnecting_client import (  # noqa: F401
    ReconnectingPipeClient,
    AsyncReconnectingPipeClient,
    ReconnectingRpcPipeClient,
    MaxRetriesExceededError,
)

# [H] Capability Negotiation 純 Python 型 (A-001)
from enum import IntEnum, IntFlag
from dataclasses import dataclass, field as dc_field


class HelloMode(IntEnum):
    """HELLO ハンドシェイクの動作モード。"""
    Compat = 0  # v1.0.0 クライアントを許容（デフォルト）
    Strict = 1  # HELLO なしの接続を拒否
    Skip   = 2  # HELLO ハンドシェイクをスキップ


class CapabilityBitmap(IntFlag):
    """Capability ネゴシエーションのビットマップ定義。"""
    ProtoV2       = 0x01
    ConcurrentRpc = 0x02
    Streaming     = 0x04
    Heartbeat     = 0x08
    PriorityQueue = 0x10


@dataclass
class HelloConfig:
    """PipeServer / PipeClient コンストラクタに渡す HELLO 設定。"""
    mode: HelloMode = HelloMode.Compat
    hello_timeout_ms: int = 500
    advertised_capabilities: int = 0

__all__ = [
    # ─── コア（C 拡張） ───────────────────────────────────────────────
    "Message",
    "PipeAcl",
    "PipeStats",
    "PipeServer",
    "PipeClient",
    "MultiPipeServer",
    "RpcPipeClient",
    "RpcPipeServer",
    "NegotiatedCapabilities",
    "PipeError",
    "TimeoutError",
    "ConnectionResetError",
    "BrokenPipeError",
    "NotConnectedError",
    "InvalidMessageError",
    "ConnectionRejectedError",
    "QueueFullError",
    # ─── Capability Negotiation [純 Python] ─────────────────
    "HelloMode",
    "CapabilityBitmap",
    "HelloConfig",
    # ─── asyncio ラッパー [A] ─────────────────────────────────────────
    "AsyncPipeClient",
    "AsyncPipeServer",
    "AsyncRpcPipeClient",
    "AsyncRpcPipeServer",
    "serve_connections",
    # ─── threading ラッパー [T] ───────────────────────────────────────
    "ThreadedPipeClient",
    "ThreadedPipeServer",
    # ─── multiprocessing ラッパー [M] ────────────────────────────────
    "WorkerPipeClient",
    "ProcessPipeServer",
    "spawn_worker_factory",
    # ─── コーデック [C] ──────────────────────────────────────────────
    "encode_json",
    "decode_json",
    "encode_msgpack",
    "decode_msgpack",
    "CodecError",
    # ─── 自動再接続クライアント [R] ───────────────────────────────────
    "ReconnectingPipeClient",
    "AsyncReconnectingPipeClient",
    "ReconnectingRpcPipeClient",
    "MaxRetriesExceededError",
]

__version__: str = "1.1.0"
