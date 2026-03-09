# python/pipeutil/__init__.py
# pipeutil パッケージ公開 API
# 相対インポートを使用（spec/04_python_wrapper.md §9, R-005 対応）

from ._pipeutil import (       # noqa: F401 — re-export
    Message,
    PipeServer,
    PipeClient,
    MultiPipeServer,
    RpcPipeClient,
    RpcPipeServer,
    PipeError,
    TimeoutError,
    ConnectionResetError,
    BrokenPipeError,
    NotConnectedError,
    InvalidMessageError,
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

__all__ = [
    # ─── コア（C 拡張） ───────────────────────────────────────────────
    "Message",
    "PipeServer",
    "PipeClient",
    "MultiPipeServer",
    "RpcPipeClient",
    "RpcPipeServer",
    "PipeError",
    "TimeoutError",
    "ConnectionResetError",
    "BrokenPipeError",
    "NotConnectedError",
    "InvalidMessageError",
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
]

__version__: str = "0.4.0"
