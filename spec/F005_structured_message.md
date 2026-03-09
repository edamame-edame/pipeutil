# F-005 詳細設計: 構造化メッセージ（JSON / msgpack）

**作成日**: 2026-03-09
**対象バージョン**: v0.5.0
**依存**: なし（C++ コア変更不要）

---

## 1. 現状と課題

### 1.1 ユーザーコードの煩雑さ

現在の `Message` は生バイト列のみを保持する。辞書を送受信するには毎回手動でシリアライズ処理が必要。

```python
# 現状 — 毎回この変換ボイラープレートが必要
import json
import pipeutil

# 送信
d = {"cmd": "ping", "seq": 1}
payload = json.dumps(d, ensure_ascii=False).encode("utf-8")
client.send(pipeutil.Message(payload))

# 受信
msg = server.receive(timeout_ms=3000)
d = json.loads(bytes(msg).decode("utf-8"))
```

### 1.2 スコープ定義

| 項目 | 対応 |
|---|---|
| JSON エンコード/デコード | ✅ 必須（stdlib `json` 使用、追加依存なし） |
| msgpack エンコード/デコード | ✅ サポート（optional 依存: `pip install pipeutil[msgpack]`） |
| C++ コア変更 | ❌ 不要（Python レイヤーのみ） |
| `Message` クラスのメソッド追加 | ❌ 不要（`Message` は C 拡張型のため monkey-patch 不可） |
| MessagePack スキーマ定義 | 対象外（v0.5.0 では生 dict のみ） |

---

## 2. アーキテクチャ概観

```
python/pipeutil/
  message_utils.py     ← 新規: encode_json / decode_json / encode_msgpack / decode_msgpack
  __init__.py          ← 更新: message_utils の public API を re-export
  __init__.pyi         ← 更新: 型スタブ追加

tests/python/
  test_message_utils.py  ← 新規: 8 件
```

### 2.1 設計原則

1. **stdlib 優先**: JSON は `import json` のみ使用し、追加依存なし
2. **opt-in 依存**: msgpack は `ImportError` を実行時に遅延チェック（import 時エラーにしない）
3. **ゼロコピー志向**: `Message` → bytes 変換は必要最低限にとどめる
4. **`Message` 非拡張**: `Message` は C 拡張型のため Python からメソッドを追加することはできない。
   ユーティリティ関数として提供し、`pipeutil` 名前空間から直接呼び出せる形にする。
5. **冪等・純粋**: 各関数は副作用なし。エンコード結果の `Message` は既存の `Message` コンストラクタで生成する。

---

## 3. API 設計

### 3.1 モジュールレベル関数（`pipeutil.message_utils`）

```python
# python/pipeutil/message_utils.py

from __future__ import annotations
from typing import Any
import json as _json
from ._pipeutil import Message

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
        json.dumps() に渡す追加キーワード引数（indent / sort_keys など）。

    Returns
    -------
    Message
        JSON バイト列を含む Message。

    Raises
    ------
    pipeutil.CodecError
        JSON シリアライズ失敗時（非対応型 / 循環参照など）。

    Examples
    --------
    >>> msg = encode_json({"cmd": "ping", "seq": 1})
    >>> client.send(msg)
    """
    ...


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
    ...


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
        True（デフォルト）にすると bytes を bin 型として扱う。
    **pack_kwargs:
        msgpack.packb() に渡す追加キーワード引数。

    Raises
    ------
    ImportError
        msgpack がインストールされていない場合。
        `pip install pipeutil[msgpack]` を実行すること。
    pipeutil.CodecError
        シリアライズ失敗時。
    """
    ...


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

    Raises
    ------
    ImportError
        msgpack がインストールされていない場合。
    pipeutil.CodecError
        デシリアライズ失敗時（不正フォーマットなど）。
    """
    ...
```

### 3.2 `pipeutil` 名前空間への公開

```python
# __init__.py に追加
from .message_utils import (   # noqa: F401
    encode_json,
    decode_json,
    encode_msgpack,
    decode_msgpack,
    CodecError,
)
```

利用側シンタックス:

```python
import pipeutil

# JSON
msg = pipeutil.encode_json({"cmd": "ping", "seq": 1})
client.send(msg)

resp = server.receive(timeout_ms=3000)
data = pipeutil.decode_json(resp)
assert data["cmd"] == "pong"

# msgpack（要 pip install pipeutil[msgpack]）
msg = pipeutil.encode_msgpack({"cmd": "ping", "seq": 1})
data = pipeutil.decode_msgpack(resp)
```

### 3.3 利用パターン集

#### RPC サーバーとの組み合わせ

```python
import pipeutil

def rpc_handler(server: pipeutil.RpcPipeServer) -> None:
    while True:
        try:
            req_msg = server.receive(timeout_ms=100)
        except pipeutil.TimeoutError:
            continue
        req = pipeutil.decode_json(req_msg)
        result = process(req)
        server.send(pipeutil.encode_json(result))
```

#### asyncio との組み合わせ

```python
import pipeutil
import pipeutil.aio as aio

async def handle(client: aio.AsyncPipeServer) -> None:
    raw = await client.receive(timeout_ms=3000)
    req = pipeutil.decode_json(raw)
    resp = pipeutil.encode_json({"status": "ok", "echo": req})
    await client.send(resp)
```

#### msgpack ストリーム送信（バイナリ効率重視）

```python
import pipeutil

# msgpack は JSON より小さい場合が多い（バイナリキーなし）
frame = pipeutil.encode_msgpack({"img": bytes(1024)})
print(len(frame))   # 生バイトのまま送信
client.send(frame)
```

---

## 4. エラー設計

### 4.1 `CodecError` 例外クラス

```python
class CodecError(pipeutil.PipeError):
    """
    エンコード/デコード処理の失敗。

    Attributes
    ----------
    codec:
        失敗したコーデック名（"json" | "msgpack"）。
    original:
        原因となった元の例外（json.JSONDecodeError / msgpack.UnpackException など）。
    """

    def __init__(self, message: str, *, codec: str, original: Exception | None = None) -> None: ...

    @property
    def codec(self) -> str: ...

    @property
    def original(self) -> Exception | None: ...
```

### 4.2 例外連鎖方針

| 原因 | 発生する例外 | `__cause__` |
|---|---|---|
| `json.dumps` 失敗 | `CodecError` | `TypeError` / `ValueError` |
| `json.loads` 失敗 | `CodecError` | `json.JSONDecodeError` |
| `UnicodeDecodeError` | `CodecError` | `UnicodeDecodeError` |
| `msgpack.packb` 失敗 | `CodecError` | `msgpack.PackException` |
| `msgpack.unpackb` 失敗 | `CodecError` | `msgpack.UnpackException` |
| `msgpack` 未インストール | `ImportError` | （そのまま re-raise、`CodecError` でラップしない） |

`ImportError` は `CodecError` でラップしない。ユーザーが `pip install pipeutil[msgpack]` をすべきことを直接伝えるため。

```python
# 利用側エラーハンドリング例
try:
    data = pipeutil.decode_json(msg)
except pipeutil.CodecError as e:
    print(f"デコード失敗 [{e.codec}]: {e}")
    print(f"  原因: {e.original!r}")
```

---

## 5. 内部実装方針

### 5.1 `message_utils.py` 実装スケルトン

```python
# python/pipeutil/message_utils.py
from __future__ import annotations

import json as _json
from typing import Any

from ._pipeutil import Message, PipeError


class CodecError(PipeError):
    """エンコード/デコード失敗例外。"""

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
        return self._codec

    @property
    def original(self) -> Exception | None:
        return self._original


# ─── JSON ────────────────────────────────────────────────────────────

def encode_json(
    data: Any,
    *,
    encoding: str = "utf-8",
    ensure_ascii: bool = False,
    **json_kwargs: Any,
) -> Message:
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

def _require_msgpack() -> Any:
    """msgpack をインポートして返す。未インストール時は ImportError を送出。"""
    try:
        import msgpack  # type: ignore[import-untyped]
        return msgpack
    except ImportError as exc:
        raise ImportError(
            "msgpack is required for encode_msgpack/decode_msgpack. "
            "Install it with: pip install pipeutil[msgpack]"
        ) from exc


def encode_msgpack(
    data: Any,
    *,
    use_bin_type: bool = True,
    **pack_kwargs: Any,
) -> Message:
    msgpack = _require_msgpack()
    try:
        payload: bytes = msgpack.packb(data, use_bin_type=use_bin_type, **pack_kwargs)
    except Exception as exc:   # msgpack.PackException はバージョン依存
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
    msgpack = _require_msgpack()
    try:
        return msgpack.unpackb(bytes(msg), raw=raw, **unpack_kwargs)
    except Exception as exc:
        raise CodecError(
            f"msgpack decode failed: {exc}",
            codec="msgpack",
            original=exc,
        ) from exc
```

### 5.2 bytes(msg) の根拠

`Message` は `__bytes__` を実装しているため `bytes(msg)` でペイロードのコピーを取得できる。
`msg.data` プロパティも同様。いずれもメモリコピーは 1 回のみ発生する。

パフォーマンスクリティカルな用途では、`bytearray` や `memoryview` を直接渡すパターンは
現バージョンでは対象外（v0.6.0 以降で検討）。

---

## 6. 型スタブ（`__init__.pyi` 更新）

`CodecError` は `PipeError` を継承するため、`__init__.pyi` の既存例外階層セクション
（`class PipeError(Exception): ...` 以降）に追加する。
`PipeError` 定義前に差し込まないこと。

```python
# __init__.pyi に追加するセクション

# ─── Codec ────────────────────────────────────────────────────────────

class CodecError(PipeError):
    """エンコード/デコード失敗例外。"""

    def __init__(
        self,
        message: str,
        *,
        codec: str,
        original: BaseException | None = ...,
    ) -> None: ...

    @property
    def codec(self) -> str: ...

    @property
    def original(self) -> BaseException | None: ...


# ─── message_utils 関数 ───────────────────────────────────────────────

from typing import Any

def encode_json(
    data: Any,
    *,
    encoding: str = ...,
    ensure_ascii: bool = ...,
    **json_kwargs: Any,
) -> Message: ...

def decode_json(
    msg: Message,
    *,
    encoding: str = ...,
    **json_kwargs: Any,
) -> Any: ...

def encode_msgpack(
    data: Any,
    *,
    use_bin_type: bool = ...,
    **pack_kwargs: Any,
) -> Message: ...

def decode_msgpack(
    msg: Message,
    *,
    raw: bool = ...,
    **unpack_kwargs: Any,
) -> Any: ...
```

---

## 7. `pyproject.toml` 変更

```toml
[project.optional-dependencies]
test    = ["pytest>=8.0", "pytest-timeout>=2.3"]
async   = ["pytest-asyncio>=0.23"]
msgpack = ["msgpack>=1.0"]                        # 追加

# 開発フルセット
dev     = [
    "pytest>=8.0",
    "pytest-timeout>=2.3",
    "pytest-asyncio>=0.23",
    "msgpack>=1.0",
]
```

msgpack のバージョン要件（`>=1.0`）の根拠:
- `msgpack 1.0.0` (2020-09) で `use_bin_type` のデフォルトが `True` に変更された
- `encoding` 引数が削除されるなど API が安定化した
- Python 3.8 以降のみサポート

---

## 8. テスト設計（`tests/python/test_message_utils.py`）

| # | テスト名 | 内容 | 期待結果 |
|---|---|---|---|
| 1 | `test_encode_json_dict` | `encode_json({"cmd": "ping"})` → 送信 → `decode_json` で復元 | 元の dict と一致 |
| 2 | `test_encode_json_non_ascii` | `encode_json({"msg": "日本語"})` | UTF-8 で正しく往復 |
| 3 | `test_encode_json_list` | `encode_json([1, "two", 3.0])` | `decode_json` で復元 |
| 4 | `test_encode_json_error` | `encode_json({"key": object()})` | `CodecError` (codec="json") |
| 5 | `test_decode_json_error` | 不正 JSON バイト列の `decode_json` | `CodecError` (codec="json") |
| 6 | `test_encode_msgpack_dict` | `encode_msgpack({"cmd": "ping"})` → `decode_msgpack` | 元の dict と一致 |
| 7 | `test_encode_msgpack_binary` | `encode_msgpack({"data": b"\x00\xff"})` | bytes 往復で一致 |
| 8 | `test_msgpack_import_error` | msgpack 未インストール状態での呼び出し（モック）| `ImportError` |

### 8.1 テストコード例

```python
# tests/python/test_message_utils.py
import pytest
import pipeutil
from pipeutil import CodecError, Message


def _msgpack_unavailable() -> bool:
    try:
        import msgpack  # noqa: F401
        return False
    except ImportError:
        return True


def test_encode_json_dict():
    d = {"cmd": "ping", "seq": 1}
    msg = pipeutil.encode_json(d)
    assert isinstance(msg, Message)
    result = pipeutil.decode_json(msg)
    assert result == d


def test_encode_json_non_ascii():
    d = {"msg": "日本語テスト", "value": 42}
    msg = pipeutil.encode_json(d)
    result = pipeutil.decode_json(msg)
    assert result["msg"] == "日本語テスト"


def test_encode_json_list():
    lst = [1, "two", 3.0, None, True]
    msg = pipeutil.encode_json(lst)
    assert pipeutil.decode_json(msg) == lst


def test_encode_json_error():
    with pytest.raises(CodecError) as exc_info:
        pipeutil.encode_json({"key": object()})
    assert exc_info.value.codec == "json"
    assert exc_info.value.original is not None


def test_decode_json_error():
    bad_msg = Message(b"not-json{{{{")
    with pytest.raises(CodecError) as exc_info:
        pipeutil.decode_json(bad_msg)
    assert exc_info.value.codec == "json"


@pytest.mark.skipif(
    _msgpack_unavailable(),
    reason="msgpack not installed",
)
def test_encode_msgpack_dict():
    d = {"cmd": "ping", "seq": 1}
    msg = pipeutil.encode_msgpack(d)
    assert isinstance(msg, Message)
    result = pipeutil.decode_msgpack(msg)
    assert result == d


@pytest.mark.skipif(
    _msgpack_unavailable(),
    reason="msgpack not installed",
)
def test_encode_msgpack_binary():
    d = {"data": b"\x00\x01\xfe\xff"}
    msg = pipeutil.encode_msgpack(d)
    result = pipeutil.decode_msgpack(msg)
    assert result["data"] == d["data"]


def test_msgpack_import_error(monkeypatch: pytest.MonkeyPatch):
    """msgpack が存在しない場合 ImportError を送出することを確認する。"""
    import sys
    import builtins

    real_import = builtins.__import__

    def mock_import(name: str, *args, **kwargs):
        if name == "msgpack":
            raise ImportError("No module named 'msgpack'")
        return real_import(name, *args, **kwargs)

    monkeypatch.setattr(builtins, "__import__", mock_import)
    # message_utils キャッシュをクリアしてモックが効くようにする
    import importlib
    import pipeutil.message_utils as mu
    importlib.reload(mu)

    with pytest.raises(ImportError, match="pip install pipeutil\\[msgpack\\]"):
        mu.encode_msgpack({"x": 1})
```

---

## 9. 後方互換性

| 項目 | 影響 |
|---|---|
| `Message` クラスの変更 | なし（C++ コア変更なし） |
| `pipeutil` 名前空間 | `encode_json` / `decode_json` / `encode_msgpack` / `decode_msgpack` / `CodecError` を追加（既存シンボルと競合なし） |
| `pyproject.toml` | `msgpack` optional extra を追加（既存 `test` / `async` に影響なし） |
| Import 時 msgpack なし | エラーにならない（遅延インポート） |

---

## 10. ファイル構成（コード変更一覧）

```
python/pipeutil/
  message_utils.py       新規: encode_json, decode_json, encode_msgpack, decode_msgpack,
                               CodecError
  __init__.py            更新: message_utils の public API を re-export
  __init__.pyi           更新: CodecError + 4 関数の型スタブ追加

tests/python/
  test_message_utils.py  新規: 8 件

pyproject.toml           更新: [msgpack] optional extra 追加
```

---

## 11. 実装タスク

| タスク | ファイル | 見積難易度 |
|---|---|:---:|
| `CodecError` クラス実装 | `message_utils.py` | 小 |
| `encode_json` / `decode_json` 実装 | `message_utils.py` | 小 |
| `encode_msgpack` / `decode_msgpack` 実装 | `message_utils.py` | 小 |
| `__init__.py` re-export 更新 | `__init__.py` | 小 |
| 型スタブ追加 | `__init__.pyi` | 小 |
| テスト 8 件作成 | `test_message_utils.py` | 小 |
| `pyproject.toml` optional extra 追加 | `pyproject.toml` | 最小 |
| `_msgpack_unavailable()` ヘルパー実装 | `test_message_utils.py` | 最小 |

**合計見積**: 半日程度（F-004 フェーズ 1 比で 1/10 未満）

---

## 12. 設計上の注意事項

### 12.1 `Message` に直接メソッドを追加しない理由

`Message` は C 拡張型（`tp_type` で定義）であり、Python から `Message.from_json = ...` のような
属性代入は `TypeError: cannot set 'from_json' attribute of immutable type 'pipeutil.Message'` となり失敗する。
Python 側でサブクラスを作る場合も `tp_new` のシグネチャ制約により複雑になるため、
本実装では「ユーティリティ関数 + 名前空間公開」アプローチを採用する。

将来的に `Message.from_json()` を実現したい場合は C++ 側での変更が必要になる（v0.6.0 候補）。

### 12.2 `json.dumps` の `default` 引数

`encode_json` は `**json_kwargs` を通じて `default` コールバックを受け付けるため、
`datetime` 等の非標準型も拡張できる。ただし本仕様ではデフォルトシリアライズ対象を
「JSON ネイティブ型（dict / list / str / int / float / bool / None）」に限定する。

### 12.3 msgpack バージョン依存

`msgpack < 1.0` では `use_bin_type=False` がデフォルトであり str/bytes の扱いが異なる。
`>=1.0` を要件とすることで一貫した動作を保証する。

### 12.4 スレッド安全性

各関数は純粋関数（グローバル状態を変更しない）のためスレッドセーフ。
msgpack のグローバルな `Packer` / `Unpacker` インスタンスは使用しない。
