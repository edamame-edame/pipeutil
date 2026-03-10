# tests/python/test_reconnecting.py
# ReconnectingPipeClient / AsyncReconnectingPipeClient / ReconnectingRpcPipeClient テスト
# 仕様: spec/F003_reconnecting_pipe_client.md §10

from __future__ import annotations

import asyncio
import threading
from unittest.mock import AsyncMock, MagicMock, call

import pytest
import pipeutil
from pipeutil import Message
from pipeutil.reconnecting_client import (
    AsyncReconnectingPipeClient,
    MaxRetriesExceededError,
    ReconnectingPipeClient,
    ReconnectingRpcPipeClient,
)


# ─── モックファクトリ ─────────────────────────────────────────────────

def _make_sync_mock(connected: bool = True) -> MagicMock:
    """PipeClient をシミュレートする MagicMock を返す。

    connect() 呼び出しで is_connected が True に、
    close() 呼び出しで False になる動作を設定済み。
    """
    mock = MagicMock()
    mock.is_connected = connected

    def do_connect(*args, **kwargs):
        mock.is_connected = True

    def do_close():
        mock.is_connected = False

    mock.connect.side_effect = do_connect
    mock.close.side_effect = do_close
    return mock


def _make_async_mock(connected: bool = True) -> AsyncMock:
    """AsyncPipeClient をシミュレートする AsyncMock を返す。

    connect() で is_connected を True、close() で False にする。
    """
    mock = AsyncMock()
    mock.is_connected = connected

    async def do_connect(*args, **kwargs):
        mock.is_connected = True

    async def do_close():
        mock.is_connected = False

    mock.connect.side_effect = do_connect
    mock.close.side_effect = do_close
    return mock


def _make_rpc_mock(connected: bool = True) -> MagicMock:
    """RpcPipeClient をシミュレートする MagicMock を返す。"""
    mock = MagicMock()
    mock.is_connected = connected

    def do_connect(*args, **kwargs):
        mock.is_connected = True

    def do_close():
        mock.is_connected = False

    mock.connect.side_effect = do_connect
    mock.close.side_effect = do_close
    return mock


# ═══════════════════════════════════════════════════════════════════════
# §10.1  同期版テスト（ReconnectingPipeClient）T1 – T12
# ═══════════════════════════════════════════════════════════════════════

class TestReconnectingPipeClient:

    # T1: 正常な送受信ラウンドトリップ
    def test_basic_send_receive(self) -> None:
        """正常接続・送受信ラウンドトリップが成功すること。（T1）"""
        mock = _make_sync_mock(connected=False)
        client = ReconnectingPipeClient("test", max_retries=1, retry_interval_ms=0)
        client._impl = mock

        client.connect()
        mock.connect.assert_called_once_with(0.0)

        msg = Message(b"hello")
        mock.receive.return_value = msg

        client.send(msg)
        mock.send.assert_called_once_with(msg)

        result = client.receive(timeout=1.0)
        mock.receive.assert_called_once_with(1.0)
        assert result is msg

    # T2: コンテキストマネージャ
    def test_context_manager(self) -> None:
        """`with` ブロック脱出後に close() が呼ばれ NotConnectedError になること。（T2）"""
        mock = _make_sync_mock()
        client = ReconnectingPipeClient("test", max_retries=1, retry_interval_ms=0)
        client._impl = mock

        with client:
            pass  # __exit__ で close() が呼ばれる

        assert client._closed is True
        with pytest.raises(pipeutil.NotConnectedError):
            client.send(Message(b"x"))

    # T3: ConnectionResetError 後に再接続して send 成功
    def test_reconnect_on_connection_reset(self) -> None:
        """`ConnectionResetError` 後に再接続して送信成功すること。（T3）"""
        mock = _make_sync_mock(connected=False)
        mock.send.side_effect = [
            pipeutil.ConnectionResetError("disconnected"),
            None,  # 再接続後の送信は成功
        ]

        client = ReconnectingPipeClient("test", max_retries=3, retry_interval_ms=0)
        client._impl = mock

        client.send(Message(b"ping"))  # 例外を送出しないこと

        # close → connect の順で呼ばれたことを確認
        mock.close.assert_called_once()
        mock.connect.assert_called_once()
        assert mock.send.call_count == 2
        assert client.retry_count == 1

    # T4: BrokenPipeError 後に再接続して send 成功
    def test_reconnect_on_broken_pipe(self) -> None:
        """`BrokenPipeError` 後に再接続して送信成功すること。（T4）"""
        mock = _make_sync_mock(connected=False)
        mock.send.side_effect = [pipeutil.BrokenPipeError("broken"), None]

        client = ReconnectingPipeClient("test", max_retries=3, retry_interval_ms=0)
        client._impl = mock

        client.send(Message(b"ping"))  # 例外を送出しないこと
        assert client.retry_count == 1

    # T5: max_retries 超過で MaxRetriesExceededError
    def test_max_retries_exceeded(self) -> None:
        """再接続失敗 N 回で `MaxRetriesExceededError` が送出されること。（T5）"""
        mock = _make_sync_mock(connected=False)
        mock.send.side_effect = pipeutil.ConnectionResetError("disconnected")
        # connect は常に失敗
        mock.connect.side_effect = pipeutil.PipeError("server down")

        client = ReconnectingPipeClient("test", max_retries=3, retry_interval_ms=0)
        client._impl = mock

        with pytest.raises(MaxRetriesExceededError):
            client.send(Message(b"ping"))

        assert mock.connect.call_count == 3

    # T6: MaxRetriesExceededError の属性
    def test_max_retries_exceeded_attributes(self) -> None:
        """`MaxRetriesExceededError.attempts` と `last_exception` が正しいこと。（T6）"""
        mock = _make_sync_mock(connected=False)
        mock.send.side_effect = pipeutil.ConnectionResetError("disconnected")
        last_error = pipeutil.PipeError("server down")
        mock.connect.side_effect = last_error

        client = ReconnectingPipeClient("test", max_retries=2, retry_interval_ms=0)
        client._impl = mock

        with pytest.raises(MaxRetriesExceededError) as exc_info:
            client.send(Message(b"ping"))

        err = exc_info.value
        assert err.attempts == 2
        assert err.last_exception is last_error

    # T7: on_reconnect コールバックが 1 回呼ばれる
    def test_on_reconnect_callback(self) -> None:
        """再接続成功後にコールバックが 1 回呼ばれること。（T7）"""
        mock = _make_sync_mock(connected=False)
        mock.send.side_effect = [pipeutil.ConnectionResetError("disconnected"), None]

        callback = MagicMock()
        client = ReconnectingPipeClient(
            "test", max_retries=3, retry_interval_ms=0, on_reconnect=callback
        )
        client._impl = mock

        client.send(Message(b"ping"))
        callback.assert_called_once()

    # T8: retry_count プロパティ
    def test_retry_count_property(self) -> None:
        """`retry_count` が再接続成功ごとにインクリメントされること。（T8）"""
        mock = _make_sync_mock(connected=False)
        # 2 回の send を行い、それぞれ 1 回再接続させる
        mock.send.side_effect = [
            pipeutil.ConnectionResetError("disconnected"),
            None,  # 1 回目再送
            pipeutil.ConnectionResetError("disconnected"),
            None,  # 2 回目再送
        ]
        # 2 回目の send で再接続が入るよう is_connected を False にリセット
        original_do_connect = mock.connect.side_effect
        connect_call = [0]

        def do_connect_and_reset(*args, **kwargs):
            connect_call[0] += 1
            mock.is_connected = True

        mock.connect.side_effect = do_connect_and_reset

        client = ReconnectingPipeClient("test", max_retries=3, retry_interval_ms=0)
        client._impl = mock

        client.send(Message(b"first"))
        assert client.retry_count == 1

        # 2 回目の送信で切断が発生するよう is_connected を False にリセット
        mock.is_connected = False
        client.send(Message(b"second"))
        assert client.retry_count == 2

    # T9: close() 後の操作は NotConnectedError
    def test_close_prevents_reconnect(self) -> None:
        """`close()` 後の `send()` が `NotConnectedError`（再接続しない）こと。（T9）"""
        mock = _make_sync_mock()
        client = ReconnectingPipeClient("test", max_retries=10, retry_interval_ms=0)
        client._impl = mock

        client.close()

        with pytest.raises(pipeutil.NotConnectedError):
            client.send(Message(b"ping"))

        # close() 後は connect() が呼ばれないこと
        mock.connect.assert_not_called()

    # T10: 無限リトライ（max_retries=0）でサーバーが n 回目で起動
    def test_infinite_retry(self) -> None:
        """`max_retries=0` でサーバーが n 回目で起動した場合に成功すること。（T10）"""
        mock = _make_sync_mock(connected=False)
        mock.send.side_effect = [
            pipeutil.ConnectionResetError("disconnected"),
            None,
        ]
        connect_call = [0]

        def do_connect(*args, **kwargs):
            connect_call[0] += 1
            if connect_call[0] < 3:
                raise pipeutil.PipeError("not ready")
            mock.is_connected = True

        mock.connect.side_effect = do_connect

        client = ReconnectingPipeClient("test", max_retries=0, retry_interval_ms=0)
        client._impl = mock

        client.send(Message(b"ping"))  # 例外なし

        assert connect_call[0] == 3
        assert client.retry_count == 1

    # T11: receive() の TimeoutError で再接続しない
    def test_timeout_not_retried(self) -> None:
        """`receive()` の `TimeoutError` では再接続しないこと。（T11）"""
        mock = _make_sync_mock()
        mock.receive.side_effect = pipeutil.TimeoutError("timeout")

        client = ReconnectingPipeClient("test", max_retries=3, retry_interval_ms=0)
        client._impl = mock

        with pytest.raises(pipeutil.TimeoutError):
            client.receive(timeout=0.1)

        # 再接続は行わない
        mock.connect.assert_not_called()

    # T12: 複数スレッドからの同時切断検知で再接続が 1 回だけ起きる
    def test_thread_safe_reconnect(self) -> None:
        """複数スレッドが同時に切断を検知しても 1 回だけ再接続すること。（T12）"""
        N = 5
        mock = _make_sync_mock(connected=False)
        connect_count = [0]
        barrier = threading.Barrier(N)

        def do_connect(*args, **kwargs):
            connect_count[0] += 1
            mock.is_connected = True

        mock.connect.side_effect = do_connect

        # barrier で全スレッドを同期させてから ConnectionResetError を送出する
        def send_with_barrier(*args, **kwargs):
            barrier.wait()
            raise pipeutil.ConnectionResetError("disconnected")

        mock.send.side_effect = send_with_barrier

        client = ReconnectingPipeClient("test", max_retries=1, retry_interval_ms=0)
        client._impl = mock

        errors: list[Exception] = []

        def worker() -> None:
            try:
                client.send(Message(b"hello"))
            except Exception as e:
                errors.append(e)

        threads = [threading.Thread(target=worker) for _ in range(N)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        # 複数スレッドが競合しても connect() は 1 回だけ呼ばれること
        assert connect_count[0] == 1


# ═══════════════════════════════════════════════════════════════════════
# §10.2  非同期版テスト（AsyncReconnectingPipeClient）T13 – T20
# ═══════════════════════════════════════════════════════════════════════

class TestAsyncReconnectingPipeClient:

    # T13: 正常な非同期送受信ラウンドトリップ
    @pytest.mark.asyncio
    async def test_async_basic_send_receive(self) -> None:
        """正常接続・非同期送受信ラウンドトリップが成功すること。（T13）"""
        mock = _make_async_mock(connected=False)
        client = AsyncReconnectingPipeClient("test", max_retries=1, retry_interval_ms=0)
        client._impl = mock

        await client.connect(timeout_ms=5000)
        mock.connect.assert_called_once_with(5000)

        msg = Message(b"hello")
        mock.receive.return_value = msg

        await client.send(msg)
        mock.send.assert_called_once_with(msg)

        result = await client.receive(timeout_ms=3000)
        mock.receive.assert_called_once_with(3000)
        assert result is msg

    # T14: 非同期コンテキストマネージャ
    @pytest.mark.asyncio
    async def test_async_context_manager(self) -> None:
        """`async with` 脱出後に close() が呼ばれ NotConnectedError になること。（T14）"""
        mock = _make_async_mock()
        client = AsyncReconnectingPipeClient("test", max_retries=1, retry_interval_ms=0)
        client._impl = mock

        async with client:
            pass  # __aexit__ で close() が呼ばれる

        assert client._closed is True
        with pytest.raises(pipeutil.NotConnectedError):
            await client.send(Message(b"x"))

    # T15: ConnectionResetError 後に再接続して非同期 send 成功
    @pytest.mark.asyncio
    async def test_async_reconnect_on_connection_reset(self) -> None:
        """`ConnectionResetError` 後に再接続して非同期送信成功すること。（T15）"""
        mock = _make_async_mock(connected=False)

        send_call = [0]

        async def mock_send(*args, **kwargs):
            send_call[0] += 1
            if send_call[0] == 1:
                raise pipeutil.ConnectionResetError("disconnected")

        mock.send.side_effect = mock_send

        client = AsyncReconnectingPipeClient("test", max_retries=3, retry_interval_ms=0)
        client._impl = mock

        await client.send(Message(b"ping"))  # 例外を送出しないこと

        assert send_call[0] == 2
        assert client.retry_count == 1

    # T16: 非同期版 max_retries 超過で MaxRetriesExceededError
    @pytest.mark.asyncio
    async def test_async_max_retries_exceeded(self) -> None:
        """再接続失敗 N 回で `MaxRetriesExceededError` が送出されること。（T16）"""
        mock = _make_async_mock(connected=False)
        mock.send.side_effect = pipeutil.ConnectionResetError("disconnected")
        mock.connect.side_effect = pipeutil.PipeError("server down")

        client = AsyncReconnectingPipeClient("test", max_retries=3, retry_interval_ms=0)
        client._impl = mock

        with pytest.raises(MaxRetriesExceededError) as exc_info:
            await client.send(Message(b"ping"))

        assert exc_info.value.attempts == 3

    # T17: 非同期版 on_reconnect コールバック
    @pytest.mark.asyncio
    async def test_async_on_reconnect_callback(self) -> None:
        """再接続成功後に同期コールバックが 1 回呼ばれること。（T17）"""
        mock = _make_async_mock(connected=False)

        send_call = [0]

        async def mock_send(*args, **kwargs):
            send_call[0] += 1
            if send_call[0] == 1:
                raise pipeutil.ConnectionResetError("disconnected")

        mock.send.side_effect = mock_send

        callback = MagicMock()
        client = AsyncReconnectingPipeClient(
            "test", max_retries=3, retry_interval_ms=0, on_reconnect=callback
        )
        client._impl = mock

        await client.send(Message(b"ping"))
        callback.assert_called_once()

    # T18: 非同期版 retry_count プロパティ
    @pytest.mark.asyncio
    async def test_async_retry_count_property(self) -> None:
        """`retry_count` が再接続成功ごとにインクリメントされること。（T18）"""
        mock = _make_async_mock(connected=False)
        send_call = [0]

        async def mock_send(*args, **kwargs):
            send_call[0] += 1
            if send_call[0] in (1, 3):  # 1・3 回目のみ失敗
                raise pipeutil.ConnectionResetError("disconnected")

        mock.send.side_effect = mock_send

        client = AsyncReconnectingPipeClient("test", max_retries=3, retry_interval_ms=0)
        client._impl = mock

        await client.send(Message(b"first"))
        assert client.retry_count == 1

        # 2 回目の送信で再度切断させるため is_connected をリセット
        mock.is_connected = False
        await client.send(Message(b"second"))
        assert client.retry_count == 2

    # T19: 非同期版 close() 後は NotConnectedError
    @pytest.mark.asyncio
    async def test_async_close_prevents_reconnect(self) -> None:
        """`close()` 後の `send()` が `NotConnectedError`（再接続しない）こと。（T19）"""
        mock = _make_async_mock()
        client = AsyncReconnectingPipeClient("test", max_retries=10, retry_interval_ms=0)
        client._impl = mock

        await client.close()

        with pytest.raises(pipeutil.NotConnectedError):
            await client.send(Message(b"ping"))

        mock.connect.assert_not_called()

    # T20: 複数タスクからの同時切断検知で再接続が 1 回だけ起きる
    @pytest.mark.asyncio
    async def test_async_task_safe_reconnect(self) -> None:
        """複数タスクが同時に切断を検知しても 1 回だけ再接続すること。（T20）"""
        N = 3
        mock = _make_async_mock(connected=False)
        connect_count = [0]
        send_call = [0]

        async def do_connect(*args, **kwargs):
            connect_count[0] += 1
            mock.is_connected = True

        async def mock_send(*args, **kwargs):
            send_call[0] += 1
            if send_call[0] <= N:
                # 最初の N 回（各タスク 1 回目）はすべて切断エラー
                raise pipeutil.ConnectionResetError("disconnected")
            # N+1 回目以降（再接続後の再送）は成功

        mock.connect.side_effect = do_connect
        mock.send.side_effect = mock_send

        client = AsyncReconnectingPipeClient("test", max_retries=1, retry_interval_ms=0)
        client._impl = mock

        async def worker() -> None:
            await client.send(Message(b"hello"))

        # asyncio.gather で同時実行。asyncio.sleep(0) で他タスクに制御を渡せる。
        await asyncio.gather(*[asyncio.create_task(worker()) for _ in range(N)])

        # N タスクが競合しても connect() は 1 回だけ呼ばれること
        assert connect_count[0] == 1


# ═══════════════════════════════════════════════════════════════════════
# §10.3  RPC 版テスト（ReconnectingRpcPipeClient）T21 – T25
# ═══════════════════════════════════════════════════════════════════════

class TestReconnectingRpcPipeClient:

    # T21: 正常な send_request
    def test_rpc_basic_send_request(self) -> None:
        """正常接続で `send_request()` が成功すること。（T21）"""
        mock = _make_rpc_mock()
        req = Message(b"ping")
        resp = Message(b"pong")
        mock.send_request.return_value = resp

        client = ReconnectingRpcPipeClient("test", max_retries=1, retry_interval_ms=0)
        client._impl = mock

        result = client.send_request(req, timeout=5.0)
        assert result is resp
        # 同一インスタンスで呼ばれたこと・タイムアウト値が正しいことを確認
        mock.send_request.assert_called_once_with(req, 5.0)

    # T22: 切断後に接続再確立し ConnectionResetError を上位に伝播
    def test_rpc_reconnect_on_connection_reset(self) -> None:
        """切断後に接続を再確立し、`ConnectionResetError` を上位に伝播すること。（T22）"""
        mock = _make_rpc_mock(connected=False)
        mock.send_request.side_effect = pipeutil.ConnectionResetError("disconnected")

        client = ReconnectingRpcPipeClient("test", max_retries=1, retry_interval_ms=0)
        client._impl = mock

        with pytest.raises(pipeutil.ConnectionResetError):
            client.send_request(Message(b"req"))

        # 再接続が行われたことを確認
        assert client.retry_count == 1
        mock.connect.assert_called_once()

    # T23: in-flight リクエストが自動再送されない（方針 A）
    def test_rpc_no_auto_resend(self) -> None:
        """切断後に in-flight リクエストが自動再送されないこと。（T23 / 方針 A）"""
        mock = _make_rpc_mock(connected=False)
        mock.send_request.side_effect = pipeutil.ConnectionResetError("disconnected")

        client = ReconnectingRpcPipeClient("test", max_retries=1, retry_interval_ms=0)
        client._impl = mock

        with pytest.raises(pipeutil.ConnectionResetError):
            client.send_request(Message(b"req"))

        # send_request は 1 回だけ呼ばれること（再送なし）
        assert mock.send_request.call_count == 1

    # T24: TimeoutError では再接続しない
    def test_rpc_timeout_not_retried(self) -> None:
        """`TimeoutError` では再接続しないこと。（T24）"""
        mock = _make_rpc_mock()
        mock.send_request.side_effect = pipeutil.TimeoutError("timeout")

        client = ReconnectingRpcPipeClient("test", max_retries=3, retry_interval_ms=0)
        client._impl = mock

        with pytest.raises(pipeutil.TimeoutError):
            client.send_request(Message(b"req"), timeout=1.0)

        mock.connect.assert_not_called()

    # T25: 再接続失敗 N 回で MaxRetriesExceededError
    def test_rpc_max_retries_exceeded(self) -> None:
        """再接続失敗 N 回で `MaxRetriesExceededError` が送出されること。（T25）"""
        mock = _make_rpc_mock(connected=False)
        mock.send_request.side_effect = pipeutil.ConnectionResetError("disconnected")
        mock.connect.side_effect = pipeutil.PipeError("server down")

        client = ReconnectingRpcPipeClient("test", max_retries=2, retry_interval_ms=0)
        client._impl = mock

        with pytest.raises(MaxRetriesExceededError) as exc_info:
            client.send_request(Message(b"req"))

        assert exc_info.value.attempts == 2
        assert mock.connect.call_count == 2


# ═══════════════════════════════════════════════════════════════════════
# コンストラクタ引数検証テスト（共通、仕様 §3.1）
# ═══════════════════════════════════════════════════════════════════════

class TestConstructorValidation:
    """コンストラクタの引数検証が正しく機能すること。"""

    @pytest.mark.parametrize("cls", [
        ReconnectingPipeClient,
        AsyncReconnectingPipeClient,
        ReconnectingRpcPipeClient,
    ])
    def test_negative_retry_interval(self, cls) -> None:
        with pytest.raises(ValueError, match="retry_interval_ms"):
            cls("test", retry_interval_ms=-1)

    @pytest.mark.parametrize("cls", [
        ReconnectingPipeClient,
        AsyncReconnectingPipeClient,
        ReconnectingRpcPipeClient,
    ])
    def test_negative_max_retries(self, cls) -> None:
        with pytest.raises(ValueError, match="max_retries"):
            cls("test", max_retries=-1)

    @pytest.mark.parametrize("cls", [
        ReconnectingPipeClient,
        AsyncReconnectingPipeClient,
        ReconnectingRpcPipeClient,
    ])
    def test_negative_connect_timeout(self, cls) -> None:
        with pytest.raises(ValueError, match="connect_timeout_ms"):
            cls("test", connect_timeout_ms=-1)


# ═══════════════════════════════════════════════════════════════════════
# プロパティ・MaxRetriesExceededError の基本テスト
# ═══════════════════════════════════════════════════════════════════════

class TestProperties:

    def test_pipe_name(self) -> None:
        client = ReconnectingPipeClient("my_pipe")
        assert client.pipe_name == "my_pipe"

    def test_retry_count_initial(self) -> None:
        client = ReconnectingPipeClient("my_pipe")
        assert client.retry_count == 0

    def test_max_retries_exceeded_is_pipe_error(self) -> None:
        err = MaxRetriesExceededError(3, Exception("last"))
        assert isinstance(err, pipeutil.PipeError)
        assert err.attempts == 3
        assert "3" in str(err)
