# pipeutil 新機能提案書

**作成日**: 2026-02-28
**対象バージョン**: v0.2.0 以降
**現行バージョン**: v0.1.0

---

## 概要

v0.1.0 では「1対1・同期ブロッキング IPC」を確立した。
本提案書は、これまでの指摘・設計・実装を踏まえ、次バージョン以降に追加すべき機能を優先度順にまとめる。

---

## 提案一覧

| # | 機能名 | 優先度 | 実装難易度 | C++ 変更 | Python 変更 |
|---|---|:---:|:---:|:---:|:---:|
| F-001 | 多重クライアント対応（MultiPipeServer） | High | 中 | ✅ 新クラス | ✅ |
| F-002 | message_id / リクエスト–レスポンス RPC | High | 中 | ✅ フレーム拡張 | ✅ |
| F-003 | 自動再接続（ReconnectingPipeClient） | High | 小 | 不要 | ✅ ラッパー |
| F-004 | Python asyncio 対応 | Medium | 大 | △ IOCP 拡張 | ✅ |
| F-005 | 構造化メッセージ（JSON / msgpack） | Medium | 小 | 不要 | ✅ Python のみ |
| F-006 | 診断・メトリクス API | Medium | 小 | ✅ stats() | ✅ |
| F-007 | テストスイート（Google Test + pytest + CI） | Low | 中 | ✅ gtest | ✅ pytest |
| F-008 | Windows セキュリティ記述子 ACL | Low | 小 | ✅ Win32 のみ | 不要 |

---

## 詳細

### F-001 — 多重クライアント対応（MultiPipeServer）

#### 背景・課題
現在の `PipeServer` は `listen → accept → 1対1通信` のモデルであり、2件目の接続を受け付けるには
`close()` → `listen()` を手動でやり直す必要がある。同時に複数クライアントと通信できない。

#### 提案 API

**C++**
```cpp
// 接続ごとにスレッドを起動し、コールバックを呼ぶマルチスレッドサーバー
class PIPEUTIL_API MultiPipeServer {
public:
    using Handler = std::function<void(PipeServer /* per-connection */)>;

    explicit MultiPipeServer(std::string pipe_name,
                             std::size_t max_connections = 8,
                             std::size_t buffer_size    = 65536);

    /// ブロッキングでクライアントを受け付け続ける
    void serve(Handler on_connect);

    /// 受付を停止して全接続をクローズする（noexcept）
    void stop() noexcept;

    [[nodiscard]] std::size_t active_connections() const noexcept;
};
```

**Python**
```python
import pipeutil

def handle(conn: pipeutil.PipeServer) -> None:
    msg = conn.receive(timeout_ms=5000)
    conn.send(pipeutil.Message(b"pong"))

server = pipeutil.MultiPipeServer("my_pipe", max_connections=8)
server.serve(handle)  # 別スレッドで受付ループ開始
# ...
server.stop()
```

#### 実装方針
- Windows: `CreateNamedPipe` を最大接続数分事前作成し、`ConnectNamedPipe` + Overlapped I/O でペンディング
- Linux: `accept` ループをスレッドプールで処理
- 各接続は独立した `PipeServer` インスタンスとしてハンドラに渡す

---

### F-002 — message_id / リクエスト–レスポンス RPC

#### 背景・課題
現在のフレームに `message_id` フィールドがない。複数スレッドが並行して `send → receive` すると
応答が取り違えるため、非同期 RPC パターンが実質不可能。

#### 提案: フレームヘッダ拡張

**現行フレーム構造**
```
[magic:4][version:1][flags:1][payload_len:4][checksum:4][payload:N]
```

**拡張後**
```
[magic:4][version:1][flags:1][payload_len:4][message_id:4][checksum:4][payload:N]
```
- `message_id == 0` は「ID なし（後方互換）」

**C++ API**
```cpp
// クライアント: メッセージ送信 → 対応 ID の応答を待機
[[nodiscard]] Message PipeClient::send_request(
    const Message&            req,
    std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});

// サーバー: リクエストをハンドラで処理してレスポンスを自動送信
void PipeServer::on_request(
    std::function<Message(const Message&)> handler);
```

**Python**
```python
resp = client.send_request(pipeutil.Message(b'{"cmd":"ping"}'), timeout_ms=3000)
```

#### 実装方針
- `message_id` の採番は `std::atomic<uint32_t>` でスレッドセーフにインクリメント
- ID → promise のマップを `std::unordered_map` で管理し、受信スレッドが応答を振り分ける

---

### F-003 — 自動再接続（ReconnectingPipeClient）

#### 背景・課題
サーバー再起動時にクライアントプロセスを再起動せず復旧したい。
現在は `ConnectionReset` / `BrokenPipe` 例外をキャッチしてユーザーが再接続処理を実装する必要がある。

#### 提案 API（Python のみ、C++ コア変更なし）

```python
client = pipeutil.ReconnectingPipeClient(
    "my_pipe",
    retry_interval_ms=500,   # 再接続間隔
    max_retries=10,           # 0 = 無限リトライ
    on_reconnect=lambda: print("reconnected"),
)
with client:
    client.send(pipeutil.Message(b"data"))  # 切断時は自動再接続後に送信
```

#### 実装方針
- `PipeClient` のラッパーとして Python レイヤーで実装
- `send` / `receive` で `PipeException(ConnectionReset | BrokenPipe)` を捕捉し、
  `connect()` をリトライしてから再試行

---

### F-004 — Python asyncio 対応

#### 背景・課題
現在すべての I/O がスレッドブロッキング。`asyncio` アプリケーションに組み込む際は
`loop.run_in_executor()` のボイラープレートが必要で使い勝手が悪い。

#### 提案 API

```python
async with pipeutil.AsyncPipeServer("my_pipe") as server:
    await server.listen()
    await server.accept(timeout_ms=10000)
    msg = await server.receive(timeout_ms=5000)
    await server.send(pipeutil.Message(b"pong"))

async with pipeutil.AsyncPipeClient("my_pipe") as client:
    await client.connect(timeout_ms=3000)
    await client.send(pipeutil.Message(b"ping"))
    resp = await client.receive(timeout_ms=3000)
```

#### 実装方針（2段階）

| フェーズ | 実装 | 備考 |
|---|---|---|
| Phase 1 | `asyncio.get_event_loop().run_in_executor()` でスレッドオフロード | 即時着手可能 |
| Phase 2 | Windows: IOCP + `ProactorEventLoop` 直接統合 | 高スループット向け、C++ 変更あり |

Phase 1 は Python ラッパーのみで実装可能なため、先行してリリースできる。

---

### F-005 — 構造化メッセージ（JSON / msgpack）

#### 背景・課題
現在の `Message` は生バイト列。Python 側で辞書を送るたびに
`json.dumps(d).encode()` → `Message(...)` → `msg.as_string_view()` → `json.loads(...)` と変換が煩雑。

#### 提案 API（Python ラッパーのみ、C++ コア変更なし）

```python
# JSON（標準 json モジュール使用）
msg = pipeutil.Message.from_json({"cmd": "ping", "seq": 1})
data: dict = msg.as_json()

# msgpack（オプション依存: pip install pipeutil[msgpack]）
msg = pipeutil.Message.from_msgpack({"cmd": "ping", "seq": 1})
data: dict = msg.as_msgpack()
```

#### 実装方針
- `python/pipeutil/message_utils.py` として Python レイヤーで実装
- msgpack は optional 依存（`pyproject.toml` の `[project.optional-dependencies]`）

---

### F-006 — 診断・メトリクス API

#### 背景・課題
本番稼働時のデバッグ・パフォーマンス分析に、送受信カウント・バイト数・レイテンシが欲しい。
現状は外部計測するしかなく、ライブラリ内部の状態が見えない。

#### 提案 API

**C++**
```cpp
struct PIPEUTIL_API PipeStats {
    std::uint64_t             messages_sent     = 0;
    std::uint64_t             messages_received = 0;
    std::uint64_t             bytes_sent        = 0;
    std::uint64_t             bytes_received    = 0;
    std::uint64_t             errors            = 0;
    std::chrono::nanoseconds  avg_round_trip    = {};
};

// PipeClient / PipeServer に追加
[[nodiscard]] PipeStats stats() const noexcept;
void reset_stats()                  noexcept;
```

**Python**
```python
stats = client.stats()
print(stats.messages_sent, stats.bytes_received, stats.avg_round_trip_ns)
client.reset_stats()
```

#### 実装方針
- `std::atomic` カウンタで `send` / `receive` のたびに加算（ロックなし）
- ラウンドトリップは `send_request` 使用時のみ計測

---

### F-007 — テストスイート（Google Test + pytest + CI）

#### 背景・課題
現在ユニットテストが存在しない。レビューで指摘された以下の境界ケースの回帰テストが必要:
- `ConnectNamedPipe` 四分岐（R-011）
- `uint32_t` overflow guard（R-014）
- `GetOverlappedResult` 失敗時の例外変換（R-013）
- タイムアウトのデッドライン計算（R-015）

#### 提案構成

```
tests/
  cpp/
    test_message.cpp          # Message 単体テスト
    test_pipe_roundtrip.cpp   # send → receive ラウンドトリップ
    test_timeout.cpp          # タイムアウト境界
    test_error_mapping.cpp    # Win32 エラーコード → PipeErrorCode マッピング
    CMakeLists.txt
  python/
    test_message.py
    test_roundtrip.py
    test_reconnect.py
    conftest.py
```

#### CI（GitHub Actions）

```yaml
# .github/workflows/ci.yml
jobs:
  build-test-windows:
    runs-on: windows-latest
    steps:
      - cmake --preset vs-release
      - cmake --build --preset build-release
      - ctest --preset release
      - pip install pipeutil[test]
      - pytest tests/python/
```

---

### F-008 — Windows セキュリティ記述子 ACL

#### 背景・課題
現在 `PipeServer` は `SECURITY_ATTRIBUTES` なしで named pipe を作成しており、
デフォルト ACL（作成ユーザーのみ）に依存している。
サービスアカウント間通信や異なるユーザー権限のプロセス間通信で権限エラーが発生しうる。

#### 提案 API

**C++**
```cpp
enum class PipeAcl {
    Default,      // OS デフォルト（現行と同等）
    LocalSystem,  // SYSTEM + ローカルユーザー
    Everyone,     // ローカルマシン上の全ユーザー（注意: セキュリティリスク）
    Custom,       // カスタム SDDL 文字列で指定
};

// PipeServer コンストラクタ拡張
explicit PipeServer(std::string pipe_name,
                    std::size_t buffer_size = 65536,
                    PipeAcl     acl         = PipeAcl::Default,
                    std::string custom_sddl = "");
```

#### 実装方針
- Windows: `ConvertStringSecurityDescriptorToSecurityDescriptor`（SDDL 形式）を使用
- Linux: UNIX ソケットはファイルパーミッション（`chmod`）で制御
- `PipeAcl::Default` の場合は現行動作と完全に同じコードパスを通る（後方互換）

---

## 推奨着手順序

```
v0.2.0: F-003（自動再接続）+ F-005（JSON/msgpack）+ F-007（テストスイート）
         → コア変更なし、品質基盤の確立

v0.3.0: F-001（MultiPipeServer）+ F-002（message_id RPC）+ F-006（メトリクス）
         → コア拡張、フレーム仕様変更を伴う

v0.4.0: F-004（asyncio）+ F-008（Windows ACL）
         → 高度な I/O 統合、プラットフォーム固有対応
```

---

*このドキュメントは実装開始前に再レビューを行い、仕様書（`spec/`）へ展開すること。*
