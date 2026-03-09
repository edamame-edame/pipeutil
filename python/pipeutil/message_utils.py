# python/pipeutil/message_utils.py
# 構造化メッセージ（JSON / msgpack）エンコード / デコードユーティリティ
# 仕様: spec/F005_structured_message.md
# 依存: stdlib json のみ（msgpack は遅延インポート）

from __future__ import annotations

import json as _json
from typing import Any

from ._pipeutil import Message, PipeError


# ─── 例外 ────────────────────────────────────────────────────────────

class CodecError(PipeError):
    """
    エンコード / デコード処理の失敗。

    Attributes
    ----------
    codec:
        失敗したコーデック名（"json" | "msgpack"）。
    original:
        原因となった元の例外（json.JSONDecodeError / msgpack.UnpackException など）。
    """

    def __init__(
        self,
        message: str,
        *,
        codec: str,
        original: Exception | None = None,
    ) -> None:
        super().__init__(message)
        self._codec = codec
        self._original = original

    @property
    def codec(self) -> str:
        """失敗したコーデック識別子を返す。"""
        return self._codec

    @property
    def original(self) -> Exception | None:
        """原因例外を返す。"""
        return self._original


# ─── msgpack 遅延インポートヘルパー ──────────────────────────────────

def _require_msgpack() -> Any:
    """
    msgpack をインポートして返す。

    Raises
    ------
    ImportError
        msgpack 未インストール時。`pip install pipeutil[msgpack]` を案内する。
    """
    try:
        import msgpack  # type: ignore[import-untyped]
        return msgpack
    except ImportError as exc:
        raise ImportError(
            "msgpack is required for encode_msgpack/decode_msgpack. "
            "Install it with: pip install pipeutil[msgpack]"
        ) from exc


# ─── JSON ────────────────────────────────────────────────────────────

def encode_json(
    data: Any,
    *,
    encoding: str = "utf-8",
    ensure_ascii: bool = False,
    **json_kwargs: Any,
) -> Message:
    """
    Python オブジェクトを JSON エンコードして Message を返す。

    Parameters
    ----------
    data:
        JSON シリアライズ可能なオブジェクト（dict / list / str / int / float / bool / None）。
    encoding:
        エンコーディング（デフォルト: utf-8）。
    ensure_ascii:
        True にすると ASCII 範囲外の文字を \\uXXXX でエスケープする。
    **json_kwargs:
        json.dumps() に渡す追加キーワード引数（indent / sort_keys / default など）。

    Returns
    -------
    Message
        JSON バイト列を含む Message。

    Raises
    ------
    pipeutil.CodecError
        JSON シリアライズ失敗時（非対応型 / 循環参照 / エンコードエラーなど）。

    Examples
    --------
    >>> msg = encode_json({"cmd": "ping", "seq": 1})
    >>> client.send(msg)
    """
    try:
        payload = _json.dumps(
            data,
            ensure_ascii=ensure_ascii,
            **json_kwargs,
        ).encode(encoding)
    except (TypeError, ValueError, UnicodeEncodeError) as exc:
        raise CodecError(
            f"JSON encode failed: {exc}",
            codec="json",
            original=exc,
        ) from exc
    return Message(payload)


def decode_json(
    msg: Message,
    *,
    encoding: str = "utf-8",
    **json_kwargs: Any,
) -> Any:
    """
    Message の payload を JSON デコードして Python オブジェクトを返す。

    Parameters
    ----------
    msg:
        JSON バイト列を含む Message。
    encoding:
        デコーディング（デフォルト: utf-8）。
    **json_kwargs:
        json.loads() に渡す追加キーワード引数。

    Returns
    -------
    Any
        デコードされた Python オブジェクト。

    Raises
    ------
    pipeutil.CodecError
        JSON パース失敗時（不正フォーマット / 文字コードエラーなど）。

    Examples
    --------
    >>> msg = server.receive(timeout_ms=3000)
    >>> d = decode_json(msg)
    >>> print(d["cmd"])  # "ping"
    """
    try:
        text = bytes(msg).decode(encoding)
        return _json.loads(text, **json_kwargs)
    except (UnicodeDecodeError, _json.JSONDecodeError, ValueError) as exc:
        raise CodecError(
            f"JSON decode failed: {exc}",
            codec="json",
            original=exc,
        ) from exc


# ─── msgpack ───────────────────────────────────────────────────────

def encode_msgpack(
    data: Any,
    *,
    use_bin_type: bool = True,
    **pack_kwargs: Any,
) -> Message:
    """
    Python オブジェクトを msgpack エンコードして Message を返す。

    Parameters
    ----------
    data:
        msgpack シリアライズ可能なオブジェクト。
    use_bin_type:
        True（デフォルト）にすると bytes を bin 型として扱う（msgpack>=1.0 のデフォルトと同じ）。
    **pack_kwargs:
        msgpack.packb() に渡す追加キーワード引数。

    Returns
    -------
    Message
        msgpack バイト列を含む Message。

    Raises
    ------
    ImportError
        msgpack がインストールされていない場合。`pip install pipeutil[msgpack]` を実行すること。
    pipeutil.CodecError
        シリアライズ失敗時。

    Examples
    --------
    >>> msg = encode_msgpack({"cmd": "ping", "seq": 1})
    >>> client.send(msg)
    """
    msgpack = _require_msgpack()
    try:
        payload: bytes = msgpack.packb(data, use_bin_type=use_bin_type, **pack_kwargs)
    except Exception as exc:  # msgpack.PackException はバージョン依存のため広く捕捉
        raise CodecError(
            f"msgpack encode failed: {exc}",
            codec="msgpack",
            original=exc,
        ) from exc
    return Message(payload)


def decode_msgpack(
    msg: Message,
    *,
    raw: bool = False,
    **unpack_kwargs: Any,
) -> Any:
    """
    Message の payload を msgpack デコードして Python オブジェクトを返す。

    Parameters
    ----------
    msg:
        msgpack バイト列を含む Message。
    raw:
        False（デフォルト）にすると bytes 型 → str にデコードする。
    **unpack_kwargs:
        msgpack.unpackb() に渡す追加キーワード引数。

    Returns
    -------
    Any
        デコードされた Python オブジェクト。

    Raises
    ------
    ImportError
        msgpack がインストールされていない場合。
    pipeutil.CodecError
        デシリアライズ失敗時（不正フォーマットなど）。

    Examples
    --------
    >>> msg = server.receive(timeout_ms=3000)
    >>> d = decode_msgpack(msg)
    >>> print(d["cmd"])
    """
    msgpack = _require_msgpack()
    try:
        return msgpack.unpackb(bytes(msg), raw=raw, **unpack_kwargs)
    except Exception as exc:  # msgpack.UnpackException はバージョン依存のため広く捕捉
        raise CodecError(
            f"msgpack decode failed: {exc}",
            codec="msgpack",
            original=exc,
        ) from exc
