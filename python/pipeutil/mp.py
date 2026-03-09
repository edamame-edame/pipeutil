# python/pipeutil/mp.py
# [M] multiprocessing 対応ラッパー
# 仕様: spec/F004_async_threading_multiprocessing.md §5
#
# spawn コンテキスト（Windows / macOS デフォルト）に対して安全な設計:
#   - PipeClient は文字列 pipe_name のみシリアライズ、C 拡張オブジェクトは渡さない
#   - fork コンテキスト使用時は RuntimeWarning を発生させる（§10.4）

from __future__ import annotations

import concurrent.futures
import multiprocessing
import threading
import warnings
from typing import Any, Callable, TypeVar

from . import Message, PipeClient, PipeServer

T = TypeVar("T")


# ─── 内部ユーティリティ ──────────────────────────────────────────────

def _check_start_method(ctx_name: str) -> None:
    """fork コンテキスト使用時に警告を送出する（仕様 §10.4）。"""
    if ctx_name == "fork":
        warnings.warn(
            "pipeutil.mp: 'fork' start method may cause pipe handle corruption. "
            "Use 'spawn' (default on Windows/macOS) instead.",
            RuntimeWarning,
            stacklevel=3,
        )


# ─── WorkerPipeClient ────────────────────────────────────────────────

class WorkerPipeClient:
    """
    multiprocessing.Process / Pool の worker 関数内で使う接続ファクトリ。
    spawn コンテキストに対応（文字列 pipe_name のみシリアライズ）。

    仕様: spec/F004 §5.2

    例::

        def worker(pipe_name: str) -> None:
            with pipeutil.mp.WorkerPipeClient(pipe_name) as client:
                client.connect(timeout_ms=5000)
                msg = client.receive(timeout_ms=5000)
                client.send(pipeutil.Message(b"done"))

        p = multiprocessing.Process(target=worker, args=("my_pipe",))
    """

    def __init__(self, pipe_name: str, buffer_size: int = 65536) -> None:
        self._impl = PipeClient(pipe_name, buffer_size)

    # ─── 接続 / 通信 ─────────────────────────────────────────────────

    def connect(self, timeout_ms: int = 5000) -> None:
        """パイプサーバーへ接続する。タイムアウト時は PipeTimeoutError。"""
        self._impl.connect(timeout_ms)

    def send(self, msg: Message) -> None:
        """メッセージを送信する。接続断時は PipeBrokenError。"""
        self._impl.send(msg)

    def receive(self, timeout_ms: int = 5000) -> Message:
        """メッセージを受信する。タイムアウト時は PipeTimeoutError。"""
        return self._impl.receive(timeout_ms)

    def close(self) -> None:
        """接続をクローズする（冪等）。"""
        self._impl.close()

    # ─── コンテキストマネージャ ───────────────────────────────────────

    def __enter__(self) -> "WorkerPipeClient":
        return self

    def __exit__(self, *args: object) -> None:
        self.close()


# ─── ProcessPipeServer ───────────────────────────────────────────────

class ProcessPipeServer:
    """
    接続ごとに別プロセスを起動するサーバー（CPU バインドな重い処理の分離に有効）。

    接続モデル（Handshake Pipe パターン — 仕様 §5.2 / §5.3）:
      1. メインプロセス: ワーカー用名前付きパイプ "pipe_N" を listen する
      2. 接続ごとに子プロセスを spawn し "pipe_N" を引数として渡す
      3. 子プロセスは WorkerPipeClient("pipe_N") で接続し処理を実行する
      4. メインプロセスは次の接続を待機する

    注意: Windows DuplicateHandle ベースのハンドル継承は Phase 2 API (§5.4)。
          本クラスは常に spawn + WorkerPipeClient ハンドシェイクを使う。
    """

    def __init__(
        self,
        pipe_name: str,
        worker_fn: Callable[..., None],
        max_processes: int = 4,
        buffer_size: int = 65536,
        context: str = "spawn",
    ) -> None:
        """
        Parameters
        ----------
        pipe_name:
            基底パイプ名。接続ごとに "{pipe_name}_{id}" が生成される。
        worker_fn:
            子プロセスで呼ばれる関数。シグネチャ: fn(pipe_name: str) -> None
        max_processes:
            同時並列プロセス数の上限。
        buffer_size:
            パイプバッファサイズ（バイト）。
        context:
            multiprocessing の start method。"spawn" | "fork" | "forkserver"
        """
        _check_start_method(context)
        self._pipe_name = pipe_name
        self._worker_fn = worker_fn
        self._max_processes = max_processes
        self._buffer_size = buffer_size
        self._mp_context = multiprocessing.get_context(context)

        self._stop_event = threading.Event()
        self._semaphore = threading.Semaphore(max_processes)
        self._active_count = 0
        self._active_lock = threading.Lock()
        self._done_cv = threading.Condition(self._active_lock)
        self._accept_thread: threading.Thread | None = None
        self._conn_counter = 0
        self._counter_lock = threading.Lock()

    # ─── ライフサイクル ───────────────────────────────────────────────

    def start(self) -> None:
        """接続待機ループをバックグラウンドスレッドで開始する。"""
        self._stop_event.clear()
        self._accept_thread = threading.Thread(
            target=self._accept_loop,
            name="ProcessPipeServer-accept",
            daemon=True,
        )
        self._accept_thread.start()

    def stop(self) -> None:
        """接続受付を停止し、全ワーカープロセスの完了を待つ。"""
        self._stop_event.set()
        if self._accept_thread is not None:
            self._accept_thread.join(timeout=5.0)
        # 実行中ワーカーの完了を待機
        with self._done_cv:
            self._done_cv.wait_for(lambda: self._active_count == 0, timeout=30.0)

    def active_workers(self) -> int:
        """現在実行中のワーカープロセス数を返す。"""
        with self._active_lock:
            return self._active_count

    # ─── 内部: 接続受付ループ ─────────────────────────────────────────

    def _next_pipe_name(self) -> str:
        """接続ごとにユニークなパイプ名を生成する。"""
        with self._counter_lock:
            self._conn_counter += 1
            return f"{self._pipe_name}_{self._conn_counter}"

    def _accept_loop(self) -> None:
        """メインスレッドで動くサーバー受付ループ。stop_event が set されるまで継続。"""
        while not self._stop_event.is_set():
            # 並列上限に達したら空きが出るまで待機
            if not self._semaphore.acquire(timeout=0.5):
                continue

            worker_pipe = self._next_pipe_name()
            srv = PipeServer(worker_pipe, self._buffer_size)
            try:
                srv.listen()
            except Exception:
                self._semaphore.release()
                continue

            # 子プロセス spawn
            proc = self._mp_context.Process(
                target=self._worker_fn,
                args=(worker_pipe,),
                daemon=False,
            )
            proc.start()

            # 接続確立（子プロセスが WorkerPipeClient で connect するまで待機）
            # accept() は positional 引数のみ（C 拡張は keyword 引数未対応）
            try:
                srv.accept(15_000)  # 15 秒タイムアウト
            except Exception:
                proc.terminate()
                srv.close()
                self._semaphore.release()
                continue

            # 完了監視スレッドを起動（スロット解放 RAII 相当）
            monitor = threading.Thread(
                target=self._monitor_worker,
                args=(proc, srv),
                name=f"ProcessPipeServer-monitor-{worker_pipe}",
                daemon=True,
            )
            with self._active_lock:
                self._active_count += 1
            monitor.start()

    def _monitor_worker(self, proc: Any, srv: Any) -> None:
        """ワーカープロセスの完了を検知してスロットを解放するスレッド。"""
        try:
            proc.join(timeout=300.0)  # 最大 5 分
            if proc.is_alive():
                proc.terminate()
                proc.join(timeout=5.0)
        finally:
            srv.close()
            self._semaphore.release()
            with self._done_cv:
                self._active_count -= 1
                self._done_cv.notify_all()


# ─── spawn_worker_factory ────────────────────────────────────────────

def _run_worker_in_process(
    pipe_name: str,
    fn: Callable[..., T],
    args: tuple[Any, ...],
    timeout_ms: int,
    context_name: str,
) -> T:
    """spawn_worker_factory の子プロセスエントリポイント（内部用）。"""
    with WorkerPipeClient(pipe_name) as client:
        client.connect(timeout_ms=timeout_ms)
        return fn(client, *args)


def spawn_worker_factory(
    pipe_name: str,
    fn: Callable[..., T],
    *args: Any,
    timeout_ms: int = 5000,
    context: str = "spawn",
) -> "concurrent.futures.Future[T]":
    """
    spawn した子プロセスで fn(client, *args) を実行し、結果を Future で返す。

    仕様: spec/F004 §5.2

    例::

        import pipeutil.mp as pimp
        fut = pimp.spawn_worker_factory(
            "my_pipe",
            lambda c: c.receive(timeout_ms=5000).as_bytes(),
        )
        result: bytes = fut.result(timeout=10)
    """
    _check_start_method(context)
    mp_ctx = multiprocessing.get_context(context)
    executor = concurrent.futures.ProcessPoolExecutor(
        max_workers=1,
        mp_context=mp_ctx,
    )
    future = executor.submit(
        _run_worker_in_process,
        pipe_name,
        fn,
        args,
        timeout_ms,
        context,
    )
    # executor を Future 完了時に自動シャットダウン
    future.add_done_callback(lambda _: executor.shutdown(wait=False))
    return future
