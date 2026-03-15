# pipeutil v2.0.0 追加実装提案

**作成日**: 2026-03-14  
**位置づけ**: `docs/feature_proposals_v2.0.md` の補完提案。レビュアー観点で「先に必要」と判断した項目を整理する。

---

## A-001 — Capability Negotiation / Feature Bitmap

### 必要性

F-009 と F-010 が protocol v2 を前提にするなら、v1/v2 が同一ホストで混在する移行期間を安全に扱う仕組みが必要です。単純な version mismatch だけでは、どの optional feature が使えるか判定できません。

### 提案内容

- 初回接続時に `HELLO` フレームまたは out-of-band handshake を送る。
- peer が対応する feature bit を交換する。
- `concurrent_rpc`, `streaming`, `heartbeat`, `priority_queue` を個別 capability として扱う。

### 配布方針

- **コア同梱** `pipeutil`
- 追加の外部依存なし

### 価値

- C# ラッパーを protocol freeze 後に安全に乗せられる。
- v2 feature を段階的に rollout できる。

---

## A-002 — Flow Control / Backpressure / Quota

### 必要性

並行 RPC とストリーミングを入れるなら、送信側が無制限に enqueue できる設計は危険です。特に Python 側はメモリ使用量が可視化しづらく、timeout や cancel も絡みます。

### 提案内容

- 接続ごとの `max_inflight_requests`
- 接続ごとの `max_buffered_bytes`
- ストリームごとの window size
- `cancel(message_id)` と `cancel(stream_id)` の明示 API
- quota 超過時のエラーコード標準化

### 配布方針

- **コア同梱** `pipeutil`
- 追加の外部依存なし

### 価値

- async lifecycle の再事故を防ぎやすい。
- メトリクスと運用監視の意味が明確になる。

---

## A-003 — Interop Certification Kit

### 必要性

多言語対応を本気でやるなら、実装そのものより**互換性検証資産**の方が長く効きます。C++, Python, C# の 3 系統が揃った時点で、仕様の曖昧さはすぐに齟齬として表面化します。

### 提案内容

- golden frame corpus (`tests/interop/golden_frames/`)
- CRC-32C の既知値テスト
- 異常系フレーム（checksum mismatch, truncated frame, unsupported version）の共通テスト
- Windows/Linux 両方で回る相互接続 CI matrix

### 配布方針

- **コア同梱** `pipeutil`
- 追加のユーザー依存なし

### 価値

- C# / Java / Rust の将来追加コストを大幅に下げる。
- 仕様書と実装の乖離をレビュー前に検知できる。

---

## A-004 — Broker を core ではなく別パッケージ化

### 必要性

F-011 のユースケース自体はありますが、broker 機能は transport core に入れるより分離した方が保守しやすいです。

### 提案内容

- `pipeutil-broker` を独立 package / executable として用意する。
- core の `MultiPipeServer` と `PipeClient` を内部利用し、topic routing は別責務にする。
- Python では `pip install pipeutil-broker`、C++ では別 target として提供する。

### 配布方針

- **別パッケージ** `pipeutil-broker`
- コア `pipeutil` の依存・API を肥大化させない

### 価値

- コアの安定性を維持したまま、Pub/Sub の市場性を検証できる。
- 将来不要と判断した場合でも core API を汚さずに撤回できる。

---

## 推奨順位

1. A-001 Capability Negotiation
2. A-002 Flow Control / Backpressure
3. A-003 Interop Certification Kit
4. A-004 Broker 別パッケージ化