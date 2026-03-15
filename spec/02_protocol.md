# pipeutil — 通信プロトコル / メッセージフォーマット仕様

## 1. 概要

`pipeutil` はストリーム型パイプ上で「**フレーム化メッセージ**」を扱います。
パイプ自体はバイトストリームであるため、メッセージ境界を明示するために
固定長ヘッダ + 可変長ペイロードの 2 部構成を採用します。

---

## 2. フレームフォーマット

```
 0               1               2               3
 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
├───────────────────────────────────────────────────────────────────┤
│  magic[0]  │  magic[1]  │  magic[2]  │  magic[3]               │
├───────────────────────────────────────────────────────────────────┤
│  version   │  flags     │  reserved[0]    │  reserved[1]        │
├───────────────────────────────────────────────────────────────────┤
│  payload_size (uint32_t, little-endian)                           │
├───────────────────────────────────────────────────────────────────┤
│  checksum   (uint32_t, little-endian, CRC-32C)                    │
├───────────────────────────────────────────────────────────────────┤
│  message_id (uint32_t, little-endian; 0x00000000 = ID なし)       │
├───────────────────────────────────────────────────────────────────┤
│  payload (payload_size バイト) ...                                │
└───────────────────────────────────────────────────────────────────┘
```

### 2.1 フィールド詳細

| フィールド | オフセット | サイズ | 型 | 説明 |
|-----------|-----------|--------|-----|------|
| `magic` | 0 | 4 バイト | `uint8_t[4]` | `{0x50, 0x49, 0x50, 0x45}` = `"PIPE"` |
| `version` | 4 | 1 バイト | `uint8_t` | プロトコルバージョン（現在 = `0x02`; v1.0.0 まで `0x01`）|
| `flags` | 5 | 1 バイト | `uint8_t` | ビットフラグ（後述） |
| `reserved` | 6 | 2 バイト | `uint8_t[2]` | 将来拡張用、送信時は `0x00` で埋めること |
| `payload_size` | 8 | 4 バイト | `uint32_t` (LE) | ペイロードのバイト数（ヘッダ含まず） |
| `checksum` | 12 | 4 バイト | `uint32_t` (LE) | ペイロード全体の CRC-32C チェックサム |
| `message_id` | 16 | 4 バイト | `uint32_t` (LE) | RPC メッセージ ID（F-002）。`0x00000000` = ID なし（通常メッセージ）|
| `payload` | 20 | 可変 | `uint8_t[]` | メッセージ本体 |

**ヘッダ固定サイズ**: `20 バイト`（v1.0.0 までは 16 バイト; F-002 で `message_id` フィールド追加）

### 2.2 `flags` ビット定義

| ビット | 名前 | 意味 |
|-------|------|------|
| 0 | `FLAG_COMPRESSED` | ペイロードが圧縮済み（将来拡張、現在は使用しない） |
| 1 | `FLAG_ACK` | ACK メッセージ（将来拡張） |
| 2 | `FLAG_REQUEST` | RPC リクエスト（F-002）|
| 3 | `FLAG_RESPONSE` | RPC レスポンス（F-002）|
| 4–6 | (reserved) | 送受信時は `0` を設定すること |
| 7 | `FLAG_HELLO` | Capability Negotiation HELLO フレーム（v1.1.0; [spec/A001](A001_capability_negotiation.md)）|

---

## 3. バイトオーダー

**全フィールド**: リトルエンディアン（x86/x64 ネイティブ、ARM LE ネイティブ）

変換ユーティリティ（C++20 `<bit>` 使用、MSVC / GCC / Clang 全対応）:
```cpp
// <pipeutil/detail/endian.hpp>
#include <bit>       // std::endian, std::byteswap (C++23) — C++20 では手動実装
#include <cstdint>
namespace pipeutil::detail {

/// ホストバイトオーダー → リトルエンディアン変換
/// C++20: std::endian で分岐。MSVC / GCC / Clang 共通コンパイル可能。
constexpr uint32_t to_le32(uint32_t v) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return v;
    } else {
        // ビッグエンディアン環境用バイトスワップ（C++20 準拠、コンパイラ最適化対象）
        return ((v & 0x000000FFu) << 24)
             | ((v & 0x0000FF00u) <<  8)
             | ((v & 0x00FF0000u) >>  8)
             | ((v & 0xFF000000u) >> 24);
    }
}

constexpr uint32_t from_le32(uint32_t v) noexcept {
    return to_le32(v);  // 対称操作
}

} // namespace pipeutil::detail
```

---

## 4. チェックサム（CRC-32C）

アルゴリズム: **CRC-32C**（Castagnoli ポリノミアル `0x1EDC6F41`）
対象データ: `payload` フィールドのみ（ヘッダは含まない）
受信側はペイロードの CRC-32C を計算し、ヘッダの `checksum` と一致しない場合は `PipeErrorCode::InvalidMessage` を送出する。

```cpp
/// payload のみを対象に CRC-32C を計算する
uint32_t compute_crc32c(std::span<const std::byte> payload) noexcept;
```

---

## 5. フレーム送信シーケンス

```
送信側 (send)
  1. payload_size = message.size()
  2. checksum    = compute_crc32c(message.payload())
  3. FrameHeader を構築（マジック・バージョン・フラグ・payload_size・checksum・message_id）
     ※ 通常メッセージは message_id = NO_MESSAGE_ID (0x00000000)
  4. write(header, 20 バイト)
  5. write(payload, payload_size バイト)
```

## 6. フレーム受信シーケンス

```
受信側 (receive)
  1. read(header_buf, 20 バイト)  ← 20 バイト揃うまでブロック
  2. magic 検証 → 不一致なら InvalidMessage
  3. version 検証 → 0x02 以外なら InvalidMessage
  4. payload_size = header.payload_size
  5. read(payload_buf, payload_size バイト)  ← 全バイト揃うまでブロック
  6. computed_crc = compute_crc32c(payload_buf)
  7. computed_crc != header.checksum → InvalidMessage
  8. Message{payload_buf, message_id=header.message_id} を返す
```

---

## 7. ヘッダ構造体（C++）

```cpp
// <pipeutil/detail/frame_header.hpp>
namespace pipeutil::detail {

#pragma pack(push, 1)  // パディングなし（明示的レイアウト）
struct FrameHeader {
    uint8_t  magic[4];        // {'P','I','P','E'}
    uint8_t  version;         // PROTOCOL_VERSION (0x02)
    uint8_t  flags;           // FLAG_* ビット
    uint8_t  reserved[2];     // 将来拡張用。送信時は 0x00 で埋めること
    uint32_t payload_size;    // ペイロードサイズ（リトルエンディアン）
    uint32_t checksum;        // CRC-32C（ペイロードのみ、リトルエンディアン）
    uint32_t message_id;      // RPC メッセージ ID（LE）; 0 = ID なし（F-002）
};
#pragma pack(pop)

static_assert(sizeof(FrameHeader) == 20, "FrameHeader must be exactly 20 bytes");

inline constexpr uint8_t  MAGIC[4]          = {0x50, 0x49, 0x50, 0x45};  // "PIPE"
inline constexpr uint8_t  PROTOCOL_VERSION  = 0x02;  // F-002 で 0x01 → 0x02 へ更新

inline constexpr uint8_t  FLAG_COMPRESSED   = 0x01;  // ペイロード圧縮済み（将来拡張）
inline constexpr uint8_t  FLAG_ACK          = 0x02;  // ACK（将来拡張）
inline constexpr uint8_t  FLAG_REQUEST      = 0x04;  // RPC リクエスト (F-002)
inline constexpr uint8_t  FLAG_RESPONSE     = 0x08;  // RPC レスポンス (F-002)
inline constexpr uint8_t  FLAG_HELLO        = 0x80;  // Capability Negotiation (A-001, v1.1.0)

inline constexpr uint32_t NO_MESSAGE_ID     = 0x00000000u;  // ID なし（通常 send/receive）
inline constexpr uint32_t EMPTY_CHECKSUM    = 0x00000000u;  // payload_size=0 時のチェックサム

} // namespace pipeutil::detail
```

---

## 8. 空メッセージ

`payload_size == 0` の場合、ペイロード読み出しはスキップする（20 バイトのヘッダのみ）。
`checksum` は `0x00000000` を設定する（未定義とせず固定値とすること）。

---

## 9. プロトコルバージョン互換性

| version | 説明 |
|---------|------|
| `0x01` | 旧バージョン（v1.0.0; 16 バイトヘッダ、`message_id` フィールドなし）|
| `0x02` | 現行バージョン（v1.1.0 以降; 20 バイトヘッダ、`message_id` フィールド追加 F-002）|
| `0x00` | 予約（使用禁止）|
| `0x03`〜 | 将来拡張（未定義）|

受信側は `version != 0x02` の場合 `InvalidMessage` 例外を送出する（フォワード互換性なし）。  
**注意**: v1.0.0 実装（version=`0x01`）は受信時に `version != 0x01` を `InvalidMessage` として拒否するため、
v1.1.0 クライアントから v1.0.0 サーバーへの接続は不可となる。
混在デプロイ戦略は [spec/A001_capability_negotiation.md](A001_capability_negotiation.md) を参照。

---

## 10. フロー制御

- 現バージョンはフロー制御なし（OS バッファに依存）
- 送信側が高速すぎる場合は OS のパイプ書き込みブロックに依存する
- 将来拡張: ウィンドウベースフロー制御 (`FLAG_ACK` 使用)

---

## 11. エラー挙動まとめ

| 状況 | 送出例外 |
|------|---------|
| マジック不一致 | `InvalidMessage` |
| バージョン不一致 | `InvalidMessage` |
| CRC-32C 不一致 | `InvalidMessage` |
| `payload_size > 0xFFFFFFFF` | （構造上発生しない） |
| タイムアウト中に接続切断 | `ConnectionReset` |
| ヘッダ途中での EOF | `ConnectionReset` |
