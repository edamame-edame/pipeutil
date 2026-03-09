# tests/python/test_message_utils.py
# F-005: 構造化メッセージ（JSON / msgpack）ユーティリティのテスト
# 仕様: spec/F005_structured_message.md §8

import builtins
import importlib

import pytest

import pipeutil
from pipeutil import CodecError, Message


# ─── ヘルパー ─────────────────────────────────────────────────────────

def _msgpack_unavailable() -> bool:
    """msgpack がインストールされていない場合 True を返す。"""
    try:
        import msgpack  # noqa: F401
        return False
    except ImportError:
        return True


# ─── JSON エンコード / デコード ──────────────────────────────────────

def test_encode_json_dict():
    """dict を encode_json → decode_json で往復し元と一致することを確認する。"""
    d = {"cmd": "ping", "seq": 1}
    msg = pipeutil.encode_json(d)
    assert isinstance(msg, Message)
    result = pipeutil.decode_json(msg)
    assert result == d


def test_encode_json_non_ascii():
    """マルチバイト文字（日本語）が UTF-8 で正しく往復できることを確認する。"""
    d = {"msg": "日本語テスト", "value": 42}
    msg = pipeutil.encode_json(d)
    result = pipeutil.decode_json(msg)
    assert result["msg"] == "日本語テスト"
    assert result["value"] == 42


def test_encode_json_list():
    """list / 混合型が encode_json → decode_json で正しく往復できることを確認する。"""
    lst = [1, "two", 3.0, None, True]
    msg = pipeutil.encode_json(lst)
    result = pipeutil.decode_json(msg)
    assert result == lst


def test_encode_json_error():
    """JSON シリアライズ不可能な型が CodecError (codec='json') を送出することを確認する。"""
    with pytest.raises(CodecError) as exc_info:
        pipeutil.encode_json({"key": object()})
    assert exc_info.value.codec == "json"
    assert exc_info.value.original is not None


def test_decode_json_error():
    """不正な JSON バイト列が CodecError (codec='json') を送出することを確認する。"""
    bad_msg = Message(b"not-json{{{{")
    with pytest.raises(CodecError) as exc_info:
        pipeutil.decode_json(bad_msg)
    assert exc_info.value.codec == "json"


# ─── msgpack エンコード / デコード ────────────────────────────────────

@pytest.mark.skipif(
    _msgpack_unavailable(),
    reason="msgpack not installed — run: pip install pipeutil[msgpack]",
)
def test_encode_msgpack_dict():
    """dict を encode_msgpack → decode_msgpack で往復し元と一致することを確認する。"""
    d = {"cmd": "ping", "seq": 1}
    msg = pipeutil.encode_msgpack(d)
    assert isinstance(msg, Message)
    result = pipeutil.decode_msgpack(msg)
    assert result == d


@pytest.mark.skipif(
    _msgpack_unavailable(),
    reason="msgpack not installed — run: pip install pipeutil[msgpack]",
)
def test_encode_msgpack_binary():
    """bytes 型が use_bin_type=True で正しく往復できることを確認する。"""
    d = {"data": b"\x00\x01\xfe\xff"}
    msg = pipeutil.encode_msgpack(d)
    result = pipeutil.decode_msgpack(msg)
    assert result["data"] == d["data"]


def test_msgpack_import_error(monkeypatch: pytest.MonkeyPatch):
    """msgpack が存在しない場合 ImportError を送出し、インストール案内が含まれることを確認する。"""
    real_import = builtins.__import__

    def mock_import(name: str, *args, **kwargs):  # type: ignore[no-untyped-def]
        if name == "msgpack":
            raise ImportError("No module named 'msgpack'")
        return real_import(name, *args, **kwargs)

    monkeypatch.setattr(builtins, "__import__", mock_import)

    # _require_msgpack のキャッシュ回避のためリロードする
    import pipeutil.message_utils as mu
    importlib.reload(mu)

    with pytest.raises(ImportError, match=r"pip install pipeutil\[msgpack\]"):
        mu.encode_msgpack({"x": 1})
