# tests/python/test_message.py — pipeutil.Message 単体テスト

import pytest
import pipeutil


class TestMessageConstruction:
    """Message の構築・内容アクセスに関するテスト。"""

    def test_default_construct_is_empty(self):
        msg = pipeutil.Message(b"")
        assert len(msg) == 0

    def test_from_bytes_roundtrip(self):
        msg = pipeutil.Message(b"hello")
        assert bytes(msg) == b"hello"
        assert len(msg) == 5

    def test_from_bytes_single_byte(self):
        msg = pipeutil.Message(b"\xAB")
        assert bytes(msg) == b"\xAB"

    def test_from_bytes_null_bytes_preserved(self):
        """null バイトを含むペイロードがそのまま保持されること（バイナリ安全）。"""
        raw = b"ab\x00cd"
        msg = pipeutil.Message(raw)
        assert bytes(msg) == raw
        assert len(msg) == 5

    def test_large_payload_64kib(self):
        raw = bytes(range(256)) * 256  # 65536 bytes
        msg = pipeutil.Message(raw)
        assert len(msg) == 65536
        assert bytes(msg) == raw

    def test_type_error_on_str_input(self):
        """str を直接渡したときは TypeError になること（bytes を要求する API）。"""
        with pytest.raises(TypeError):
            pipeutil.Message("文字列")  # type: ignore[arg-type]


class TestMessageEquality:
    """同一内容の Message は等価であること（実装依存：__eq__ 実装があれば）。"""

    def test_same_content_bytes_equal(self):
        a = pipeutil.Message(b"abc")
        b = pipeutil.Message(b"abc")
        assert bytes(a) == bytes(b)

    def test_different_content_bytes_not_equal(self):
        a = pipeutil.Message(b"abc")
        b = pipeutil.Message(b"xyz")
        assert bytes(a) != bytes(b)
