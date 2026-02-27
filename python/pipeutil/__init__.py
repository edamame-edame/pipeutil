# python/pipeutil/__init__.py
# pipeutil パッケージ公開 API
# 相対インポートを使用（spec/04_python_wrapper.md §9, R-005 対応）

from ._pipeutil import (       # noqa: F401 — re-export
    Message,
    PipeServer,
    PipeClient,
    PipeError,
    TimeoutError,
    ConnectionResetError,
    BrokenPipeError,
    NotConnectedError,
    InvalidMessageError,
)

__all__ = [
    "Message",
    "PipeServer",
    "PipeClient",
    "PipeError",
    "TimeoutError",
    "ConnectionResetError",
    "BrokenPipeError",
    "NotConnectedError",
    "InvalidMessageError",
]

__version__: str = "0.1.0"
