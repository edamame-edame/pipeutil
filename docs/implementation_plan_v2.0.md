# pipeutil v1.x → v2.0.0 実装計画

**作成日**: 2026-03-14  
**対象バージョン範囲**: v1.1.0 〜 v2.0.0  
**前提**: v1.0.0 リリース済み（Windows / Linux × Python 3.8-3.14 全通過）  
**方針**: v1.x.y マイルストーンを段階的に重ね、全機能の実装・interop 試験が完了した後に v2.0.0 を公開する。protocl version フィールドを `0x02` に変更するのは v2.0.0 リリース時の一回のみ。

---

## 1. 優先度 × 影響度グルーピング

### 軸の定義

| 軸 | 定義 |
|---|---|
| **優先度** | 後続機能のブロッカー度 ＋ 開発速度向上への寄与 |
| **影響度** | wire format / API 変更の広さ ＋ ユーザー価値の大きさ |

### 優先度 × 影響度マトリクス

|  | 影響度: 高（wire format 変更含む） | 影響度: 中（API 追加のみ） | 影響度: 低（別 pkg / ドキュメント） |
|---|---|---|---|
| **優先度: 高**（ブロッカー） | F-009 並行 RPC | A-001 Capability Negotiation | — |
|  | F-012 Heartbeat | A-002 Flow Control Spec | — |
| **優先度: 中**（コア価値） | F-010 Streaming | L-002 Protocol Spec | F-011 pipeutil-broker |
|  | F-013 Priority Queue | A-003 Interop Kit | F-014 pipeutil-tls |
|  | — | L-001 C# ラッパー | — |
| **優先度: 低**（将来価値） | — | L-003 Java ラッパー | L-004 Rust クレート |

### グループ分類

| グループ | 機能 | 役割 | 着手条件 |
|---|---|---|---|
| **G-A 基盤** | A-001, A-002, L-002, A-003 | 後続全作業の安全網 | なし（最初） |
| **G-B コア機能** | F-009, F-012, F-010, F-013 | wire format 変更を含む主要機能 | G-A 完了後 |
| **G-C 言語ブリッジ** | L-001, L-003, L-004 | protocol freeze 後の多言語化 | G-B 完了後 |
| **G-D 別パッケージ** | F-011, F-014 | core に影響しない拡張 | G-B 完了後（G-C と並行可） |

---

## 2. マイルストーン一覧

| バージョン | テーマ | グループ | 主要機能 | 前提 |
|---|---|---|---|---|
| **v1.1.0** | 基盤確立 | G-A | A-001 + A-002 spec | v1.0.0 |
| **v1.2.0** | 仕様整備 | G-A | L-002 + A-003 + F-010 事前評価 | v1.1.0 |
| **v1.3.0** | コア機能 Part 1 | G-B | F-009 + F-012 | v1.2.0 |
| **v1.4.0** | コア機能 Part 2 | G-B | F-010 + F-013 | v1.3.0 |
| **v1.5.0** | 言語ブリッジ C# | G-C | L-001 C# | v1.4.0 |
| **v1.6.0** | 言語ブリッジ拡張 | G-C | L-003 Java + L-004 Rust | v1.5.0 |
| **v1.7.0** | 別パッケージ群 | G-D | F-011 broker + F-014 TLS | v1.4.0 ※G-C と並行可 |
| **v2.0.0** | Protocol v2 公開 | — | version=0x02 + Migration Guide | v1.5.0 + v1.7.0 |

---

## 3. バージョン別詳細

---

### v1.1.0 — 基盤確立

> **テーマ**: protocol v2 実装着手前の必須基盤を整える。後続の G-B, G-C, G-D すべてが依存する。

#### スコープ

**A-001: Capability Negotiation / Feature Bitmap**

- 接続確立直後に `HELLO` フレームを交換し、peer が対応する capability を feature bitmap で通知する。
- bitmap ビット定義:

  | bit | 機能 |
  |---|---|
  | 0 | `PROTO_V2`（24 バイトヘッダ対応）|
  | 1 | `CONCURRENT_RPC`（F-009）|
  | 2 | `STREAMING`（F-010）|
  | 3 | `HEARTBEAT`（F-012）|
  | 4 | `PRIORITY_QUEUE`（F-013）|
  | 5-31 | (reserved) |

- C++ + Python 両実装。`HELLO` フレームを送受信しない旧クライアントは v1 モードで動作継続（後退互換）。
- `spec/A001_capability_negotiation.md` を作成する。

**A-002: Flow Control / Backpressure 契約仕様策定**

- 実装は v1.3.0; v1.1.0 では仕様書の完成のみ。
- 仕様に含める内容:
  - `max_inflight_requests`（接続ごとの上限、overflow 時のエラーコード）
  - `max_buffered_bytes`（送信バッファ上限）
  - `window_size`（ストリームごとのフロー制御サイズ）
  - `cancel(message_id)` / `cancel(stream_id)` の API とキャンセル完了契約
  - quota 超過時エラーコード一覧
- `spec/A002_flow_control.md` を作成する。

#### リリース条件

- `HELLO` フレームの Python ↔ C++ 往来テスト通過。
- feature bitmap の各 bit 識別テスト通過（capability 有/無の両パターン）。
- `HELLO` 未対応の旧クライアントとの後退互換テスト通過。
- `spec/A001_capability_negotiation.md` + `spec/A002_flow_control.md` 完成。
- Windows / Linux × Python 3.8─3.14 全通過。

---

### v1.2.0 — 仕様整備

> **テーマ**: 実装を始める前に「正解の形」を文書とテストベクタでロックする。

#### スコープ

**L-002: `spec/PROTOCOL_COMPAT.md` 作成**

- v1 / v2 ヘッダ比較表（フィールド名 / サイズ / バイトオーダー / bitfield 割り当て）。
- CRC-32C の既知値テストベクタ 5 本以上（ペイロード・期待値の組み合わせ）。
- バイトオーダー（リトルエンディアン）・マジックバイト・エラーコード一覧。
- バージョンネゴシエーション手順（capability bitmap と合わせた移行フロー）。

**A-003: golden frame corpus（v1 版）**

- `tests/interop/golden_frames/` ディレクトリを作成。
- 収録フレーム: v1 PING, v1 RPC request, v1 RPC response, checksum mismatch, truncated frame, unsupported version。
- 各バイナリに対して CRC 一致・長さ一致・フィールド値一致を確認するテストスクリプト追加。

**F-010 事前評価**

- v1 互換 large payload / chunked read で 1 GB ファイル転送をベンチマーク（Python asyncio + C++ 両方）。
- 評価レポート `docs/F010_preeval_report.md` を作成。
- 「v2 ヘッダ（新規ストリームフレーム）が必要か / v1 互換範囲で解決可能か」を明記して v1.3.0 着手前に合意を得る。

#### リリース条件

- golden frame テストが Windows / Linux × Python 3.8-3.14 全通過。
- `spec/PROTOCOL_COMPAT.md` 完成（CRC-32C テストベクタ 5 本以上含む）。
- `docs/F010_preeval_report.md` 完成（「v2 ヘッダ必要 / 不要」の判断を明記）。

---

### v1.3.0 — コア機能 Part 1

> **テーマ**: 並行 RPC とハートビートを安全に追加する。24 バイトヘッダの一部実装（message_id 拡張）。

#### スコープ

**F-009: Concurrent RPC（複数 in-flight リクエスト）**

- A-001 negotiation で `CONCURRENT_RPC` capability を確認した場合のみ有効化。
- 24 バイトヘッダ（message_id: 4B → 8B 拡張）を capability 確認後に使用。旧クライアントは 16B ヘッダ継続。
- C++: `ConcurrentRpcClient` クラス（`std::map<uint64_t, std::promise<Message>>` 内部管理）。
- Python: `asyncio` ベースの `AsyncRpcClient` 拡張（Task per in-flight）。
- A-002 のフロー制御実装:
  - `max_inflight_requests` 上限チェック（overflow 時 `PipeError::QUEUE_FULL`）。
  - timeout propagation（孤立 response の後始末）。
  - `cancel(message_id)` API の実装。

**F-012: Heartbeat / プロセス監視**

- `flags` byte の bit 2 を `FLAG_HEARTBEAT = 0x04` として使用（16B ヘッダで動作可能、v2 専用ではない）。
- C++: `HeartbeatConfig { interval_ms, timeout_ms }` を `PipeServer` / `PipeClient` コンストラクタに追加。
- Python: `heartbeat_interval_ms=` / `heartbeat_timeout_ms=` キーワード引数、`on_client_dead=` コールバック。
- spec 追加: `spec/F009_concurrent_rpc.md`, `spec/F012_heartbeat.md`。

#### リリース条件

- F-009: 同時 10 リクエスト in-flight テスト、timeout テスト、`cancel(message_id)` テスト通過。
- F-012: 強制 kill → `on_client_dead` コールバック呼び出しテスト通過。
- `max_inflight_requests` overflow → `QUEUE_FULL` エラーテスト通過。
- Windows / Linux × Python 3.8-3.14 全通過。
- `spec/F009_concurrent_rpc.md` + `spec/F012_heartbeat.md` 完成。

---

### v1.4.0 — コア機能 Part 2

> **テーマ**: ストリーミングと優先度キューを追加し、24 バイトヘッダを完結させる。G-B の protocol freeze 完了。

#### スコープ

**F-010: Streaming Message Mode**

- v1.2.0 の事前評価レポートに基づいて実装方針を決定する。
  - 「v2 ヘッダ必要」判定の場合: `priority_stream` byte (byte +6) の `FLAG_STREAM_CHUNK` (bit4), `FLAG_STREAM_END` (bit3) を使用。
  - 「v1 互換で足りる」判定の場合: chunked read 方式で実装し、新ヘッダフィールドは使わない。
- C++: `StreamPipeWriter` / `StreamPipeReader` (`std::streambuf` 派生)。
- Python: `async for chunk in conn.stream_receive(stream_id=N):` 非同期ジェネレーター API。
- A-002 のストリームフロー制御実装: `window_size`, `cancel(stream_id)`。

**F-013: Priority Queue**

- `priority_stream` byte (byte +6) の bit7-5 を `priority` フィールド（0-7、default=4）として使用。
- C++: `MultiPipeServer` 内部の受信キューを `std::priority_queue` に変更。
- Python: `send(msg, priority=N)` キーワード引数追加。
- HEARTBEAT / PING フレームはデフォルト priority=7 で送出。

**24 バイトヘッダ完結**

- C++: `static_assert(sizeof(PipeFrameHeaderV2) == 24);` 追加。
- `spec/02_protocol.md`, フレーム構造体定義, static_assert の同時更新（CLAUDE.md 実装原則）。
- golden frame corpus (A-003) に v2 フレームサンプル（streaming chunk, priority RPC）を追加。
- spec 追加: `spec/F010_streaming.md`, `spec/F013_priority_queue.md`。

#### リリース条件

- F-010: 1 GB ストリーム転送テスト通過（送受スループット計測）。
- F-013: HEARTBEAT (priority=7) フレームが通常データ (priority=4) を追い抜くテスト通過。
- `static_assert(sizeof(PipeFrameHeaderV2) == 24)` コンパイルエラーなし（Windows / Linux）。
- golden frame corpus に v2 フレーム追加完了。
- Windows / Linux × Python 3.8-3.14 全通過。
- `spec/F010_streaming.md` + `spec/F013_priority_queue.md` 完成。

> **この時点で protocol freeze**。v1.5.0 以降の言語ブリッジはこの仕様に基づいて実装する。

---

### v1.5.0 — 言語ブリッジ: C#

> **テーマ**: protocol freeze 完了後、C# 実装を追加する。（レビュー V200-004 対応）

#### スコープ

**L-001: PipeUtil.NET**

- `source/csharp/PipeUtil.NET/` — 純粋 C# ライブラリ（PInvoke なし）。
- `.NET Standard 2.0` ターゲット（Unity 互換）。CRC-32C は自前実装 (`Crc32C.cs`)。
  - ※ `.NET 6+` で `System.IO.Hashing.Crc32C` を使う判断は Section 8（項目 2）に従う。
- 実装クラス: `PipeMessage`, `PipeClient`, `PipeServer`, `PipeStats`, `Crc32C`。
- v1（16B）/ v2（24B）両ヘッダ対応（A-001 negotiation 使用）。
- NuGet パッケージ設定 (`PipeUtil.NET.csproj` に `PackageId`, `Version` 設定)。

**A-003 拡張: C# interop テスト**

- `tests/interop/` に Python ↔ C# 双方向 RPC テスト追加。
- Windows（.NET 6+）+ Linux（.NET 6 / Mono）の CI matrix に組み込む。
- C++ ↔ C# フレームデコードテスト（golden frame corpus を C# から読み込み検証）追加。

#### リリース条件

- Python ↔ C# 双方向 RPC テスト通過（Windows + Linux）。
- C++ ↔ C# golden frame デコードテスト通過。
- `tests/interop/` CI に C# ターゲット追加完了。
- NuGet publish 準備完了（`dotnet pack` でエラーなし）。

---

### v1.6.0 — 言語ブリッジ拡張（Java + Rust）

> **テーマ**: オプション言語ブリッジを追加する。市場ニーズや実装リソースに応じた任意リリース。

#### スコープ

**L-003: Java ラッパー（Java 17+）**

- `source/java/` — Maven 最小プロジェクト（`pipeutil-java`）。
- `java.nio.channels.UnixDomainSocketChannel`（Java 16+）/ `java.net.UnixDomainSocketAddress`（Java 16+）使用。
- c++ と同様にプロトコルを Java で独自実装（JNI/JNA 依存なし）。
- v2 ヘッダ（24B）対応。
- Maven Central / GitHub Packages への公開設定。
- `tests/interop/` に Python ↔ Java RPC テスト追加（Java 17 + Windows / Linux）。

**L-004: Rust クレート（任意）**

- `source/rust/` — `crates.io` 配布の `pipeutil-rs`。
- `tokio` 非同期対応、`serde` 統合。
- v2 ヘッダ対応。
- `tests/interop/` に Python ↔ Rust フレームデコードテスト追加（任意）。

#### リリース条件

- Java: Java 17 + Windows / Linux で Python ↔ Java RPC テスト通過。
- Java: Maven / GitHub Packages publicize 準備完了。
- Rust（任意）: `cargo test` 全通過 + Python ↔ Rust golden frame テスト通過。

---

### v1.7.0 — 別パッケージ群

> **テーマ**: core に入れない拡張機能を独立パッケージとして整備する。v1.4.0 完了後なら G-C と並行で開始可能。

#### スコープ

**F-011: `pipeutil-broker`**

- 独立した PyPI パッケージ (`pip install pipeutil-broker`)。
- `pipeutil` を `install_requires` に宣言し、`MultiPipeServer` / `PipeClient` を内部利用。
- Python API: `MessageBroker.publish(topic, msg)`, `PipeSubscriber.subscribe(topic)`, `PipeSubscriber.iter(timeout_ms)`。
- C++ 側: `pipeutil_broker.dll` / `libpipeutil_broker.so` として分離した別ターゲット。
- 複数 subscriber + broadcast テスト、topic フィルタリングテスト追加。

**F-014: `pipeutil-tls`（任意）**

- 独立した PyPI パッケージ (`pip install pipeutil-tls`)。
- `pipeutil` を `install_requires` に宣言し、`pipeutil.tls.TlsPipeClient` / `TlsPipeServer` を追加エクスポート。
- C++ 側: `pipeutil_tls.dll` / `libpipeutil_tls.so`（コア DLL に OpenSSL 依存を持ち込まない）。
- TLS 1.3, 証明書ピンニング（SHA-256 フィンガープリント）対応。
- OpenSSL / WolfSSL 両方でコンパイル可能な抽象層。

#### リリース条件

- F-011: Publish / Subscribe 基本フロー + 複数 subscriber ブロードキャストテスト通過。
- F-011: `pip install pipeutil-broker` で独立インストール可能な状態。
- F-014（任意）: 自己署名証明書での C++ ↔ Python TLS 接続テスト通過。
- F-014（任意）: `pip install pipeutil-tls` で独立インストール可能な状態。
- Windows / Linux × Python 3.8-3.14 全通過。

---

### v2.0.0 — Protocol v2 正式公開

> **テーマ**: v1.5.0 ＋ v1.7.0 が揃った時点で、protocol version フィールドを `0x02` に上げてリリース。（レビュー V200-007）

#### スコープ

**protocol version 更新** — 一箇所・一回だけ

- `spec/02_protocol.md` のバージョン記述を `0x02` に更新。
- C++ フレーム構造体の `version` デフォルト値を `0x02` に変更。
- `static_assert` による定数検証コメント更新。
- `static_assert`, 構造体定義, 図, 表を **同時更新**（CLAUDE.md 実装原則）。

**Migration Guide**

- `docs/migration_v1_to_v2.md` 作成。
  - v1 → v2 移行手順（Capability Negotiation による段階移行）。
  - 共存期間中の `feature bitmap` 運用ガイド。
  - 変更一覧（API 差分 / ヘッダバイト差分）。

**最終 interop テスト確認**

- C++, Python, C#, Java の四者間 RPC + Streaming の相互通信テスト全通過。
- golden frame corpus に `version=0x02` フレームを追加。

**リリース関連**

- `CHANGELOG.md` の v2.0.0 エントリ整備。
- Windows / Linux × Python 3.8-3.14 全 wheel ビルド + テスト通過確認（CLAUDE.md 実装原則）。
- PyPI publish 準備（`twine check dist/*` エラーなし）。

#### リリース条件

- v1.5.0, v1.7.0（F-011 のみ必須\*）の全リリース条件充足。  
  (\* v1.6.0 の Java は任意、v1.7.0 の F-014 TLS は任意)
- 四者間（C++, Python, C#, Java）interop テスト全通過。
- Migration Guide レビュー完了。
- golden frame corpus に `version=0x02` フレーム追加完了。
- `CHANGELOG.md` 整備完了。

---

## 4. 依存関係グラフ

```
v1.0.0（リリース済み）
  │
  └── v1.1.0  A-001 Capability Negotiation
              A-002 Flow Control 仕様策定
        │
        └── v1.2.0  L-002 PROTOCOL_COMPAT.md
                    A-003 golden frames (v1)
                    F-010 事前評価レポート
              │
              └── v1.3.0  F-009 Concurrent RPC
                          F-012 Heartbeat
                    │
                    └── v1.4.0  F-010 Streaming
                                F-013 Priority Queue
                                ─── protocol freeze ───
                          │
                          ├── v1.5.0  L-001 C#  ──┐
                          │     │     A-003 C# interop │
                          │     │                   │
                          │     └── v1.6.0  L-003 Java   │  （任意）
                          │                L-004 Rust   │
                          │                             │
                          └── v1.7.0  F-011 pipeutil-broker ──┐
                                      F-014 pipeutil-tls（任意）│
                                                               │
                                            v2.0.0 ◄──────────┘
                                    version=0x02 + Migration Guide
                                    （v1.5.0 + v1.7.0 の充足が条件）
```

---

## 5. 機能別バージョン対応表（早見表）

| 機能 | グループ | 導入バージョン | 形態 |
|---|---|---|---|
| A-001 Capability Negotiation | G-A | **v1.1.0** | pipeutil コア |
| A-002 Flow Control Spec | G-A | **v1.1.0**（仕様）/ v1.3.0（実装） | pipeutil コア |
| L-002 PROTOCOL_COMPAT.md | G-A | **v1.2.0** | spec / docs |
| A-003 golden frame corpus | G-A | **v1.2.0**（v1）/ v1.4.0 拡張（v2）| テスト資産 |
| F-010 事前評価 | G-A | **v1.2.0** | docs のみ |
| F-009 Concurrent RPC | G-B | **v1.3.0** | pipeutil コア |
| F-012 Heartbeat | G-B | **v1.3.0** | pipeutil コア |
| F-010 Streaming | G-B | **v1.4.0** | pipeutil コア |
| F-013 Priority Queue | G-B | **v1.4.0** | pipeutil コア |
| L-001 C# ラッパー | G-C | **v1.5.0** | PipeUtil.NET (NuGet) |
| L-003 Java ラッパー | G-C | **v1.6.0**（任意） | pipeutil-java (Maven) |
| L-004 Rust クレート | G-C | **v1.6.0**（任意） | pipeutil-rs (crates.io) |
| F-011 pipeutil-broker | G-D | **v1.7.0** | pipeutil-broker (PyPI 別 pkg) |
| F-014 pipeutil-tls | G-D | **v1.7.0**（任意） | pipeutil-tls (PyPI 別 pkg) |
| Protocol version=0x02 | — | **v2.0.0** | 全パッケージ一斉更新 |

---

## 6. リスクと軽減策

| リスク | 影響 | 軽減策 |
|---|---|---|
| F-010 事前評価で「v1 互換で足りる」と判定 | F-010 の wire format 変更が不要になり v1.4.0 スコープ縮小 | 好Material に評価結果を反映し、スコープを縮小 (priority_stream byte の FLAG_STREAM_* を未使用のまま予約) |
| F-010 実装が想定よりブロッキングになり v1.4.0 が遅延 | v1.5.0 以降がスライド | v1.4.0 を F-013 のみで先行リリース → F-010 を v1.4.1 で追従 |
| C# / Java の CI 環境整備が複雑 | v1.5.0 / v1.6.0 が遅延 | Docker でランタイムを隔離（既存 `docker/` 活用）; 先に仕様書 + golden frame テストのみ先行リリース |
| pipeutil-broker の設計がコア API を引き込む | core の肥大化 | v1.7.0 内で broker がコア内部 API に依存した場合は API を公開せず、proxy 関数で隔離 |
| v2.0.0 リリース後に仕様バグが発覚 | wire format の再変更 | v2.0.0 の golden frame / interop テスト（四者間）を網羅的に揃えたうえでリリース |

---

*本計画は `docs/feature_proposals_v2.0.md` と `docs/feature_proposals_v2.0_additional.md` の提案に基づく。レビュー詳細は `review/whole.md` および `review/20260314224547.csv` を参照。*
