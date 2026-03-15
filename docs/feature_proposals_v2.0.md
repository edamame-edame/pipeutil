# pipeutil v2.0.0 機能提案書

**作成日**: 2026-03-14  
**対象バージョン**: v2.0.0  
**前バージョン**: v1.0.0（リリース確認済み、Windows/Linux × Python 3.8-3.14 全通過）

---

## 1. 調査概要 — パイプ通信の実際の用途

ウェブ上のリファレンスから以下のユースケースを確認した。

| 用途カテゴリ | 具体例 | 出典 |
|---|---|---|
| ブラウザのプロセス間通信 | Chromium: ブラウザプロセス ↔ レンダラープロセス ↔ プラグインプロセス（非同期 + 同期 RPC） | chromium.org |
| データベースバルクロード | MySQL: `LOAD DATA INFILE '/tmp/namedPipe'` でストリーム圧縮解凍→DBインポート | Wikipedia |
| デバッガトランスポート | WinDbg: カーネルデバッグセッションのシリアル代替トランスポート | Wikipedia |
| VM シリアルエミュレーション | VMware Workstation: 仮想マシンのシリアルポートを名前付きパイプで expose | Wikipedia |
| .NET / C# IPC 標準 | System.IO.Pipes (.NET 3.5+): `NamedPipeServerStream` / `NamedPipeClientStream` による多クライアント対応 + なりすまし (impersonation) | Microsoft Learn |
| SQL Server 接続トランスポート | SQL Server: TCP/IP の代替 IPC チャネル | Wikipedia |
| ゲームエンジン / ツール連携 | Unity ↔ C++ ネイティブプラグイン、DCC ツール (Maya/Blender) ↔ プレビューエンジン | 業界標準パターン |
| AI 推論バックエンド | LLM/画像モデルを C++ バックエンドで実行し、Python/C# フロントエンドからストリームで受信 | 業界標準パターン |
| プロセスオーケストレーション | マイクロサービスの代替として同一ホスト内プロセス群のメッセージルーティング | 業界標準パターン |

---

## 2. v1.0.0 実装済み機能の確認

| # | 機能 | C++ | Python |
|---|---|:---:|:---:|
| F-001 | 多重クライアント (`MultiPipeServer`) | ✅ | ✅ |
| F-002 | RPC (`message_id` / `RpcPipeServer` / `RpcPipeClient`) | ✅ | ✅ |
| F-003 | 自動再接続 (`ReconnectingPipeClient`) | ✅ | ✅ |
| F-004 | asyncio 非同期対応 (Python) | ✅ | ✅ |
| F-005 | 構造化メッセージ (JSON) | — | ✅ |
| F-006 | 診断・メトリクス (`PipeStats`) | ✅ | ✅ |
| F-007 | テストスイート (Google Test + pytest) | ✅ | ✅ |
| F-008 | Windows セキュリティ記述子 ACL | ✅ | — |

---

## 3. v2.0.0 提案一覧

### 3.1 優先度マトリクス

| # | 機能名 | 優先度 | 実装難易度 | C++ 変更 | Python 変更 | 新言語 |
|---|---|:---:|:---:|:---:|:---:|:---:|
| L-001 | C# / .NET ラッパー | **High** | 中 | なし | なし | ✅ C# |
| L-002 | プロトコル互換仕様 + 参照実装 | **High** | 小 | なし | なし | ✅ C# / Java |
| F-009 | 並行 RPC（複数 in-flight リクエスト） | **High** | 中 | ✅ | ✅ | — |
| F-010 | ストリーミングメッセージモード | **High** | 大 | ✅ | ✅ | — |
| F-011 | Pub/Sub メッセージルーター | Medium（→ 別pkg 推奨） | 中 | ✅ | ✅ | — |
| F-012 | ハートビート / プロセス監視 | Medium | 小 | ✅ | ✅ | — |
| F-013 | メッセージ優先度キュー | Medium | 中 | ✅ | ✅ | — |
| L-003 | Java ラッパー（Java 17+） | Low | 中 | なし | なし | ✅ Java |
| L-004 | Rust クレート | Low | 大 | なし | なし | ✅ Rust |
| F-014 | TLS オーバーレイ（経路暗号化） | Low | 大 | ✅ | ✅ | — |

---

### 3.2 ユーザー向け導入コスト・バンドル分類

「`pip install pipeutil` だけで使える機能」と「別途大きな外部ライブラリや別言語ランタイムが必要な機能」を事前に整理し、メインパッケージのインストール負担を最小限に保つ。

**区分の定義**

| 区分 | 基準 |
|---|---|
| **コア同梱** | 外部バイナリ依存なし。Python 標準ライブラリと C++ STL のみで完結。`pip install pipeutil` で即時利用可能。 |
| **別 PyPI パッケージ** | 大きな動的ライブラリ（OpenSSL 等）を C++ ビルド時にリンクする必要があり、メイン wheel に含めると全ユーザーに強制インストールされる。別途 `pip install pipeutil-xxx` が必要。 |
| **言語固有パッケージ** | Python と無関係な実行環境（.NET / JVM / Rust ツールチェーン）を前提とする。Python ユーザーへの影響ゼロ。 |

**各機能の分類**

| # | 機能名 | ユーザー側の前提依存 | 配布形態 | 備考 |
|---|---|---|---|---|
| F-009 | 並行 RPC | なし（C++ STL のみ） | **コア同梱** `pipeutil` | `pip install pipeutil` で即利用 |
| F-010 | ストリーミング | なし（C++ STL のみ） | **コア同梱** `pipeutil` | `pip install pipeutil` で即利用 |
| F-011 | Pub/Sub ルーター | なし（C++ STL のみ） | **別パッケージ** `pipeutil-broker` | レビュー V200-003: transport core 外を推奨。`pip install pipeutil-broker` |
| F-012 | ハートビート | なし（C++ STL のみ） | **コア同梱** `pipeutil` | `pip install pipeutil` で即利用 |
| F-013 | 優先度キュー | なし（C++ STL のみ） | **コア同梱** `pipeutil` | `pip install pipeutil` で即利用 |
| L-002 | プロトコル互換仕様 | なし（ドキュメント + テストのみ） | **コア同梱** `pipeutil` | interop テスト追加のみ |
| L-001 | C# ラッパー | .NET ランタイム（C# ユーザー向け） | **言語固有** `PipeUtil.NET`（NuGet） | Python ユーザーへの影響なし |
| L-003 | Java ラッパー | JVM 17+（Java ユーザー向け、標準 UDS 対応） | **言語固有** `pipeutil-java`（Maven） | Python ユーザーへの影響なし |
| L-004 | Rust クレート | なし（完全静的ビルド） | **言語固有** `pipeutil-rs`（crates.io） | Python ユーザーへの影響なし |
| **F-014** | **TLS オーバーレイ** | **OpenSSL / WolfSSL（システム DLL / .so）** | **別 PyPI パッケージ** `pipeutil-tls` | ▼ 次節参照 |

#### F-014 (TLS) を別 PyPI パッケージにする根拠

TLS 対応には OpenSSL / WolfSSL を C++ ビルドにリンクする必要がある。メインの `pipeutil` wheel に含めた場合の問題点:

- TLS を使わない全ユーザーに OpenSSL バイナリ（数 MB〜十数 MB）が強制インストールされる。
- Windows ではシステム OpenSSL が存在しないため、wheel 内に `libssl.dll` / `libcrypto.dll` を同梱（または vcredist 依存）する必要があり、配布サイズと保守コストが跳ね上がる。
- manylinux wheel で `libssl.so` のシンボルバージョンが Linux ディストリビューション間で異なり、CI ビルドマトリクスが複雑化する。

**配布方針**: `pipeutil-tls` を独立した PyPI パッケージとする。

```
pip install pipeutil          # TLS なし（デフォルト）
pip install pipeutil-tls      # TLS 対応を追加する場合のみ
```

`pipeutil-tls` は `pipeutil` に依存し、`pipeutil.tls.TlsPipeClient` / `pipeutil.tls.TlsPipeServer` のみを追加エクスポートする。C++ 側も `pipeutil_tls.dll` / `libpipeutil_tls.so` として分離し、コア DLL 側の OpenSSL 依存を完全に排除する。

---

## 4. 詳細仕様

---

### L-001 — C# / .NET ラッパー

#### 背景

- .NET 3.5+ から `System.IO.Pipes` が標準搭載。`NamedPipeServerStream` / `NamedPipeClientStream` で Windows Named Pipe / Linux UDS を抽象化している。  
- pipeutil のフレームフォーマット（`"PIPE"` マジック + CRC-32C）を C# 側でも実装することで、**C++ プロセス ↔ C# プロセス** の異言語 IPC が実現できる。  
- Unity ゲームエンジン（C# スクリプティング）、.NET バックオフィスサービス、WPF/MAUI ツールとのブリッジで特に需要が高い。

#### 設計方針

- **C++ コアを直接 DLL 呼び出しはしない**（PInvoke 依存は避ける）。  
- **C# 側で pipeutil フレームプロトコルを独自実装**する純粋 C# ライブラリ（`PipeUtil.NET`）として構成する。  
- プロジェクト構成: `source/csharp/` 配下に独立した .NET Standard 2.0 プロジェクト（NuGet 配布可能）を置く。  
- 通信相手が pipeutil C++ / Python サーバーでも C# サーバーでも透過的に通信できる。

#### 提案 API (C#)

```csharp
// C# クライアント側: C++ の PipeServer に接続して送受信
using PipeUtil;

var client = new PipeClient("my_pipe");
await client.ConnectAsync(timeout: TimeSpan.FromSeconds(5));

var msg = PipeMessage.FromBytes(Encoding.UTF8.GetBytes("hello"));
await client.SendAsync(msg);

var reply = await client.ReceiveAsync();
Console.WriteLine(Encoding.UTF8.GetString(reply.Payload));

await client.CloseAsync();
```

```csharp
// C# サーバー側: Python/C++ クライアントからの接続を受け付ける
using PipeUtil;

var server = new PipeServer("my_pipe");
server.OnClientConnected += async (conn) =>
{
    var msg = await conn.ReceiveAsync();
    await conn.SendAsync(PipeMessage.FromBytes(msg.Payload)); // echo
};
await server.ListenAsync();
```

#### フレームマッピング

| フィールド | pipeutil C++ | C# 実装 |
|---|---|---|
| magic | `{0x50,0x49,0x50,0x45}` | `byte[] { (byte)'P', (byte)'I', (byte)'P', (byte)'E' }` |
| version | `0x01`（初期実装） / `0x02`（v2 公開後に追加） | `const byte Version = 0x01` から開始し、v2 正式公開後に `0x02` を追加対応 |
| checksum | CRC-32C (ハードウェア支援) | `System.IO.Hashing.Crc32C` (.NET 6+) か自前実装 |
| バイトオーダー | リトルエンディアン | `BinaryPrimitives.ReadUInt32LittleEndian` |

#### 実装ファイル構成

```
source/csharp/
  PipeUtil.NET/
    PipeMessage.cs          # フレームシリアライズ / デシリアライズ
    PipeClient.cs           # クライアント接続・送受信
    PipeServer.cs           # サーバー accept ループ
    PipeStats.cs            # メトリクス
    Crc32C.cs               # CRC-32C 実装（.NET Standard 2.0 互換）
    PipeUtil.NET.csproj
  tests/
    PipeUtil.Tests/
      PipeProtocolTests.cs  # フレームエンコード・デコード
      PipeRoundtripTests.cs # C# ↔ C# 通信
```

---

### L-002 — プロトコル互換仕様書 + 参照実装

#### 背景

- 現在プロトコル仕様は `spec/02_protocol.md` に記述されているが、他言語実装者向けの「最低限の互換実装手順書」がない。  
- 参照実装（C# 、Java）を用意することで、将来の言語ブリッジコストを大幅に下げられる。

#### 成果物

| ファイル | 内容 |
|---|---|
| `spec/PROTOCOL_COMPAT.md` | フレームフォーマット詳細、CRC-32C アルゴリズム、バイトオーダー、エラーコード、バージョンネゴシエーション手順 |
| `source/csharp/PipeUtil.NET/` | C# 参照実装（L-001 と共通） |
| `source/java/` | Java 参照実装（最小 Maven プロジェクト） |
| `tests/interop/` | Python ↔ C# 、Python ↔ Java 相互通信テスト |

---

### F-009 — 並行 RPC（複数 in-flight リクエスト）

#### 背景

- v1.0.0 の `RpcPipeClient` は **1 リクエスト完了後に次を送る逐次モデル**。  
- Chromium の IPC 設計（複数 `routing_id` を管理する非同期チャネル）、AI 推論クラスタ（複数モデル並行呼び出し）では、同一接続で複数リクエストを同時に in-flight にする必要がある。

#### 提案変更点

> ⚠️ **実装前の必須要件（レビュー V200-006）**: 以下の契約を先に仕様化すること。定義を省略すると v1.0.0 で解決済みの async lifecycle 問題を再発しやすい。
> - `max_inflight_requests` 上限（キュー overflow 時のエラーコード）
> - timeout 伝播と orphan response の後始末
> - `cancel(message_id)` の明示 API と完了契約

- `flags` フィールドのビット 3 を `FLAG_CONCURRENT_RPC` として定義（フレーム v2 への移行）。  
- `message_id` を 4 バイト → 8 バイト（`uint64_t`）に拡張し、Section 6 の canonical 定義に従って v2 ヘッダ全体を 24 バイトへ統一する（`static_assert` 含む図・表・構造体を同時更新）。  
- C++ 側: `ConcurrentRpcClient` クラスを追加（内部で `std::map<uint64_t, std::promise>` を保持）。
- Python 側: `asyncio` ベースの `AsyncRpcClient` を拡張。

```cpp
// C++: 複数リクエストを並行送信し、それぞれの結果を Future で待つ
pipeutil::ConcurrentRpcClient client("inference_server");
client.connect();

auto f1 = client.call_async(request_a);  // 結果待ちなしに次の呼び出しへ
auto f2 = client.call_async(request_b);

auto result_a = f1.get();  // ここで初めてブロック
auto result_b = f2.get();
```

---

### F-010 — ストリーミングメッセージモード

#### 背景

> ⚠️ **実装前評価必須（レビュー V200-002）**: 現在のストリーミング非対応の主因は固定 64 KB 受信バッファ（実装制約）であり、wire format の限界ではない。プロトコル v2 に移行する前に、**v1 互換の large payload / chunked read 対応で要件を満たせるか検証**すること。

- 現在のフレームは `payload_size` が `uint32_t`（最大 4 GB）だが、実際には受信バッファが固定サイズ（デフォルト 64 KB）のためラージペイロードに非対応。  
- MySQL 名前付きパイプ経由のバルクロード、LLM ストリーミング応答（トークン逐次受信）、動画フレームストリームなど、**実質無制限の逐次チャンク転送**が求められる。

#### 設計方針

- `flags` バイトには stream 用ビットは追加しない。`FLAG_STREAM_CHUNK` ・ `FLAG_STREAM_END` は、v2 ヘッダの専用バイト（`priority_stream`バイト； Section 6 参照）に割り当てる。  
- ストリーム ID（`stream_id: uint32_t`）をヘッダに追加。F-009 の `message_id`（8バイト）と合わせてヘッダ全体を 24 バイトに揃える（Section 6 参照）。  
- C++ 側: `StreamPipeWriter` / `StreamPipeReader` クラス（`std::streambuf` 派生）を追加。  
- Python 側: 非同期ジェネレーター `async for chunk in conn.stream_receive():` API。

```python
# Python: LLM トークンを逐次受信するストリーム API
async for chunk in client.stream_receive(stream_id=1):
    print(chunk.decode(), end="", flush=True)
```

---

### F-011 — Pub/Sub メッセージルーター

> ⚠️ **配布形態（レビュー V200-003）**: Pub/Sub 機能は transport core (`pipeutil`) には含めず、**`pipeutil-broker` 別パッケージ**に分離して実装する。以下の設計は `pipeutil-broker` のものとして読むこと。

#### 背景

- ゲームエンジンのイベントバス、プロセスオーケストレーター（複数ワーカーへのタスク配布）、リアルタイムダッシュボード（複数購読者へのブロードキャスト）で需要が高い。  
- 現在の `MultiPipeServer` は「接続→ハンドラ」の 1 対 1 ディスパッチ。「トピック」で送信先をフィルタリングする仕組みがない。

#### 提案 API

```python
# Publisher
broker = pipeutil.MessageBroker("broker_pipe")
broker.start()

broker.publish("metrics", pipeutil.Message(b'{"cpu": 0.8}'))
broker.publish("alerts",  pipeutil.Message(b'critical'))
```

```python
# Subscriber
sub = pipeutil.PipeSubscriber("broker_pipe")
sub.subscribe("metrics")

for msg in sub.iter(timeout_ms=1000):
    print(msg.payload)
```

```cpp
// C++: トピックサブスクライバー
pipeutil::MessageBroker broker("broker_pipe");
broker.subscribe("alerts", [](const Message& m) {
    std::cerr << "ALERT: " << m.payload_string() << "\n";
});
broker.start();
```

---

### F-012 — ハートビート / プロセス監視

#### 背景

- クライアントプロセスが突然終了（SIGKILL、クラッシュ）すると、サーバー側は次の `receive()` タイムアウトまで気づけない。  
- WinDbg のようなデバッガトランスポートや長時間稼働するサービスでは、**デッドコネクションの早期検出**が不可欠。

#### 設計

- `flags` の `FLAG_HEARTBEAT = 0x02` を使用（現在 `FLAG_ACK` として将来拡張中）。  
- サーバーが定期的に PING 送信 → クライアントが PONG 返却 → タイムアウトで Dead 判定。  
- C++ 側: `HeartbeatConfig { interval_ms, timeout_ms }` オプションを `PipeServer` / `PipeClient` の両コンストラクタに追加。  
- Python 側: `heartbeat_interval_ms=` キーワード引数。

```python
server = pipeutil.PipeServer("my_pipe",
                             heartbeat_interval_ms=5000,
                             heartbeat_timeout_ms=15000)
server.on_client_dead = lambda client_id: print(f"Client {client_id} died")
```

---

### F-013 — メッセージ優先度キュー

#### 背景

- 現在のパイプは FIFO 保証のみ。制御メッセージ（HEARTBEAT、SHUTDOWN、緊急アラート）が大量データの後ろに詰まって遅延するのは問題。  
- Boost.Interprocess の `message_queue` は優先度対応済み。pipeutil も同様の機能を持つべき。

#### 設計

- v2 ヘッダの専用バイト（`priority_stream` バイト; Section 6 参照）の上位 3 ビット（bit7−5）を `priority` フィールド（0−7の 3 bit）として定義。`flags` バイトの上位ビットは priority に使用しない。  
- 受信側 `MultiPipeServer` の内部キューを優先度付きキュー（`std::priority_queue`）に変更。  
- デフォルト優先度 = 4（中）、HEARTBEAT / PING = 7（最高）。

---

### L-003 — Java ラッパー

#### 設計方針

- **最低要件: Java 17 LTS**。Java 17+ で `java.nio.channels` の Unix Domain Socket 対応が標準化されている。Java 11 は標準 UDS なしのため、維持するには外部依存が発生し、導入コスト分離方針と矛盾する（レビュー V200-005）。  
- Maven / Gradle 対応の最小 Java ライブラリ（Android 非対応で OK）。  
- Java NIO の `java.nio.channels` で UDS を使用。  
- C++ コアを JNI/JNA 経由で呼び出すのではなく、C# 同様にプロトコルを Java で独自実装。

#### 主な用途

- Spring Boot マイクロサービス ↔ C++ エンジンの IPC。  
- Apache Flink / Spark のネイティブバックエンドブリッジ。

---

### L-004 — Rust クレート

#### 設計方針

- `crates.io` 配布の `pipeutil-rs` クレート。  
- `tokio` 非同期ランタイム対応。  
- `serde` との組み合わせで構造化メッセージを扱いやすくする。

#### 主な用途

- Rust で書かれた高性能バックエンド ↔ Python / C# フロントエンドの IPC。  
- OS システムプログラミング層から pipeutil プロトコルを使用。

---

### F-014 — TLS オーバーレイ（経路暗号化）

> **配布形態**: 別 PyPI パッケージ `pipeutil-tls`（Section 3.2 参照）。メインの `pipeutil` wheel には含めない。

#### 背景

- Windows Named Pipe はネットワーク越しのアクセスが可能（`\\server\pipe\name`）。  
- SQL Server や WinDbg のような用途では、ホスト外部からの接続時に暗号化が必要。

#### 設計方針

- **接続確立後に TLS 1.3 ハンドシェイク**を実施するオプション層として実装（明示的な opt-in）。  
- 証明書ピンニング（SHA-256 フィンガープリント指定）でサーバー認証。  
- OpenSSL / WolfSSL のどちらでもコンパイル可能な抽象層を設ける。  
- **C++ 側の分離**: `pipeutil_tls.dll` / `libpipeutil_tls.so` をコアとは別の共有ライブラリとして実装。コア DLL に OpenSSL 依存を持ち込まない。  
- **Python 側の分離**: `pipeutil-tls` パッケージが `pipeutil` を `install_requires` に宣言し、`pipeutil.tls` サブモジュール名前空間で提供。  
- **優先度を Low にした理由**: ローカル IPC では不要であり、実装コストが大きいため v2.1.0 以降での扱いを推奨。

---

## 5. アーキテクチャ変化のサマリ

```
v2.0.0 追加後のシステム構成（変更箇所 ★）

┌───────────────────────────────────────────────────────────────────────┐
│                     アプリケーション層                                 │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐            │
│  │ C++ App  │  │Python App│  │ C# App ★ │  │ Java App★│            │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘            │
│       │              │    import    │  PipeUtil.NET│   pipeutil-java  │
│       │ include      │              │   (NuGet★)  │   (Maven★)       │
│       ▼              ▼              └──────────────┘                  │
│  ┌────────────┐  ┌──────────────────────────────────┐                │
│  │pipeutil.hpp│  │  pipeutil (Python pkg)            │                │
│  └────┬───────┘  └──────────────────────────────────┘                │
│       │                          │                                     │
│       │                          │ Python C API                        │
│       │                 ┌────────▼────────┐                           │
│       │                 │  _pipeutil.pyd  │                           │
│       │                 └────────┬────────┘                           │
│       └──────────────┬───────────┘                                    │
│                      ▼                                                 │
│           ┌──────────────────────────┐                                │
│           │  pipeutil_core (.dll/.so)│                                │
│           │  ┌──────────────────────┤                                 │
│           │  │ ConcurrentRpcClient★ │  ← F-009                       │
│           │  │ StreamPipeWriter★    │  ← F-010                       │
│           │  │ HeartbeatLayer★      │  ← F-012                       │
│           │  │ PriorityQueue★       │  ← F-013                       │
│           │  │ PlatformLayer (Win/Linux) │                             │
│           │  └──────────────────────┘                                 │
│           └──────────────────────────┘                                │
│                      │                                                │
│           ┌──────────────────────────┐                                │
│           │  pipeutil-broker (★別pkg)│  ← F-011（core 外）              │
│           └──────────────────────────┘                                │
└───────────────────────────────────────────────────────────────────────┘
                       │ OS IPC (同一プロトコル★ — 全言語共通)
             ┌─────────┴──────────┐
             │  Named Pipe / UDS  │
             └────────────────────┘
```

---

## 6. フレームヘッダ v2 の変更案

> ⚠️ **バージョニング方針（レビュー V200-007）**: プロトコルバージョンは既存 v1 を「変更」するのではなく、v1 を維持したまま v2 を「追加」する。v2 の version フィールドを `0x02` に変えるのは、F-009・F-010・F-012・F-013 の全機能の仕様・実装・相互接続試験がすべて完了した後に行う。先に version だけ増やして実装を追従させる順序は取らない。  
> capability negotiation （A-001）により、v1/v2 の共存期間中は feature bitmap で機能を識別する。変更は `spec/02_protocol.md`、フレーム構造体定義、`static_assert`、図・表を「同時更新」する（CLAUDE.md 実装原則より）。

```
 0               1               2               3
 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
├───────────────────────────────────────────────────────────────────┤
│  magic[0]  │  magic[1]  │  magic[2]  │  magic[3]               │
├───────────────────────────────────────────────────────────────────┤
│  version=2 │  flags    │  priority_stream          │ (reserved) │  ← 変更
├───────────────────────────────────────────────────────────────────┤
│  payload_size (uint32_t, little-endian)                           │
├───────────────────────────────────────────────────────────────────┤
│  message_id  (uint64_t, little-endian) — 上位 32bit              │  ← 拡張
├───────────────────────────────────────────────────────────────────┤
│  message_id  (uint64_t, little-endian) — 下位 32bit              │
├───────────────────────────────────────────────────────────────────┤
│  checksum    (uint32_t, little-endian, CRC-32C)                   │
├───────────────────────────────────────────────────────────────────┤
│  payload (payload_size バイト) ...                                │
└───────────────────────────────────────────────────────────────────┘

ヘッダ固定サイズ v2: 24 バイト（v1: 16 バイト）
バイト内訳: magic(4) + version(1) + flags(1) + priority_stream(1) + reserved(1) + payload_size(4) + message_id(8) + checksum(4) = 24B
```

**flags バイト（byte +5）のビット定義:**

| ビット | 名前 | 意味 |
|-------|------|------|
| 0 | `FLAG_COMPRESSED` | ペイロード圧縮（将来） |
| 1 | `FLAG_ACK` | ACK（将来） |
| 2 | `FLAG_HEARTBEAT` | ハートビート PING/PONG（F-012） |
| 3 | `FLAG_CONCURRENT_RPC` | 並行 RPC モード（F-009） |
| 4−7 | (reserved) | 将来拡張 |

**`priority_stream` バイト（byte +6）のビット定義:**

| ビット | フィールド | 意味 |
|-------|------|------|
| 7−5 | `priority`（3 bit） | 優先度 0−7（7=最高）。F-013 で使用。デフォルト = 4 |
| 4 | `FLAG_STREAM_CHUNK` | ストリームチャンク。F-010 で使用。 |
| 3 | `FLAG_STREAM_END` | ストリームの最終チャンク。F-010 で使用。 |
| 2−0 | (reserved) | 将来拡張 |

---

## 7. 実装ロードマップ（推奨順序）

```
Phase 1（基盤固め — protocol v2 実装着手前の必須作業）
  1. A-001 Capability Negotiation 仕様確定 — v1/v2 共存方針と feature bitmap を先に固める
  2. A-002 Flow Control / Backpressure 契約仕様化 — F-009/F-010 実装前の安全網
  3. L-002 / A-003 PROTOCOL_COMPAT.md + golden frame corpus 作成
  4. F-010 事前評価: v1 互換 large payload / chunked read で要件を満たせるか検証

Phase 2（コア機能追加）
  5. F-009 並行 RPC — A-002 の契約を前提に実装。message_id 周りのヘッダ変更。
  6. F-010 ストリーミング — Phase 1 評価結果に基づいて実装方針を決定
  7. F-012 ハートビート — プロトコル破壊なしで実装可能（`FLAG_HEARTBEAT` のみ使用）
  8. F-013 優先度キュー — priority_stream バイト (Section 6) を使用

Phase 3（言語ブリッジ）
  9. L-001 C# ラッパー — protocol freeze 完了後に着手（V200-004）
  10. A-003 interop テスト — Python ↔ C#、C++ ↔ C# 相互通信
  11. L-003 Java ラッパー（Java 17+、オプション）

Phase 4（別パッケージ / 将来）
  12. F-011 → pipeutil-broker 別パッケージ（core API を汚さない形で推進）
  13. L-004 Rust クレート
  14. F-014 TLS オーバーレイ → pipeutil-tls 別パッケージ

最終ステップ（Phase 2−4 完了後）
  15. protocol v2 正式公開 — v1 を維持したまま v2 を「追加」する形でリリース
              （version フィールドを 0x02 に上げるのはこのステップのみ）
              移行ガイド・ interop テスト完了で確認後に公開
```

---

## 8. 決定事項の確認（実装着手前に合意が必要な項目）

1. **versioning 方針 — 合意済み（レビュー V200-007）**: 既存 v1 を維持したまま、v2 は capability negotiation で識別する「追加」として扱う。version フィールドを `0x02` にするのは、Phase 2-4 の全機能の仕様・実装・interop 試験が完了した後の最終ステップ一回のみ。
2. **C# プロジェクトの依存関係ポリシー** — .NET Standard 2.0 のみ（Unity 互換）にするか、.NET 6+ も許容して `System.IO.Hashing.Crc32C` を使うか。
3. **Java 実装のターゲット JVM——Java 17+ に決定（レビュー V200-005）**: Java 11 は標準 UDS なし、追加依存が発生するため除外。
4. **Phase 1 の着手タイミング** — v2.0.0 ブランチ (`feature/v2.0.0-planning`) をそのまま使うか、機能ごとにブランチを切り直すか。

---

*本提案書に対するレビューは [review/](../review/) の GPT 系レビュアーへの依頼手順に従う。*
