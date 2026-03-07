// detail/frame_header.hpp — ワイヤーフォーマットのヘッダ構造体定義
// プロトコル仕様は spec/02_protocol.md / spec/F002_rpc_message_id.md を参照。
#pragma once

#include <cstdint>

namespace pipeutil::detail {

// ──────────────────────────────────────────────────────────────────────────────
// FrameHeader — 20 バイト固定長ヘッダ (PROTOCOL_VERSION = 0x02)
//
//  Offset | Size | Field
//  -------+------+---------------------------------------------------
//    0    |  4   | magic[4]       {'P','I','P','E'}
//    4    |  1   | version        0x02
//    5    |  1   | flags          FLAG_* ビット
//    6    |  2   | reserved[2]    0x00 0x00（送信時は必ず 0 で埋める）
//    8    |  4   | payload_size   ペイロードバイト数（LE）    ← v0.01 と同オフセット
//   12    |  4   | checksum       CRC-32C（ペイロードのみ対象、LE）← v0.01 と同オフセット
//   16    |  4   | message_id     RPC メッセージ ID（LE）; 0 = ID なし ← 末尾追加
//  -------+------+---------------------------------------------------
//   合計  | 20   |
// ──────────────────────────────────────────────────────────────────────────────

#pragma pack(push, 1)  // パディングなし（明示的レイアウト）
struct FrameHeader {
    uint8_t  magic[4];        // {'P','I','P','E'}
    uint8_t  version;         // PROTOCOL_VERSION (0x02)
    uint8_t  flags;           // FLAG_* ビット
    uint8_t  reserved[2];     // 将来拡張用。送信時は 0x00 で埋めること
    uint32_t payload_size;    // ペイロードサイズ（リトルエンディアン）
    uint32_t checksum;        // CRC-32C（ペイロードのみ、リトルエンディアン）
    uint32_t message_id;      // RPC メッセージ ID（LE）; 0 = ID なし（末尾追加 F-002）
};
#pragma pack(pop)

// FrameHeader のサイズは常に 20 バイトでなければならない (F-002 仕様)
static_assert(sizeof(FrameHeader) == 20, "FrameHeader must be exactly 20 bytes");

// ─── マジック・バージョン定数 ─────────────────────────────────────────
inline constexpr uint8_t  MAGIC[4]          = {0x50, 0x49, 0x50, 0x45};  // "PIPE"
inline constexpr uint8_t  PROTOCOL_VERSION  = 0x02;  // F-002 で 0x01 → 0x02 へ更新

// ─── flags ビット定義 ─────────────────────────────────────────────────
inline constexpr uint8_t  FLAG_COMPRESSED   = 0x01;  // ペイロード圧縮済み（将来拡張）
inline constexpr uint8_t  FLAG_ACK          = 0x02;  // ACK メッセージ（将来拡張）
inline constexpr uint8_t  FLAG_REQUEST      = 0x04;  // RPC リクエスト (F-002)
inline constexpr uint8_t  FLAG_RESPONSE     = 0x08;  // RPC レスポンス (F-002)

// ─── message_id 特殊値 ───────────────────────────────────────────────
inline constexpr uint32_t NO_MESSAGE_ID     = 0x00000000u;  // ID なし（通常 send/receive）
inline constexpr uint32_t RESERVED_ID_MAX   = 0xFFFFFFFFu;  // 予約（使用禁止）

// ─── 空メッセージのチェックサム固定値 ────────────────────────────────
inline constexpr uint32_t EMPTY_CHECKSUM    = 0x00000000u;

} // namespace pipeutil::detail
