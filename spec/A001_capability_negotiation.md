# A-001 — Capability Negotiation / Feature Bitmap

**バージョン**: 1.0 (v1.1.0 実装)  
**作成日**: 2026-03-15  
**ステータス**: 確定  
**関連仕様**: [spec/02_protocol.md](02_protocol.md), [docs/design_change_spec_v1.1.0.md](../docs/design_change_spec_v1.1.0.md)

---

## 1. 概要

v1.1.0 から新たに追加する **HELLO フレーム** とそれを使った **Capability Negotiation** の仕様を定める。

### 目的

| 目的 | 説明 |
|---|---|
| 段階的 rollout | v1.3.0〜v1.4.0 で追加される機能（F-009/F-010/F-012/F-013）をバージョン混在環境で安全に有効化する |
| v1/v2 共存 | v2.0.0 公開前の移行期間中、feature bitmap により機能可否を識別する |
| 後退互換 | `HelloMode::Compat`（デフォルト）のサーバーは v1.0.0 クライアントを透過的に受け入れる（v1-compat モード）。Rolling upgrade 期間はサーバーを先に v1.1.0 へアップグレードするだけでよい。`HelloMode::Strict` にすると v1.0.0 クライアントを拒否してハード移行を強制できる。 |

---

## 2. HELLO フレーム定義

### 2.1 ヘッダ識別方法

既存の `flags` バイト（byte +5）の **bit 7** を `FLAG_HELLO = 0x80` として定義する。

```
flags バイトのビット割り当て（v1.1.0 時点）
┌─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┐
│  7  │  6  │  5  │  4  │  3  │  2  │  1  │  0  │
│HELLO│ (r) │ (r) │ (r) │RESP │ REQ │ ACK │COMP │
└─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┘
 FLAG_HELLO=0x80  reserved bits 4-6
 FLAG_RESPONSE=0x08  FLAG_REQUEST=0x04
 FLAG_ACK=0x02   FLAG_COMPRESSED=0x01
```

HELLO フレームは `FLAG_HELLO` **単独**で使用し、他の FLAG と OR することはない。

### 2.2 フレーム固定レイアウト

現在の 20 バイトヘッダを使用する（`spec/02_protocol.md` 記載の FrameHeader そのまま）。

```
Offset  Size  Type         Field           Value（HELLO 時）
──────────────────────────────────────────────────────────────
  0      4    uint8_t[4]   magic           {'P','I','P','E'} = 0x50494550
  4      1    uint8_t      version         0x02 (PROTOCOL_VERSION)
  5      1    uint8_t      flags           0x80 (FLAG_HELLO のみ)
  6      2    uint8_t[2]   reserved        0x00 0x00
  8      4    uint32_t LE  payload_size    0x00000008 (固定 8 バイト)
 12      4    uint32_t LE  checksum        CRC-32C (payload 8 バイト対象)
 16      4    uint32_t LE  message_id      0x00000000 (NO_MESSAGE_ID)
────────────────────────────────────────── 20 bytes (ヘッダ固定長)
 20      4    uint32_t LE  feature_bitmap  対応する capability の OR
 24      4    uint32_t LE  (reserved)      0x00000000 (将来拡張)
────────────────────────────────────────── 28 bytes (ヘッダ + ペイロード合計)
```

**ペイロード固定サイズ**: 常に `8 バイト`（`payload_size = 8` 固定）。  
将来バージョンで HELLO ペイロードが拡張される場合は `payload_size` を増やすが、受信側は `payload_size` が 8 以上であれば先頭 8 バイトを本ペイロードとして解釈せよ（前方互換）。

### 2.3 `HelloPayload` 構造体（C++）

```cpp
// detail/frame_header.hpp に追加
namespace pipeutil::detail {

#pragma pack(push, 1)
struct HelloPayload {
    uint32_t feature_bitmap;   // 送信側が対応する capability の OR (LE)
    uint32_t reserved;         // 0x00000000 固定
};
#pragma pack(pop)

static_assert(sizeof(HelloPayload) == 8, "HelloPayload must be 8 bytes");

inline constexpr uint8_t FLAG_HELLO = 0x80;  // bit 7: HELLO フレーム識別子

}  // namespace pipeutil::detail
```

---

## 3. feature_bitmap ビット定義

| ビット | 16進値 | 名前 | 対応機能 | 実装バージョン |
|-------|--------|------|---------|--------------|
| 0 | `0x01` | `PROTO_V2` | 24 バイト v2 ヘッダ対応 | v2.0.0 |
| 1 | `0x02` | `CONCURRENT_RPC` | 並行 RPC (F-009) | v1.3.0 |
| 2 | `0x04` | `STREAMING` | ストリーミング (F-010) | v1.4.0 |
| 3 | `0x08` | `HEARTBEAT` | ハートビート (F-012) | v1.3.0 |
| 4 | `0x10` | `PRIORITY_QUEUE` | 優先度キュー (F-013) | v1.4.0 |
| 5-31 | — | (reserved) | 将来拡張。送信時 0 | — |

**v1.1.0 での advertised_capabilities**: `0x00000000`  
各機能は実装が完了したバージョン（v1.3.0〜）でビットを立てる。

### Capability 列挙体（C++）

```cpp
// capability.hpp
namespace pipeutil {
enum class Capability : uint32_t {
    ProtoV2        = 0x01u,
    ConcurrentRpc  = 0x02u,
    Streaming      = 0x04u,
    Heartbeat      = 0x08u,
    PriorityQueue  = 0x10u,
};
}
```

### CapabilityBitmap（Python）

```python
class CapabilityBitmap(IntFlag):
    PROTO_V2       = 0x01
    CONCURRENT_RPC = 0x02
    STREAMING      = 0x04
    HEARTBEAT      = 0x08
    PRIORITY_QUEUE = 0x10
```

---

## 4. ハンドシェイク手順

### 4.1 基本原則

1. **クライアントが先送り**: TCP の SYN/SYN-ACK に倣い、接続確立直後にクライアントが HELLO を送信する。
2. **サーバーが応答**: HELLO を受信したサーバーは自分の HELLO を返す。
3. **合意 = AND**: `negotiated_capabilities.bitmap = client_bitmap & server_bitmap`
4. **version バイトで即時識別**: サーバーは先頭 5 バイト（magic[4] + version[1]）を読み取ることで、`hello_timeout` を待たずにクライアントが v1.0.0（`0x01`）か v1.1.0（`0x02`）かを即座に判別できる。
5. **HelloMode ポリシー**: per-connection の HELLO ハンドシェイク戦略。`Compat`（デフォルト）/ `Strict` / `Skip` の 3 モードで接続受け入れと HELLO 不達時の動作を制御する。**サーバーの `HelloMode` が最終受け入れポリシーを決定する。クライアントの `mode` は HELLO 送信の有無など送信側の振る舞いのみを制御し、サーバーの判定を上書きしない。**

### 4.2 正常系フロー（v1.1.0 同士）

```
Client (v1.1.0)                    Server (v1.1.0)
────────────────────────────────────────────────────
 1. connect() 完了
 2. HELLO(bitmap=0x00) を送信 ──────────────────►
                                    3. accept() 後、先読みで FLAG_HELLO を検出
                                    4. HELLO を受信・デコード
                                    5. HELLO(bitmap=0x00) を返信 ────────────►
 6. HELLO を受信・デコード
 7. negotiated = 0x00 & 0x00 = 0     negotiated = 0x00 & 0x00 = 0
 8. 通常フレーム送受信開始 ──────────────────────────────────────────────────
```

### 4.3 後退互換フロー（v1.0.0 クライアント → v1.1.0 サーバー；`HelloMode::Compat`）

先頭 5 バイト（magic + version）の受信でクライアントが v1.0.0 と確定するため、`hello_timeout` 待機は発生しない。

```
OldClient (v1.0.0)                 Server (v1.1.0, Compat)
────────────────────────────────────────────────────
 1. connect() 完了
 2. 通常フレーム（version=0x01, 16B ヘッダ）を送信 ───►
                                    3. magic[4] + version[1]（5B）を読み取り
                                    4. version=0x01 → v1-compat モード確定
                                    5. ヘッダ残り 11B を読み取り（計 16B; FrameHeaderV1）
                                    6. on_hello_complete(caps={bitmap=0, v1_compat=true})
 ◄──── 通常フレーム（version=0x01, 16B ヘッダ）─────
 7. 通常フレームとして受信・dispatch
```

**サーバー v1-compat モードの制約**:
- **受信**: `FrameHeaderV1`（16B、`message_id` なし）として処理。`message_id` は常に `NO_MESSAGE_ID` 扱い。
- **送信**: 応答フレームも `version=0x01`・16B ヘッダで送り返すこと（v1.0.0 クライアントは `version != 0x01` を `InvalidMessage` として拒否するため）。

**`HelloMode::Strict` の場合**: version=0x01 を検出した時点で即座に `ConnectionRejected` として接続を閉じる。

---

### 4.4 非互換フロー（v1.1.0 クライアント → v1.0.0 サーバー）

```
NewClient (v1.1.0)                  OldServer (v1.0.0)
────────────────────────────────────────────────────
 1. connect() 完了
 2. HELLO(version=0x02, flags=0x80) を送信 ──►
                                    3. フレームを受信
                                    4. version 検証 → 0x01 以外 → InvalidMessage!
                                    5. 接続受け入れを拒否 / リセット
 ←────── ConnectionReset / InvalidMessage 小例外 ──────
```

**❗ 非サポート**: v1.1.0 クライアントは `version=0x02` で HELLO フレームを送信する。
v1.0.0 サーバーは受信時に `version != 0x01` を `InvalidMessage` として拒否するため、接続は必ず失敗する。

**回避策**:  
1. 全 endpoint を同時に v1.1.0 へアップグレードする（カットオーバー方式）。  
2. 移行期間中は v1.0.0 と v1.1.0 を別パイプ（別エンドポイント）で並行稼働させ、全クライアント移行後に v1.0.0 エンドポイントを廃止する（デュアルスタック方式）。  

> ⚠️ `HelloMode::Skip` は `version` フィールドの非互換を解消しない。v1.1.0 クライアントが送信する `version=0x02` フレームは v1.0.0 サーバーに `InvalidMessage` で拒否される。

### 4.5 HELLO タイムアウト / Strict 拒否フロー（v1.1.0 クライアント + HELLO なし）

v1.1.0 クライアント（version=0x02）が接続後に HELLO を送らない場合の動作。`hello_timeout` は `accept()` 直後から開始し、先頭 5 バイト受信を含む最初のフレーム全体の到着待ちに適用される（「5 バイト受信後に限定」ではない）。

```
Client (v1.1.0, Skip / HELLO 未送信)   Server (v1.1.0)
────────────────────────────────────────────────────
 1. connect() 完了
                                    2. magic[4] + version[1]（5B）を読み取り
                                    3. version=0x02 → v1.1.0 クライアントと確定
                                    4. ヘッダ残り 15B を読み取り → フレーム完成
                                    5. flags & FLAG_HELLO == 0 → HELLO なし
                                       ┌── Compat: v1 fallback モード確定
                                       └── Strict:  ConnectionRejected → 切断
 3. 通常フレーム送信 ──────────────────────────────►
                                    6. [Compat のみ] 通常フレームとして受信・dispatch
```

**`HelloMode::Compat` の振舅**: 受信したフレームをバッファに保持したまま v1 フォールバックへ移行し、切断は発生しない。`on_hello_complete(caps={bitmap=0, v1_compat=false})` が呼ばれる。

**`HelloMode::Strict` の挙動**: HELLO なしのフレーム受信時点で `ConnectionRejected` を送出して接続を閉じる。`on_hello_complete` は呼ばれない。

**`HelloMode::Skip` の挙動**: サーバーも HELLO を待機しない。version=0x02 クライアントが接続してきた場合、HELLO の有無にかかわらず即座に v1 モードへ移行する。

> ℹ️ `hello_timeout` は version=0x02 クライアントのうち「最初のフレーム到達まで時間がかかる場合」のフォールバック猟予時間である。タイムアウトは先頭 5 バイトの読み取りも対象に含む（`accept()` 直後から計測）。v1.0.0 クライアント（version=0x01）は先頭 5 バイト受信時点で即座に判別されるため、このフローの対象外となる。

---

## 5. HelloConfig パラメータ仕様

| パラメータ | 型 | デフォルト | 説明 |
|---|---|---|---|
| `mode` | `HelloMode` | `Compat` | ハンドシェイクポリシー（後述） |
| `hello_timeout` | `std::chrono::milliseconds` | `500ms` | version=0x02 クライアントの HELLO 待機上限。0 で無制限待機 |
| `advertised_capabilities` | `uint32_t` | `0x00000000` | 自身が対応する機能のビット OR |

### HelloMode の選択指針

| モード | 動作 | 推奨用途 |
|---|---|---|
| `Compat` | v1.0.0 クライアントを v1-compat モードで受け入れる。v1.1.0 クライアントの HELLO なし（timeout）→ v1 fallback | Rolling upgrade 期間中のサーバー（デフォルト推奨） |
| `Strict` | v1.0.0 クライアントを拒否。v1.1.0 クライアントの HELLO なし → `ConnectionRejected` | 全 endpoint が v1.1.0 以降に移行完了後の厳格運用 |
| `Skip` | HELLO 交換を行わない。即座に v1 モードへ移行（両側設定必須） | 専用クラスタ内通信・ベンチマーク |

> ⚠️ `HelloMode::Skip` は v1.0.0 ↔ v1.1.0 の wire 非互換を解消しない。v1.0.0 サーバーは `version=0x02` フレームを `InvalidMessage` で拒否するため、`Skip` は v1.1.0 同士の専用環境でのみ使用すること。

---

## 6. NegotiatedCapabilities 仕様

### 定義

```
NegotiatedCapabilities {
    bitmap:    uint32_t  // client_bitmap BITAND server_bitmap
    v1_compat: bool      // true: 接続相手が v1.0.0 クライアント（version=0x01 で接続）
}
```

### 取得タイミング

| 役割 | 取得可能タイミング |
|---|---|
| Server | `accept()` 返却後（`on_hello_complete` コールバック内または直後） |
| Client | `connect()` 返却後（HELLO の往来を `connect()` 内部で完結させる） |

### 利用例（C++）

```cpp
pipeutil::PipeClient client("my_pipe", 65536,
    pipeutil::HelloConfig{.advertised_capabilities = 0x00});
client.connect();

auto caps = client.negotiated_capabilities();
if (caps.has(pipeutil::Capability::ConcurrentRpc)) {
    // v1.3.0 以降: 並行 RPC を使用する
} else {
    // v1 逐次 RPC を使用する
}
```

### 利用例（Python）

```python
client = pipeutil.PipeClient("my_pipe", hello_config=pipeutil.HelloConfig())
client.connect()

caps = client.negotiated_capabilities
if caps.has(pipeutil.CapabilityBitmap.CONCURRENT_RPC):
    # 並行 RPC モード
else:
    # 逐次 RPC モード（v1 互換）
```

---

## 7. 受信側のフレーム解釈ルール

HELLO フレームは **接続確立直後の最初の 1 フレームのみ**有効とする。

| 条件 | 解釈 |
|---|---|
| 最初のフレームの `flags & FLAG_HELLO != 0` | HELLO フレームとして処理（payload から feature_bitmap を読み取る）|
| 最初のフレームの `flags & FLAG_HELLO == 0` | 通常フレームとして処理（メッセージ損失なし）; negotiated = v1 モード |
| 2 番目以降のフレームの `FLAG_HELLO != 0` | **`InvalidMessage` エラー**（HELLO は接続ごとに 1 回のみ）|
| `payload_size < 8` の HELLO フレーム | **`InvalidMessage` エラー**（ペイロードが短すぎる） |
| `payload_size > 8` の HELLO フレーム | 先頭 8 バイトを valid ペイロードとして読み取り（前方互換; 残りのバイトは無視）|

> ⚠️ **`HelloMode::Skip` の例外**: Skip に設定されたサーバーは HELLO 処理を一切行わない。最初のフレームに `FLAG_HELLO` が含まれていても、そのビットを無視して通常フレームとして処理する（`InvalidMessage` にはならない）。

---

## 8. CRC-32C の計算対象

HELLO フレームの `checksum` は `HelloPayload` の 8 バイトに対して CRC-32C を計算する。
ヘッダ部分は計算対象に含めない（通常フレームと同じルール）。

---

## 9. 将来の拡張予約

- feature_bitmap のビット 5〜31: 将来の機能向けに予約。現時点では送信時 0 を設定し、受信時は無視すること。
- `HelloPayload.reserved` フィールド（4 バイト）: 将来 bitmap を 64-bit に拡張する際に使用予定。現時点では `0x00000000` 固定で送受信すること。

---

## 10. テストベクタ

### Vector 1 — v1.1.0 標準 HELLO フレーム（bitmap=0x00000000）

```
Hex: 50 49 50 45 02 80 00 00 08 00 00 00 [CRC] 00 00 00 00 00 00 00 00 00 00 00 00
     ────────── ── ── ───── ─────────── ───── ─────────── ─────────────────────────
     PIPE magic v  flags ps=8        checksum  message_id   payload(feature=0,rsv=0)
```

payload (8 bytes) = `00 00 00 00 00 00 00 00`  
CRC-32C of payload = `0xAA36918A`（CRC-32C(`\x00\x00\x00\x00\x00\x00\x00\x00`) の計算値）

完全なバイト列（28 バイト）:
```
50 49 50 45  02 80 00 00  08 00 00 00  8A 91 36 AA  00 00 00 00  00 00 00 00  00 00 00 00
```

### Vector 2 — ConcurrentRpc + Heartbeat 対応 HELLO（bitmap=0x0000000A）

payload = `0A 00 00 00 00 00 00 00`  
CRC-32C(`0A 00 00 00 00 00 00 00`) = 実装時に計算して本仕様に追記する。

---

*本仕様書に変更が生じた場合、`spec/02_protocol.md` の flags ビット表および `source/core/include/pipeutil/detail/frame_header.hpp` の定数を**同時更新**すること（CLAUDE.md 実装原則）。*
