# A-002 — Flow Control / Backpressure / Quota 仕様

**バージョン**: 1.0 (v1.1.0 にて仕様策定, 実装は v1.3.0)  
**作成日**: 2026-03-15  
**ステータス**: 確定（仕様のみ; 実装は v1.3.0 以降）  
**関連仕様**: [spec/A001_capability_negotiation.md](A001_capability_negotiation.md), [spec/F002_rpc_message_id.md](F002_rpc_message_id.md)

---

## 1. 概要と目的

並行 RPC（F-009, v1.3.0）とストリーミング（F-010, v1.4.0）を安全に実装するために必要な
**フロー制御・バックプレッシャー・クォータ**の契約を事前に定義する。

この仕様書は **実装ガイドとしての拘束力を持つ**。
F-009 / F-010 の実装者は必ず本仕様を読んでから着手すること。

### なぜ先に仕様化するか

| 理由 | 詳細 |
|---|---|
| メモリ安全 | 送信側が無制限に enqueue できる設計はメモリ使用量が制御できない |
| タイムアウト安全 | orphan response（受信者がいなくなった in-flight 応答）の後始末がないと資源リークする |
| キャンセル契約 | `cancel()` が「キャンセルの試み」ではなく「キャンセルの確約」であることを定義する |
| メトリクスの整合 | v1.0.0 の `PipeStats` に追加する統計項目をここで確定する |

---

## 2. 用語定義

| 用語 | 定義 |
|---|---|
| **in-flight リクエスト** | 送信済みだがレスポンスをまだ受け取っていない RPC リクエスト |
| **orphan response** | 元リクエストがタイムアウト/キャンセルされた後に届いたレスポンス |
| **backpressure** | 受信側が処理しきれない場合に送信側を一時停止させる仕組み |
| **window size** | ストリームごとに送信側が確認応答なしに送信できる最大バイト数 |
| **quota** | 接続ごとに設定する in-flight 数・バッファサイズの上限 |

---

## 3. 接続レベルのフロー制御パラメータ

### 3.1 `FlowControlConfig` 構造体

```cpp
// capability.hpp、または専用の flow_control.hpp に追加（v1.3.0 実装時）
namespace pipeutil {

struct FlowControlConfig {
    // ── in-flight 数制御 ──────────────────────────────────────────────────
    /// 接続ごとに同時に in-flight にできる RPC リクエストの上限
    /// 0 = 上限なし（非推奨; デバッグ用途のみ）
    /// デフォルト = 64
    uint32_t max_inflight_requests = 64;

    // ── 送信バッファ制御 ───────────────────────────────────────────────────
    /// 接続ごとの送信バッファ上限（バイト単位）
    /// 上限に達すると send() がブロック（または QueueFull を返す）
    /// 0 = 上限なし（非推奨）
    /// デフォルト = 8 MiB
    uint64_t max_buffered_bytes = 8u * 1024 * 1024;

    // ── タイムアウト ───────────────────────────────────────────────────────
    /// RPC リクエストのデフォルトタイムアウト（0 = 無制限）
    /// 個別リクエストでオーバーライド可能
    std::chrono::milliseconds default_rpc_timeout{30'000};

    // ── overflow 時の挙動 ─────────────────────────────────────────────────
    /// true  = 上限到達時に send() / call_async() が QueueFull 例外を即時送出
    /// false = 上限到達時に send() / call_async() がブロック（デフォルト）
    bool throw_on_overflow = false;
};

}  // namespace pipeutil
```

### 3.2 パラメータ適用タイミング

`FlowControlConfig` は接続確立時（`PipeServer::accept()` / `PipeClient::connect()` 内部）に適用する。  
接続確立後の変更はサポートしない（スレッド競合を避けるため）。

```cpp
// v1.3.0 での利用イメージ（設計予約; v1.1.0 時点では未実装）
pipeutil::PipeClient client(
    "my_pipe",
    65536,
    pipeutil::HelloConfig{},
    pipeutil::FlowControlConfig{
        .max_inflight_requests = 32,
        .max_buffered_bytes    = 4 * 1024 * 1024,
        .default_rpc_timeout   = std::chrono::seconds{10},
    }
);
```

---

## 4. ストリームレベルのフロー制御

### 4.1 ウィンドウサイズ

F-010 ストリーミング（v1.4.0）向けの仕様予約。

```cpp
struct StreamFlowConfig {
    /// ストリームごとに確認応答（ACK）なしに送信できる最大バイト数
    /// ウィンドウが枯渇すると ChunkWriter がブロックする
    /// デフォルト = 1 MiB
    uint64_t window_size = 1u * 1024 * 1024;

    /// 受信側が消費した分を ACK で通知する際の閾値
    /// window_size の半分を消費した時点で ACK を送る
    /// デフォルト = window_size / 2
    uint64_t ack_threshold = window_size / 2;
};
```

### 4.2 ウィンドウ制御シーケンス

```
Sender                              Receiver
 │ send CHUNK(seq=1, 512KB) ──────► │  window_remaining = 1MB - 512KB = 512KB
 │ send CHUNK(seq=2, 512KB) ──────► │  window_remaining = 0  → window 枯渇
 │ (ブロック waiting for ACK)       │
 │                                  │  アプリが 600KB 消費
 │ ◄────────────── ACK(600KB) ───── │  window_grant += 600KB
 │ send CHUNK(seq=3, 256KB) ──────► │  ...
```

**ACK フレームの識別**: 既存 `FLAG_ACK = 0x02` フラグを使用する（v2.0.0 正式化前は予約）。  
ストリーム ACK フレームのペイロード: `stream_id(4B) + bytes_consumed(4B)` = 8 バイト固定。  
（詳細は F-010 実装時の仕様書に委ねる）

---

## 5. キャンセル API 仕様

### 5.1 `cancel(message_id)` — RPC リクエストのキャンセル

#### 契約

```
cancel(message_id: uint64_t) の事後条件:
  - 戻り値 true  : 対象リクエストをキャンセルした。対応する Future は PipeErrorCode::Cancelled を保持する。
  - 戻り値 false : message_id が存在しない（既に完了 or タイムアウト済み）。
  - 例外          : キャンセル操作自体の失敗（NotConnected, SystemError など）。
```

```cpp
// v1.3.0 ConcurrentRpcClient への追加（設計予約）
namespace pipeutil {

class ConcurrentRpcClient {
public:
    // ...
    /// キャンセルを試みる。戻り値 = キャンセルが受理されたか（false = すでに完了済み）
    /// この関数は対象 Future を Cancelled 状態（PipeErrorCode::Cancelled）にして即時返る。
    /// 相手側へのキャンセル通知は「ベストエフォート」（到達保証なし）。
    [[nodiscard]] bool cancel(uint64_t message_id);
    // ...
};

}  // namespace pipeutil
```

#### orphan response の後始末

キャンセル後に届いた対象の response フレームは**黙って破棄**する（例外を投げない）。  
orphan window は接続単位で最大 `max_inflight_requests` 数の response を保持できる。

#### Python API（設計予約）

```python
# v1.3.0 AsyncRpcClient への追加（設計予約）
success = await client.cancel(message_id=42)
# True = キャンセル受理; False = すでに完了
```

### 5.2 `cancel(stream_id)` — ストリームのキャンセル

```
cancel(stream_id: uint32_t) の事後条件:
  - 送信側から呼び出した場合: FLAG_STREAM_END フレームを stream を即時送信し、以降の ChunkWriter への書き込みは BrokenPipe を返す。
  - 受信側から呼び出した場合: 以降の受信チャンクを黙って破棄し、相手側へキャンセル通知（CANCEL_STREAM フレーム; v1.4.0 仕様で確定）を送る。
```

---

## 6. エラーコード拡張

### v1.1.0 で先行予約（実装は v1.3.0 以降）

```cpp
enum class PipeErrorCode : int {
    // ...既存 (〜 TooManyConnections = 33)...

    // Flow Control / Concurrent RPC (A-002; 実装 v1.3.0)
    QueueFull        = 40,  // max_inflight_requests 上限超過, または max_buffered_bytes 超過
    Cancelled        = 41,  // cancel() によってキャンセルされた
    WindowExhausted  = 42,  // stream window size 枯渇（送信ブロック解除に使用; v1.4.0）
};
```

### Python 例外クラス

```python
class QueueFullError(PipeError): ...    # max_inflight_requests or max_buffered_bytes 超過
class CancelledError(PipeError): ...    # cancel() によりキャンセル
class WindowExhaustedError(PipeError): ...  # stream window 消耗（通常はブロックで表現）
```

---

## 7. `PipeStats` 拡張（メトリクス追加）

A-002 の実装に伴い `PipeStats` に以下の統計項目を追加する（v1.3.0 実装時に合わせて変更）。

```cpp
// pipe_stats.hpp への追加（v1.3.0 実装時）
struct PipeStats {
    // ...既存...

    // ── 並行 RPC 統計 ─────────────────────────────────────────────
    uint64_t current_inflight_requests = 0;   // 現在の in-flight 数
    uint64_t peak_inflight_requests    = 0;   // 接続確立以降のピーク in-flight 数
    uint64_t queue_full_events         = 0;   // QueueFull が発生した回数
    uint64_t cancelled_requests        = 0;   // cancel() が成功した回数
    uint64_t orphan_responses_dropped  = 0;   // タイムアウト後に届き破棄した response 数
    uint64_t rpc_timeout_events        = 0;   // RPC タイムアウトが発生した回数

    // ── ストリーム統計 ────────────────────────────────────────────
    uint64_t stream_bytes_sent     = 0;   // ストリームで送信した累積バイト数
    uint64_t stream_bytes_received = 0;   // ストリームで受信した累積バイト数
    uint64_t stream_cancel_events  = 0;   // ストリームキャンセルが発生した回数
};
```

**累積統計の契約（Metrics-Contract）**: 接続終了後も `stats()` の値は消えない（v1.0.0 からの既存契約を維持する）。`reset_stats()` が呼ばれるまで加算し続ける。

---

## 8. バックプレッシャー動作定義

### 8.1 `throw_on_overflow = false`（デフォルト: ブロック）

```
送信側が max_buffered_bytes に達した場合:
  send() / call_async() はブロックし、内部バッファに空きが生じるまで待機する。
  deadline（default_rpc_timeout）を超えた場合は PipeErrorCode::Timeout を送出する。
```

### 8.2 `throw_on_overflow = true`（即時失敗）

```
送信側が max_buffered_bytes または max_inflight_requests に達した場合:
  send() / call_async() は即時に PipeErrorCode::QueueFull を送出する。
  呼び出し側でリトライ判断を行う（指数バックオフを推奨）。
```

### 8.3 max_inflight_requests 超過

```
ConcurrentRpcClient が新規 call_async() を呼ぶ時点で inflight 数が
max_inflight_requests に達している場合:
  - throw_on_overflow = false → ブロック（スロットが空くまで待機）
  - throw_on_overflow = true  → 即時 QueueFull 例外
```

---

## 9. デッドロック回避ガイドライン

並行 RPC とストリーミングを組み合わせる場合、次のパターンでデッドロックが
発生しうる。実装時は必ず下記を確認すること。

| パターン | リスク | 回避策 |
|---|---|---|
| 同一スレッドで send + receive をブロッキング実行 | A → B の応答待ち中に B → A の応答が必要になる | 送受信を別スレッドに分離、または asyncio/Future を使用 |
| stream window 枯渇 + RPC 応答ブロック | ストリーム ACK を受け取る前に RPC がタイムアウト | RPC タイムアウトを stream flush 時間より長く設定 |
| 最大 in-flight 到達 + cancel() のブロック | cancel 自体が新規フレームの送信が必要なため、send キューが詰まっている時に cancel() がブロック | cancel() は高優先で送信キューをバイパスする（実装要件） |

---

## 10. 設計上の決定事項（レビュー確認済み）

| 項目 | 決定内容 | 根拠 |
|---|---|---|
| `cancel()` の保証レベル | best-effort（到達保証なし）| パイプが切断されている場合に cancel 通知を保証するとデッドロックリスクが増す |
| orphan response の扱い | 黙って破棄 | 既にキャンセルされたリクエストの応答をアプリ層に渡しても無意味 |
| `FlowControlConfig` の不変性 | 接続中は変更不可 | スレッド競合を根本的に排除 |
| ストリーム window ACK フレーム | FLAG_ACK を流用（詳細は F-010 仕様） | 新フレームタイプを増やさない |
| cancel 優先送信 | cancel フレームを通常 send キューより先に送信 | キューが詰まっていても cancel は届かなければならない |

---

*本仕様書は v1.3.0（F-009 Concurrent RPC）の実装着手前に実装者が参照すること。  
変更が必要な場合は `review/` に Issue を起票し、レビュアー（CODEX）の承認を得ること。*
