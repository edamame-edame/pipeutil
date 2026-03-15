# pipeutil v1.1.0 詳細設計変更仕様書

**作成日**: 2026-03-15  
**対象バージョン**: v1.1.0  
**前バージョン**: v1.0.0（リリース済み）  
**ベースブランチ**: `main`（または `feature/v1.1.0-capability-negotiation`）  
**スコープ**: A-001 Capability Negotiation 実装 / A-002 Flow Control 仕様策定  
**関連ドキュメント**:
- [docs/implementation_plan_v2.0.md](implementation_plan_v2.0.md)
- [spec/A001_capability_negotiation.md](../spec/A001_capability_negotiation.md) ←本バージョンで新規作成
- [spec/A002_flow_control.md](../spec/A002_flow_control.md) ←本バージョンで新規作成

---

## 1. 変更概要

| # | 変更種別 | 対象 | 内容 |
|---|---|---|---|
| C-001 | 新規ヘッダ定義 | `detail/frame_header.hpp` | `FLAG_HELLO` / `HelloPayload` 追加 |
| C-002 | 新規公開ヘッダ | `pipeutil/capability.hpp` | `Capability`, `NegotiatedCapabilities`, `HelloConfig` 追加 |
| C-003 | 既存 API 拡張 | `pipeutil/pipe_server.hpp` | `HelloConfig` パラメータ追加（デフォルト引数で後方互換） |
| C-004 | 既存 API 拡張 | `pipeutil/pipe_client.hpp` | `HelloConfig` パラメータ追加、`negotiated_capabilities()` 追加 |
| C-005 | エラーコード追加 | `pipeutil/pipe_error.hpp` | `QueueFull = 40`（A-002 先行予約） |
| C-006 | 実装追加 | `source/core/src/pipe_server.cpp` | HELLO フレーム送受信・ネゴシエーションロジック |
| C-007 | 実装追加 | `source/core/src/pipe_client.cpp` | HELLO フレーム送信・ネゴシエーションロジック |
| C-008 | Python 型追加 | `source/python/` | `PyCapability`, `PyNegotiatedCapabilities`, `PyHelloConfig`, `PyHelloMode` 型 |
| C-009 | Python モジュール更新 | `_pipeutil_module.cpp` | 新型の登録 |
| C-010 | Python 公開 API | `python/pipeutil/__init__.py` | `HelloMode`, `CapabilityBitmap`, `HelloConfig`, `NegotiatedCapabilities` |
| C-011 | 新規仕様書 | `spec/A001_capability_negotiation.md` | HELLO フレーム・ハンドシェイク仕様 |
| C-012 | 新規仕様書 | `spec/A002_flow_control.md` | Flow Control / Backpressure 仕様（実装は v1.3.0） |
| C-013 | 新規テスト | `tests/cpp/test_capability_negotiation.cpp` | C++ 単体・統合テスト |
| C-014 | 新規テスト | `tests/python/test_capability_negotiation.py` | Python 単体・統合テスト |

---

## 2. 前提条件

### 2.1 現在のワイヤーフォーマット（v1.0.0 実装済み）

```
Offset  Size  Field
──────────────────────────────────────────────────────────────
 0      4     magic[4]       = {'P','I','P','E'}
 4      1     version        = 0x02
 5      1     flags          FLAG_COMPRESSED(0x01) / FLAG_ACK(0x02)
                              FLAG_REQUEST(0x04)   / FLAG_RESPONSE(0x08)
                              bits 4-7 = (reserved, 送信時 0)
 6      2     reserved[2]    = 0x00 0x00
 8      4     payload_size   (uint32_t, LE)
12      4     checksum       (uint32_t, LE, CRC-32C)
16      4     message_id     (uint32_t, LE) ; 0 = ID なし
────────────────────────────────────────
合計   20 bytes (static_assert 済み)
```

### 2.2 既存 `PipeErrorCode` 最大値

現在 `TooManyConnections = 33`。新規コードは 40 番台から割り当てる。

---

## 3. HELLO フレーム設計（C-001）

### 3.1 FLAG_HELLO の定義

```cpp
// detail/frame_header.hpp への追加
inline constexpr uint8_t FLAG_HELLO = 0x80;  // bit 7: Capability Negotiation HELLO フレーム
```

既存の FLAG 定数（COMPRESSED / ACK / REQUEST / RESPONSE）が bit 0-3 を使用しており、bit 4-7 は reserved。`FLAG_HELLO = 0x80` は bit 7 に割り当て、他の FLAG との OR 同士は定義しない（HELLO フレームは単独で使用）。

### 3.2 HelloPayload 構造体

```cpp
// detail/frame_header.hpp への追加
struct HelloPayload {
    uint32_t feature_bitmap;   // 送信側が対応する capability の OR (LE)
    uint32_t reserved;         // 0x00000000 固定（将来拡張、64-bit 拡張の余地を残す）
};
static_assert(sizeof(HelloPayload) == 8, "HelloPayload must be 8 bytes");
```

### 3.3 HELLO フレームのバイト列イメージ

```
Offset  Size  Value（v1.1.0 初期接続時）
──────────────────────────────────────────
 0- 3  4     0x50 0x49 0x50 0x45  ('P','I','P','E')
 4     1     0x02                  (PROTOCOL_VERSION)
 5     1     0x80                  (FLAG_HELLO 単独)
 6- 7  2     0x00 0x00             (reserved)
 8-11  4     0x08 0x00 0x00 0x00  (payload_size = 8, LE)
12-15  4     [CRC-32C of payload]  (checksum)
16-19  4     0x00 0x00 0x00 0x00  (message_id = NO_MESSAGE_ID)
────────────────────────────────────────── 20 byte header
20-23  4     0x00 0x00 0x00 0x00  (feature_bitmap = 0x00: v1.1.0 ではコア機能のみ)
24-27  4     0x00 0x00 0x00 0x00  (reserved)
```

---

## 4. capability.hpp — 新規公開ヘッダ設計（C-002）

`source/core/include/pipeutil/capability.hpp` を新規作成する。

```cpp
// capability.hpp — Capability Negotiation 公開型 (v1.1.0)
#pragma once

#include "pipeutil_export.hpp"
#include <chrono>
#include <cstdint>
#include <string>

namespace pipeutil {

// ──────────────────────────────────────────────────────────────────────────────
// Capability — feature_bitmap の各ビットに対応する機能識別子
// ──────────────────────────────────────────────────────────────────────────────
enum class Capability : uint32_t {
    ProtoV2        = 0x01u,  // 24-byte v2 ヘッダ対応（F-009/F-010/F-012/F-013 全完了後）
    ConcurrentRpc  = 0x02u,  // 並行 RPC / in-flight 複数リクエスト（F-009, v1.3.0）
    Streaming      = 0x04u,  // ストリーミングモード（F-010, v1.4.0）
    Heartbeat      = 0x08u,  // ハートビート / プロセス監視（F-012, v1.3.0）
    PriorityQueue  = 0x10u,  // 優先度キュー（F-013, v1.4.0）
    // bits 6-31 = reserved（将来拡張）
};

// ──────────────────────────────────────────────────────────────────────────────
// NegotiatedCapabilities — HELLO 交換後に得られる合意済み機能セット
// （client_bitmap & server_bitmap の結果）
// ──────────────────────────────────────────────────────────────────────────────
struct PIPEUTIL_API NegotiatedCapabilities {
    uint32_t bitmap    = 0u;    // 合意済み機能の OR
    bool     v1_compat = false; // true: 接続相手が v1.0.0 クライアント（version=0x01 で接続）

    /// 指定した機能が合意されているか確認する
    [[nodiscard]] bool has(Capability cap) const noexcept {
        return (bitmap & static_cast<uint32_t>(cap)) != 0u;
    }

    /// 何も合意されていない（v1 モードにフォールバックした）か
    [[nodiscard]] bool is_legacy_v1() const noexcept { return bitmap == 0u; }

    /// v1.0.0 クライアントとの v1-compat 接続かどうかを返す。
    /// is_legacy_v1() == true でも、v1-compat（旧クライアント）と
    /// フォールバック（v1.1.0 クライアントが HELLO 未送信）を区別できる。
    [[nodiscard]] bool is_v1_compat() const noexcept { return v1_compat; }
};

// ──────────────────────────────────────────────────────────────────────────────
// HelloMode — 接続ごとの HELLO ハンドシェイクポリシー
// ──────────────────────────────────────────────────────────────────────────────
enum class HelloMode {
    /// (デフォルト) v1.0.0 クライアントを受け入れ v1-compat モードで処理する。
    /// v1.1.0 クライアントから HELLO が届かない場合は v1 フォールバック（例外なし）。
    /// Rolling upgrade : サーバー先行アップグレード期間の推奨設定。
    Compat,

    /// HELLO なしで接続してきた v1.1.0 クライアントや v1.0.0 クライアントを拒否する。
    /// `hello_timeout` 内に HELLO が届かなければ ConnectionRejected。
    /// 全内部ネットワークなど v1.1.0 専用環境で推奨。
    Strict,

    /// HELLO 詳細シェイクをスキップし、即座に v1 モードで動作する。
    /// 接続待機オーバーヘッドを最小化したい場合（テスト・ベンチマーク等）に使用。
    Skip,
};

// ──────────────────────────────────────────────────────────────────────────────
// HelloConfig — HELLO ハンドシェイクの動作設定
// ──────────────────────────────────────────────────────────────────────────────
struct PIPEUTIL_API HelloConfig {
    /// ハンドシェイクポリシー。デフォルトは Compat（Rolling upgrade 対応）。
    HelloMode mode = HelloMode::Compat;

    /// サーバー側の HELLO 受信タイムアウト。
    /// version=0x02 クライアントからの HELLO をこの時間内に待つ。
    /// 0 を指定すると無制限待機。該当しない場候 (version=0x01) は即座に判別されるためこの値は適用外。
    std::chrono::milliseconds hello_timeout{500};

    /// 自身が対応する capability ビットの OR。
    /// v1.1.0 では未実装機能のビットは 0 のまま。各機能実装時（v1.3.0〜）に更新する。
    uint32_t advertised_capabilities = 0u;
};

}  // namespace pipeutil
```

---

## 5. PipeServer API 変更（C-003）

### 5.1 コンストラクタ拡張

```cpp
// pipe_server.hpp — 変更箇所のみ抜粋
#include "capability.hpp"  // 追加

class PIPEUTIL_API PipeServer {
public:
    /// hello_config を追加（デフォルト引数で既存コードへの影響ゼロ）
    explicit PipeServer(std::string              pipe_name,
                        std::size_t              buffer_size  = 65536,
                        PipeAcl                  acl          = PipeAcl::Default,
                        std::string              custom_sddl  = "",
                        HelloConfig              hello_config = HelloConfig{});
    // ...
```

### 5.2 HELLO 完了コールバック

```cpp
    /// accept() 完了直後に呼び出されるコールバック。HELLO 成否に関わらず必ず呼ばれる。
    /// v1 フォールバック時は caps.is_legacy_v1() == true
    std::function<void(const NegotiatedCapabilities&)> on_hello_complete;
```

### 5.3 negotiated_capabilities() 追加

```cpp
    /// 直近の accept() で確立した接続の合意済み機能を返す
    /// accept() 呼び出し前は bitmap == 0（未定義）
    [[nodiscard]] NegotiatedCapabilities negotiated_capabilities() const noexcept;
```

### 5.4 既存 API 不変

`listen()`, `accept()`, `send()`, `receive()`, `close()`, `stats()` のシグネチャ変更なし。

---

## 6. PipeClient API 変更（C-004）

### 6.1 コンストラクタ拡張

```cpp
// pipe_client.hpp — 変更箇所のみ抜粋
#include "capability.hpp"  // 追加

class PIPEUTIL_API PipeClient {
public:
    /// hello_config を追加（デフォルト引数で既存コードへの影響ゼロ）
    explicit PipeClient(std::string pipe_name,
                        std::size_t buffer_size  = 65536,
                        HelloConfig hello_config = HelloConfig{});
    // ...
```

### 6.2 negotiated_capabilities() 追加

```cpp
    /// connect() 後に使用可能。HELLO が無効またはタイムアウトした場合は bitmap == 0
    [[nodiscard]] NegotiatedCapabilities negotiated_capabilities() const noexcept;
```

### 6.3 既存 API 不変

`connect()`, `send()`, `receive()`, `close()`, `stats()` のシグネチャ変更なし。

---

## 7. エラーコード追加（C-005）

```cpp
// pipe_error.hpp への追加（A-002 先行予約; 実装は v1.3.0）
enum class PipeErrorCode : int {
    // ...既存...
    // Capability Negotiation / Flow Control (v1.1.0+)
    QueueFull        = 40,  // in-flight 上限超過（A-002; v1.3.0 で実装）
};
```

Python 例外マッピング:

| `PipeErrorCode` | Python 例外 |
|---|---|
| `QueueFull` | `pipeutil.QueueFullError`（`PipeError` のサブクラス） |

---

## 8. ハンドシェイク実装ロジック（C-006 / C-007）

### 8.0 受信共通ロジック（全 mode・全フレーム共通）

```
サーバー側 accept() 直後の version 判別（HelloMode::Skip 以外）
── Step 1: magic + version の読み取り（hello_timeout 内）──────────────────
read(buf, 5)タイムアウト → Compat: v1 フォールバック、Strict: ConnectionRejected  // Skip はこのフローを通らない（8.5 参照）
magic 検証 → 不一致なら InvalidMessage

── Step 2: version からヘッダサイズを確定し残りを読む ───────────────
if buf[4] == 0x01:           // v1.0.0 クライアント
    read(header_rest, 11)    // 残り 11 バイト → FrameHeaderV1 完成
    Compat → v1-compat モードへ
    Strict → ConnectionRejected
elif buf[4] == 0x02:         // v1.1.0 クライアント
    read(header_rest, 15)    // 残り 15 バイト → FrameHeader 完成
    → Section 8.1 または 8.3 へ
else: InvalidMessage

── Step 3: ペイロード読み取りと検証 ─────────────────────────────
read(payload_buf, header.payload_size)
computed_crc != header.checksum → InvalidMessage
→ version=0x02: Message{payload_buf, message_id=header.message_id}
   version=0x01: Message{payload_buf, message_id=NO_MESSAGE_ID}
```

> **サーバーの `HelloMode` が最終受け入れポリシーを決定する。クライアントの `mode` は送信側の振る舞いのみを制御し、サーバーの判定を上書きしない。**
> 
> `hello_timeout` は version=0x02 クライアントの HELLO 待機にのみ適用する。`accept()` 直後から計測を開始し、先頭 5 バイトの read も対象に含む。  
> version=0x01 は最初の 5 バイト読み取り時点で即座に判別できるため、タイムアウト待機は発生しない。

---

### 8.1 正常系シーケンス（v1.1.0 同士）

```
Client                              Server
  │ connect()                          │
  │ ─────────────────────────────────► │ accept()
  │                                    │
  │ ──── HELLO(bitmap=0x00) ─────────► │  ← クライアントが先に送信
  │                                    │  フレームを読み取り FLAG_HELLO 確認
  │ ◄─── HELLO(bitmap=0x00) ────────── │  ← サーバーが返信
  │                                    │
  │  negotiated = 0x00 & 0x00 = 0x00  │  negotiated = 0x00 & 0x00 = 0x00
  │                                    │
  │ ──── 通常フレーム（send/receive）── │
```

### 8.2 後退互換シーケンス（v1.0.0 クライアント → v1.1.0 サーバー；`HelloMode::Compat`）

Step 1 で version=0x01 と判別されるため、hello_timeout の待機なしに即座确定。

```
OldClient (v1.0.0)                  Server (v1.1.0, HelloMode::Compat)
  │ connect()                          │ accept()
  │                                    │
  │ ── 先頭 5B（magic+version=0x01） ─► │  Step1: version=0x01 → v1-compat 確定
  │                                    │  Step2: 残り 11B 読み → FrameHeaderV1 完成
  │                                    │  on_hello_complete(caps={bitmap=0, v1_compat=true})
  │ ──── payload ─────────────────► │  Step3: payload 読み取り
  │ ◄─── 応答フレーム（version=0x01, 16B）──
```

**v1-compat モード制約**:
- **送信**: 応答フレームも `version=0x01` ・16B ヘッダ（`FrameHeaderV1`）で送ること。旧クライアントは `version != 0x01` を `InvalidMessage` で拒否するため。
- **受信**: `message_id` フィールドなし。尊詐式 RPC のみ（⚠️ 並行 RPC は v1.3.0 機能のため利用不可）。
- 接続共有メモリ: `conn.protocol_version = 0x01`、`conn.header_size = 16` を per-connection 状態として保持する。

### 8.3 HELLO なしフロー（v1.1.0 クライアント + HELLO タイムアウト）

`HelloMode::Compat` または `HelloMode::Strict` のサーバーに対して、version=0x02 のフレームが届き、`FLAG_HELLO` がない場合の処理。
*（`HelloMode::Skip` クライアントが HELLO を送らずに接続した場合、または `hello_timeout` 内に何も届かない場合）*

```
Client (v1.1.0, mode=Skip)          Server (v1.1.0, Compat/Strict)
  │ connect()                          │ accept()
  │                                    │
    │ ── 先頭 5B（magic+version=0x02） ─► │  Step1: version=0x02 → v1.1.0 確定
  │                                    │  Step2: 残り 15B 読み → FrameHeader 完成
  │                                    │  flags & FLAG_HELLO == 0
    │                                    │  [ Compat ]: フレームをバッファ先頭に戻し v1 フォールバック
  │                                    │  [ Strict       ]: ConnectionRejected
  │ ──── payload ─────────────────► │  (バッファ済みフレーム + 以降のフレームを通常動作)
  │ ◄─── 応答フレーム（version=0x02, 20B） ──
```

**タイムアウトの場合**（先頭 5B すら届かない）:

```
  hello_timeout 経過、何も届かない場合の動作:
    [ Compat ]: on_hello_complete(caps={bitmap=0, v1_compat=false}) 呼び出し → v1 フォールバック
  [ Strict       ]: ConnectionRejected
```

### 8.4 非互換シーケンス（v1.1.0 クライアント → v1.0.0 サーバー）

```
NewClient (v1.1.0)                  OldServer (v1.0.0)
  │ connect()                          │ accept()
  │ ──── HELLO(bitmap=0x00) ─────────► │  ← 旧サーバーは version=0x02 を InvalidMessage で拒否
  │                                    │  → 接続リセット
  │ ◄── ConnectionReset / InvalidMessage ───
```

**⚠️ 非サポート**: v1.0.0 サーバーは `version !=0x01` のフレームを `InvalidMessage` として拒否するため、
v1.1.0 クライアントからの HELLO（`version=0x02`）は接続確立直後に必ず失敗する。
混在デプロイは行わず、1回のカットオーバーで全 endpoint を同時にアップグレードすること。

### 8.5 HelloMode::Skip シーケンス（内部最適化・テスト用途）

```
Client (v1.1.0, mode=Skip)          Server (v1.1.0, mode=Skip)
  │ connect()                          │ accept()
  │                                    │  hello_timeout 待機なし、即座 v1 モード
  │ ──── 通常フレーム（version=0x02） ─► │
  │ ◄─── 応答フレーム（version=0x02） ───
```

`mode=Skip` ではサーバーは Step 1 をスキップして即座に v1 モードで待機。
Skip サーバーは `FLAG_HELLO` を一切処理しない。クライアントが HELLO フレームを送信した場合でも、サーバーは `FLAG_HELLO` ビットを無視して通常フレームとして処理する（初回・2回目以降を問わず `InvalidMessage` にはならない）。両側 Skip の時のみ使用すること。

---

## 9. Python 公開 API 設計（C-008 / C-009 / C-010）

### 9.1 `pipeutil/__init__.py` 追加エクスポート

```python
from enum import IntFlag, IntEnum
from dataclasses import dataclass, field

class HelloMode(IntEnum):
    """接続ごとの HELLO ハンドシェイクポリシー。"""
    COMPAT = 0  # v1.0.0 クライアントを受け入れ、HELLO なし → v1 フォールバック
    STRICT = 1  # HELLO なしまたは v1.0.0 クライアント → ConnectionRejected
    SKIP   = 2  # HELLO 交換をスキップ（両側設定必須）

class CapabilityBitmap(IntFlag):
    """HELLO フレームの feature_bitmap ビット定義。"""
    PROTO_V2       = 0x01  # 24-byte v2 ヘッダ (v2.0.0 完成時)
    CONCURRENT_RPC = 0x02  # 並行 RPC (v1.3.0)
    STREAMING      = 0x04  # ストリーミング (v1.4.0)
    HEARTBEAT      = 0x08  # ハートビート (v1.3.0)
    PRIORITY_QUEUE = 0x10  # 優先度キュー (v1.4.0)

@dataclass(frozen=True)
class NegotiatedCapabilities:
    """HELLO 交換で合意した機能セット。"""
    bitmap:    int  = 0
    v1_compat: bool = False  # True: 接続相手が v1.0.0 クライアント（version=0x01 で接続）

    def has(self, cap: CapabilityBitmap) -> bool:
        return bool(self.bitmap & int(cap))

    @property
    def is_legacy_v1(self) -> bool:
        return self.bitmap == 0

    @property
    def is_v1_compat(self) -> bool:
        """v1.0.0 クライアントとの v1-compat 接続かどうかを返す。is_legacy_v1 と併用することで
        v1-compat（旧クライアント）とフォールバック（v1.1.0 クライアントが HELLO 未送信）を区別できる。"""
        return self.v1_compat

@dataclass
class HelloConfig:
    """接続ごとの HELLO ハンドシェイク設定。"""
    mode: HelloMode = HelloMode.COMPAT        # ポリシー（rolling upgrade 期間は COMPAT 推奨）
    hello_timeout_ms: int = 500               # version=0x02 クライアントの HELLO 待機 (ms)
    advertised_capabilities: int = 0          # CapabilityBitmap の OR (int)
```

### 9.2 `PipeServer` / `PipeClient` Python 型の変更

```python
# C API 変更後の利用イメージ
server = pipeutil.PipeServer(
    "my_pipe",
    hello_config=pipeutil.HelloConfig()
)
server.on_hello_complete = lambda caps: print(f"caps={caps.bitmap:#010x}")
server.listen()
server.accept()
caps = server.negotiated_capabilities  # NegotiatedCapabilities

client = pipeutil.PipeClient(
    "my_pipe",
    hello_config=pipeutil.HelloConfig()
)
client.connect()
caps = client.negotiated_capabilities  # NegotiatedCapabilities
```

### 9.3 新規例外クラス

```python
# pipeutil.QueueFullError を PipeError のサブクラスとして登録
class QueueFullError(PipeError): ...
```

---

## 10. 変更対象ファイル一覧

### 10.1 C++ コア — 既存ファイル変更

| ファイル | 変更内容 |
|---|---|
| `source/core/include/pipeutil/detail/frame_header.hpp` | `FLAG_HELLO = 0x80`, `HelloPayload` 構造体, `FrameHeaderV1`（16B）構造体, `static_assert` 追加 |
| `source/core/include/pipeutil/pipe_server.hpp` | `#include "capability.hpp"`, `HelloConfig` パラメータ, `negotiated_capabilities()`, `on_hello_complete` コールバック追加 |
| `source/core/include/pipeutil/pipe_client.hpp` | `#include "capability.hpp"`, `HelloConfig` パラメータ, `negotiated_capabilities()` 追加 |
| `source/core/include/pipeutil/pipe_error.hpp` | `QueueFull = 40` 追加 |
| `source/core/include/pipeutil/pipeutil.hpp` | `#include "capability.hpp"` 追加 |
| `source/core/src/pipe_server.cpp` | HELLO 先読み・送受信・ネゴシエーションロジック実装 |
| `source/core/src/pipe_client.cpp` | HELLO 送信・受信・ネゴシエーションロジック実装 |

### 10.2 C++ コア — 新規ファイル

| ファイル | 内容 |
|---|---|
| `source/core/include/pipeutil/capability.hpp` | `Capability`, `NegotiatedCapabilities`, `HelloConfig` 定義 |
| `source/core/src/capability.cpp` | `NegotiatedCapabilities` の非インライン実装（必要な場合） |

### 10.3 Python C API — 既存ファイル変更

| ファイル | 変更内容 |
|---|---|
| `source/python/py_pipe_server.cpp` | `HelloConfig` パラメータ受け取り, `on_hello_complete` コールバック, `negotiated_capabilities` プロパティ追加 |
| `source/python/py_pipe_server.hpp` | 上記の宣言追加 |
| `source/python/py_pipe_client.cpp` | `HelloConfig` パラメータ受け取り, `negotiated_capabilities` プロパティ追加 |
| `source/python/py_pipe_client.hpp` | 上記の宣言追加 |
| `source/python/py_exceptions.cpp` | `QueueFullError` 登録追加 |
| `source/python/py_exceptions.hpp` | `g_QueueFullError` 外部宣言追加 |
| `source/python/_pipeutil_module.cpp` | `CapabilityBitmap` 定数, `PyNegotiatedCapabilities`, `PyHelloConfig` 型登録 |

### 10.4 Python C API — 新規ファイル

| ファイル | 内容 |
|---|---|
| `source/python/py_capability.cpp` | `PyNegotiatedCapabilities`, `PyHelloConfig` 型実装 |
| `source/python/py_capability.hpp` | 上記宣言 |

### 10.5 Python パッケージ — 変更

| ファイル | 変更内容 |
|---|---|
| `python/pipeutil/__init__.py` | `CapabilityBitmap`, `NegotiatedCapabilities`, `HelloConfig`, `QueueFullError` 追加 |

### 10.6 仕様書 — 新規

| ファイル | 内容 |
|---|---|
| `spec/A001_capability_negotiation.md` | HELLO フレーム定義・ハンドシェイク手順・ビット割り当て |
| `spec/A002_flow_control.md` | Flow Control / Backpressure 契約仕様（実装は v1.3.0） |

### 10.7 テスト — 新規

| ファイル | 内容 |
|---|---|
| `tests/cpp/test_capability_negotiation.cpp` | C++ 単体 + 統合テスト |
| `tests/python/test_capability_negotiation.py` | Python 単体 + 統合テスト |

---

## 11. テスト計画

### 11.1 C++ テストケース（`test_capability_negotiation.cpp`）

| テスト ID | テスト名 | 検証内容 |
|---|---|---|
| T-CN-001 | `HelloPayloadSize` | `sizeof(HelloPayload) == 8` |
| T-CN-002 | `FlagHellosValue` | `FLAG_HELLO == 0x80` かつ既存 FLAG と重複しない |
| T-CN-003 | `HelloFrameEncode` | HELLO フレームのバイト列が仕様通り（magic, version, FLAG_HELLO, payload_size=8, CRC-32C） |
| T-CN-004 | `HelloFrameDecode` | 正常 HELLO バイト列 → `HelloPayload` の正しいデコード |
| T-CN-005 | `NegotiationBothV110` | クライアント・サーバー双方 v1.1.0 → `negotiated_capabilities().is_legacy_v1() == true`（v1.1.0 は bitmap=0） |
| T-CN-006 | `NegotiationCompatV1Client` | v1.0.0 クライアントのダミー（version=0x01 送信） + サーバー `Compat` → v1-compat モードで正常応答 |
| T-CN-007 | `NegotiationStrictRejectsV1Client` | v1.0.0 クライアントのダミー + サーバー `Strict` → ConnectionRejected |
| T-CN-008 | `NegotiationSkipMode` | 両側 `HelloMode::Skip` → HELLO 交換なしで通常メッセージ送受信が正常 |
| T-CN-009 | `NegotiationCompatTimeout` | `HelloMode::Compat` + hello_timeout=50ms → タイムアウト後 v1 フォールバック |
| T-CN-010 | `NegotiationStrictTimeout` | `HelloMode::Strict` + hello_timeout=50ms + クライアント Skip → ConnectionRejected |
| T-CN-011 | `BitAndNegotiation` | server_bitmap=0x0F, client_bitmap=0x05 → negotiated=0x05 |
| T-CN-012 | `OnHelloCompleteCallback` | `on_hello_complete` が accept() 後に呼び出される |
| T-CN-013 | `NormalMessageAfterHello` | HELLO 後に通常メッセージ 10 往復が正常に動作する |
| T-CN-014 | `V1CompatNormalMessages` | v1-compat モードで通常メッセージ 10 往復が正常に動作する |

### 11.2 Python テストケース（`test_capability_negotiation.py`）

| テスト ID | テスト名 | 検証内容 |
|---|---|---|
| T-PY-CN-001 | `test_capability_bitmap_values` | `CapabilityBitmap` 各値が仕様通り |
| T-PY-CN-002 | `test_negotiated_capabilities_has` | `NegotiatedCapabilities(bitmap=0x05).has(CONCURRENT_RPC) == True` |
| T-PY-CN-003 | `test_negotiated_capabilities_legacy` | `NegotiatedCapabilities().is_legacy_v1 == True` |
| T-PY-CN-004 | `test_hello_config_defaults` | `HelloConfig()` のデフォルト値確認 |
| T-PY-CN-005 | `test_server_client_negotiation` | Python PipeServer ↔ PipeClient で HELLO 往来 → `negotiated_capabilities` が正しく取得できる |
| T-PY-CN-006 | `test_negotiation_skip_mode` | 両側 `HelloConfig(mode=HelloMode.SKIP)` → HELLO 交換なしで通常メッセージ送受信が正常 |
| T-PY-CN-007 | `test_on_hello_complete_callback` | `on_hello_complete` コールバックが accept 後に呼ばれる |
| T-PY-CN-008 | `test_queue_full_error_is_pipe_error` | `QueueFullError` が `PipeError` のサブクラス |
| T-PY-CN-009 | `test_hello_config_custom_timeout` | `hello_timeout_ms=100` が C++ 側に正しく渡る |

### 11.3 リリース条件

- T-CN-001 〜 T-CN-014 の全通過（Windows / Linux）。
- T-PY-CN-001 〜 T-PY-CN-009 の全通過（Windows / Linux × Python 3.8 〜 3.14）。
- `HelloConfig` 未指定（デフォルト引数）での既存テスト（`test_roundtrip.cpp`, `test_rpc.cpp`, 等）の全通過（後方互換確認）。
- `spec/A001_capability_negotiation.md` + `spec/A002_flow_control.md` 完成。

---

## 12. 後方互換性保証

| シナリオ | 挙動 | 注意 |
|---|---|---|
| 既存コードが `HelloConfig` 引数を指定しない | デフォルト引数適用（`mode=Compat`, `hello_timeout=500ms`） | 既存コードの再コンパイルのみ |
| v1.0.0 クライアント → v1.1.0 サーバー（`Compat`） | v1-compat モードで透明に指行（version=0x01 ヘッダで応答） | version バイトで即座判別、HELLO 待機なし |
| v1.0.0 クライアント → v1.1.0 サーバー（`Strict`） | ConnectionRejected | `Compat` でサーバーを公開し、全クライアント移行後に `Strict` に切り替え |
| v1.1.0 クライアント → v1.0.0 サーバー | **非サポート**（旧サーバーが `version=0x02` フレームを `InvalidMessage` で拒否） | サーバーを先に v1.1.0 にアップグレードすること |
| 両側 `HelloMode::Skip` | HELLO フレームの送受信なし、version=0x02 ワイヤーで即座対話開始 | テスト・専用クラスタ内用途 |

---

## 13. 実装順序（推奨）

1. `spec/A001_capability_negotiation.md` + `spec/A002_flow_control.md` を完成させてレビュー依頼。
2. `capability.hpp` + `frame_header.hpp` の変更（C-001, C-002）→ コンパイル確認。
3. `pipe_error.hpp` に `QueueFull` 追加（C-005）。
4. `pipe_client.cpp` に HELLO 送信ロジック実装（C-007）。
5. `pipe_server.cpp` に HELLO 先読み・受信・フォールバックロジック実装（C-006）。
6. C++ テスト作成 + 全通過確認（C-013）。
7. Python C API 変更（C-008, C-009）+ `capability.cpp`。
8. `python/pipeutil/__init__.py` 更新（C-010）。
9. Python テスト作成 + 全通過確認（C-014）。
10. 既存テスト（全スイート）のリグレッション確認。

---

*仕様詳細は [spec/A001_capability_negotiation.md](../spec/A001_capability_negotiation.md) および [spec/A002_flow_control.md](../spec/A002_flow_control.md) を参照。*
